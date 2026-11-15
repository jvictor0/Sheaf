# Delta — `synth-parameter-modulation`

Every clause the promoted requirements below already assert is carried
forward verbatim, scoped to a group configured with modulation blend mode
`kCrossfade` (today's default, unchanged for every existing caller —
`braid-4` and `miniapp` neither set nor read the new field). Each MODIFIED
requirement then adds the `kAttenuverter` alternative as new preamble
clauses and new scenarios; nothing existing is dropped, so no
`RESTATES-EXCEPT` declaration is needed anywhere in this delta.

## MODIFIED Requirements

### Requirement: spm-4 — Group config: dynamic shape and upfront allocation
WHEN a `ParameterGroup` is configured, THE group SHALL use runtime configuration for voice count, modulator count, scene count, maximum parameter count, process-lite alpha, target-center alpha, target compute interval in samples, UI display-center alpha, UI display-spread alpha, and modulation blend mode; SHALL NOT accept parameter base colors, voice indicator colors, a voice color palette, or an independent gesture count in group configuration; the target-center alpha SHALL default to a 50 Hz-style one-pole alpha at the default target-compute cadence; the target compute interval SHALL default to 16 samples and SHALL be positive; process-lite alpha, target-center alpha, UI display-center alpha, and UI display-spread alpha SHALL be in `[0, 1]`; modulation blend mode SHALL default to `ModulationBlendMode::kCrossfade` when not set, and MAY be set to `ModulationBlendMode::kAttenuverter` at construction; SHALL size parameter per-scene/per-gesture arrays from the owning manager's gesture count injected by `ParameterManager::CreateGroup`; SHALL allocate per-parameter subarrays upfront through a group-owned allocator; and SHALL perform no heap allocation during `Compute`, per-sample parameter processing, `ProcessLite`, `GetRaw`, cached knob reads, routed reset-modifier press, routed random-modifier press, routed tick handling, or routed unmodified press handling after any needed modulation-depth parameters for that view have already been materialized. Routed unmodified press and random-mod modifier operations MAY lazily materialize missing modulation-depth parameter objects from preconfigured group capacity.

#### Scenario: Same-shaped group parameters
- **WHEN** two parameters are created in the same group
- **THEN** both parameters have subarrays sized from the same group shape and the owning manager's gesture count

#### Scenario: Allocation exhaustion
- **WHEN** creating a parameter would exceed the group allocator capacity
- **THEN** creation fails without registering a partial parameter in the manager or any bank

#### Scenario: Gesture count is not group-owned
- **WHEN** group configuration is inspected
- **THEN** it contains no independent gesture count field
- **AND** every parameter in the group uses the gesture count supplied by the manager

#### Scenario: Voice indicator colors are not group-owned
- **WHEN** group configuration is inspected
- **THEN** it contains no voice indicator palette
- **AND** parameter UI state obtains indicator colors from each parameter's resolved configuration

#### Scenario: Default target center alpha is configured
- **WHEN** a group is created without overriding its target-center alpha
- **THEN** the group configuration reports the default target-center alpha

#### Scenario: Target center alpha must be normalized
- **WHEN** a group is configured with target-center alpha less than `0` or greater than `1`
- **THEN** group creation rejects the configuration

#### Scenario: Default target compute interval is 16 samples
- **WHEN** a group is created without overriding its target compute interval
- **THEN** the group configuration reports a target compute interval of 16 samples

#### Scenario: Target compute interval must be positive
- **WHEN** a group is configured with target compute interval `0`
- **THEN** group creation rejects the configuration

#### Scenario: Default modulation blend mode is crossfade
- **WHEN** a group is created without setting modulation blend mode
- **THEN** the group configuration reports `ModulationBlendMode::kCrossfade`
- **AND** every existing caller (`braid-4`'s three groups, `miniapp`'s group) that does not set the field is unaffected

### Requirement: spm-9 — Compute: signed modulation normalization
WHEN `Parameter::Compute()` calculates per-voice modulation depths for a group configured with modulation blend mode `kCrossfade`, THE parameter SHALL derive the normalization factor from `sum(abs(rawDepth[voice][modIx]))`; SHALL use each raw depth unchanged when the factor is less than or equal to `1`; SHALL divide every raw depth by the factor when the factor is greater than `1`; SHALL set the per-voice center scale to `max(0, 1 - normalizationFactor)`; SHALL derive a per-voice normalization offset from the effective normalized depths as `-sum(min(0, effectiveDepth[voice][modIx]))`; and SHALL include that offset in both target and current audio-rate reads before adding the modulator dot product. `ProcessLite` SHALL slew the normalization offset with the same control-rate smoothing model as center scale and modulation depths.

WHEN `Parameter::Compute()` calculates per-voice modulation depths for a group configured with modulation blend mode `kAttenuverter`, THE parameter SHALL NOT derive a normalization factor and SHALL NOT divide or renormalize raw depths between routes regardless of their summed magnitude; SHALL set the per-voice center scale to `1` unconditionally, so the parameter's own commanded center keeps full weight at every depth; and SHALL compute the per-voice result as `clamp(knob + sum(rawDepth[voice][modIx] * signal[modIx] * 0.5), range)`, where `signal[modIx]` decodes that route's registered source to its real signal from `ModulatorMetadata::restsAtZero` (spm-93), read by source index and never by a hardcoded route or slot number: `signal = modulatorSource` (range `[0, 1]`) when `restsAtZero` is `true`, or `signal = 2 * modulatorSource - 1` (range `[-1, 1]`) when `restsAtZero` is `false` (the default) — so a `restsAtZero = false` route swings the full `±0.5` of the parameter's range at full depth and full signal, and a `restsAtZero = true` route swings only up to `+0.5` (never `+1`) at full depth and full signal, and contributes exactly `0` when its own source rests (`0` for `restsAtZero = true`, `0.5` for `restsAtZero = false`), regardless of depth. THE parameter SHALL realize this through the SAME `centerScale`/`normalizationOffset`/modulator-dot-product expression `kCrossfade` uses (`Parameter::GetRaw`/`Parameter::TargetValue`), without changing that expression, as follows: for each active route, an effective depth `effectiveDepth[modIx] = restsAtZero ? 0.5 * rawDepth[modIx] : rawDepth[modIx]` (a `restsAtZero = true` route's own stored depth is halved in place before the rest of this computation, mirroring the `kCrossfade` branch's own existing in-place `/= weightSum` mutation at the same point in `ComputeAtDepth`); a rest point `restPoint[modIx] = restsAtZero ? 0 : 0.5`; and a per-voice normalization offset `-sum(effectiveDepth[modIx] * restPoint[modIx])` over active routes (a `restsAtZero = true` route's own `restPoint = 0` contributes `0` to this sum regardless of the halving), included in both target and current audio-rate reads before adding the modulator dot product `sum(effectiveDepth[modIx] * modulatorSource[modIx])`. Algebraically this makes each route's contribution `effectiveDepth[route] * (modulatorSource[route] - restPoint[route])`, which equals `rawDepth[route] * signal[route] * 0.5` for either category (shown by direct substitution). A negative raw depth inverts the sign of its route's contribution with no separate branch. `ProcessLite` SHALL slew the normalization offset with the same control-rate smoothing model as center scale and modulation depths.

#### Scenario: Mixed positive and negative depths offset the unipolar modulator range
- **WHEN** a unipolar parameter has raw depths `+0.25` and `-0.5`
- **THEN** its center scale is `0.25`
- **AND** its normalization offset is `0.5`
- **AND** audio-rate reads use `center * 0.25 + 0.5 + mod0 * 0.25 + mod1 * -0.5`

#### Scenario: Overfull mixed depths derive offset after normalization
- **WHEN** a unipolar parameter has raw depths `+1.0` and `-1.0`
- **THEN** its effective depths are `+0.5` and `-0.5`
- **AND** its normalization offset is `0.5`
- **AND** the offset is not derived from the unnormalized negative raw depth

#### Scenario: An attenuverter parameter's knob keeps full weight at any depth
- **WHEN** a unipolar `kAttenuverter` parameter has one active route, registered with the default `restsAtZero = false`, at raw depth `1.0` (full depth)
- **THEN** its center scale is `1`
- **AND** its normalization offset is `-0.5`
- **AND** audio-rate reads use `center * 1 + (-0.5) + mod0 * 1.0`, algebraically `center + 1.0 * (mod0 - 0.5)`

#### Scenario: Several attenuverter routes add independently with no renormalization
- **WHEN** a unipolar `kAttenuverter` parameter has two active routes, both `restsAtZero = false`, at raw depths `0.8` and `0.6` (summed magnitude `1.4`, greater than `1`)
- **THEN** neither raw depth is divided or scaled by the summed magnitude
- **AND** its center scale remains `1`
- **AND** its normalization offset is `-0.5 * (0.8 + 0.6) = -0.7`
- **AND** the read is `center + 0.8 * (mod0 - 0.5) + 0.6 * (mod1 - 0.5)`, clamped to the parameter's range by `GetRaw`'s own final clamp if the sum would otherwise leave it

#### Scenario: Negative attenuverter depth inverts the source's contribution
- **WHEN** an `kAttenuverter` route, `restsAtZero = false`, has raw depth `-1.0` and its modulator source is `1.0`
- **THEN** that route's contribution is `-1.0 * (1.0 - 0.5) = -0.5`
- **AND** the same route at modulator source `0.0` contributes `-1.0 * (0.0 - 0.5) = 0.5`

#### Scenario: A rest-at-zero source contributes nothing at its own rest value, at any depth
- **WHEN** a unipolar `kAttenuverter` parameter has one active route registered `restsAtZero = true`, at raw depth `1.0` (full depth), with that route's modulator source held at its own rest value `0.0`
- **THEN** its effective depth is `0.5 * 1.0 = 0.5` (halved, per the unified law's `* 0.5` factor) and its normalization offset is `0.5 * 0 = 0.0` (its rest point is `0`, so the halving does not matter here)
- **AND** audio-rate reads use `center * 1 + 0.0 + mod0 * 0.5` with `mod0 = 0.0`, algebraically `center + 0.5 * (0.0 - 0.0) = center`, unchanged regardless of the depth's magnitude or sign
- **AND** the same route registered `restsAtZero = false` instead, at the same depth and the same source value `0.0`, reads `center - 0.5` — a different, non-zero result, which is the defect this field exists to prevent

#### Scenario: A rest-at-0.5 source contributes nothing at its own midpoint
- **WHEN** a unipolar `kAttenuverter` parameter has one active route registered `restsAtZero = false`, at raw depth `1.0`, with that route's modulator source held at `0.5`
- **THEN** its normalization offset is `-1.0 * 0.5 = -0.5`
- **AND** audio-rate reads use `center * 1 + (-0.5) + mod0 * 1.0` with `mod0 = 0.5`, algebraically `center + 1.0 * (0.5 - 0.5) = center`, unchanged

### Requirement: spm-70 — Parameters: unipolar core value domain
WHEN parameter state is configured, edited, computed, smoothed, cached, or serialized, THE synth parameter modulation system SHALL represent defaults, scene centers, gesture values, current and target centers, cached knob values, dynamic min/max values, and modulation-depth control values in normalized `[0, 1]` space regardless of bipolar presentation metadata; SHALL use bipolar metadata only when validating bipolar mapping helpers or converting normalized values for signed consumption and UI publication; SHALL preserve the existing bounded crossfade modulation law and signed overfull-depth normalization in normalized space for a group configured with modulation blend mode `kCrossfade`; and SHALL preserve a bounded attenuverter modulation law in the same normalized space for a group configured with modulation blend mode `kAttenuverter`: `clamp(knob + sum(depth[route] * signal[route] * 0.5), range)`, constant full-weight center scale, unrenormalized signed depths, each route's own registered source decoded to a signal (spm-93) before the shared `* 0.5` factor, and a range-clamped sum of independent per-route contributions. Both laws use the identical normalized `[0, 1]` representation, differing only in how `centerScale` and `normalizationOffset` are derived (spm-9); no other stored or published value's domain or shape depends on the group's blend mode.

#### Scenario: Bipolar neutral is stored at normalized center
- **WHEN** a bipolar parameter represents signed neutral `0`
- **THEN** its internal default, scene, current, target, and cached knob values are `0.5`

#### Scenario: Positive full-depth crossfade reaches the source
- **WHEN** a parameter has effective depth `1` from one modulation route
- **THEN** its center scale is `0`
- **AND** its normalized result equals that route's unipolar modulation source
- **AND** a bipolar read maps the resulting source range `0..1` to `-1..1`

#### Scenario: Negative full-depth crossfade reaches the inverted source
- **WHEN** a parameter has effective depth `-1` from one modulation route
- **THEN** its center scale is `0`
- **AND** its normalization offset is `1`
- **AND** its normalized result equals `1 - source`
- **AND** a bipolar read maps that result to the negated signed source

#### Scenario: Bipolar UI conversion occurs at publication
- **WHEN** a bipolar parameter has normalized display center `0.25` and normalized min/max values `0` and `1`
- **AND** `Parameter::PopulateUIState` publishes its snapshot
- **THEN** the snapshot reports signed display center `-0.5` and signed min/max values `-1` and `1`

#### Scenario: Serialized parameter values use the core domain
- **WHEN** parameter value JSON is saved after this change
- **THEN** bipolar parameter values and modulation-depth control values are serialized in normalized `[0, 1]` space

#### Scenario: Full-depth attenuverter swing reaches exactly half the range, never the bare source
- **WHEN** a unipolar `kAttenuverter` parameter has center `0.5`, one active route registered `restsAtZero = false` at raw depth `1.0`, and that route's modulator source at `1.0`
- **THEN** its center scale is `1` (never `0`)
- **AND** its normalized result is `0.5 + 1.0 * (1.0 - 0.5) = 1.0`, the top of the range, not the bare unipolar source value substituted for the center
- **AND** at modulator source `0.0` the same configuration reads `0.5 + 1.0 * (0.0 - 0.5) = 0.0`, the bottom of the range
- **AND** at modulator source `0.5` (the source's own registered rest point) the read is unchanged at `0.5` regardless of depth, because `(source - restPoint) = 0`

#### Scenario: Full-depth attenuverter swing on a rest-at-zero source reaches only half the range, and only ever pushes one way
- **WHEN** a unipolar `kAttenuverter` parameter has center `0.5`, one active route registered `restsAtZero = true` at raw depth `1.0` (full depth), and that route's modulator source at its own rest value `0.0`
- **THEN** its effective depth is `0.5 * 1.0 = 0.5` (the unified law's shared `* 0.5` factor, unlike a `restsAtZero = false` route which is not halved) and its normalized result is `0.5 + 0.5 * (0.0 - 0.0) = 0.5`, unchanged — a resting follower adds nothing
- **AND** at the same route's own maximum signal `1.0` the result is `0.5 + 0.5 * (1.0 - 0.0) = 1.0` — full depth and full signal push the target up by exactly `0.5`, never by `1.0`, confirming the operator's 2026-09-28 correction to the earlier (wrong) revision, which read this case as pushing by the full raw depth
- **AND** a `restsAtZero = false` route at the same depth and center, with its own stored source at `0.0` (that category's own OPPOSITE extreme, not its rest), reads `0.5 + 1.0 * (0.0 - 0.5) = 0.0` — pulled DOWN, unlike the rest-at-zero route above, which at the identical stored value `0.0` (its own rest) stayed unchanged at `0.5`; the same stored value means a different thing to each category

#### Scenario: Attenuverter serialization is unchanged
- **WHEN** parameter value JSON is saved for a parameter in an `kAttenuverter` group
- **THEN** the depth knob's own commanded value is serialized identically to the `kCrossfade` case (spm-50), since the blend mode is a group-level compute-time configuration, not a per-value stored field

### Requirement: spm-10 — Compute: dynamic modulation min/max
WHEN `Parameter::Compute()` calculates per-voice modulation state for a group configured with modulation blend mode `kCrossfade`, THE parameter SHALL also compute per-voice target minimum and maximum values in normalized `[0, 1]` space representing the audio-rate range reachable from unipolar `[0, 1]` modulation sources; SHALL slew current minimum and maximum values in normalized space in `ProcessLite`; and SHALL publish those values through `Parameter::UIState::minValues` and `Parameter::UIState::maxValues`, converting them to `[-1, 1]` only when the parameter has bipolar presentation metadata. If `sum(abs(rawDepth[voice][modIx])) > 1`, the normalized target min/max SHALL be the full `0..1` range. Otherwise, min/max SHALL be computed from the current effective formula as `base + sum(min(0, effectiveDepth))` and `base + sum(max(0, effectiveDepth))`, clamped to `[0, 1]`, where `base = center * centerScale + normalizationOffset`.

WHEN `Parameter::Compute()` calculates per-voice modulation state for a group configured with modulation blend mode `kAttenuverter`, THE parameter SHALL compute per-voice target minimum and maximum values, in the same normalized `[0, 1]` space and published through the same `Parameter::UIState::minValues`/`maxValues` fields, from each active route's own effective depth and registered rest point (spm-93): `effectiveDepth = restsAtZero ? 0.5 * rawDepth : rawDepth` and `restPoint = restsAtZero ? 0 : 0.5`; per route, `candidateA = effectiveDepth * (0 - restPoint)` and `candidateB = effectiveDepth * (1 - restPoint)` (that route's own contribution at the two ends of its source's `[0,1]` range); `minContribution = sum(min(candidateA, candidateB))` and `maxContribution = sum(max(candidateA, candidateB))` over active routes; and the published minimum/maximum are `center + minContribution` and `center + maxContribution`, clamped to `[0, 1]`. For every active route with `restsAtZero = false` (`restPoint = 0.5`, `effectiveDepth = rawDepth`, today's only case) this reduces exactly to `center - reach`/`center + reach` with `reach = sum(0.5 * abs(rawDepth[voice][modIx]))`, the symmetric half-width already defined; for a route with `restsAtZero = true` (`restPoint = 0`, `effectiveDepth = 0.5 * rawDepth`), it reduces to a one-sided `[min(0, 0.5 * rawDepth), max(0, 0.5 * rawDepth)]` contribution — half the magnitude of a `restsAtZero = false` route at the same raw depth, matching the unified law's shared `* 0.5` factor, since that source's own range never crosses its rest point. No `sum(abs(rawDepth)) > 1` special case is needed, since the ordinary clamp already bounds the published range.

#### Scenario: UI state publishes dynamic underfull min/max
- **WHEN** a unipolar parameter with center `0.5` has raw depths `+0.25` and `-0.5`
- **THEN** its UI-state minimum is `0.125`
- **AND** its UI-state maximum is `0.875`

#### Scenario: UI state publishes signed full range for overfull bipolar depths
- **WHEN** a bipolar parameter has `sum(abs(rawDepths)) > 1`
- **THEN** its normalized internal minimum and maximum are `0` and `1`
- **AND** its UI-state minimum is `-1.0`
- **AND** its UI-state maximum is `1.0`

#### Scenario: The ring shows an attenuverter's swing reach, not the full range, at moderate depth
- **WHEN** a unipolar `kAttenuverter` parameter has center `0.5` and one active route, `restsAtZero = false`, at raw depth `0.4`
- **THEN** `reach = 0.5 * 0.4 = 0.2`
- **AND** its UI-state minimum is `0.3` and maximum is `0.7`

#### Scenario: An attenuverter's swing reach clamps to the parameter's range when it would overflow
- **WHEN** a unipolar `kAttenuverter` parameter has center `0.8` and one active route, `restsAtZero = false`, at raw depth `1.0` (`reach = 0.5`)
- **THEN** the unclamped maximum `0.8 + 0.5 = 1.3` is clamped to `1.0`
- **AND** the unclamped minimum `0.8 - 0.5 = 0.3` is unaffected by the clamp

#### Scenario: A rest-at-zero source's dynamic range is one-sided, not symmetric, and half the magnitude of a rest-at-0.5 source at the same raw depth
- **WHEN** a unipolar `kAttenuverter` parameter has center `0.5` and one active route, `restsAtZero = true`, at raw depth `0.4`
- **THEN** its effective depth is `0.5 * 0.4 = 0.2`, so `candidateA = 0.2 * (0 - 0) = 0.0` and `candidateB = 0.2 * (1 - 0) = 0.2`
- **AND** its UI-state minimum is `0.5 + min(0.0, 0.2) = 0.5` (unchanged from center — the route can never pull the value below its own rest contribution of `0`)
- **AND** its UI-state maximum is `0.5 + max(0.0, 0.2) = 0.7` — reaching exactly as far upward as a `restsAtZero = false` route at the identical raw depth `0.4` does ("The ring shows an attenuverter's swing reach..." above also reaches `0.7`, since the unified law's shared `* 0.5` factor gives both categories the same maximum upward reach), but never downward: its minimum stays at center (`0.5`) instead of the `0.3` a `restsAtZero = false` route reaches at the same depth, since a rest-at-zero source's own range never crosses its rest point

## ADDED Requirements

### Requirement: spm-92 — Compute: per-group modulation blend mode
WHEN a `ParameterGroup` is created, THE `ParameterGroupConfig` SHALL carry a `ModulationBlendMode` field (`kCrossfade` or `kAttenuverter`, defaulting to `kCrossfade`) that selects which law `Parameter::ComputeAtDepth` (spm-9) and the dynamic min/max computation (spm-10) use for every parameter that group owns; a modulation-depth `Parameter` materialized under `Parameter::EnsureModulationDepth` SHALL always be allocated from its parent's own `ParameterGroup` (`Parameter::AssignModulationDepth` SHALL reject any other group), so a nested (depth-of-a-depth) `Parameter` computes under the identical blend mode as its top-level ancestor with no additional per-parameter configuration or branching. The blend mode SHALL NOT be serialized as parameter value state (spm-50/spm-51): it is code-configured per group at construction, not a patch field, so a saved patch's depth-knob values are read under whichever law the running build's group configuration selects.

#### Scenario: A group's blend mode governs every parameter it owns, including nested depths
- **WHEN** a `ParameterGroup` is created with modulation blend mode `kAttenuverter`
- **THEN** every top-level `Parameter` that group allocates computes under the attenuverter law (spm-9, spm-10)
- **AND** a modulation-depth `Parameter` materialized on any of those parameters, and a further depth-of-depth `Parameter` materialized on that depth parameter, both compute under the same attenuverter law, because both are allocated from the identical `ParameterGroup`

#### Scenario: Existing groups are unaffected
- **WHEN** `braid-4`'s three groups and `miniapp`'s group are created without setting modulation blend mode
- **THEN** they report `kCrossfade` and their `Parameter::ComputeAtDepth`/`GetRaw`/`TargetValue`/`Parameter::UIState` results are numerically identical to before this change

#### Scenario: Cross-group assignment stays rejected regardless of blend mode
- **WHEN** `Parameter::AssignModulationDepth` is called with a `Parameter*` owned by a different `ParameterGroup`, in either blend mode
- **THEN** it returns `false` and does not attach the parameter, exactly as it does today (spm-2's "banks and slots hold non-owning parameter pointers only" and the group-ownership guard are unaffected by this requirement)

### Requirement: spm-93 — Compute: per-source attenuverter rest point
WHEN a modulation source is registered through `Modulators::SetModulationSource`, THE `ModulatorMetadata` SHALL carry a `restsAtZero` field (boolean, defaulting to `false`) that decides how a `kAttenuverter` group (spm-92) reads that one source's real signal, independent of every other source's: the unified law is `signal = restsAtZero ? modulatorSource : (2 * modulatorSource - 1)`, and every route's contribution to a `kAttenuverter` parameter's resolved value is `rawDepth * signal * 0.5` — the SAME `0.5` factor for every source regardless of `restsAtZero`, so a `restsAtZero = false` source (the default, matching every source registered before this requirement; its own neutral/inert value sits at the middle of its `[0,1]` range, decoding to `signal` in `[-1, 1]`) swings the full `±0.5` of the parameter's range at full depth and full signal, and a `restsAtZero = true` source (its own neutral/inert value sits at the floor of its `[0,1]` range, such as an envelope follower or another magnitude-only source; decodes to `signal` in `[0, 1]`) swings only up to `+0.5` — never `+1` — at full depth and full signal, and contributes exactly `0` when its own source rests, for either category. `Parameter::ComputeAtDepth` (spm-9) and the dynamic min/max computation (spm-10) SHALL read each active route's `restsAtZero` from that route's own registered source's `ModulatorMetadata`, by source index (`RouteSourceIndex`/`ActiveRouteSourceIndices`), and SHALL NOT read or branch on a route's slot number, position, or any other hardcoded index to decide it. This field SHALL have no effect for a group configured with modulation blend mode `kCrossfade`, and adding it SHALL NOT change any existing `ModulatorMetadata` aggregate-initializer's meaning, since it is a new trailing field with a default.

#### Scenario: A rest-at-zero source contributes nothing at its own rest value
- **WHEN** a source registered `restsAtZero = true` is read by an `kAttenuverter` parameter at any nonzero depth, with that source held at `0`
- **THEN** the route's contribution to the parameter's resolved value is `0`, regardless of the depth's magnitude or sign

#### Scenario: A rest-at-0.5 source contributes nothing at its own midpoint
- **WHEN** a source registered `restsAtZero = false` (the default) is read by an `kAttenuverter` parameter at any nonzero depth, with that source held at `0.5`
- **THEN** the route's contribution to the parameter's resolved value is `0`, regardless of the depth's magnitude or sign

#### Scenario: Both categories reach the same maximum swing, 0.5, never 1.0
- **WHEN** a source registered `restsAtZero = true` is read at full depth (`1.0`) with that source at its own maximum (`1.0`, `signal = 1.0`), and separately a source registered `restsAtZero = false` is read at full depth (`1.0`) with that source at its own maximum (`1.0`, `signal = 2*1.0-1 = 1.0`)
- **THEN** both routes contribute `1.0 * 1.0 * 0.5 = 0.5` to their parameter's resolved value — the identical maximum, never `1.0`
- **AND** only the `restsAtZero = false` route can also swing the other way: at that same source held at its own minimum (`0.0`, `signal = -1.0`), it contributes `1.0 * -1.0 * 0.5 = -0.5`, while the `restsAtZero = true` route at its own minimum (`0.0`, `signal = 0.0`, its own rest) contributes `0`

#### Scenario: The distinction is per-source registered data, not a hardcoded index
- **WHEN** two sources are registered at adjacent indices, one `restsAtZero = true` and the other `restsAtZero = false`
- **THEN** `Parameter::ComputeAtDepth` resolves each route's rest point by reading that route's own source's `ModulatorMetadata`, not by comparing the route's index or slot number against a fixed set
- **AND** swapping which physical index each source is registered at (with its own `restsAtZero` value carried along) produces the identical resolved values, confirming no index is hardcoded

#### Scenario: Existing sources are unaffected
- **WHEN** every `ModulatorMetadata` registered before this requirement is inspected
- **THEN** each reports `restsAtZero == false`, its aggregate-initializer having supplied no sixth value
- **AND** their resolved values under `kAttenuverter` are unchanged by this requirement's addition
