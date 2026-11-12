## Purpose

Define the C++ synth parameter and modulation library, including ownership,
scene and gesture computation, audio-rate modulation reads, physical-control
routing, and randomized simulation coverage.
## Requirements
### Requirement: spm-1 — Project: C++ synth library
WHEN the synth parameter modulation capability is implemented, THE repository SHALL contain a `projects/synth` C++20 project with library sources, public headers, a project Makefile exposing `all`, `build`, `test`, and `clean`, and root Makefile targets that include `synth` in normal project build and test flows.

#### Scenario: Project builds through root Makefile
- **WHEN** a developer runs the root `make synth-build`
- **THEN** the synth library builds from `projects/synth`

#### Scenario: Project tests through root Makefile
- **WHEN** a developer runs the root `make synth-test`
- **THEN** the synth parameter modulation test suite runs from `projects/synth`

### Requirement: spm-2 — Ownership: manager, groups, parameters, banks, and slots
WHEN constructing the synth parameter system, THE system SHALL use a single `ParameterManager` to own global scene state, global gesture state, page state, parameter groups, banks, slots, and the global parameter ID space; the manager's gesture count SHALL default to zero and MAY be changed only before any group is created; `ParameterManager::CreateGroup` SHALL inject the manager's fixed gesture count and a non-owning manager pointer into each `ParameterGroup`; each `ParameterGroup` SHALL own its processing/storage config, `Modulators`, injected gesture count for arena sizing, non-owning manager pointer, and allocator, but SHALL NOT own gesture values, gesture selection, gesture metadata, parameter base colors, or parameter indicator palettes; each `Parameter` SHALL be allocated by exactly one group, own its resolved base/indicator appearance, and read runtime gesture values, selection, and metadata through the owning manager context rather than through group-local gesture state; banks and slots SHALL hold non-owning parameter pointers only.

#### Scenario: Parameter receives globally unique ID
- **WHEN** parameters are created across multiple groups
- **THEN** the manager assigns each parameter a unique global ID

#### Scenario: Bank does not own parameter lifetime
- **WHEN** a bank is populated with parameters from one or more groups
- **THEN** destroying or deselecting the bank does not destroy those parameters

#### Scenario: Gestures are manager-owned across groups
- **WHEN** parameters from two groups are edited while gesture 1 is selected
- **THEN** both parameters observe the same manager-owned gesture 1 selected state and gesture value

#### Scenario: Gesture count is fixed before groups
- **WHEN** a manager is configured with two gestures before any group is created
- **THEN** every subsequently created group sizes parameter gesture arrays from that count
- **AND** changing gesture count after group creation is not supported

#### Scenario: Default manager gesture count is zero
- **WHEN** a manager is default constructed and no gesture count is configured before group creation
- **THEN** subsequently created groups size parameter gesture arrays for zero gestures

#### Scenario: Group cannot carry independent gesture count
- **WHEN** a group is created through a manager configured with two gestures
- **THEN** the group receives gesture count `2` from the manager
- **AND** no `ParameterGroupConfig` field can override that count

#### Scenario: Group cannot carry parameter appearance
- **WHEN** two parameters share one group
- **THEN** each parameter can own a different base color and per-voice indicator palette
- **AND** no group field can override either appearance

### Requirement: spm-3 — Pages: assignment and active UI page
WHEN parameters are assigned to pages, THE manager SHALL maintain page ordinals within the manager, allow each parameter to be assigned to zero or more pages, and expose exactly one active page for UI routing without changing parameter audio values, scene values, gesture values, or modulation routes.

#### Scenario: Active page changes routing only
- **WHEN** the active page changes
- **THEN** subsequent routed UI actions target the new page mapping
- **AND** existing parameter center, depth, scene, gesture, and modulator values remain unchanged

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

### Requirement: spm-5 — Modulators: flat per-voice values and metadata
WHEN a group owns modulators, THE `Modulators` struct SHALL store current modulator values in one flat row-major array indexed as `voiceIx * numModulators + modulatorIx`, store per-modulator metadata including name, short name, color, and connected flag, and provide an `Apply(voiceIx, depths)` function that returns only the dot product of that voice's modulator row and the supplied depth row.

#### Scenario: Apply computes a voice-local dot product
- **WHEN** a group has `numModulators = 3`, voice 2 values `[0.25, -0.5, 1.0]`, and supplied depths `[0.4, 0.2, -0.1]`
- **THEN** `Apply(2, depths)` returns `(0.25 * 0.4) + (-0.5 * 0.2) + (1.0 * -0.1)`

#### Scenario: Metadata is not per voice
- **WHEN** a modulator name, short name, or color is changed
- **THEN** the changed metadata applies to that modulator for every voice

### Requirement: spm-6 — Gestures: values, selection, metadata, and active flags
WHEN the synth parameter system owns gestures, THE `ParameterManager` SHALL own one `Gestures` state containing global gesture values, selected flags, and per-gesture metadata, while each parameter SHALL store per-scene/per-gesture gesture values and active flags; gesture values and active flags SHALL NOT be stored per voice, and gesture selection SHALL NOT be stored per group.

#### Scenario: Gesture selection is global to manager
- **WHEN** gesture 1 is selected on the manager
- **THEN** all parameters in every group observe gesture 1 as selected for edit routing

#### Scenario: Parameter gesture state is per scene
- **WHEN** a gesture is active for scene 0 on one parameter
- **THEN** the same gesture can remain inactive for scene 1 and for other parameters

#### Scenario: Gesture metadata is shared
- **WHEN** gesture 1 metadata is changed on the manager
- **THEN** every group and UI snapshot reads the same gesture 1 metadata

### Requirement: spm-7 — Parameter data: metadata, scene state, modulation routes, and runtime state
WHEN a `Parameter` is created, THE parameter SHALL store name, short name, global ID, recursion depth, default normalized value, bipolar flag, group pointer, current center value, target center value, current center scale for each voice, target center scale for each voice, current modulation depths for each voice/modulator pair, target modulation depths for each voice/modulator pair, nullable modulation-depth parameter pointers for each modulator, scene center values, and per-scene/per-gesture gesture values and active flags.

#### Scenario: Default parameter state
- **WHEN** a parameter is created with default value `0.3`
- **THEN** its scene center values, current center, and target center are initialized to `0.3`
- **AND** its current and target center scales are initialized to `1.0`
- **AND** its modulation-depth pointers are null
- **AND** its current and target modulation depths are zero

### Requirement: spm-8 — Compute: scene, gesture interpolation, and target-center smoothing
WHEN `Parameter::Compute()` calculates target state, THE parameter SHALL first compute the raw center as `sceneCenter[leftScene] * (1 - blend) + sceneCenter[rightScene] * blend`; SHALL compute each gesture's blended gesture value from the parameter's scene gesture values; SHALL compute each gesture's effective weight from the manager-owned gesture value where that gesture is active in the blended scenes; SHALL use the base scene value as the raw center when no effective gesture weight is active; SHALL otherwise use the weighted average of `base * (1 - weight) + gestureValue * weight` across active gestures as the raw center; SHALL slew a top-level parameter's `targetCenter` toward the raw center using the owning group's target-center alpha and the one-pole formula `targetCenter += alpha * (rawCenter - targetCenter)`; and SHALL keep recursive modulation-depth computations assigning `targetCenter` from the raw center before snapping current state to target for parent consumption.

#### Scenario: Scene blend without active gestures
- **WHEN** left scene center is `0.2`, right scene center is `0.8`, blend is `0.25`, and no gesture has effective weight
- **THEN** the raw computed center is `0.35`

#### Scenario: Active gesture blends from base to gesture value
- **WHEN** base is `0.4`, one gesture has blended gesture value `0.9`, and its manager-owned effective weight is `0.5`
- **THEN** the raw computed center is `0.65`

#### Scenario: Cross-group parameters use the same gesture weight
- **WHEN** two parameters in different groups have gesture 1 active
- **AND** the manager-owned gesture 1 value is `0.5`
- **THEN** both parameters compute gesture 1's effective weight from the same manager-owned value

#### Scenario: Top-level compute slews target center
- **WHEN** a top-level parameter has target center `0.0`, raw computed center `1.0`, and group target-center alpha `0.25`
- **THEN** one `Compute()` call sets target center to `0.25`

#### Scenario: Target center alpha one snaps top-level target center
- **WHEN** a top-level parameter has target center `0.0`, raw computed center `1.0`, and group target-center alpha `1.0`
- **THEN** one `Compute()` call sets target center to `1.0`

#### Scenario: Recursive modulation-depth compute remains immediate
- **WHEN** a modulation-depth parameter is computed recursively for a parent parameter
- **THEN** the modulation-depth parameter assigns its target center from the raw computed center for that recursive compute
- **AND** the modulation-depth parameter snaps current center, center scales, normalization offsets, min/max values, and modulation depths to target before the parent reads the depth value

### Requirement: spm-9 — Compute: signed modulation normalization
WHEN `Parameter::Compute()` calculates per-voice modulation depths for a group configured with modulation blend mode `kCrossfade`, THE parameter SHALL derive the normalization factor from `sum(abs(rawDepth[voice][modIx]))`; SHALL use each raw depth unchanged when the factor is less than or equal to `1`; SHALL divide every raw depth by the factor when the factor is greater than `1`; SHALL set the per-voice center scale to `max(0, 1 - normalizationFactor)`; SHALL derive a per-voice normalization offset from the effective normalized depths as `-sum(min(0, effectiveDepth[voice][modIx]))`; and SHALL include that offset in both target and current audio-rate reads before adding the modulator dot product. `ProcessLite` SHALL slew the normalization offset with the same control-rate smoothing model as center scale and modulation depths.

WHEN `Parameter::Compute()` calculates per-voice modulation depths for a group configured with modulation blend mode `kAttenuverter` and the parameter's own `ParameterConfig::modulationTargetKind` is `kBipolar` (the default), THE parameter SHALL NOT derive a normalization factor and SHALL NOT divide or renormalize raw depths between routes regardless of their summed magnitude; SHALL set the per-voice center scale to `1` unconditionally, so the parameter's own commanded center keeps full weight at every depth; and SHALL compute the per-voice result as `clamp(knob + sum(rawDepth[voice][modIx] * signal[modIx] * 0.5), range)`, where `signal[modIx]` decodes that route's registered source to its real signal from `ModulatorMetadata::restsAtZero` (spm-93), read by source index and never by a hardcoded route or slot number: `signal = modulatorSource` (range `[0, 1]`) when `restsAtZero` is `true`, or `signal = 2 * modulatorSource - 1` (range `[-1, 1]`) when `restsAtZero` is `false` (the default) — so a `restsAtZero = false` route swings the full `±0.5` of the parameter's range at full depth and full signal, and a `restsAtZero = true` route swings only up to `+0.5` (never `+1`) at full depth and full signal, and contributes exactly `0` when its own source rests (`0` for `restsAtZero = true`, `0.5` for `restsAtZero = false`), regardless of depth. THE parameter SHALL realize this through the SAME `centerScale`/`normalizationOffset`/modulator-dot-product expression `kCrossfade` uses (`Parameter::GetRaw`/`Parameter::TargetValue`), without changing that expression, as follows: for each active route, an effective depth `effectiveDepth[modIx] = restsAtZero ? 0.5 * rawDepth[modIx] : rawDepth[modIx]` (a `restsAtZero = true` route's own stored depth is halved in place before the rest of this computation, mirroring the `kCrossfade` branch's own existing in-place `/= weightSum` mutation at the same point in `ComputeAtDepth`); a rest point `restPoint[modIx] = restsAtZero ? 0 : 0.5`; and a per-voice normalization offset `-sum(effectiveDepth[modIx] * restPoint[modIx])` over active routes (a `restsAtZero = true` route's own `restPoint = 0` contributes `0` to this sum regardless of the halving), included in both target and current audio-rate reads before adding the modulator dot product `sum(effectiveDepth[modIx] * modulatorSource[modIx])`. Algebraically this makes each route's contribution `effectiveDepth[route] * (modulatorSource[route] - restPoint[route])`, which equals `rawDepth[route] * signal[route] * 0.5` for either category (shown by direct substitution). A negative raw depth inverts the sign of its route's contribution with no separate branch. `ProcessLite` SHALL slew the normalization offset with the same control-rate smoothing model as center scale and modulation depths.

WHEN `Parameter::Compute()` calculates per-voice modulation depths for a group configured with modulation blend mode `kAttenuverter` and the parameter's own `ParameterConfig::modulationTargetKind` is `kOneWayAmount` (spm-94), THE parameter SHALL realize a one-way law through the IDENTICAL `centerScale`/`normalizationOffset`/modulator-dot-product expression the `kCrossfade` case above uses, unchanged, restricted to non-negative depths, rather than through the `kBipolar` case's own attenuverter expression, as follows: THE parameter SHALL read each active route's own depth from the curve `OneWayModulationDepthTargetFromKnob` (`include/synth/ParameterModulation.hpp`, defined as `max(0, ModulationDepthTargetFromKnob(normalizedKnob))`) rather than `ModulationDepthTargetFromKnob`, so `rawDepth[modIx] ∈ [0, 1]` (never negative); SHALL derive the normalization factor `W = sum(rawDepth[voice][modIx])` exactly as the `kCrossfade` case derives its own factor from `sum(abs(rawDepth[voice][modIx]))` (the two are identical here, since no one-way raw depth is ever negative); SHALL use each raw depth unchanged when `W <= 1` and set the per-voice center scale to `1 - W`; SHALL divide every raw depth by `W` when `W > 1` and set the per-voice center scale to `0`, exactly as the `kCrossfade` case does; SHALL derive a per-voice normalization offset as `-sum(min(0, effectiveDepth[voice][modIx]))`, which is always `0` since no effective depth is ever negative; and SHALL compute the per-voice result as `clamp(knob * centerScale + normalizationOffset + sum(effectiveDepth[voice][modIx] * modulatorSource[modIx]), range)` — each route's own registered source read exactly as stored (`u[modIx] = modulatorSource[modIx]`), REGARDLESS of `restsAtZero`, so an envelope-follower route (`restsAtZero = true`) contributes `effectiveDepth * modulatorSource` directly, with NO halving, unlike the `kBipolar` case's own treatment of the identical registration. A negative raw depth cannot occur (`OneWayModulationDepthTargetFromKnob`'s own floor at `0`), so no route ever subtracts, and the parameter's own commanded center no longer keeps unconditional full weight the way the `kBipolar` case's center scale does — once `W` reaches `1`, the knob drops out entirely, exactly as it would for any other `kCrossfade` parameter at full effective depth. `ProcessLite` SHALL slew center scale, the normalization offset, and modulation depths with the same control-rate smoothing model as the other two cases.

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

#### Scenario: A one-way amount target crossfades with the knob, never subtracts
- **WHEN** a unipolar `kOneWayAmount` parameter has one active route at raw depth `0.6` (from `OneWayModulationDepthTargetFromKnob`) and modulator source `0.8`, with knob (center) `0.3` (`W = 0.6`)
- **THEN** its center scale is `1 - 0.6 = 0.4` and its normalization offset is `0`
- **AND** the read is `0.4 * 0.3 + 0.6 * 0.8 = 0.60`

#### Scenario: A one-way amount target's route is read directly regardless of restsAtZero
- **WHEN** a `kOneWayAmount` parameter has one active route registered `restsAtZero = true`, at raw depth `1.0`, with modulator source `0.3`
- **THEN** its effective depth is `1.0` (NOT halved to `0.5`, unlike the `kBipolar` case's own treatment of `restsAtZero = true`)
- **AND** its contribution is `1.0 * 0.3 = 0.3`, not `0.15`

#### Scenario: A one-way amount target's routes crossfade together below the ceiling, unrenormalized
- **WHEN** a `kOneWayAmount` parameter has knob `0.2` and two active routes at raw depth `0.5`/source `1.0` and raw depth `0.4`/source `1.0` (`W = 0.9`)
- **THEN** neither raw depth is divided or scaled by the other's presence, since `W <= 1`
- **AND** the read is `(1 - 0.9) * 0.2 + 0.5 * 1.0 + 0.4 * 1.0 = 0.92`

#### Scenario: A one-way amount target's routes renormalize once their total depth passes the ceiling
- **WHEN** a `kOneWayAmount` parameter has knob `0.2` and two active routes at raw depth `0.8`/source `1.0` and raw depth `0.6`/source `0.0` (`W = 1.4`, greater than `1`)
- **THEN** the knob drops out entirely (center scale `0`) and each raw depth is divided by `W`: `0.8 / 1.4 = 0.5714286` and `0.6 / 1.4 = 0.4285714`
- **AND** the read is `0.5714286 * 1.0 + 0.4285714 * 0.0 = 0.5714286`

#### Scenario: A one-way amount target's depth curve cannot produce a negative depth
- **WHEN** `OneWayModulationDepthTargetFromKnob` is evaluated at a knob value that would make `ModulationDepthTargetFromKnob` (the `kBipolar` curve) return a negative depth
- **THEN** `OneWayModulationDepthTargetFromKnob` returns `0` instead, at every such knob value

### Requirement: spm-70 — Parameters: unipolar core value domain
WHEN parameter state is configured, edited, computed, smoothed, cached, or serialized, THE synth parameter modulation system SHALL represent defaults, scene centers, gesture values, current and target centers, cached knob values, dynamic min/max values, and modulation-depth control values in normalized `[0, 1]` space regardless of bipolar presentation metadata; SHALL use bipolar metadata only when validating bipolar mapping helpers or converting normalized values for signed consumption and UI publication; SHALL preserve the existing bounded crossfade modulation law and signed overfull-depth normalization in normalized space for a group configured with modulation blend mode `kCrossfade`; SHALL preserve a bounded attenuverter modulation law in the same normalized space for a group configured with modulation blend mode `kAttenuverter` and a parameter's own `modulationTargetKind` of `kBipolar`: `clamp(knob + sum(depth[route] * signal[route] * 0.5), range)`, constant full-weight center scale, unrenormalized signed depths, each route's own registered source decoded to a signal (spm-93) before the shared `* 0.5` factor, and a range-clamped sum of independent per-route contributions; and SHALL preserve a bounded one-way amount modulation law in the same normalized space for a `kAttenuverter` group and a parameter's own `modulationTargetKind` of `kOneWayAmount` (spm-94): the SAME crossfade law `kCrossfade` uses above, restricted to `depth[route] ∈ [0, 1]` (never negative, `OneWayModulationDepthTargetFromKnob`'s own floor) — with `W = sum(depth[route])`: `clamp((1 - W) * knob + sum(depth[route] * modulatorSource[route]), range)` when `W <= 1`, or `clamp(sum((depth[route] / W) * modulatorSource[route]), range)` when `W > 1` — each route's own registered source read exactly as stored regardless of `restsAtZero`, and (unlike the `kBipolar` law's own constant full-weight center scale) a center scale that falls to `0` once `W` reaches `1`. All three laws use the identical normalized `[0, 1]` representation, differing only in how `centerScale`, `normalizationOffset`, and each route's own effective depth/signal are derived (spm-9); no other stored or published value's domain or shape depends on the group's blend mode or a parameter's target kind.

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

#### Scenario: A one-way amount target at full depth crossfades completely to the source, no longer floored at the knob
- **WHEN** a unipolar `kOneWayAmount` parameter has center (knob) `0.3` and one active route at raw depth `1.0` (full depth, `W = 1.0`)
- **THEN** its center scale is `1 - 1.0 = 0`, so the knob drops out entirely
- **AND** at modulator source `1.0` its normalized result is `1.0`
- **AND** at modulator source `0.0` its normalized result is `0.0`, NOT the knob's own value `0.3` — unlike the add-only law this change replaces, a one-way amount target's own source CAN pull the result below the knob once `W` reaches `1.0`

#### Scenario: A one-way amount target's own depth-knob storage is unchanged, and one-way serialization needs no migration
- **WHEN** a `kOneWayAmount` parameter's depth-knob raw stored value is `0.75`, `0.25`, or `0.5`
- **THEN** the resolved depth is, respectively, the SAME positive magnitude `ModulationDepthTargetFromKnob(0.75)` already produces, exactly `0`, and exactly `0` — `OneWayModulationDepthTargetFromKnob`'s own floor at `0`, substituted at each value
- **AND** `Parameter::ToValueJSON`/`LoadValuesFromJSON` serialize and load that SAME raw float exactly as the `kBipolar` case does (spm-50), with no version marker and no migration

### Requirement: spm-10 — Compute: dynamic modulation min/max
WHEN `Parameter::Compute()` calculates per-voice modulation state for a group configured with modulation blend mode `kCrossfade`, THE parameter SHALL also compute per-voice target minimum and maximum values in normalized `[0, 1]` space representing the audio-rate range reachable from unipolar `[0, 1]` modulation sources; SHALL slew current minimum and maximum values in normalized space in `ProcessLite`; and SHALL publish those values through `Parameter::UIState::minValues` and `Parameter::UIState::maxValues`, converting them to `[-1, 1]` only when the parameter has bipolar presentation metadata. If `sum(abs(rawDepth[voice][modIx])) > 1`, the normalized target min/max SHALL be the full `0..1` range. Otherwise, min/max SHALL be computed from the current effective formula as `base + sum(min(0, effectiveDepth))` and `base + sum(max(0, effectiveDepth))`, clamped to `[0, 1]`, where `base = center * centerScale + normalizationOffset`.

WHEN `Parameter::Compute()` calculates per-voice modulation state for a group configured with modulation blend mode `kAttenuverter` and a parameter's own `modulationTargetKind` of `kBipolar`, THE parameter SHALL compute per-voice target minimum and maximum values, in the same normalized `[0, 1]` space and published through the same `Parameter::UIState::minValues`/`maxValues` fields, from each active route's own effective depth and registered rest point (spm-93): `effectiveDepth = restsAtZero ? 0.5 * rawDepth : rawDepth` and `restPoint = restsAtZero ? 0 : 0.5`; per route, `candidateA = effectiveDepth * (0 - restPoint)` and `candidateB = effectiveDepth * (1 - restPoint)` (that route's own contribution at the two ends of its source's `[0,1]` range); `minContribution = sum(min(candidateA, candidateB))` and `maxContribution = sum(max(candidateA, candidateB))` over active routes; and the published minimum/maximum are `center + minContribution` and `center + maxContribution`, clamped to `[0, 1]`. For every active route with `restsAtZero = false` (`restPoint = 0.5`, `effectiveDepth = rawDepth`, today's only case) this reduces exactly to `center - reach`/`center + reach` with `reach = sum(0.5 * abs(rawDepth[voice][modIx]))`, the symmetric half-width already defined; for a route with `restsAtZero = true` (`restPoint = 0`, `effectiveDepth = 0.5 * rawDepth`), it reduces to a one-sided `[min(0, 0.5 * rawDepth), max(0, 0.5 * rawDepth)]` contribution — half the magnitude of a `restsAtZero = false` route at the same raw depth, matching the unified law's shared `* 0.5` factor, since that source's own range never crosses its rest point. No `sum(abs(rawDepth)) > 1` special case is needed, since the ordinary clamp already bounds the published range.

WHEN `Parameter::Compute()` calculates per-voice modulation state for a group configured with modulation blend mode `kAttenuverter` and a parameter's own `modulationTargetKind` of `kOneWayAmount` (spm-94), THE parameter SHALL compute per-voice target minimum and maximum values through the SAME `kCrossfade` min/max rule the first paragraph above states, restricted to non-negative depths: if `W = sum(rawDepth[voice][modIx]) > 1`, the normalized target min/max SHALL be the full `0..1` range, exactly as an overfull `kCrossfade` group's own min/max is; otherwise min/max SHALL be `base + sum(min(0, rawDepth))` and `base + sum(max(0, rawDepth))`, clamped to `[0, 1]`, where `base = center * (1 - W) + normalizationOffset` and `normalizationOffset = 0` (no raw depth is ever negative). Since no raw depth is ever negative, `sum(min(0, rawDepth)) = 0` always, so the published minimum reduces to `center * (1 - W)`, clamped to `[0, 1]` — no longer `center` itself once `W > 0`, unlike the add-only law this change replaces, whose published minimum never moved below the knob.

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

#### Scenario: A one-way amount target's ring is visible even at full knob
- **WHEN** a unipolar `kOneWayAmount` parameter has center (knob) `1.0` and one active route at raw depth `0.25` (`W = 0.25`)
- **THEN** its center scale is `1 - 0.25 = 0.75` and `base = 1.0 * 0.75 = 0.75`
- **AND** its UI-state minimum is `0.75 + min(0, 0.25) = 0.75` and its UI-state maximum is `0.75 + max(0, 0.25) = 1.0`
- **AND** unlike the add-only law this change replaces (which published `1.0`/`1.0`, hiding the band entirely at full knob), a modulation band remains visible at every knob position

#### Scenario: A one-way amount target's ring crossfades below the ceiling with several routes
- **WHEN** a unipolar `kOneWayAmount` parameter has center `0.2` and two active routes at raw depths `0.5` and `0.4` (`W = 0.9`)
- **THEN** its center scale is `1 - 0.9 = 0.1` and `base = 0.2 * 0.1 = 0.02`
- **AND** its UI-state minimum is `0.02` and its UI-state maximum is `0.02 + 0.5 + 0.4 = 0.92`

#### Scenario: A one-way amount target's ring is the full range once total depth passes the ceiling
- **WHEN** a unipolar `kOneWayAmount` parameter has center `0.2` and two active routes at raw depths `0.8` and `0.6` (`W = 1.4`, greater than `1`)
- **THEN** its UI-state minimum is `0.0` and its UI-state maximum is `1.0`, exactly as an overfull `kCrossfade` group's own min/max is

### Requirement: spm-11 — Audio path: Get and ProcessLite
WHEN the audio engine samples a parameter, THE synth parameter modulation system SHALL provide `Parameter::GetRaw(voiceIx)` as the explicit normalized `[0, 1]` read path for every parameter polarity, returning the `[0, 1]`-clamped value `currentCenter * currentCenterScale[voiceIx] + currentNormalizationOffset[voiceIx] + group.modulators.Apply(voiceIx, currentDepthsForVoice)` without traversing manager, page, bank, slot, scene, gesture, or modulation route state; SHALL provide `ProcessLitePhase1()` to advance current center, current center scales, current normalization offsets, current min/max values, and current modulation depths toward targets using `current += alpha * (target - current)` and then sample each voice's normalized cached knob from `GetRaw(voiceIx)`; SHALL provide `ReplaceCachedKnobValue(voiceIx, normalizedValue)` to replace and `[0,1]`-clamp that cache entry without changing raw/current/target or UI smoothing state; SHALL provide `ProcessLitePhase2()` to update UI smoothing from the final cache; and SHALL retain `ProcessLite()` as the phase-1-then-phase-2 convenience operation. Per-sample parameter and group processing SHALL expose matching phase-1 and phase-2 operations, SHALL recompute target state from the owning manager's current scene when `sampleIndex % group.Config().targetComputeIntervalSamples == 0` before phase 1, and SHALL retain `ProcessSample()` as the uninterrupted phase-1-then-phase-2 convenience operation.

#### Scenario: GetRaw uses current state only
- **WHEN** `GetRaw(0)` is called after `Compute()` and `ProcessLite()`
- **THEN** it uses only the parameter's current center, current center scale for voice 0, current normalization offset for voice 0, the current depth row for voice 0, and the group's current modulator row for voice 0
- **AND** it returns a normalized value in `[0, 1]` regardless of bipolar presentation metadata

#### Scenario: Phase 1 slews center
- **WHEN** current center is `0.0`, target center is `1.0`, and alpha is `0.25`
- **THEN** one `ProcessLitePhase1()` call sets current center to `0.25`

#### Scenario: Phase 1 slews center scale
- **WHEN** current center scale for voice 0 is `1.0`, target center scale for voice 0 is `0.5`, and alpha is `0.25`
- **THEN** one `ProcessLitePhase1()` call sets current center scale for voice 0 to `0.875`

#### Scenario: Phase 1 samples cached knob value
- **WHEN** `ProcessLitePhase1()` has advanced the current center, center scale, normalization offset, and depth state for a voice
- **THEN** it stores that voice's normalized cached knob value from `GetRaw(voiceIx)` using the group's current modulator row
- **AND** it does not yet update that voice's UI display center or spread energy

#### Scenario: Application replaces phase-1 cache before phase 2
- **WHEN** phase 1 caches `0.8` for a voice and compile-time-known application code replaces it with `0.3` before phase 2
- **THEN** subsequent cached mapping reads return `0.3`
- **AND** phase 2 derives UI smoothing from `0.3`
- **AND** `GetRaw()` and the parameter's current and target state remain unchanged

#### Scenario: Cache replacement preserves normalized domain
- **WHEN** application code replaces a cached knob value with a value below `0` or above `1`
- **THEN** the stored replacement is respectively clamped to `0` or `1`

#### Scenario: Convenience ProcessLite preserves ordinary behavior
- **WHEN** a caller invokes `ProcessLite()` without an intervening cache replacement
- **THEN** phase 1 and phase 2 run consecutively
- **AND** its resulting audio cache and UI smoothing state match explicit consecutive calls to `ProcessLitePhase1()` and `ProcessLitePhase2()`

#### Scenario: Per-sample processing recomputes on configured interval
- **WHEN** a parameter's group target compute interval is 16 samples
- **AND** per-sample phase 1 is called with sample indexes 15 and 16
- **THEN** the sample 15 call runs lite phase 1 without recomputing targets
- **AND** the sample 16 call recomputes targets from the owning manager's current scene before running lite phase 1

#### Scenario: Per-sample processing recomputes on first sample
- **WHEN** a parameter's group target compute interval is 16 samples
- **AND** per-sample phase 1 is called with sample index 0
- **THEN** the call recomputes targets from the owning manager's current scene before running lite phase 1

#### Scenario: Per-sample recompute slews top-level target center
- **WHEN** a top-level parameter's group target compute interval is 16 samples
- **AND** the parameter has target center `0.0`, raw computed center `1.0`, and group target-center alpha `0.25`
- **AND** per-sample phase 1 is called with sample index `16`
- **THEN** the target recompute sets target center to `0.25` before lite phase 1 slews current center toward that target

#### Scenario: Per-sample group phases cover group parameters
- **WHEN** both per-sample phases are invoked for a parameter group at an absolute sample index
- **THEN** every top-level parameter owned by that group runs phase 1 once and phase 2 once

#### Scenario: Per-sample group processing refreshes modulation-depth targets
- **WHEN** a top-level group parameter has a materialized modulation-depth parameter
- **AND** per-sample group phase 1 recomputes targets for that group
- **THEN** the top-level parameter's modulation-depth target state reflects the owning manager's current scene through the same recursive compute path used by top-level `Compute()`

#### Scenario: Cached knob formalizes one-sample modulation delay
- **WHEN** mapped DSP reads consume a parameter after phase 1
- **AND** a modulation source updates the group's modulator row later in the same sample frame
- **THEN** mapped DSP reads continue to use the cached knob value sampled or replaced after the earlier phase-1 call
- **AND** the later modulator update is visible to the next sample frame's phase-1 cache refresh

#### Scenario: Cached knob is seeded before steady-state ProcessLite
- **WHEN** a parameter is constructed, snapped to target during initialization, loaded from patch values, or reverted through a snap-to-target path
- **THEN** each voice's cached normalized knob value is seeded from `GetRaw(voiceIx)`
- **AND** each voice's normalized UI display center is seeded to that cached knob value
- **AND** each voice's UI display spread energy is seeded to `0`

### Requirement: spm-12 — Edits: HandleIncDec scene and gesture distribution
WHEN `Parameter::HandleIncDec(delta)` is called, THE parameter SHALL apply the delta to the active scene center value when blend is at one scene endpoint and no gesture distribution is active, SHALL distribute the delta across the two active scene center values when blend is between scenes using the Smart Grid scene distribution formula, SHALL treat the first turn for any selected inactive gesture as an arming turn that activates the gesture for the touched scene endpoints and snapshots each touched parent scene value into the matching gesture value without applying the delta, and SHALL distribute non-arming turns between active gesture values and base scene values according to Smart Grid-style effective gesture weights regardless of current gesture selection.

#### Scenario: Endpoint scene edit
- **WHEN** blend is `0`, left scene is active, and `HandleIncDec(0.1)` is called
- **THEN** only the left scene center value is incremented and clamped to the parameter range

#### Scenario: Selected inactive gesture arming
- **WHEN** a selected gesture is inactive for the current scene and `HandleIncDec(delta)` is called
- **THEN** the gesture becomes active for that scene
- **AND** its gesture value is copied from the current parent scene value
- **AND** the delta is not applied to the parent scene value or to the newly activated gesture value on that call

#### Scenario: Active high gesture edit after deselection
- **WHEN** a gesture is already active for the current scene
- **AND** the gesture is not currently selected
- **AND** the manager-owned gesture weight is `1.0`
- **AND** the gesture value is above the parent scene value
- **THEN** `HandleIncDec(delta)` applies the full clamped delta to the gesture value
- **AND** leaves the parent scene value unchanged

#### Scenario: Active gesture distribution
- **WHEN** one or more gestures are already active for the current scene selection
- **AND** their active effective weight sum is greater than zero
- **THEN** `HandleIncDec(delta)` distributes the gesture portion as `delta * weight * weight / activeEffectiveWeightSum` for each active gesture
- **AND** distributes the base portion as `delta * sum(weight * (1 - weight)) / activeEffectiveWeightSum`
- **AND** applies each portion through the scene distribution formula for the active scene blend

### Requirement: spm-13 — Revert: defaults and modulation clearing
WHEN a parameter is reverted to default for the current scene selection, THE parameter SHALL clear all modulation-depth parameter assignments, zero current and target modulation depth arrays, set applicable scene center values to the default normalized value, set applicable scene gesture active flags to false, and update current/target center consistently with the owning group's slew and recursion-depth rules.

#### Scenario: Reset modifier clears modulation
- **WHEN** a routed reset-modifier press resets a parameter
- **THEN** the parameter has no non-null modulation-depth parameter pointers
- **AND** `GetRaw(voiceIx)` returns the default-centered value after compute/process settling with zero modulators
- **AND** the cached knob value is seeded or slewed consistently with the current raw value through the applicable snap or `ProcessLite` path

### Requirement: spm-14 — Banks and slots: physical control mapping
WHEN banks and bank slots are configured, THE system SHALL allow each bank to list parameter pointers and physical encoder IDs, allow banks to include parameters from different groups, allow each slot to select one bank at a time, and require slot bank selection to deselect any modulation view open on the previously selected bank before showing the newly selected bank.

#### Scenario: Mixed-group bank
- **WHEN** a bank contains parameters from two groups
- **THEN** routed encoder actions dispatch to each parameter through its own owning group

#### Scenario: Slot bank switch deselects prior view
- **WHEN** a slot selects a new bank while the previous bank is showing a modulation-depth view
- **THEN** the previous bank returns to its top-level parameter view before the new bank is selected

### Requirement: spm-15 — Banks and slots: press, shift-press, and tick routing
WHEN a bank handles a press on a mapped physical encoder, THE bank SHALL populate the pressed parameter's visible modulation-depth cells from parent-owned modulation-depth controls, SHALL materialize missing modulation-depth controls as local bipolar default-zero controls when group capacity allows, SHALL NOT append those modulation-depth controls to the manager global parameter list, SHALL initialize lazily materialized modulation-depth control short name and color from the corresponding modulator metadata, SHALL derive an effective depth control name from the target parameter name plus the modulator metadata name when the metadata name is non-empty, SHALL derive an effective fallback name from the target parameter name plus the one-based modulation index when the metadata name is empty, and SHALL place the selected top-level parameter in the final physical slot position as the return cell when the press is routed through a `BankSlot`; direct bank presses without a slot layout SHALL use the bank's compact top-level mapping order as the physical layout fallback. If a routed slot has `N` physical positions, THE system SHALL reserve position `N - 1` for the return cell, SHALL use positions `0..N-2` for modulation-depth cells, SHALL leave unused positions before the return cell disconnected, and SHALL treat `numModulators > N - 1` as a configuration error. Pressing a modulation cell with no effective modifier SHALL open that modulation control's modulation view; pressing the return cell with no effective modifier SHALL restore the top-level bank; tick SHALL route to the parameter or modulation-depth control visible in the cell; reset-modifier press SHALL revert the visible top-level parameter to default and neutralize its modulation-depth subtree without deleting those controls; random-modifier press SHALL randomize the visible knob value while leaving that target's modulation routes intact; random-mod modifier press SHALL apply geometric modulation randomization to the visible target; and routed manager/slot APIs SHALL dispatch press and tick/inc-dec events by physical encoder ID to the selected bank.

#### Scenario: Press opens modulation view
- **WHEN** a bank is showing top-level parameters and the user presses a parameter encoder through a slot with no effective modifier
- **THEN** the bank shows that parameter's modulation-depth cells in the first slot positions
- **AND** shows the selected parameter as the return cell in the final slot position

#### Scenario: Slot gap remains disconnected
- **WHEN** a slot has three physical positions
- **AND** the pressed parameter's group has one modulator
- **THEN** the modulation-depth cell occupies the first slot position
- **AND** the middle slot position is disconnected
- **AND** the return cell occupies the third slot position

#### Scenario: Too many modulators for slot layout is an error
- **WHEN** a slot has three physical positions
- **AND** the pressed parameter's group has three modulators
- **THEN** opening that parameter's modulation view fails as a configuration error because no final slot position remains reserved for return

#### Scenario: Lazy depth metadata follows modulator
- **WHEN** opening a modulation view materializes a missing depth control for target parameter `Carrier` and modulator `0`
- **AND** modulator `0` has name `Filter Env`, short name `Env`, and color `Cyan`
- **THEN** the created depth control has name `Carrier Filter Env`
- **AND** has short name `Env`
- **AND** has color `Cyan`

#### Scenario: Duplicate modulator names remain unique per parent
- **WHEN** two target parameters both materialize local depth controls for a modulator named `Filter Env`
- **THEN** each created depth control name includes its target parameter name
- **AND** manager global parameter registration is not involved

#### Scenario: Empty modulator name uses indexed fallback
- **WHEN** opening a modulation view materializes a missing depth control for target parameter `Carrier` and modulator `1`
- **AND** modulator `1` has no name
- **THEN** the created depth control has name `Carrier Mod Depth 2`

#### Scenario: Reset keeps depth controls attached
- **WHEN** a reset-modifier press resets a parameter that owns modulation-depth controls
- **THEN** the parameter returns to its default value
- **AND** its modulation-depth controls are reset to neutral zero values
- **AND** those controls remain attached to the parent parameter for future modulation-view routing

#### Scenario: Random leaves depth controls attached
- **WHEN** a random-modifier press targets a parameter that owns modulation-depth controls
- **THEN** the parameter's active knob value changes to a generated normalized value
- **AND** its modulation-depth controls remain attached
- **AND** its modulation-depth control values remain unchanged

#### Scenario: Random-mod materializes depth controls
- **WHEN** a random-mod modifier press targets a top-level parameter with at least one empty modulator slot
- **AND** the random-mod loop chooses that empty slot
- **THEN** the system materializes that slot's modulation-depth control through the normal modulation-view capacity path
- **AND** randomizes the created depth control value

#### Scenario: Return cell closes modulation view
- **WHEN** a modulation-depth view is open
- **AND** the return cell in the final slot position is pressed with no effective modifier
- **THEN** the bank restores the top-level parameter view

#### Scenario: Tick routes to selected bank
- **WHEN** the manager receives a tick for a physical encoder ID owned by a slot's selected bank
- **THEN** the manager routes the delta through the slot and bank to the mapped parameter's `HandleIncDec`

### Requirement: spm-16 — Gesture API: external selection and values
WHEN external code controls gestures, THE parameter manager SHALL default to zero gestures, expose a non-replacing pre-group setup API to set gesture count before any group is created, reject or report failure for gesture-count changes after group creation, and expose runtime functions to select and deselect gestures, set gesture values, read gesture selected state, read gesture values, read and write gesture metadata, and clear a gesture's active flags across parameters for the active scene selection.

#### Scenario: External select gesture
- **WHEN** external code selects gesture 3 on the manager
- **THEN** subsequent parameter edits in every group treat gesture 3 as selected

#### Scenario: External change gesture value
- **WHEN** external code sets gesture 3 value to `0.75` on the manager
- **THEN** the next control-rate `Compute()` uses `0.75` as gesture 3's weight where that gesture is active

#### Scenario: External clear gesture active flags
- **WHEN** external code clears gesture 3 active flags for the active scene selection
- **THEN** matching gesture 3 active flags are cleared across parameters in every group owned by the manager

#### Scenario: Gesture count configuration precedes group creation
- **WHEN** a manager is constructed or configured with four gestures before groups are created
- **THEN** gesture APIs accept indices 0 through 3
- **AND** parameters created later allocate four per-scene gesture slots

#### Scenario: Gesture count cannot change after group creation
- **WHEN** a manager has created any group
- **THEN** attempting to change the manager gesture count fails or is rejected without resizing existing group arenas

### Requirement: spm-17 — Scene API: validated global scene endpoints
WHEN external or direct code changes manager scene endpoints, THE parameter manager SHALL expose a validated scene-endpoint setter used by tests, direct routing, and new external code; the setter SHALL accept endpoints only when both endpoint indices are valid for every existing group, SHALL leave existing scene state unchanged on rejection, and SHALL leave scene blend unchanged on endpoint changes. The parameter manager SHALL also expose a scene-selection API that accepts one scene ordinal, writes it to the less-selected endpoint of the current scene blend, writes the right endpoint when blend is exactly `0.5`, and rejects invalid ordinals without changing scene state.

#### Scenario: Direct scene setter accepts compatible endpoints
- **WHEN** all groups support scenes `0` through `2`
- **AND** direct code sets scene endpoints to left `1` and right `2` through the manager setter
- **THEN** the manager scene endpoints become `1` and `2`
- **AND** the previous scene blend is unchanged

#### Scenario: Direct scene setter rejects incompatible endpoints
- **WHEN** any existing group supports only scene `0`
- **AND** direct code tries to set scene endpoint `1` through the manager setter
- **THEN** the setter rejects the change
- **AND** the previous manager scene endpoints and blend remain unchanged

#### Scenario: Direct less-selected scene selection
- **WHEN** the manager scene endpoints are left `0` and right `2`
- **AND** the scene blend is `0.75`
- **AND** direct code selects scene `1` through the manager scene-selection API
- **THEN** the manager left scene endpoint becomes `1`
- **AND** the right scene endpoint remains `2`
- **AND** the previous scene blend is unchanged

### Requirement: spm-18 — Tests: deterministic randomized simulation oracle
WHEN automated tests cover the synth parameter modulation system, THE test suite SHALL include deterministic randomized simulation tests that repeatedly choose actions from encoder turns, encoder presses, reset/random/random-mod modifier presses and releases, manager-owned gesture select/deselect, manager-owned gesture value changes, page changes, bank selection, modified bank actions, scene changes, blend changes, modulator value changes, compute, and process-lite, and SHALL verify after each action against a separate deterministic oracle model maintained outside the production parameter classes.

#### Scenario: Random action loop checks invariants
- **WHEN** a randomized simulation test runs one seed
- **THEN** every action is followed by checks for expected page, bank, slot, scene, manager-owned gesture, modifier, parameter, modulation-route, target, current, `GetRaw(voiceIx)`, cached knob value, UI display center, and UI display spread state

#### Scenario: Cross-group gesture invariants are checked
- **WHEN** the randomized simulation includes parameters from multiple groups
- **THEN** the oracle checks that gesture selection and gesture values are manager-owned and observed consistently by all groups

#### Scenario: Modifier invariants are checked
- **WHEN** the randomized simulation chooses reset, random, random-mod, press, or bank-selection actions
- **THEN** the oracle checks the effective modifier precedence and the resulting single modifier behavior

#### Scenario: Failure is reproducible
- **WHEN** the randomized simulation detects a mismatch
- **THEN** the failure output includes seed, step number, action, affected parameter or physical encoder ID, random samples consumed for that action, and the mismatched expected and actual values

#### Scenario: Default and stress modes
- **WHEN** the test suite runs the randomized simulation in normal mode
- **THEN** it runs a short deterministic seed set suitable for routine test runs
- **WHEN** the stress environment is enabled
- **THEN** it runs a larger deterministic seed set

### Requirement: spm-19 — Color: UI-safe RGB and HSV helpers
WHEN external UI state or message-thread rendering needs colors from the synth parameter system, THE synth library SHALL provide one small trivially copyable 32-bit RGB/RGBA color type with equality, named basic colors, shared alpha/darkening/brightening helpers, `ToHsv` returning a unit-explicit `hueTurns` field, and separately named `FromHsvTurns` and `FromHsvDegrees` helpers, without requiring JUCE or Smart Grid headers; `FromHsvTurns` SHALL reject non-finite hue or hue outside `[0,1)`, `FromHsvDegrees` SHALL reject non-finite hue and wrap finite degrees modulo 360, and color UI-state storage SHALL be lock-free through one atomic 32-bit value or an equivalently lock-free representation.

#### Scenario: Convert color through hue turns
- **WHEN** a synth color is converted to HSV turns and back to RGB
- **THEN** the resulting color matches the original within one 8-bit channel step

#### Scenario: Degree and turn constructors agree
- **WHEN** one color is constructed at 120 degrees and another at one-third turn with equal saturation/value
- **THEN** the resulting RGB channels match within one 8-bit channel step

#### Scenario: Wrong hue unit fails loudly
- **WHEN** hue `120` is passed to `FromHsvTurns`
- **THEN** the call rejects the value rather than wrapping it to red

#### Scenario: Core color has no JUCE dependency
- **WHEN** the synth library is built without the miniapp target
- **THEN** public synth headers expose synth color types only
- **AND** do not include JUCE headers or expose `juce::Colour`
- **AND** portable UI drawing uses the same synth color type rather than a second RGBA struct

#### Scenario: Color atomics are lock-free
- **WHEN** color UI state is compiled on the supported build target
- **THEN** color storage is represented as a lock-free atomic 32-bit value or equivalent

### Requirement: spm-20 — UI State: parameter and visible-cell snapshots
WHEN a parameter or visible-cell UI snapshot is populated, THE synth parameter modulation system SHALL write a `Parameter::UIState` whose scalar fields are individually atomic and which contains the parameter base color and resolved per-voice indicator colors from `ParameterConfig`, connected state, bipolar flag, short name pointer or stable short name view, per-voice display center values, per-voice display spread values, per-voice minimum values, per-voice maximum values, per-voice switch bucket values, switch cardinality, a normalized single control-center `rawKnobValue` before modulation and display smoothing, the slot position's latest processed absolute-input epoch, a synth-native modulator affecting bitmask, a 64-bit synth-native gesture affecting bitmask, source colors for the parameter's owning-group modulators, and manager-owned gesture colors; every field SHALL be inside the existing snapshot revision transaction; disconnected visible cells SHALL use `connected=false` with neutral values, zero spread, zero color counts, and off colors while preserving the slot position's processed absolute-input epoch instead of a separate page/navigation role; `rawKnobValue` SHALL remain normalized in `[0, 1]` for every parameter range kind, bipolar parameter UI display values and min/max values SHALL be reported in `[-1, 1]`, and unipolar parameter UI display values and min/max values SHALL be reported in `[0, 1]`.

#### Scenario: Parameter UI state reports smoothed per-voice display values
- **WHEN** a parameter has two voices with different cached knob values
- **AND** `Parameter::PopulateUIState` is called after compute/process work
- **THEN** the UI state exposes the parameter's per-voice smoothed display center values
- **AND** it does not expose unsmoothed audio-rate cached knob values as the encoder indicator center

#### Scenario: Parameter UI state reports normalized raw control center
- **WHEN** a parameter's scene and gesture composition has normalized center `0.25`
- **AND** audio-rate modulation or display smoothing makes voice-0 display value differ from `0.25`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** `rawKnobValue` is `0.25` within numeric tolerance
- **AND** it is neither modulation-adjusted nor converted to bipolar presentation

#### Scenario: Processed epoch and raw center are coherent
- **WHEN** absolute epoch `E` has been processed for a slot position and its resulting visible raw center is `X`
- **AND** that visible-cell UI state is populated
- **THEN** a stable revision read returns both processed epoch `E` and raw center `X` from the same publication transaction
- **AND** a torn read is rejected by the existing revision protocol

#### Scenario: Parameter UI state reports display spread
- **WHEN** audio-rate modulation causes a voice's cached knob value to vary around its smoothed display center
- **AND** `Parameter::PopulateUIState` is called after process work
- **THEN** the UI state exposes a non-negative per-voice display spread derived from the smoothed residual energy

#### Scenario: Parameter UI state reports configured base color
- **WHEN** a parameter is configured with base color `C`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** the UI state reports parameter base color `C`

#### Scenario: Parameter UI state reports parameter indicator colors
- **WHEN** a two-voice parameter resolves indicator colors `A` and `B`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** voice 0 indicator color is `A`
- **AND** voice 1 indicator color is `B`
- **AND** another parameter in the same group may report different colors

#### Scenario: Parameter UI state reports local source and global gesture colors
- **WHEN** a parameter's group has source colors `M0` and `M1` and its manager has gesture color `G0`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** the snapshot reports `M0`, `M1`, and `G0` in their indexed color arrays

#### Scenario: Bipolar UI state reports signed values
- **WHEN** a bipolar parameter has smoothed display center values `-0.5` and `0.75`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** the UI state reports the bipolar flag as true
- **AND** reports per-voice display center values `-0.5` and `0.75`
- **AND** reports minimum value `-1` and maximum value `1`

#### Scenario: Unipolar UI state reports unipolar values
- **WHEN** a unipolar parameter UI state is populated
- **THEN** the UI state reports the bipolar flag as false
- **AND** reports minimum value `0` and maximum value `1`

#### Scenario: Parameter UI state reports switch metadata
- **WHEN** a switch/discrete parameter UI state is populated
- **THEN** the UI state reports the parameter's switch cardinality
- **AND** reports each voice's precomputed switch bucket value using the same helper as `Parameter::GetSwitchVal(voiceIx)`
- **AND** reports display spread `0` for each voice

#### Scenario: Parameter UI state reports affecting masks
- **WHEN** a parameter has active or assigned modulation/gesture relationships that should be visible to the external encoder renderer
- **THEN** the UI state reports synth-native modulator and gesture affecting bitmasks
- **AND** those masks do not use Smart Grid `BitSet16` or Smart Grid enum types
- **AND** the gesture mask is 64 bits and covers gesture indices `0..63`
- **AND** a modulator bit is set when the parameter has a non-neutral assigned modulation-depth parameter for that modulator
- **AND** a gesture bit is set when the parameter has that gesture active in the manager's active scene selection: left scene only at blend 0, right scene only at blend 1, and both endpoint scenes for intermediate blends

#### Scenario: Unused UI state voices are disconnected or neutral
- **WHEN** a UI state has capacity for more voices than the parameter group uses
- **THEN** populated voice entries beyond the configured voice count are neutral and do not report stale values as connected
- **AND** their display spread values and indicator colors are zero/off

#### Scenario: Modulation target cell stays parameter-owned
- **WHEN** a visible bank cell is the target encoder in an open modulation view
- **AND** slot UI state is populated
- **THEN** that reserved `Parameter::UIState` reports `connected=true`
- **AND** it reports the target parameter's switch cardinality, per-voice switch buckets, affecting masks, base color, indicator colors, source colors, gesture colors, short name, bipolar flag, display center values, display spread values, and min/max values exactly as the target parameter would outside the modulation view
- **AND** renderers do not distinguish this cell from normal parameter cells through parameter UI-state page/navigation data

#### Scenario: Short name lifetime is stable
- **WHEN** a parameter UI state exposes a short name pointer or stable view
- **THEN** that reference remains valid for the lifetime of the owning manager topology
- **AND** UI state consumers do not retain it after the manager or parameter is destroyed

### Requirement: spm-21 — UI State: slot, bank, gesture, and manager snapshots
WHEN manager-level UI state is populated, THE synth parameter modulation system SHALL populate a parallel atomic UI-state tree for selected slot bank views, visible parameter cells, per-slot modulation-view active state, manager-owned gesture values and selected flags, scene selection, scene blend, reset-held state, random-held state, and random-mod-held state, while marking unused visible cells disconnected through each reserved visible cell's embedded `Parameter::UIState.connected` flag.

#### Scenario: Manager creates pre-sized UI state
- **WHEN** external code requests a manager UI state after the parameter topology is configured
- **THEN** the manager provides a setup or factory API that returns or initializes a non-copyable UI-state tree sized from current manager topology, including slots, slot physical-encoder counts, group voice counts, and manager gesture count
- **AND** `PopulateUIState` performs no UI-state array allocation

#### Scenario: UI state is not copyable or resizable during population
- **WHEN** UI state contains atomic scalar fields
- **THEN** UI-state arrays are allocated to final capacity during setup
- **AND** the implementation does not rely on copying, moving, or resizing atomic UI-state elements during population

#### Scenario: Slot UI state follows selected bank
- **WHEN** a slot changes from bank 0 to bank 1
- **AND** manager UI state is populated
- **THEN** the slot UI state contains bank 1's visible parameter cells
- **AND** no visible cells from bank 0 remain connected unless they are also visible in bank 1

#### Scenario: Modulation view appears in slot UI state
- **WHEN** a parameter push opens that parameter's modulation-depth view
- **AND** manager UI state is populated
- **THEN** the selected slot UI state contains the visible modulation-depth parameters and the target parameter at the final visible position for that bank view
- **AND** reports that the slot is showing a modulation view

#### Scenario: Gesture UI state exposes values and selection
- **WHEN** gesture 1 is selected and has value `0.75`
- **AND** manager UI state is populated
- **THEN** the gesture UI state reports gesture 1 as selected with value `0.75`

#### Scenario: Gesture UI state is manager-wide
- **WHEN** a manager owns parameters in multiple groups
- **AND** manager UI state is populated
- **THEN** the gesture UI state appears once for the manager
- **AND** does not expose separate group-local gesture selections

#### Scenario: Modifier UI state exposes held states
- **WHEN** reset, random, or random-mod held state changes
- **AND** manager UI state is populated
- **THEN** the UI state reports each modifier's held state independently
- **AND** UI consumers can derive the same effective modifier as `GetCurrentModifier`

#### Scenario: Slot UI positions follow slot encoder order
- **WHEN** a slot has physical encoders added in the order `[10, 12, 11]`
- **AND** the selected bank has visible cells for those encoders
- **THEN** slot UI position 0 reports the visible cell for encoder 10
- **AND** slot UI position 1 reports the visible cell for encoder 12
- **AND** slot UI position 2 reports the visible cell for encoder 11

#### Scenario: Empty visible cell uses embedded parameter connection flag
- **WHEN** a selected bank has no visible cell for a slot UI position
- **THEN** the reserved parameter UI state at that position has `connected` set to false
- **AND** no separate slot-level connected flag contradicts it

#### Scenario: Bank exposes visible parameter for slot population
- **WHEN** a bank slot populates UI state for a physical encoder
- **THEN** it uses a bank accessor that returns the visible parameter pointer for that encoder
- **AND** target cells in an open modulation view populate through the same parameter UI-state path as any other connected parameter cell
- **AND** modulation-return behavior is handled by bank press routing, not by parameter UI-state data

### Requirement: spm-22 — Message input: command model
WHEN external UI or MIDI code sends commands to the synth parameter system, THE system SHALL represent each command as a timestamped `MessageIn` with no route field and with typed support for `ParamIncDec`, `ParamPush`, `ToggleReset`, `SetReset`, `ToggleRandom`, `SetRandom`, `ToggleRandomMod`, `SetRandomMod`, `ToggleGestureSelect`, `SetGestureSelect`, `SelectParamBank`, `Start`, `Stop`, `Clock`, `SetGestureValue`, `SceneSelect`, and `SetSceneBlend`.

#### Scenario: Parameter messages carry slot and position
- **WHEN** a parameter inc/dec or push message is created
- **THEN** the message carries the target slot index and visible position
- **AND** does not require a physical encoder ID from the sender

#### Scenario: Slot position maps through slot encoder order
- **WHEN** a parameter message targets slot position `i`
- **THEN** the manager resolves position `i` to the physical encoder at index `i` in that slot's `AddPhysicalEncoder` order
- **AND** routes the resolved physical encoder ID through the selected bank's visible cells

#### Scenario: Reset messages can toggle or set explicit state
- **WHEN** a reset toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit reset set message is created
- **THEN** the message carries a boolean payload indicating the desired reset-held state

#### Scenario: Random messages can toggle or set explicit state
- **WHEN** a random toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit random set message is created
- **THEN** the message carries a boolean payload indicating the desired random-held state

#### Scenario: Random-mod messages can toggle or set explicit state
- **WHEN** a random-mod toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit random-mod set message is created
- **THEN** the message carries a boolean payload indicating the desired random-mod-held state

#### Scenario: Gesture messages carry gesture index and optional explicit selection state
- **WHEN** a gesture select toggle or gesture value message is created
- **THEN** the message carries the gesture index
- **AND** the value-setting message also carries the normalized gesture value
- **WHEN** an explicit gesture select message is created
- **THEN** the message carries the gesture index
- **AND** carries a boolean payload indicating the desired selected state

#### Scenario: Bank selection carries slot and bank
- **WHEN** a parameter bank selection message is created
- **THEN** the message carries which slot to set and which bank index to select
- **AND** the bank index refers to the manager's global bank list

#### Scenario: Scene selection carries one ordinal
- **WHEN** a scene selection message is created
- **THEN** the message carries one scene ordinal
- **AND** does not change scene blend unless a scene blend message is also processed

### Requirement: spm-23 — Message bus: queued timestamp processing
WHEN a `MessageInBus` processes messages, THE synth parameter modulation system SHALL use a bounded single-producer/single-consumer queue that accepts `MessageIn` values, assumes producers push messages in nondecreasing timestamp order, pops only messages whose timestamp is visible at the supplied process timestamp from the queue head, and applies visible messages in FIFO order to the attached `ParameterManager`; callers that need multiple producers SHALL serialize externally before calling `Push`.

#### Scenario: Future message waits
- **WHEN** the bus contains a message timestamped after the current process timestamp
- **THEN** `Process(currentTimestamp)` leaves that message queued
- **AND** does not apply it to the manager

#### Scenario: Visible messages apply in order
- **WHEN** the bus contains multiple messages with timestamps at or before the current process timestamp
- **THEN** `Process(currentTimestamp)` applies them to the manager in insertion order

#### Scenario: Queue overflow is reported
- **WHEN** the queue is full
- **THEN** pushing another message fails without corrupting already queued messages

#### Scenario: Future head message blocks later messages
- **WHEN** the queue head has a timestamp after the current process timestamp
- **THEN** `Process(currentTimestamp)` leaves that message and all later messages queued

#### Scenario: Single producer and single consumer can operate concurrently
- **WHEN** one producer thread pushes timestamp-ordered messages
- **AND** one consumer thread processes visible messages
- **THEN** the bus preserves message integrity and FIFO order within the supported single-producer/single-consumer contract

### Requirement: spm-24 — Message bus: manager application
WHEN `MessageInBus` applies supported parameter, bank, gesture, scene, or modifier messages, THE system SHALL mutate the attached `ParameterManager` through manager-owned APIs so message-driven behavior matches direct manager, slot, bank, manager gesture, scene, and modifier calls.

#### Scenario: Inc/dec through bus edits visible parameter
- **WHEN** a `ParamIncDec` message targets slot 0 position 1 with delta `0.2`
- **AND** slot 0 position 1 is connected to a parameter in the selected bank
- **AND** the current modifier is none
- **THEN** bus processing applies the same parameter edit as the corresponding direct routed tick

#### Scenario: Modified inc/dec is ignored
- **WHEN** a `ParamIncDec` message targets a connected visible parameter
- **AND** the current modifier is reset, random, or random-mod
- **THEN** bus processing leaves the visible parameter value and modulation routes unchanged

#### Scenario: Push through bus opens modulation
- **WHEN** a `ParamPush` message targets a visible top-level parameter cell
- **AND** the current modifier is none
- **THEN** bus processing opens that parameter's modulation-depth view using the same rules as direct bank press handling

#### Scenario: Reset-held state affects reset routing
- **WHEN** reset is held through a reset message
- **AND** a `ParamPush` message targets a connected non-return parameter cell
- **THEN** bus processing routes that action through the same reset behavior as direct reset-modified press

#### Scenario: Random-held state affects random routing
- **WHEN** random is held through a random message
- **AND** a `ParamPush` message targets a connected parameter cell
- **THEN** bus processing randomizes that visible knob value
- **AND** leaves existing modulation-depth assignments and values unchanged

#### Scenario: Random-mod-held state affects modulation randomization
- **WHEN** random-mod is held through a random-mod message
- **AND** a `ParamPush` message targets a connected parameter cell
- **THEN** bus processing applies geometric modulation randomization to that visible target

#### Scenario: Bank select through bus deselects old bank view
- **WHEN** a `SelectParamBank` message selects bank 1 for a slot whose previous bank is showing a modulation view
- **AND** the current modifier is none
- **THEN** bus processing deselects the previous bank view
- **AND** selects bank 1 for subsequent slot-position messages

#### Scenario: Modified bank select resets target bank
- **WHEN** reset is the current modifier
- **AND** a `SelectParamBank` message targets bank 1
- **THEN** bus processing resets every top-level parameter mapped by bank 1
- **AND** leaves the slot's previously selected bank unchanged

#### Scenario: Modified bank select randomizes target bank values
- **WHEN** random is the current modifier
- **AND** a `SelectParamBank` message targets bank 1
- **THEN** bus processing randomizes every top-level parameter value mapped by bank 1
- **AND** leaves those parameters' modulation-depth assignments and values unchanged
- **AND** leaves the slot's previously selected bank unchanged

#### Scenario: Modified bank select randomizes target bank modulation
- **WHEN** random-mod is the current modifier
- **AND** a `SelectParamBank` message targets bank 1
- **THEN** bus processing applies geometric modulation randomization to every top-level parameter mapped by bank 1
- **AND** leaves the slot's previously selected bank unchanged

#### Scenario: Explicit reset set is idempotent
- **WHEN** a `SetReset` message carries `true`
- **THEN** bus processing sets manager reset-held state to true
- **WHEN** a later `SetReset` message carries `false`
- **THEN** bus processing sets manager reset-held state to false
- **AND** applying either message when the manager already has that reset-held state leaves other manager state unchanged

#### Scenario: Explicit random set is idempotent
- **WHEN** a `SetRandom` message carries `true`
- **THEN** bus processing sets manager random-held state to true
- **WHEN** a later `SetRandom` message carries `false`
- **THEN** bus processing sets manager random-held state to false
- **AND** applying either message when the manager already has that random-held state leaves other manager state unchanged

#### Scenario: Explicit random-mod set is idempotent
- **WHEN** a `SetRandomMod` message carries `true`
- **THEN** bus processing sets manager random-mod-held state to true
- **WHEN** a later `SetRandomMod` message carries `false`
- **THEN** bus processing sets manager random-mod-held state to false
- **AND** applying either message when the manager already has that random-mod-held state leaves other manager state unchanged

#### Scenario: Scene selection through bus sets less-selected endpoint
- **WHEN** the manager scene blend is less than or equal to `0.5`
- **AND** a `SceneSelect` message carries scene `2`
- **THEN** bus processing uses the manager's validated scene API to set the right endpoint to `2`
- **AND** leaves the existing left endpoint unchanged
- **AND** leaves the existing scene blend unchanged

#### Scenario: Scene selection through bus sets left endpoint when right is dominant
- **WHEN** the manager scene blend is greater than `0.5`
- **AND** a `SceneSelect` message carries scene `1`
- **THEN** bus processing uses the manager's validated scene API to set the left endpoint to `1`
- **AND** leaves the existing right endpoint unchanged
- **AND** leaves the existing scene blend unchanged

#### Scenario: Scene selection rejects invalid ordinals
- **WHEN** a `SceneSelect` message carries a scene ordinal that is out of range for any existing parameter group
- **THEN** bus processing rejects the scene endpoint change
- **AND** leaves the manager's previous scene endpoints and blend unchanged

#### Scenario: Scene blend through bus sets blend only
- **WHEN** a `SetSceneBlend` message carries blend `0.25`
- **THEN** bus processing sets the manager scene blend to `0.25`
- **AND** leaves the manager scene endpoints unchanged

#### Scenario: Clock and transport are safely accepted
- **WHEN** `Clock`, `Start`, or `Stop` messages are processed in this change
- **THEN** the bus accepts and drains them without changing parameter, bank, gesture, scene, or modifier state

#### Scenario: Gesture selection through bus is manager-owned
- **WHEN** a `ToggleGestureSelect` message toggles gesture 1
- **THEN** bus processing updates the manager-owned gesture 1 selected state
- **AND** parameters in every group observe the updated selection for subsequent edits

#### Scenario: Explicit gesture selection through bus is manager-owned
- **WHEN** a `SetGestureSelect` message sets gesture 1 selected state to true
- **THEN** bus processing selects manager-owned gesture 1
- **WHEN** a later `SetGestureSelect` message sets gesture 1 selected state to false
- **THEN** bus processing deselects manager-owned gesture 1
- **AND** parameters in every group observe the updated selection for subsequent edits

#### Scenario: Gesture value through bus is manager-owned
- **WHEN** a `SetGestureValue` message sets gesture 1 to `0.75`
- **THEN** bus processing updates the manager-owned gesture 1 value
- **AND** parameters in every group observe `0.75` on the next compute where gesture 1 is active

### Requirement: spm-25 — Tests: message-driven randomized UI-state simulation
WHEN automated tests cover the external synth parameter control surface, THE test suite SHALL include a deterministic randomized simulation that drives the existing operation set and reset/random/random-mod modifier operations through `MessageInBus`, includes unmodified and modified bank selection as message-driven operations, periodically populates UI state, and verifies UI-state atomics against the separate deterministic oracle model.

#### Scenario: Bus random test matches model
- **WHEN** the message-driven randomized simulation runs one seed
- **THEN** every applied visible message leaves manager, parameter, bank, slot, gesture, scene, modifier, and modulation state matching the oracle

#### Scenario: UI state checks match oracle
- **WHEN** the randomized simulation calls `PopulateUIState`
- **THEN** every connected visible parameter UI cell matches the oracle's expected visible parameter, per-voice display center values, per-voice display spread values, per-voice switch buckets when switch metadata is configured, bipolar flag, signed bipolar or unipolar min/max values, color, indicator colors, modulator affecting masks for all visible source indices, 64-bit gesture affecting masks for indices `0..63`, manager-owned gesture values, selected flags, scene selection, scene blend, reset-held state, random-held state, and random-mod-held state

#### Scenario: Modifier random samples are modeled
- **WHEN** a random or random-mod modifier action consumes random samples
- **THEN** the randomized simulation oracle consumes the same number of samples in the same order

#### Scenario: Existing randomized oracle is migrated to manager-owned gestures
- **WHEN** the randomized simulation creates parameters in multiple groups
- **THEN** its oracle stores gesture values and selection at manager scope rather than group scope

#### Scenario: Cross-group randomized scenes are compatible
- **WHEN** the randomized simulation includes auxiliary-group parameters in cross-group checks
- **THEN** the auxiliary group has scene capacity compatible with the manager scene endpoint range used by the simulation
- **AND** scene endpoint changes go through the manager's validated setter

#### Scenario: Failure output is reproducible
- **WHEN** the message-driven randomized simulation detects a mismatch
- **THEN** the failure output includes seed, step number, message/action, random samples consumed for that action, and the mismatched expected and actual UI or model field

### Requirement: spm-26 — Miniapp: JUCE external control probe
WHEN the synth external UI/message layer, DSP miniapp integration, and module-backed VCO patch are implemented, THE repository SHALL contain a `projects/synth/apps/miniapp` JUCE application, hosted by the synth application runtime, that demonstrates the parameter system, MIDI message routing, DSP-backed module parameters, scope UI-state snapshots, and reusable JUCE components while keeping JUCE code outside core synth library headers and sources.

#### Scenario: Miniapp shows current feature set
- **WHEN** the miniapp runs
- **THEN** it displays reusable synth JUCE encoder components, buttons, sliders, one parameter group with two voices, module-backed page-bank controls for Tune, Phase, Shape, Volume, and LFO Speed, three scene selection buttons, scene blend, visible left/right scene endpoint state, gesture selection, gesture value, latching reset state, random and random-mod modifier state, three modulation sources, and one waveform pane containing both VCO traces

#### Scenario: Miniapp uses local JUCE checkout
- **WHEN** the miniapp target is built in this repository layout
- **THEN** it uses the developer-local `~/JUCE` checkout by default or documents the missing local dependency precisely

#### Scenario: Miniapp double-click creates modulation view
- **WHEN** the user double-clicks an encoder representing a top-level parameter
- **THEN** the miniapp sends a parameter push message through `MessageInBus`
- **AND** the visible UI updates to show modulation-depth controls for that parameter and the target parameter at the final visible position

#### Scenario: Miniapp modulator uses module and LFO sources
- **WHEN** the miniapp processing step advances modulation sources
- **THEN** modulator 0 receives the dual VCO module's direct normalized source floats for the two voices
- **AND** modulator 1 receives the dual VCO module's swapped normalized source floats for the two voices
- **AND** modulator 2 receives the existing sine/cosine LFO values for the two voices

#### Scenario: Miniapp converts colors at JUCE boundary
- **WHEN** the miniapp paints synth UI state
- **THEN** it converts `synth::Color` to `juce::Colour` in miniapp code
- **AND** core synth files remain free of JUCE includes

### Requirement: spm-27 — Reusable JUCE encoder component
WHEN the synth external UI/message layer provides a JUCE encoder renderer, THE repository SHALL provide a reusable synth JUCE encoder component outside core synth headers and sources, mechanically modeled on Smart Grid's encoder component while binding to synth UI-state and `MessageInBus` types.

#### Scenario: Encoder component is reusable outside the miniapp
- **WHEN** a JUCE surface needs to render one visible parameter cell
- **THEN** it can instantiate the reusable synth encoder component from the segregated JUCE layer
- **AND** bind it to a `synth::Parameter::UIState`, optional `synth::MessageInBus`, slot index, and slot position
- **AND** does not need to copy renderer code from the miniapp

#### Scenario: Encoder sends message bus interactions
- **WHEN** the user drags the reusable encoder
- **THEN** it sends a parameter inc/dec message through `MessageInBus` for the configured slot and position
- **WHEN** the user double-clicks the reusable encoder
- **THEN** it sends a parameter push message through `MessageInBus` for the configured slot and position

#### Scenario: Encoder renders Smart Grid geometry
- **WHEN** the reusable encoder paints a connected cell with multiple voices
- **THEN** it renders Smart Grid-style encoder geometry, including concentric per-voice rings, min/max arcs, indicator dots, color conversion at the JUCE boundary, and connected/off state handling from synth UI-state atomics

#### Scenario: Encoder renders bipolar values
- **WHEN** the reusable encoder paints a cell whose UI state has `bipolar=true`
- **THEN** raw values and min/max ranges remain exposed as `[-1, 1]` in UI state
- **AND** the renderer maps those values into `[0, 1]` arc space only for drawing

#### Scenario: Encoder renders switch parameters
- **WHEN** a connected cell's UI state has `switchValues > 1`
- **THEN** the reusable encoder draws visible switch gaps on base and min/max arcs
- **AND** draws each voice's precomputed UI-state switch bucket as a visually separated highlighted segment

#### Scenario: Encoder renders affecting badges
- **WHEN** a connected cell's UI state reports modulator or gesture affecting bitmasks
- **THEN** the reusable encoder draws Smart Grid-style badge geometry from those masks
- **AND** uses synth-owned modulator/gesture metadata colors or neutral fallback colors
- **AND** does not depend on Smart Grid bitmap glyph assets

#### Scenario: Encoder shows 14-segment display
- **WHEN** the reusable encoder paints a connected parameter cell
- **THEN** it uses the ported 14-segment display component in the lower gap
- **AND** the display is hardcoded on rather than replaced by Smart Grid's rounded color-pill fallback
- **AND** it shows the short name from UI state
- **AND** it does not derive navigation labels from parameter UI-state data

### Requirement: spm-28 — Switch/discrete parameter UI state
WHEN synth parameters represent discrete or switch-like values, THE synth parameter modulation system SHALL expose the switch cardinality and per-voice switch bucket in parameter config, parameter API, and atomic UI state without introducing a separate discrete parameter type in this change; switch/discrete parameters SHALL keep switch bucket computation based on the display-normalized unslewed target value and SHALL publish zero display spread.

#### Scenario: Parameter exposes switch value
- **WHEN** a parameter has `switchValues <= 1`
- **THEN** `Parameter::IsSwitch()` is false
- **AND** `Parameter::GetSwitchVal(voiceIx)` returns 0
- **WHEN** a parameter has `switchValues > 1`
- **THEN** `Parameter::IsSwitch()` is true
- **AND** `Parameter::GetSwitchVal(voiceIx)` rounds the display-normalized unslewed target value to `[0, switchValues - 1]`

#### Scenario: UI state carries switch values
- **WHEN** `Parameter::PopulateUIState` populates a connected parameter cell
- **THEN** `Parameter::UIState::switchValues` stores that parameter's switch cardinality
- **AND** per-voice switch bucket atomics store the values returned by `Parameter::GetSwitchVal(voiceIx)`
- **AND** per-voice display spread atomics store `0` for switch parameters
- **WHEN** a cell is disconnected or empty
- **THEN** `switchValues` and per-voice switch bucket atomics are reset to 0

#### Scenario: Switch values remain JUCE-free
- **WHEN** switch metadata is added to the synth core
- **THEN** it is represented only by core C++ parameter config/API/UI-state fields

### Requirement: spm-29 — MIDI: Basic message model
WHEN synth MIDI code exchanges raw MIDI messages, THE synth parameter modulation system SHALL provide a JUCE-free `BasicMidi` value type with timestamp, raw MIDI bytes, message size, status/channel/data accessors, CC/note/realtime constructors, and no route field.

#### Scenario: CC message exposes status and data
- **WHEN** a CC message is created for timestamp `10`, channel `2`, CC `7`, and value `99`
- **THEN** `BasicMidi` reports timestamp `10`
- **AND** reports status `0xB0`
- **AND** reports channel `2`
- **AND** reports CC `7`
- **AND** reports value `99`
- **AND** reports size `3`

#### Scenario: Realtime messages are one byte
- **WHEN** a supported realtime status such as MIDI clock is created
- **THEN** `BasicMidi` reports the realtime status byte
- **AND** reports size `1`
- **AND** does not require route metadata

### Requirement: spm-30 — MIDI input: chainable processor contract
WHEN MIDI input is converted into synth external-control messages, THE synth parameter modulation system SHALL provide an abstract `MidiInProcessor` that is not called on the audio thread, owns or references a `MessageInBus*`, exposes `Process(BasicMidi)`, supports a configurable bus-domain timestamp provider for created `MessageIn` values, and supports an optional thru processor that implementations use for supported-but-unused messages.

#### Scenario: Processor pushes to message bus
- **WHEN** a concrete MIDI input processor converts a `BasicMidi` message to a `MessageIn`
- **THEN** it pushes the created message to the configured `MessageInBus`
- **AND** stamps the created message with the configured bus-domain timestamp rather than raw JUCE or wall-clock MIDI time

#### Scenario: Immediate timestamp drains next process
- **WHEN** a MIDI input processor's timestamp provider returns `0`
- **AND** it converts a MIDI message to a `MessageIn`
- **THEN** the created message is visible to the next `MessageInBus::Process` call without waiting for a wall-clock timestamp

#### Scenario: Unused message passes to thru
- **WHEN** a MIDI input processor receives a valid MIDI message that it does not consume
- **AND** a thru processor is configured
- **THEN** it passes the original `BasicMidi` to the thru processor exactly once

#### Scenario: Unused message without thru is ignored
- **WHEN** a MIDI input processor receives a message it does not consume
- **AND** no thru processor is configured
- **THEN** the message is dropped without mutating `MessageInBus`

### Requirement: spm-31 — MIDI input: encoder mapping
WHEN encoder MIDI input is processed, THE synth parameter modulation system SHALL map configured turn and pushbutton channel/CC pairs to `MessageIn` commands addressed by `(slotIx, position)`, SHALL decode signed-7-bit and direction-only modes as relative ticks scaled by the configured normalized `turnStep`, SHALL decode absolute mode byte `B` as a `ParamSetAbsolute` parameter-space float in `[0, 1]` equal to `(B / 127)^a` without applying `turnStep`, where `a = log(0.5) / log(64 / 127)`, SHALL NOT store the raw 7-bit velocity as the message's parameter target, and SHALL allow one controller to map multiple slots or leave some physical controls unmapped.

#### Scenario: Relative turn CC maps to parameter inc/dec
- **WHEN** an encoder input config maps channel `0` CC `5` to slot `1` position `2` in signed-7-bit mode
- **AND** the processor receives a CC on channel `0` CC `5` with value `65`
- **THEN** it pushes `MessageIn::ParamIncDec` for slot `1` position `2`

#### Scenario: Signed relative mode uses value minus 64
- **WHEN** a mapped turn CC is configured for signed-7-bit relative mode
- **AND** the turn step is `0.01`
- **AND** the processor receives values `63` and `66`
- **THEN** it sends normalized deltas `-0.01` and `0.02` respectively

#### Scenario: Direction-only mode ignores magnitude
- **WHEN** a mapped turn CC is configured for direction-only relative mode
- **AND** the turn step is `0.01`
- **AND** the processor receives values `1`, `64`, and `127`
- **THEN** it sends normalized deltas `-0.01`, no message, and `0.01` respectively

#### Scenario: Absolute mode curves the represented position
- **WHEN** a mapped turn CC is configured for absolute mode
- **AND** the processor receives raw values `0`, `64`, and `127`
- **THEN** it pushes `MessageIn::ParamSetAbsolute` float targets `0`, `0.5`, and `1` respectively for the mapped slot and position
- **AND** changing `turnStep` does not change those values

#### Scenario: Default turn step is small
- **WHEN** an encoder input config is created without an explicit turn step
- **THEN** it uses a default turn step of `1 / 128`

#### Scenario: Pushbutton nonzero value maps to push
- **WHEN** an encoder input config maps pushbutton channel `1` CC `5` to slot `1` position `2`
- **AND** the processor receives a CC on channel `1` CC `5` with value `127`
- **THEN** it pushes `MessageIn::ParamPush` for slot `1` position `2`

#### Scenario: Pushbutton zero value is not consumed as a parameter command
- **WHEN** an encoder input config maps pushbutton channel `1` CC `5`
- **AND** the processor receives a CC on channel `1` CC `5` with value `0`
- **THEN** it does not push a parameter command

### Requirement: spm-32 — MIDI input: controller presets
WHEN synth code requests built-in encoder MIDI configs, THE synth parameter modulation system SHALL provide `TwisterDefault` and `WrldBldrDefault` presets with Smart Grid-compatible encoder defaults: turn channel `0`, pushbutton channel `1`, CCs `0..15`, row-major 4x4 positions, and signed-7-bit relative turn mode.

#### Scenario: Twister default maps first 4x4 grid
- **WHEN** the Twister default config is created for slot `0`
- **THEN** turn channel `0` CC `0` maps to slot `0` position `0`
- **AND** turn channel `0` CC `15` maps to slot `0` position `15`
- **AND** pushbutton channel `1` CC `0` maps to slot `0` position `0`
- **AND** pushbutton channel `1` CC `15` maps to slot `0` position `15`

#### Scenario: WrldBldr default matches Smart Grid encoder rows
- **WHEN** the Wrld.Bldr default config is created for slot `0`
- **THEN** encoder turn input uses channel `0`
- **AND** encoder pushbutton input uses channel `1`
- **AND** CCs `0..15` map to slot positions `0..15` in row-major order
- **AND** the mapping is verified against Smart Grid's Wrld.Bldr MIDI input adapter before implementation completion

#### Scenario: Default config can be trimmed for incomplete slots
- **WHEN** a caller builds a preset for a slot with fewer than 16 exposed positions
- **THEN** the caller can remove unmapped CC entries without changing the meaning of remaining channel/CC mappings

### Requirement: spm-33 — MIDI JUCE: input handler
WHEN the JUCE synth layer connects MIDI input devices, THE system SHALL provide a `MidiInHandler` that owns a `MidiInProcessor`, registers as a JUCE MIDI input callback, opens and closes devices by identifier, reports device/open state, and converts supported JUCE MIDI messages to `synth::BasicMidi` while preserving the source MIDI timestamp only in `BasicMidi`.

#### Scenario: Three-byte JUCE MIDI is forwarded
- **WHEN** the handler receives a 3-byte JUCE MIDI message from an open device
- **THEN** it converts the raw bytes and timestamp to `BasicMidi`
- **AND** calls the owned processor's `Process`

#### Scenario: Realtime JUCE MIDI is forwarded
- **WHEN** the handler receives a supported one-byte realtime JUCE MIDI message
- **THEN** it converts the status and timestamp to `BasicMidi`
- **AND** calls the owned processor's `Process`

#### Scenario: Open failure is observable
- **WHEN** opening a JUCE MIDI input device fails
- **THEN** the handler reports that it is not open
- **AND** keeps the owned processor available for a later successful open

### Requirement: spm-34 — MIDI output: sender and processor contract
WHEN synth code mirrors parameter UI state to MIDI hardware, THE synth parameter modulation system SHALL provide a sender queue and a `MidiOutProcessor` abstraction whose implementations read `ParameterManager::UIState` using the `Parameter::UIState::revision` snapshot protocol, debounce changed mapped encoder cells, enqueue outgoing `BasicMidi` to a MIDI sender from message-thread or UI refresh code, and actively blank mapped disconnected cells by emitting zero value, off color, and controller-specific brightness-off feedback as appropriate for the selected controller.

#### Scenario: Sender drains queued MIDI to sink
- **WHEN** a `BasicMidi` message is enqueued to a MIDI sender with an output sink
- **THEN** the sender thread eventually delivers the same message to the sink in FIFO order
- **AND** MIDI device I/O is not performed by the caller that enqueued the message

#### Scenario: Processor uses stable UI snapshots
- **WHEN** a MIDI output processor reads a mapped cell whose UI-state revision is odd or changes during the read
- **THEN** it retries the cell snapshot before comparing or emitting MIDI feedback

#### Scenario: Unstable UI snapshot skips cell
- **WHEN** a MIDI output processor cannot obtain a stable mapped-cell UI snapshot after its bounded retry count
- **THEN** it emits no MIDI feedback for that cell during the current process call
- **AND** it does not update that cell's debounce cache

#### Scenario: Output processor skips unchanged state
- **WHEN** a MIDI output processor processes a UI-state snapshot twice without relevant mapped-cell changes
- **THEN** the second process call emits no duplicate encoder feedback messages

#### Scenario: Output reset re-renders state
- **WHEN** a MIDI output processor is reset after previously sending feedback
- **AND** it processes the same connected UI-state snapshot again
- **THEN** it emits the feedback required to restore hardware state

#### Scenario: Disconnected mapped cell blanks hardware
- **WHEN** a mapped UI-state cell is disconnected
- **THEN** the MIDI output processor emits the controller-specific blank feedback for that cell on the next output process call
- **AND** repeated process calls without state changes emit no duplicate blank feedback

### Requirement: spm-35 — MIDI output: Twister encoder feedback
WHEN Twister encoder MIDI output is processed, THE synth parameter modulation system SHALL provide a Twister output processor that maps configured slot positions to controller CCs and emits separate CC feedback for the primary encoder value and LED-ring position on zero-based channel `0`, parameter color on channel `1`, RGB brightness on channel `2`, and primary LED-ring brightness on channel `5` using the MIDI Fighter Twister manual conventions; THE processor SHALL NOT mirror primary position to channel `4`, which is reserved by the controller for shifted encoders and shifted LED rings.

#### Scenario: Twister value and primary ring feedback share channel 0
- **WHEN** a mapped connected relative-feedback cell has voice-0 normalized display value `0.5`
- **THEN** the Twister output processor emits one channel `0` CC for that cell with a value near `64`
- **AND** that CC is both the encoder value and primary LED-ring position feedback
- **AND** it emits no mirrored position message on channel `4`

#### Scenario: Twister color feedback uses channel 1
- **WHEN** a mapped connected cell's parameter-level color changes
- **THEN** the Twister output processor emits a channel `1` CC for that cell using the Twister color code derived from synth color

#### Scenario: Twister brightness feedback uses channel 2
- **WHEN** a mapped connected cell has UI-state brightness `1.0`
- **THEN** the Twister output processor emits a channel `2` CC for that cell using the Smart Grid full-brightness animation value

#### Scenario: Twister brightness feedback follows UI state
- **WHEN** a mapped connected cell has UI-state brightness `0.5`
- **THEN** the Twister output processor emits a brightness animation value derived from `17 + 0.5 * 30`

#### Scenario: Twister disconnected cell blanks brightness
- **WHEN** a mapped cell is disconnected
- **THEN** the Twister output processor emits RGB brightness-off value `17` and primary ring brightness-off value `65` for that cell rather than applying visible brightness

#### Scenario: Twister indicator brightness follows UI state
- **WHEN** a mapped connected cell has UI-state brightness `0.5`
- **THEN** the Twister output processor emits primary ring brightness value derived from `65 + 0.5 * 30`

#### Scenario: Twister color helper uses full hue range
- **WHEN** a saturated synth color is converted to an MF Twister color code
- **THEN** the result is a deterministic nonzero value in the manual hue range `1..126`
- **AND** the mapping is verified against the Smart Grid `RGB2MFTHue` shape before implementation completion

### Requirement: spm-36 — MIDI output: Wrld.Bldr encoder feedback
WHEN Wrld.Bldr encoder MIDI output is processed, THE synth parameter modulation system SHALL provide a Wrld.Bldr output processor that maps configured slot positions to controller CCs, emits voice-0 encoder value feedback as CC messages, emits parameter-level button color adjusted by UI-state brightness and voice-0 indicator color through Yaeltex-compatible SysEx color packets, and verifies those packet bytes against Smart Grid source-derived golden data.

#### Scenario: WrldBldr value feedback uses encoder CC channel
- **WHEN** a mapped connected cell has voice-0 normalized value `0.25`
- **THEN** the Wrld.Bldr output processor emits an encoder value CC for that cell with a value near `32`

#### Scenario: WrldBldr encoder colors use SysEx packets
- **WHEN** a mapped connected cell's parameter-level button color or voice-0 indicator color changes
- **THEN** the Wrld.Bldr output processor emits a Yaeltex-compatible SysEx color packet for that cell

#### Scenario: WrldBldr button color uses UI-state brightness
- **WHEN** a mapped connected cell's parameter-level button color is full red
- **AND** the cell's UI-state brightness is `0.5`
- **THEN** the Wrld.Bldr output processor emits the button color packet using dimmed red

#### Scenario: WrldBldr output applies cooldown budget
- **WHEN** many mapped cell colors change at once
- **THEN** the Wrld.Bldr output processor respects its per-process output budget and cooldown state
- **AND** continues emitting remaining changed cells on later process calls

### Requirement: spm-37 — Miniapp: MIDI controller configuration
WHEN the synth miniapp runs, THE miniapp SHALL expose MIDI configuration exclusively through the library Controllers and Audio pages (sru-3, sru-4, sru-5) hosted by the runtime main pane: the configured controllers come from the instrument configuration, device selection is per controller, and connection is automatic through reconciliation (smi-3, smi-4) rather than manual open/close; the miniapp front page SHALL contain no MIDI or file configuration UI; the real synth MIDI processors SHALL be registered against a MIDI-specific `MessageInBus` and `ParameterManager::UIState` by the runtime, MIDI sender/device resources SHALL shut down cleanly, and disconnected slot positions SHALL render as empty space rather than inactive controller chrome.

#### Scenario: Miniapp instrument controls visible encoders
- **WHEN** the instrument configuration contains a twister or wrldbldr controller whose device is connected
- **AND** the hardware sends a mapped encoder turn CC for a visible miniapp slot position
- **THEN** the miniapp processes a scaled `ParamIncDec` through the MIDI input bus on the next runtime bus-processing pass
- **AND** the visible encoder value changes according to the selected bank and slot-position mapping

#### Scenario: Miniapp push opens modulation view
- **WHEN** a connected controller sends a mapped pushbutton CC for a top-level miniapp parameter
- **THEN** the miniapp processes a `ParamPush` through `MessageInBus`
- **AND** the visible encoder grid updates to the modulation view

#### Scenario: Miniapp output follows UI state
- **WHEN** a controller with an open output device is configured
- **AND** miniapp UI-state values or colors change
- **THEN** the registered MIDI output processor enqueues the corresponding hardware feedback messages through the MIDI sender

#### Scenario: Miniapp remains usable without MIDI hardware
- **WHEN** no mapped controller device is present
- **THEN** the existing on-screen miniapp controls continue to work through `MessageInBus`
- **AND** the Controllers page reports the controllers as offline rather than the app failing to start

#### Scenario: Miniapp keeps bus producers isolated
- **WHEN** the miniapp has both on-screen controls and MIDI input enabled
- **THEN** on-screen controls push only to the existing UI message bus
- **AND** MIDI callbacks push only to the MIDI input bus
- **AND** the runtime's audio-thread pump drains both buses into the same parameter manager

#### Scenario: Unassigned slot position leaves space
- **WHEN** a visible slot position has no assigned parameter
- **THEN** the miniapp leaves that encoder position visually empty
- **AND** does not draw the encoder controller body for that position

#### Scenario: Miniapp front page is config-free
- **WHEN** the miniapp front page is inspected
- **THEN** it contains no MIDI device, controller, patch, or file configuration controls

#### Scenario: Miniapp shuts MIDI down cleanly
- **WHEN** the miniapp closes
- **THEN** the MIDI sender stops and joins its worker thread
- **AND** open JUCE MIDI input and output devices are closed

### Requirement: spm-38 — Message bus: safe out-of-bounds application
WHEN `MessageInBus` applies externally produced messages, THE synth parameter modulation system SHALL treat slot, position, bank, gesture, and scene indices as untrusted and SHALL ignore out-of-bounds targets without mutating manager, bank, scene, gesture, parameter, modifier, or UI-state configuration.

#### Scenario: Invalid bank select is a no-op
- **WHEN** a `SelectParamBank` message targets a bank index that does not exist for the addressed slot
- **THEN** bus processing leaves the slot's selected bank unchanged
- **AND** no parameter, scene, modifier, or gesture state changes

#### Scenario: Invalid gesture select is a no-op
- **WHEN** a gesture select or gesture value message targets a gesture index greater than or equal to the manager gesture count
- **THEN** bus processing leaves all gesture selected flags and values unchanged
- **AND** no parameter, bank, scene, or modifier state changes

#### Scenario: Invalid scene select is a no-op
- **WHEN** a `SceneSelect` message targets a scene index that is invalid for any existing parameter group
- **THEN** bus processing leaves scene endpoints and scene blend unchanged
- **AND** no parameter, bank, gesture, or modifier state changes

#### Scenario: Invalid slot position is a no-op
- **WHEN** a parameter push or inc/dec message targets a slot or slot position that does not exist
- **THEN** bus processing leaves all bank, parameter, modulation-view, scene, gesture, and modifier state unchanged

### Requirement: spm-39 — UI State: bank colors and gesture-bank affectation
WHEN manager-level UI state is populated, THE synth parameter modulation system SHALL publish bank UI state containing each bank's configured color, connected flag, and selected state, and SHALL publish enough gesture-bank-affecting state for MIDI feedback to determine whether each gesture affects zero, one, or multiple banks in the active scene selection.

#### Scenario: Bank UI state reports configured color
- **WHEN** a bank is configured with color `C`
- **AND** manager UI state is populated
- **THEN** that bank's UI-state entry reports `connected=true`
- **AND** reports color `C`

#### Scenario: Selected bank state follows slots
- **WHEN** any bank slot selects bank `1`
- **AND** manager UI state is populated
- **THEN** bank `1` reports selected state true
- **AND** an existing unselected bank reports selected state false

#### Scenario: Missing bank is not readable as valid
- **WHEN** UI-state feedback asks about a bank index greater than or equal to the configured bank UI-state capacity
- **THEN** the bank is treated as disconnected
- **AND** feedback derived from that bank uses off color and `isOn=false`

#### Scenario: Gesture affecting one bank is recorded
- **WHEN** a gesture affects at least one visible parameter in exactly one bank for the active scene selection
- **AND** manager UI state is populated
- **THEN** gesture-bank-affecting state identifies that bank as affected by the gesture

#### Scenario: Gesture affecting multiple banks is recorded
- **WHEN** a gesture affects visible parameters in two or more banks for the active scene selection
- **AND** manager UI state is populated
- **THEN** gesture-bank-affecting state identifies multiple affected banks for that gesture

#### Scenario: Gesture affecting no banks is recorded
- **WHEN** a gesture affects no visible parameters in any bank for the active scene selection
- **AND** manager UI state is populated
- **THEN** gesture-bank-affecting state identifies zero affected banks for that gesture

### Requirement: spm-40 — MIDI input: analog mapping
WHEN analog MIDI input is processed, THE synth parameter modulation system SHALL provide an `AnalogMidiInProcessor` with an `AnalogMidiInConfig` that maps configured channel/CC pairs to gesture value or scene blend messages, normalizes CC values to `[0, 1]`, stamps created messages with the configured bus-domain timestamp provider, and passes supported-but-unmapped messages to its thru processor.

#### Scenario: Analog CC maps to gesture value
- **WHEN** analog input config maps channel `2` CC `3` to gesture `3`
- **AND** the processor receives a CC on channel `2` CC `3` with value `64`
- **THEN** it pushes `MessageIn::SetGestureValue` for gesture `3` with a normalized value near `64 / 127`

#### Scenario: Analog CC maps to scene blend
- **WHEN** analog input config maps channel `2` CC `16` to scene blend
- **AND** the processor receives a CC on channel `2` CC `16` with value `127`
- **THEN** it pushes `MessageIn::SetSceneBlend` with value `1.0`

#### Scenario: Analog zero maps to zero
- **WHEN** a mapped analog CC is received with value `0`
- **THEN** the created message carries value `0.0`

#### Scenario: Unmapped analog passes to thru
- **WHEN** the analog input processor receives a valid CC that is not configured
- **AND** a thru processor is configured
- **THEN** it passes the original `BasicMidi` to thru exactly once
- **AND** it pushes no analog message

### Requirement: spm-41 — MIDI input: system button mapping
WHEN system button MIDI input is processed, THE synth parameter modulation system SHALL provide a `SystemButtonMidiInProcessor` with a config struct that maps channel/CC pairs to a press `MessageIn` and an optional release `MessageIn`, emits the press message when the CC value is greater than zero, emits the release message only when configured and the CC value is zero, stamps emitted messages with the configured bus-domain timestamp provider, and passes supported-but-unmapped messages to its thru processor.

#### Scenario: Button press emits configured message
- **WHEN** system button config maps channel `5` CC `32` to `MessageIn::ToggleReset`
- **AND** the processor receives a CC on channel `5` CC `32` with value `127`
- **THEN** it pushes a `ToggleReset` message

#### Scenario: Button release emits optional configured message
- **WHEN** system button config maps channel `5` CC `32` to `MessageIn::SetReset(true)` on press and `MessageIn::SetReset(false)` on release
- **AND** the processor receives a CC on channel `5` CC `32` with value `0`
- **THEN** it pushes a reset message whose boolean payload clears reset

#### Scenario: Button release can clear random modifier
- **WHEN** system button config maps channel `5` CC `33` to `MessageIn::SetRandom(true)` on press and `MessageIn::SetRandom(false)` on release
- **AND** the processor receives a CC on channel `5` CC `33` with value `0`
- **THEN** it pushes a random message whose boolean payload clears random

#### Scenario: Button release can clear random-mod modifier
- **WHEN** system button config maps channel `5` CC `34` to `MessageIn::SetRandomMod(true)` on press and `MessageIn::SetRandomMod(false)` on release
- **AND** the processor receives a CC on channel `5` CC `34` with value `0`
- **THEN** it pushes a random-mod message whose boolean payload clears random-mod

#### Scenario: Button release can clear gesture selection
- **WHEN** system button config maps channel `5` CC `0` to `MessageIn::SetGestureSelect(0, true)` on press and `MessageIn::SetGestureSelect(0, false)` on release
- **AND** the processor receives a CC on channel `5` CC `0` with value `0`
- **THEN** it pushes a gesture select message whose boolean payload deselects gesture `0`

#### Scenario: Button release without message is ignored
- **WHEN** system button config maps channel `5` CC `32` to a press message and no release message
- **AND** the processor receives a CC on channel `5` CC `32` with value `0`
- **THEN** it pushes no message
- **AND** it does not pass the mapped release to thru

#### Scenario: Unmapped button passes to thru
- **WHEN** the system button processor receives a CC that is not configured
- **AND** a thru processor is configured
- **THEN** it passes the original `BasicMidi` to thru exactly once

### Requirement: spm-42 — MIDI output: system message output info
WHEN MIDI output feedback needs to mirror system-message state, THE synth parameter modulation system SHALL provide a reusable system message output-info helper that owns or references `ParameterManager::UIState` and returns a synth color plus `isOn` flag for supported `MessageIn` values without reading the live manager tree.

#### Scenario: Bank select info follows selected bank
- **WHEN** the output-info helper evaluates a `SelectParamBank` message for an existing selected bank
- **THEN** it returns that bank's configured color
- **AND** returns `isOn=true`

#### Scenario: Bank select info dims unselected bank
- **WHEN** the output-info helper evaluates a `SelectParamBank` message for an existing unselected bank
- **THEN** it returns a dimmed version of that bank's configured color
- **AND** returns `isOn=false`

#### Scenario: Missing bank info is off
- **WHEN** the output-info helper evaluates a `SelectParamBank` message for a bank that does not exist
- **THEN** it returns off color
- **AND** returns `isOn=false`

#### Scenario: Reset info follows reset state
- **WHEN** the output-info helper evaluates a reset message while UI state reports reset held
- **THEN** it returns white
- **AND** returns `isOn=true`
- **WHEN** UI state reports reset not held
- **THEN** it returns grey
- **AND** returns `isOn=false`

#### Scenario: Random info follows random state
- **WHEN** the output-info helper evaluates a random message while UI state reports random held
- **THEN** it returns white
- **AND** returns `isOn=true`
- **WHEN** UI state reports random not held
- **THEN** it returns grey
- **AND** returns `isOn=false`

#### Scenario: Random-mod info follows random-mod state
- **WHEN** the output-info helper evaluates a random-mod message while UI state reports random-mod held
- **THEN** it returns white
- **AND** returns `isOn=true`
- **WHEN** UI state reports random-mod not held
- **THEN** it returns grey
- **AND** returns `isOn=false`

#### Scenario: Scene select info uses blend-weighted endpoint colors
- **WHEN** UI state reports left scene `0`, right scene `1`, and scene blend `0.25`
- **AND** the output-info helper evaluates `SceneSelect(0)`
- **THEN** it returns orange adjusted by brightness `0.5 + 0.5 * (1 - 0.25)`
- **AND** returns `isOn=true`
- **WHEN** it evaluates `SceneSelect(1)`
- **THEN** it returns green adjusted by brightness `0.5 + 0.5 * 0.25`
- **AND** returns `isOn=true`

#### Scenario: Scene select tie uses left endpoint precedence
- **WHEN** UI state reports the same scene ordinal on both scene endpoints
- **AND** the output-info helper evaluates that scene ordinal
- **THEN** it returns the left endpoint orange brightness rule
- **AND** returns `isOn=true`

#### Scenario: Missing scene info is off
- **WHEN** the output-info helper evaluates a `SceneSelect` message for a scene that does not exist
- **THEN** it returns off color
- **AND** returns `isOn=false`

#### Scenario: Gesture select info follows Smart Grid colors
- **WHEN** the output-info helper evaluates a gesture select message for a selected gesture
- **THEN** it returns white
- **AND** returns `isOn=true`
- **WHEN** the gesture is unselected and affects exactly one bank
- **THEN** it returns that bank's color
- **AND** returns `isOn=false`
- **WHEN** the gesture is unselected and affects multiple banks
- **THEN** it returns white
- **AND** returns `isOn=false`
- **WHEN** the gesture is unselected and affects no banks
- **THEN** it returns dimmed grey
- **AND** returns `isOn=false`

#### Scenario: Unsupported message info is off
- **WHEN** the output-info helper evaluates a message type without system feedback semantics
- **THEN** it returns off color
- **AND** returns `isOn=false`

### Requirement: spm-43 — MIDI output: system feedback processors
WHEN system-message MIDI output feedback is processed, THE synth parameter modulation system SHALL provide output processors that own a `SystemMessageOutputInfo`, store their associations in config structs, debounce per association, and emit feedback only when the derived output state changes or the processor is reset.

#### Scenario: CC system output sends on value
- **WHEN** a CC system output processor maps channel `5` CC `32` to a system message whose output info returns `isOn=true`
- **THEN** it emits a CC on channel `5` CC `32` with value `127`

#### Scenario: CC system output sends off value
- **WHEN** a CC system output processor maps channel `5` CC `32` to a system message whose output info returns `isOn=false`
- **THEN** it emits a CC on channel `5` CC `32` with value `0`

#### Scenario: WRLD.Bldr system output sends position color
- **WHEN** a WRLD.Bldr system output processor maps position channel `5`, x `0`, y `4` to a reset message
- **AND** the output info returns white
- **THEN** it emits WRLD.Bldr-compatible color feedback for that position using white

#### Scenario: System output debounces unchanged state
- **WHEN** a system output processor processes the same derived state twice
- **THEN** the second process call emits no duplicate feedback for that association

#### Scenario: System output reset re-renders state
- **WHEN** a system output processor is reset
- **AND** it processes a mapped association with unchanged derived state
- **THEN** it emits the feedback required to restore hardware state

### Requirement: spm-44 — MIDI controller profiles
WHEN a MIDI controller profile is created, THE synth parameter modulation system SHALL provide profile config and factory APIs that build a controller's input processor chain and output processors from shared encoder, analog, and system-message association config, including WRLD.Bldr positions, Launchpad grid positions, and MF Twister side-button addresses, without owning JUCE MIDI devices.

#### Scenario: Profile builds chained input processors
- **WHEN** a profile config contains encoder mappings, analog mappings, system button mappings, MF Twister side-button mappings, and Launchpad grid mappings
- **THEN** the profile factory creates an input processor chain that gives each processor the configured message bus and timestamp provider
- **AND** chains processors through thru so unconsumed MIDI can reach later processors
- **AND** uses the same generic system-message input processor for channel/CC system buttons, MF Twister side buttons, and Launchpad grid positions

#### Scenario: Profile builds independent output processors
- **WHEN** a profile config contains encoder output mappings, WRLD.Bldr system output mappings, and Launchpad system output mappings
- **THEN** the profile factory creates output processors for each configured output protocol
- **AND** callers can invoke each output processor independently without an output chain

#### Scenario: Profile shares system associations
- **WHEN** a profile config maps a controller button, MF Twister side-button address, WRLD.Bldr position, or Launchpad grid position to a `MessageIn`
- **THEN** the same association can be used to configure system button or Launchpad input and controller-specific system output feedback where that controller has feedback LEDs
- **AND** the channel/CC, MF Twister side-button address, WRLD.Bldr position, or Launchpad `(x,y)` position data is not duplicated in separate unrelated input and output config entries

#### Scenario: Profile does not own device lifecycle
- **WHEN** a profile creates processors for a controller
- **THEN** JUCE input and output handlers remain responsible for opening, closing, and reporting MIDI device state

### Requirement: spm-45 — MIDI controller profiles: default WRLD.Bldr and default instrument use
WHEN the default WRLD.Bldr MIDI controller profile is requested, THE synth parameter modulation system SHALL build Smart Grid-derived encoder, analog, reset, random, random-mod, system button, and system output defaults for the WRLD.Bldr controller; and THE synth system SHALL provide one shared default instrument configuration containing one WRLD.Bldr controller seeded with that profile instead of having individual apps construct app-local default controller profiles.

#### Scenario: Default WRLD.Bldr profile maps encoders
- **WHEN** the default WRLD.Bldr profile is created for slot `0`
- **THEN** encoder turn input uses channel `0`
- **AND** encoder pushbutton input uses channel `1`
- **AND** CCs `0..15` map to slot positions `0..15` in row-major order
- **AND** encoder output maps the same positions for value and color feedback

#### Scenario: Default WRLD.Bldr profile maps analogs
- **WHEN** the default WRLD.Bldr profile is created
- **THEN** it maps logical analog index `0` to `SetSceneBlend`
- **AND** maps logical analog indices `1..16` to gesture value messages for gestures `0..15`
- **AND** treats WRLD.Bldr channel `2` CC `N` as logical analog index `N`
- **AND** treats WRLD.Bldr channel `14` CC `N` as logical analog index `N + 2`

#### Scenario: Default WRLD.Bldr profile maps system buttons
- **WHEN** the default WRLD.Bldr profile is created
- **THEN** it maps aux `(0,4)` to momentary reset
- **AND** maps aux `(1,4)` to momentary random
- **AND** maps aux `(2,4)` to momentary random-mod
- **AND** maps aux row `6` to scene select messages
- **AND** maps Smart Grid-derived bank select positions to bank select messages
- **AND** maps configured gesture selector positions to momentary gesture select messages
- **AND** does not map aux focus `(0,5)`

#### Scenario: Default WRLD.Bldr bank buttons tolerate small apps
- **WHEN** the default WRLD.Bldr profile includes bank buttons for bank indices that a specific app has not created
- **AND** those buttons are pressed without an effective modifier
- **THEN** bus processing ignores the missing-bank messages without changing current app state

#### Scenario: Shared default instrument carries WRLD.Bldr
- **WHEN** an app initializes from the shared default instrument configuration
- **THEN** it contains a named WRLD.Bldr controller whose profile is the default WRLD.Bldr profile with sixteen visible encoder mappings, eight scene selectors, sixteen bank selectors, one gesture selector, and scene-blend analog input
- **AND** the runtime builds that controller's input chain and output processors from the instrument configuration

#### Scenario: Miniapp hardware controls exercise profile
- **WHEN** the miniapp runs with the WRLD.Bldr controller connected
- **THEN** the first gesture button can momentarily select gesture `0`
- **AND** the gesture analog CC can set gesture `0` value
- **AND** the scene blend analog CC can set scene blend
- **AND** scene select buttons can select valid scenes
- **AND** reset, random, random-mod, and encoder controls continue to operate through the profile-created processors

### Requirement: spm-56 — MIDI Launchpad: grid position mapping
WHEN Launchpad grid MIDI mapping is configured, THE synth parameter modulation system SHALL provide JUCE-free helpers and data types that represent Launchpad X, Launchpad Pro MK3, and Launchpad Mini MK3 controller identities, translate supported logical `(x,y)` positions to MIDI notes, translate incoming MIDI notes back to logical `(x,y)` positions, and reject positions outside the selected controller shape.

#### Scenario: Launchpad X position mapping follows Smart Grid
- **WHEN** Launchpad X position `(0, 7)` is translated to a note
- **THEN** the helper returns note `11`
- **WHEN** Launchpad X position `(0, -1)` is translated to a note
- **THEN** the helper returns note `91`
- **WHEN** Launchpad X note `11` is translated to a position
- **THEN** the helper returns `(0, 7)` for Launchpad X

#### Scenario: Launchpad Mini MK3 uses Launchpad X shape
- **WHEN** Launchpad Mini MK3 position `(8, 0)` is validated
- **THEN** the helper reports the position is supported
- **WHEN** Launchpad Mini MK3 position `(-1, 0)` is validated
- **THEN** the helper reports the position is unsupported

#### Scenario: Launchpad Pro MK3 supports Pro-only edge positions
- **WHEN** Launchpad Pro MK3 position `(-1, 0)` is validated
- **THEN** the helper reports the position is supported
- **WHEN** Launchpad Pro MK3 position `(0, 9)` is validated
- **THEN** the helper reports the position is supported

#### Scenario: Unsupported position is rejected
- **WHEN** a Launchpad X or Mini MK3 profile association contains position `(-1, 0)`
- **THEN** profile config loading rejects that association
- **AND** does not mutate the target profile config

### Requirement: spm-57 — MIDI input: Launchpad system-message address mapping
WHEN Launchpad grid MIDI input is processed, THE synth parameter modulation system SHALL extend the generic system-message input processor and config association model so Launchpad controller `(x,y)` positions can map to a press `MessageIn` and optional release `MessageIn`; the generic processor SHALL emit the press message for Launchpad Note On or Control Change messages with value greater than zero, SHALL emit the release message for Note Off or value-zero Note On or Control Change messages only when configured, SHALL stamp emitted messages with the configured bus-domain timestamp provider, and SHALL pass supported-but-unmapped messages to its thru processor.

#### Scenario: Grid note press emits configured message
- **WHEN** Launchpad X input config maps position `(0, 7)` to `MessageIn::SceneSelect(0)`
- **AND** the generic system-message input processor receives Note On note `11` with velocity `127`
- **THEN** it pushes a `SceneSelect` message for scene `0`
- **AND** the message timestamp comes from the timestamp provider

#### Scenario: Grid note release emits optional release
- **WHEN** Launchpad Pro MK3 input config maps position `(0, 0)` to `MessageIn::SetGestureSelect(0, true)` on press and `MessageIn::SetGestureSelect(0, false)` on release
- **AND** the generic system-message input processor receives Note Off for that position's note
- **THEN** it pushes a gesture select message whose boolean payload deselects gesture `0`

#### Scenario: Value-zero note on is release
- **WHEN** Launchpad Mini MK3 input config maps position `(1, 1)` to a press message and release message
- **AND** the generic system-message input processor receives Note On for that position with velocity `0`
- **THEN** it emits the release message

#### Scenario: Edge button CC maps to position
- **WHEN** Launchpad X input config maps an edge-button position that translates from a Control Change message
- **AND** the processor receives that CC with value greater than zero
- **THEN** it emits the configured press message for that `(x,y)` position

#### Scenario: Unmapped Launchpad event passes to thru
- **WHEN** the generic system-message input processor receives a supported Launchpad note or CC event whose translated position is not configured
- **AND** a thru processor is configured
- **THEN** it passes the original `BasicMidi` to thru exactly once
- **AND** it pushes no Launchpad message

### Requirement: spm-58 — MIDI output: Launchpad RGB feedback
WHEN Launchpad grid MIDI output feedback is processed, THE synth parameter modulation system SHALL provide a Launchpad output processor that owns or references `SystemMessageOutputInfo`, maps Launchpad controller `(x,y)` positions to feedback `MessageIn` values, evaluates those messages from UI state, debounces per association, and emits Novation-compatible RGB LED SysEx for Launchpad X, Launchpad Pro MK3, and Launchpad Mini MK3.

#### Scenario: Launchpad X output uses X product byte
- **WHEN** a Launchpad X output association maps position `(0, 7)` to a reset feedback message whose output info returns white
- **THEN** the processor emits SysEx bytes beginning `F0 00 20 29 02 0C 03`
- **AND** the color spec uses RGB lighting type `3`
- **AND** the LED index is the note translated from `(0, 7)`

#### Scenario: Launchpad Mini MK3 output uses Mini product byte
- **WHEN** a Launchpad Mini MK3 output association maps a supported position to a feedback message
- **THEN** the processor emits SysEx bytes beginning `F0 00 20 29 02 0D 03`

#### Scenario: Launchpad Pro MK3 output uses Pro product byte
- **WHEN** a Launchpad Pro MK3 output association maps a supported position to a feedback message
- **THEN** the processor emits SysEx bytes beginning `F0 00 20 29 02 0E 03`

#### Scenario: Launchpad RGB output converts synth colors to MIDI bytes
- **WHEN** the output info returns synth color `{r=255, g=128, b=0}`
- **THEN** the emitted RGB color data is `{127, 64, 0}`

#### Scenario: Launchpad output debounces unchanged state
- **WHEN** a Launchpad output processor processes the same derived color state twice
- **THEN** the second process call emits no duplicate SysEx for that association

#### Scenario: Launchpad output reset re-renders state
- **WHEN** a Launchpad output processor is reset
- **AND** it processes a mapped association with unchanged derived state
- **THEN** it emits the feedback required to restore hardware state

### Requirement: spm-59 — MIDI controller profiles: default Launchpad grid profiles
WHEN a default Launchpad MIDI controller profile is requested, THE synth parameter modulation system SHALL build Launchpad X, Launchpad Pro MK3, and Launchpad Mini MK3 profile configs from system-message associations only, SHALL map Launchpad `(x,y)` positions to existing `MessageIn` press, optional release, and feedback values, SHALL leave encoder and analog profile sections unset, and SHALL create profile processors without owning JUCE MIDI devices.

#### Scenario: Default Launchpad profile has no analogs or encoders
- **WHEN** a default Launchpad X, Launchpad Pro MK3, or Launchpad Mini MK3 profile config is created
- **THEN** `encoderInput`, `encoderOutput`, and `analogInput` are absent
- **AND** the profile has at least one Launchpad system-message association

#### Scenario: Default Launchpad profile maps scene buttons
- **WHEN** a default Launchpad profile is created with scene count `4`
- **THEN** it maps four supported Launchpad positions to `SceneSelect(0)` through `SceneSelect(3)` press and feedback messages

#### Scenario: Default Launchpad profile maps gesture buttons momentarily
- **WHEN** a default Launchpad profile is created with gesture selector count `2`
- **THEN** it maps two supported Launchpad positions to `SetGestureSelect(gesture, true)` press messages
- **AND** maps their releases to `SetGestureSelect(gesture, false)` messages

#### Scenario: Default Launchpad profile maps bank buttons
- **WHEN** a default Launchpad profile is created with bank button count `3`
- **THEN** it maps three supported Launchpad positions to `SelectParamBank` press and feedback messages for bank indices `0`, `1`, and `2`

#### Scenario: Profile factory builds Launchpad processors
- **WHEN** a profile config contains Launchpad system-message associations
- **THEN** the profile factory includes those associations in the generic system-message input processor in the input chain
- **AND** creates a Launchpad grid output processor for each Launchpad controller represented in the config
- **AND** callers can invoke each output processor independently without an output chain

### Requirement: spm-46 — Parameters: registration IDs and lookup
WHEN modules or application code register top-level parameters, THE synth parameter modulation system SHALL provide a `ParameterManager::RegisterParameter` API, or equivalent manager-level API, that accepts a parameter group and `ParameterConfig`, validates the group and effective parameter name, creates the parameter in the supplied group, appends it to the manager's global parameter list, and returns a `ParameterId` equal to that parameter's index in the global parameter list for future module lookup; `CreateParameter` SHALL use the same global-list path, while modulation-depth controls SHALL remain parent-owned local controls without `ParameterId` values in the manager global list.

#### Scenario: First registered parameter is list index zero
- **WHEN** a fresh manager registers its first parameter
- **THEN** the returned `ParameterId` is `0`
- **AND** looking up parameter ID `0` returns that parameter

#### Scenario: IDs follow global registration order
- **WHEN** parameters are registered across two groups
- **THEN** each returned parameter ID equals the parameter's index in the manager's global parameter list
- **AND** the IDs remain stable for the lifetime of the manager

#### Scenario: Duplicate effective name fails
- **WHEN** a parameter is registered with an effective name already present in the manager's global parameter list
- **THEN** registration raises a coding error
- **AND** the manager does not append a partial parameter

#### Scenario: Invalid lookup fails
- **WHEN** code requests a parameter ID outside the manager's global parameter list
- **THEN** lookup raises a coding error

#### Scenario: CreateParameter uses list-index IDs
- **WHEN** existing code creates a parameter through `CreateParameter`
- **THEN** the created parameter is appended to the manager's global parameter list
- **AND** its `ParameterId` equals its index in that list

#### Scenario: Lazy modulation-depth controls do not use global IDs
- **WHEN** a routed modulation view lazily materializes a modulation-depth control
- **THEN** the manager global parameter list length is unchanged
- **AND** previously returned top-level parameter IDs remain stable

### Requirement: spm-47 — Parameters: normalized mapping helpers
WHEN modules map normalized parameter values into natural units from their `SetInput` functions, THE synth parameter modulation system SHALL provide manager-level helpers that read a parameter by voice ID and parameter ID and return mapped values for linear interpolation, exponential geometric interpolation, zero-based exponential interpolation with a specified midpoint value, and bipolar variants of each supported mapping; those helpers SHALL map the normalized cached knob value sampled by the most recent `ProcessLite()` call for that voice, and bipolar helpers SHALL first convert it with `bipolar = 2 * normalized - 1`.

#### Scenario: Linear mapping reaches endpoints
- **WHEN** a parameter's normalized cached knob value is `0`
- **AND** code calls `GetLinear(minValue, maxValue, voiceIx, parameterId)`
- **THEN** the helper returns `minValue`
- **WHEN** the same parameter's normalized cached knob value is `1`
- **THEN** the helper returns `maxValue`

#### Scenario: Exponential mapping reaches endpoints
- **WHEN** a parameter's normalized cached knob value is `0`
- **AND** code calls `GetExponential(minValue, maxValue, voiceIx, parameterId)` with positive endpoint values
- **THEN** the helper returns `minValue`
- **WHEN** the same parameter's normalized cached knob value is `1`
- **THEN** the helper returns `maxValue`

#### Scenario: Zero-based exponential honors midpoint
- **WHEN** a parameter's normalized cached knob value is `0`
- **AND** code calls `GetZeroBasedExponential(maxValue, midpointValue, voiceIx, parameterId)`
- **THEN** the helper returns `0`
- **WHEN** the same parameter's normalized cached knob value is `0.5`
- **THEN** the helper returns `midpointValue` within numeric tolerance
- **WHEN** the same parameter's normalized cached knob value is `1`
- **THEN** the helper returns `maxValue`

#### Scenario: Bipolar mapping converts normalized values
- **WHEN** a bipolar mapping helper is called for normalized cached knob values `0`, `0.5`, and `1`
- **THEN** it interprets them as signed values `-1`, `0`, and `1` before applying its natural-unit mapping

#### Scenario: Mapping helper uses cached ProcessLite value
- **WHEN** a mapped parameter has current modulation applied for a voice
- **AND** `ProcessLite()` has sampled that voice's normalized cached knob value
- **THEN** each mapping helper maps the cached knob value rather than recomputing the raw expression at helper-call time

### Requirement: spm-48 — Banks: module slot registration safety
WHEN module-level code registers visible parameters into a bank, THE synth parameter modulation system SHALL provide bank APIs, or equivalent safe registration helpers, in which a bank is durably associated with exactly one `BankSlot`, a bank slot owns the physical layout and MAY be associated with multiple page banks while selecting only one bank for runtime routing, and registration validates duplicate physical slots, duplicate visible parameter names within the registration operation, and the number of available physical positions before mutating the bank's visible mapping.

#### Scenario: Duplicate slot rejected
- **WHEN** module registration attempts to map two parameters to the same bank slot or physical encoder position
- **THEN** bank registration raises a coding error
- **AND** the bank mapping remains unchanged by that failed operation

#### Scenario: Duplicate visible name rejected
- **WHEN** module registration attempts to map two parameters with the same effective visible name in one bank registration operation
- **THEN** bank registration raises a coding error

#### Scenario: Capacity overrun rejected
- **WHEN** module registration requests more visible positions than the bank layout can provide after the requested offset
- **THEN** bank registration raises a coding error rather than silently truncating the mapping

#### Scenario: Bank uses slot layout for capacity
- **WHEN** a bank is associated with a bank slot with four physical encoders
- **AND** module registration asks that bank for available capacity
- **THEN** the bank reports capacity from the associated bank slot's physical layout

#### Scenario: Multiple page banks share one slot
- **WHEN** two banks are associated with the same bank slot during initialization
- **THEN** both banks can validate visible registration against that slot's physical layout
- **AND** the bank slot selects only one associated bank at a time for routed press, modifier press, tick, and UI-state behavior

#### Scenario: Missing slot layout rejected
- **WHEN** module registration requests capacity from a bank that is not associated with a bank slot
- **THEN** bank registration raises a coding error
- **AND** does not guess a capacity from existing top-level mappings

### Requirement: spm-49 — Modulators: pointer-backed source registration and update
WHEN runtime modulation sources are produced by modules or application code, THE synth parameter modulation system SHALL provide a group-owned modulation-source manager, or equivalent `ParameterGroup`/`Modulators` API, where `SetModulationSource` is the source of truth for a modulator's metadata and per-voice source pointers, registers a modulation source by modulation index, per-voice array of `float` pointers, source name, short name, color, connected state, and related metadata, then updates current modulator values by dereferencing those pointers through `UpdateModValues` without applying scaling, normalization, or voice swapping.

#### Scenario: Source registration stores metadata
- **WHEN** code registers modulation source `0` with name `VCO`, short name `VCO`, color `Cyan`, and two voice pointers
- **THEN** the group stores the supplied metadata for modulation source `0`
- **AND** marks that modulation source connected

#### Scenario: Update dereferences source pointers
- **WHEN** a registered modulation source points to voice values `0.25` and `0.75`
- **AND** `UpdateModValues` is called
- **THEN** the group's flat modulator values for that source become `0.25` for voice `0` and `0.75` for voice `1`

#### Scenario: Update does not transform source values
- **WHEN** a registered modulation source points to already-normalized voice values
- **AND** `UpdateModValues` is called
- **THEN** the group copies those values into the flat modulator values unchanged
- **AND** does not clamp, scale, normalize, or swap voices during the update

#### Scenario: Manager updates group modulation values
- **WHEN** `ParameterManager::UpdateModValues` or an equivalent manager API is called for a group
- **THEN** the manager delegates to that group's modulation-source update function

#### Scenario: Update is sample-rate safe
- **WHEN** `UpdateModValues` is called every sample after initialization
- **THEN** it performs no heap allocation
- **AND** does not mutate source metadata

#### Scenario: Invalid source pointers fail
- **WHEN** source registration supplies the wrong number of voice pointers or a null pointer for a connected source
- **THEN** registration raises a coding error
- **AND** the previous source configuration remains unchanged

### Requirement: spm-50 — Persistence: recursive parameter value JSON
WHEN synth parameter values are saved, THE synth parameter modulation system SHALL provide recursive JSON serialization for an initialized `ParameterManager` that stores each top-level parameter's value state keyed by parameter name, including scene centers, parameter-owned per-scene gesture values, parameter-owned per-scene gesture active flags, and each non-default modulation-depth parameter subtree keyed by that depth parameter's live modulation slot index.

#### Scenario: Top-level parameter values are saved
- **WHEN** a parameter has non-default scene center values and gesture values
- **THEN** parameter JSON contains those scene and gesture values under that parameter's initialized name

#### Scenario: Modulation-depth subtree is saved by live slot
- **WHEN** a parameter has an attached modulation-depth parameter for modulator `1` with non-default value state
- **THEN** the parent parameter JSON contains a child value object under `modDepths["1"]`
- **AND** that child object recursively stores its own scene, gesture, and modulation-depth values

#### Scenario: Default modulation-depth subtree is omitted
- **WHEN** a modulation-depth parameter exists only because a modulation page was opened
- **AND** that depth parameter and its recursive children remain at their code-defined default value state
- **THEN** save omits that branch from `modDepths`

#### Scenario: Derived modulation arrays are not authoritative
- **WHEN** parameter JSON is saved
- **THEN** the saved value state is sufficient to recompute current/target modulation depths, min/max values, center scales, and normalization offsets after load
- **AND** those derived arrays are not required as independent patch fields

#### Scenario: Parameter metadata is not saved
- **WHEN** parameter JSON is saved
- **THEN** it does not store parameter names as definitions, short names, colors, ranges, polarity, switch metadata, modulation-source metadata, or bank/page placement

### Requirement: spm-51 — Persistence: value-only parameter load
WHEN synth parameter values are loaded from JSON, THE synth parameter modulation system SHALL first reset all initialized top-level parameter values and existing modulation-depth parameter values to their code-defined defaults, then match saved entries to initialized parameters by parameter name, apply values only to matching parameters and modulation-depth controls addressed by live code-defined modulator slots, leave missing initialized parameters at their initialization defaults, ignore saved parameters that no longer exist, and SHALL NOT create, delete, rename, or reconfigure groups, top-level parameters, pages, banks, slots, modulation sources, MIDI mappings, modulation assignments, colors, polarity, ranges, or parameter configs during value load.

#### Scenario: Load replaces existing parameter values
- **WHEN** JSON contains values for an already registered parameter name
- **THEN** load replaces that parameter's scene center, gesture value, gesture active, and recursive modulation-depth value state

#### Scenario: Deleted parameter is ignored
- **WHEN** JSON contains values for a parameter that does not exist in the initialized manager
- **THEN** load does not create that parameter
- **AND** load continues applying other matching parameter values

#### Scenario: Added parameter keeps default
- **WHEN** the initialized manager contains a parameter name that is absent from JSON
- **THEN** load leaves that parameter at its initialized default value

#### Scenario: Missing modulation-depth branch resets to default
- **WHEN** an existing modulation-depth branch has dirty live values
- **AND** the loaded JSON does not contain that branch under `modDepths`
- **THEN** load leaves that branch's existing parameter values at their code-defined defaults
- **AND** a subsequent save omits that default branch

#### Scenario: Saved modulation-depth branch materializes from code-defined slot
- **WHEN** JSON contains a modulation-depth child for a live parent parameter whose corresponding `modulationDepths_[modIx]` `Parameter*` pointer is null
- **THEN** load materializes that local depth parameter using the live code-defined modulator slot metadata
- **AND** load applies the saved child values recursively
- **AND** load does not read parameter definitions, names, colors, ranges, polarity, or modulation assignments from the JSON child

#### Scenario: Missing modulation-depth storage skips without partial topology
- **WHEN** JSON contains a modulation-depth child for a live parent parameter but no storage is available for the corresponding local depth parameter
- **THEN** load leaves that child branch unapplied
- **AND** load continues applying other matching parameter values

#### Scenario: Saved value array shape mismatch keeps initialized values
- **WHEN** JSON contains a matching parameter name but its saved scene-center, gesture-value, or gesture-active array length differs from the live initialized parameter shape
- **THEN** load skips the mismatched array
- **AND** leaves the corresponding live values at their code-defined defaults from the load reset
- **AND** continues applying other matching arrays, recursive depth values, and other matching parameters

### Requirement: spm-54 — Persistence: manager-wide value reset
WHEN patch lifecycle code requests a full value reset, THE synth parameter modulation system SHALL reset every initialized top-level parameter and every existing modulation-depth parameter to its code-defined default value state without creating, deleting, renaming, recoloring, rerouting, or reconfiguring any parameter topology.

#### Scenario: Full reset clears top-level values
- **WHEN** initialized parameters have edited scene centers, gesture values, gesture active flags, modifier state, gesture selections, and scene blend
- **THEN** a full value reset restores parameter values to each parameter config's default value
- **AND** clears gesture-active flags and gesture selections
- **AND** restores scene selection/blend and modifier state to a captured manager default control state, or the manager's constructor defaults if no app default has been captured

#### Scenario: Full reset clears existing modulation depths
- **WHEN** a parameter has existing modulation-depth parameters with non-default values
- **THEN** a full value reset restores those existing depth parameter values to their neutral defaults
- **AND** does not create missing modulation-depth parameters
- **AND** does not change modulation source metadata or assignments

### Requirement: spm-55 — Recursive modulation view storage
WHEN a bank opens a modulation-depth view for a parameter, THE synth parameter modulation system SHALL populate all visible modulator-depth controls for that page as one atomic operation using code-defined modulator slot metadata, SHALL leave the current bank view unchanged if storage is not sufficient for the whole page, and SHALL request additional parameter storage when the unused storage count falls below twice the group's modulator count.

#### Scenario: Page open materializes all depth controls
- **WHEN** a parameter has missing modulation-depth controls and storage is available for every modulator in the group
- **THEN** opening that parameter's modulation view materializes the missing controls from live modulator metadata
- **AND** the bank shows every modulator-depth cell plus the return/target cell

#### Scenario: Page open is no-op when storage is short
- **WHEN** a parameter has missing modulation-depth controls and storage is not available for every missing control required by the page
- **THEN** opening that parameter's modulation view leaves the selected bank, visible cells, and selected parameter unchanged
- **AND** it does not create a partial set of modulation-depth controls

#### Scenario: Low storage requests reinforcement
- **WHEN** a group's unused parameter storage falls below twice its modulator count
- **THEN** the parameter system emits a message requesting another parameter storage batch
- **AND** a caller can provide that batch without moving or invalidating existing parameter storage

### Requirement: spm-52 — Persistence: MIDI profile config JSON
WHEN MIDI controller profile configuration is saved, THE synth parameter modulation system SHALL provide library JSON serialization and loading helpers for `MidiControllerProfileConfig` and nested encoder input, encoder output, analog input, and system-message association config structs, including WRLD.Bldr positions, Launchpad positions, and MF Twister side-button addresses; SHALL persist encoder input mode under `mode` with values `signed7Bit`, `directionOnly`, or `absolute`; SHALL accept the legacy `relativeMode` field when `mode` is absent; and SHALL preserve every message type and payload needed to rebuild equivalent processors outside any specific app.

#### Scenario: Encoder mappings round trip
- **WHEN** a MIDI profile config contains encoder turn, push, and output mappings
- **THEN** serializing and loading that config preserves channel, CC, slot index, position, encoder mode, turn step, and output color-budget fields

#### Scenario: Absolute encoder mode round trips
- **WHEN** a MIDI profile config contains an encoder input whose mode is absolute
- **THEN** serialization writes `"mode": "absolute"`
- **AND** loading the serialized profile reconstructs absolute mode and the same turn and push mappings

#### Scenario: Legacy relative-mode field remains loadable
- **WHEN** MIDI profile JSON omits `mode` and contains `"relativeMode": "signed7Bit"` or `"relativeMode": "directionOnly"`
- **THEN** loading succeeds with the equivalent `EncoderMode`
- **AND** saving the loaded profile writes the corresponding `mode` field

#### Scenario: New mode field is authoritative
- **WHEN** MIDI profile JSON contains both `mode` and legacy `relativeMode`
- **THEN** loading uses the value in `mode`

#### Scenario: Absolute parameter message round trips
- **WHEN** a serialized message association contains `ParamSetAbsolute` with a slot, position, and normalized value
- **THEN** loading reconstructs the same message type and payload

#### Scenario: System associations round trip
- **WHEN** a MIDI profile config contains system-message associations with press, optional release, feedback for feedback-capable controllers, WRLD.Bldr positions, Launchpad positions, and MF Twister side-button control addresses
- **THEN** serializing and loading that config preserves the messages, controller enum, coordinates, and controller addresses needed to rebuild equivalent input and output processors

#### Scenario: Legacy shift action strings load as reset
- **WHEN** MIDI profile JSON contains legacy shift action strings for toggle, set-true, or set-false system messages
- **THEN** loading that JSON succeeds
- **AND** the loaded messages use the equivalent reset message type and boolean payload

#### Scenario: MF Twister side-button profile round trips
- **WHEN** a MIDI profile config contains six MF Twister side-button associations on zero-based channel `3` CCs `8..13`
- **THEN** serializing and loading that config preserves each side-button control address, press message, and optional release message

#### Scenario: Profile factory uses loaded config
- **WHEN** a loaded MIDI profile config is passed to the profile factory
- **THEN** the factory builds the same processor categories as it would from the original config
- **AND** JUCE MIDI device handlers remain outside the profile factory

#### Scenario: Legacy WRLD.Bldr-only profile JSON remains valid
- **WHEN** MIDI profile JSON contains WRLD.Bldr system associations and no Launchpad positions or MF Twister-specific associations
- **THEN** loading that JSON succeeds
- **AND** the loaded config preserves WRLD.Bldr behavior

### Requirement: spm-60 — UI State: encoder brightness snapshot
WHEN visible parameter-cell UI state is populated for MIDI hardware feedback, THE synth parameter modulation system SHALL publish a per-cell atomic brightness value in `Parameter::UIState`, set connected cells to full brightness `1.0` unless another producer explicitly supplies a different normalized brightness, set disconnected cells to `0.0`, and load that value through the same stable revision snapshot protocol used for value and color feedback.

#### Scenario: Connected cell defaults to full brightness
- **WHEN** a connected parameter cell is populated into UI state
- **THEN** its brightness snapshot value is `1.0`

#### Scenario: Disconnected cell blanks brightness
- **WHEN** a visible cell is disconnected or empty
- **THEN** its brightness snapshot value is `0.0`

#### Scenario: Output snapshot reads stable brightness
- **WHEN** a MIDI output processor reads a mapped cell snapshot
- **THEN** the snapshot includes the cell brightness read under the same bounded revision check as the cell value, color, and indicator color

### Requirement: spm-61 — MIDI input: MF Twister side-button mapping
WHEN the default MF Twister side-button MIDI input is configured, THE synth parameter modulation system SHALL map the six side buttons captured as user-facing MIDI channel 4 CCs 8 through 13 to configurable system-message press and optional release associations, using zero-based `MidiControlAddress.channel = 3` internally, and SHALL emit the press message for nonzero CC values and the release message for zero CC values when configured.

#### Scenario: First side button press emits configured message
- **WHEN** an MF Twister side-button profile maps side button `0` to `MessageIn::SetReset(true)` on press
- **AND** the input processor receives `BasicMidi::CC(..., 3, 8, 127)`
- **THEN** it pushes the configured reset press message

#### Scenario: First side button release emits configured release
- **WHEN** an MF Twister side-button profile maps side button `0` to `MessageIn::SetReset(true)` on press and `MessageIn::SetReset(false)` on release
- **AND** the input processor receives `BasicMidi::CC(..., 3, 8, 0)`
- **THEN** it pushes the configured reset release message

#### Scenario: All six side-button CCs are available
- **WHEN** an MF Twister default profile is created
- **THEN** side-button indices `0` through `5` are addressable as zero-based channel `3` CCs `8` through `13`

#### Scenario: Unconfigured side button does not emit
- **WHEN** an MF Twister side-button association is omitted or has no press message configured by the profile builder
- **AND** the corresponding side-button CC is received
- **THEN** no system message is pushed for that button

### Requirement: spm-62 — MIDI controller profiles: default MF Twister profile
WHEN the default MF Twister MIDI controller profile is requested, THE synth parameter modulation system SHALL build a profile config and profile-created processors for row-major encoder turns, encoder presses, encoder output feedback, and six configurable side-button system-message associations on user-facing channel 4 CCs 8-13, without owning JUCE MIDI devices.

#### Scenario: Default MF Twister profile maps encoders
- **WHEN** the default MF Twister profile is created for slot `0`
- **THEN** encoder turn input uses zero-based channel `0`
- **AND** encoder pushbutton input uses zero-based channel `1`
- **AND** CCs `0..15` map to slot positions `0..15` in row-major order
- **AND** encoder output maps the same positions for primary encoder/ring value, RGB color, RGB brightness, and primary ring brightness feedback
- **AND** encoder output does not use zero-based channel `4` as primary indicator-position feedback

#### Scenario: Default MF Twister profile exposes six side-button slots
- **WHEN** the default MF Twister profile is created
- **THEN** it exposes exactly six configurable side-button system-message associations
- **AND** those associations use zero-based channel `3` CCs `8..13`

#### Scenario: Profile factory builds MF Twister processors
- **WHEN** a profile config contains MF Twister encoder mappings and side-button system-message associations
- **THEN** the profile factory includes encoder input and system-button input in the input chain
- **AND** creates Twister encoder output for primary encoder/ring value, RGB color, RGB brightness, and primary ring brightness feedback
- **AND** creates no side-button output processor for MF Twister side-button associations
- **AND** callers can invoke each output processor independently without an output chain

#### Scenario: Profile does not require all side buttons to be assigned
- **WHEN** a caller creates an MF Twister profile with fewer than six configured side-button messages
- **THEN** the profile remains valid
- **AND** only configured side buttons emit input messages

### Requirement: spm-63 — Modifiers: reset, random, and random-mod
WHEN the synth parameter modulation system tracks control modifiers, THE `ParameterManager` SHALL track independent reset-held, random-held, and random-mod-held state, expose a `GetCurrentModifier` API returning an enum value of none, reset, random, or random-mod, and resolve the effective modifier as random-mod before random before reset before none.

#### Scenario: No modifier held
- **WHEN** reset, random, and random-mod are not held
- **THEN** `GetCurrentModifier` returns none

#### Scenario: Reset modifier held
- **WHEN** reset is held
- **AND** random and random-mod are not held
- **THEN** `GetCurrentModifier` returns reset

#### Scenario: Random takes precedence over reset
- **WHEN** reset and random are both held
- **AND** random-mod is not held
- **THEN** `GetCurrentModifier` returns random

#### Scenario: Random-mod takes precedence
- **WHEN** random-mod is held
- **THEN** `GetCurrentModifier` returns random-mod regardless of reset-held or random-held state

#### Scenario: Random value edit leaves modulation routes
- **WHEN** a random modifier operation targets a visible top-level parameter
- **THEN** the system samples one normalized value and routes the delta from the parameter's current effective knob value to that sampled value through the same active scene and gesture distribution rules as direct knob edits
- **AND** leaves existing modulation-depth parameter assignments attached
- **AND** leaves modulation-depth parameter values unchanged

#### Scenario: Random-mod uses geometric modulation count
- **WHEN** a random-mod modifier operation targets a parameter with available modulator slots
- **THEN** the system repeats while the random coin sample is less than `0.5`
- **AND** each successful iteration chooses one random modulator slot with replacement
- **AND** randomizes the existing depth parameter in that slot when present
- **AND** otherwise materializes the slot's modulation-depth parameter through the normal capacity-checked path and randomizes that created depth parameter

#### Scenario: Random-mod stops when materialization is unavailable
- **WHEN** a random-mod modifier operation chooses an empty modulator slot
- **AND** that slot's modulation-depth parameter cannot be materialized because parameter storage capacity is unavailable
- **THEN** the operation stops without consuming further random-mod loop samples
- **AND** preserves all earlier random-mod edits from the same operation

#### Scenario: Random-mod can perform zero edits
- **WHEN** the first random-mod coin sample is greater than or equal to `0.5`
- **THEN** the operation performs no modulation edits
- **AND** leaves the target parameter value and modulation routes unchanged

#### Scenario: Random-mod with no modulators is a no-op
- **WHEN** a random-mod modifier operation targets a parameter in a group with zero modulators
- **THEN** the operation leaves the target parameter value and modulation routes unchanged

#### Scenario: Random source is reproducible in tests
- **WHEN** tests install or seed the random source used by modifier operations
- **THEN** random value samples, random-mod coin samples, and random-mod slot selections are reproducible for the same seed

### Requirement: spm-64 — Parameters: centered bipolar exponential mapping
WHEN modules map bipolar parameter values into positive multiplicative natural units around a center, THE synth parameter modulation system SHALL provide a manager-level helper that reads a normalized cached parameter value, converts it with `bipolar = 2 * normalized - 1`, and returns a positive exponential value where normalized `0` maps to the supplied left value, normalized `0.5` maps to the supplied center value, and normalized `1` maps to the supplied right value.

#### Scenario: Centered bipolar exponential reaches left endpoint
- **WHEN** a bipolar parameter's normalized cached knob value is `0`
- **AND** code calls the centered bipolar exponential helper with left `0.2`, center `1`, and right `5`
- **THEN** the helper returns `0.2` within numeric tolerance

#### Scenario: Centered bipolar exponential maps center
- **WHEN** a bipolar parameter's normalized cached knob value is `0.5`
- **AND** code calls the centered bipolar exponential helper with left `0.2`, center `1`, and right `5`
- **THEN** the helper returns `1` within numeric tolerance

#### Scenario: Centered bipolar exponential reaches right endpoint
- **WHEN** a bipolar parameter's normalized cached knob value is `1`
- **AND** code calls the centered bipolar exponential helper with left `0.2`, center `1`, and right `5`
- **THEN** the helper returns `5` within numeric tolerance

#### Scenario: Centered bipolar exponential interpolates geometrically on each side
- **WHEN** a bipolar parameter's normalized cached knob value is `0.25`
- **AND** code calls the centered bipolar exponential helper with left `0.25`, center `2`, and right `32`
- **THEN** the helper returns `2 * sqrt(0.25 / 2)` within numeric tolerance
- **WHEN** the normalized cached knob value is `0.75`
- **THEN** the helper returns `2 * sqrt(32 / 2)` within numeric tolerance

#### Scenario: Centered bipolar exponential uses cached ProcessLite value
- **WHEN** a mapped parameter has current modulation applied for a voice
- **AND** `ProcessLite()` has sampled that voice's normalized cached knob value
- **THEN** the centered bipolar exponential helper maps the cached knob value rather than only the scene center value or a raw expression recomputed at helper-call time

#### Scenario: Invalid centered values are rejected
- **WHEN** code calls the centered bipolar exponential helper with a left, center, or right value less than or equal to `0`
- **THEN** the helper raises a coding error rather than returning an invalid mapping

### Requirement: spm-65 — Parameters: signed bipolar zero-based exponential mapping
WHEN modules map bipolar parameter values into signed zero-centered natural units with exponential magnitude, THE synth parameter modulation system SHALL provide a manager-level helper that reads a normalized cached parameter value, converts it with `bipolar = 2 * normalized - 1`, and returns `sign(bipolar) * ZeroBasedExponential(abs(bipolar), maxAbsValue, midpointAbsValue)`.

#### Scenario: Signed zero-based bipolar exponential reaches endpoints
- **WHEN** a bipolar parameter's normalized cached knob value is `0`
- **AND** code calls the signed zero-based bipolar exponential helper with max absolute value `1` and midpoint absolute value `0.1`
- **THEN** the helper returns `-1` within numeric tolerance
- **WHEN** the normalized cached knob value is `1`
- **THEN** the helper returns `1` within numeric tolerance

#### Scenario: Signed zero-based bipolar exponential reaches midpoints
- **WHEN** a bipolar parameter's normalized cached knob value is `0.25`
- **AND** code calls the signed zero-based bipolar exponential helper with max absolute value `1` and midpoint absolute value `0.1`
- **THEN** the helper returns `-0.1` within numeric tolerance
- **WHEN** the normalized cached knob value is `0.75`
- **THEN** the helper returns `0.1` within numeric tolerance

#### Scenario: Signed zero-based bipolar exponential maps center to zero
- **WHEN** a bipolar parameter's normalized cached knob value is `0.5`
- **AND** code calls the signed zero-based bipolar exponential helper
- **THEN** the helper returns `0` within numeric tolerance

#### Scenario: Signed zero-based bipolar exponential uses cached ProcessLite value
- **WHEN** a mapped parameter has current modulation applied for a voice
- **AND** `ProcessLite()` has sampled that voice's normalized cached knob value
- **THEN** the signed zero-based bipolar exponential helper maps the cached knob value rather than a raw expression recomputed at helper-call time

### Requirement: spm-66 — UI State: smoothed parameter center and modulation spread
WHEN `ProcessLitePhase2()` runs for a parameter, THE synth parameter modulation system SHALL update per-voice UI display smoothing state in normalized `[0, 1]` space from the final cached knob value by first applying a group-configured one-pole EMA to that value for display center, then applying a group-configured one-pole EMA to the squared normalized residual between the cached value and updated display center for spread energy; SHALL perform no UI smoothing update during phase 1 or cache replacement; and `Parameter::PopulateUIState` SHALL publish normalized center and square root of spread energy unchanged for unipolar parameters, SHALL publish `2 * center - 1` and twice the normalized spread for bipolar parameters, and SHALL publish zero display spread for switch/discrete parameters.

#### Scenario: Display center smooths final cached knob value
- **WHEN** a voice's normalized cached knob value jumps from `0.0` to `1.0`
- **AND** the group display-center alpha is `0.25`
- **THEN** one `ProcessLitePhase2()` update sets that voice's normalized display center to `0.25`

#### Scenario: Replacement precedes UI smoothing
- **WHEN** phase 1 caches `1.0`, application code replaces that cache with `0.5`, the prior display center is `0.0`, and display-center alpha is `0.25`
- **THEN** phase 2 sets the display center to `0.125`
- **AND** the superseded `1.0` value makes no contribution to that update

#### Scenario: Display spread uses updated center residual
- **WHEN** a unipolar voice's normalized cached knob value is `1.0`
- **AND** its prior normalized display center is `0.0`
- **AND** its prior spread energy is `0.0`
- **AND** the group display-center alpha is `0.25`
- **AND** the group display-spread alpha is `0.5`
- **THEN** one `ProcessLitePhase2()` update sets that voice's normalized display center to `0.25`
- **AND** sets that voice's normalized spread energy to `0.28125`
- **AND** `Parameter::PopulateUIState` publishes display spread `sqrt(0.28125)` within numeric tolerance

#### Scenario: Bipolar display converts center and spread
- **WHEN** a bipolar voice has normalized display center `0.25` and normalized display spread `0.125`
- **AND** `Parameter::PopulateUIState` is called
- **THEN** the UI state publishes signed display center `-0.5`
- **AND** publishes display spread `0.25`

#### Scenario: Static parameter spread decays
- **WHEN** a voice's normalized cached knob value remains equal to its normalized display center
- **AND** the group spread alpha is greater than zero
- **THEN** repeated `ProcessLitePhase2()` calls decay that voice's spread energy toward `0`

#### Scenario: Switch parameter spread is zero
- **WHEN** a switch/discrete parameter has a nonzero cached knob residual relative to its display center
- **AND** `Parameter::PopulateUIState` is called
- **THEN** the UI state publishes display spread `0` for that switch/discrete parameter voice

#### Scenario: Encoder can render blur from UI state
- **WHEN** a connected encoder cell has a nonzero UI display spread
- **THEN** the JUCE encoder renderer can draw the voice indicator as a display center with a spread-proportional blur or cloud without reading live audio-rate parameter state

### Requirement: spm-67 — Compute: curved modulation-depth target mapping
WHEN `Parameter::Compute()` calculates per-voice modulation depths from recursively computed modulation-depth parameters, THE synth parameter modulation system SHALL convert each existing depth parameter's normalized `[0, 1]` knob value to signed travel with `signedKnob = 2 * normalized - 1`, then convert that signed value into a raw depth target with a bipolar zero-based exponential curve before applying signed depth normalization, using maximum absolute depth `1.0`, halfpoint absolute depth `0.25` at absolute signed knob travel `0.5`, and a precomputed mapping base derived from that maximum and halfpoint.

#### Scenario: Quarter and three-quarter depth positions map to one-quarter magnitude
- **WHEN** a modulation-depth parameter's recursively computed normalized knob value is `0.75`
- **THEN** the parent parameter's raw target depth for that modulator is `0.25` within numeric tolerance
- **WHEN** the modulation-depth parameter's recursively computed normalized knob value is `0.25`
- **THEN** the parent parameter's raw target depth for that modulator is `-0.25` within numeric tolerance

#### Scenario: Center and extremes remain anchored
- **WHEN** a modulation-depth parameter's recursively computed normalized knob value is `0.5`
- **THEN** the parent parameter's raw target depth for that modulator is `0` within numeric tolerance
- **WHEN** the normalized knob value is `1`
- **THEN** the parent parameter's raw target depth for that modulator is `1` within numeric tolerance
- **WHEN** the normalized knob value is `0`
- **THEN** the parent parameter's raw target depth for that modulator is `-1` within numeric tolerance

#### Scenario: Curved depth participates in existing normalization
- **WHEN** two modulation-depth parameters on a parent have recursively computed normalized knob values `1.0` and `0.0`
- **THEN** their curved raw depth targets are `1.0` and `-1.0`
- **AND** the parent parameter normalizes those effective depths to `0.5` and `-0.5`

#### Scenario: Audio-rate modulation remains a linear dot product
- **WHEN** a modulation-depth parameter's recursively computed normalized knob value is `0.75`
- **AND** the modulator value for the same voice is `0.8`
- **THEN** the modulation contribution from that route is `0.8 * 0.25` before final range clamping
- **AND** no exponential mapping is applied to the modulator value itself

### Requirement: spm-68 — MIDI output: Twister unbacked encoder brightness
WHEN MF Twister encoder MIDI output is processed, THE synth parameter modulation system SHALL process only configured Twister output mappings, SHALL emit live feedback for mapped encoders whose target slot/position has a connected visible UI cell, SHALL emit MF Twister brightness-off animation values plus blank primary encoder/ring value and RGB color feedback for mapped encoders whose target slot/position has no connected visible UI cell, SHALL emit no primary position feedback on zero-based channel `4`, and SHALL ignore physical encoders that have no configured output mapping.

#### Scenario: Unused slot position blanks Twister brightness
- **WHEN** a Twister output mapping targets a realized slot position whose visible cell is disconnected or empty
- **THEN** the Twister output processor emits channel `2` RGB brightness-off value `17` for that encoder
- **AND** it emits channel `5` primary ring brightness-off value `65` for that encoder
- **AND** it emits blank RGB color and one blank primary encoder/ring value on channel `0`
- **AND** it emits no channel `4` position value

#### Scenario: Mapping beyond visible-cell capacity blanks Twister brightness
- **WHEN** a Twister output mapping targets a slot or position outside the current `ParameterManager::UIState` slot/cell capacity
- **THEN** the Twister output processor treats that mapped hardware encoder as having blank feedback state instead of skipping it as an unstable UI-state read
- **AND** it emits channel `2` RGB brightness-off value `17` for that encoder
- **AND** it emits channel `5` primary ring brightness-off value `65` for that encoder
- **AND** it emits blank RGB color and one blank primary encoder/ring value on channel `0`
- **AND** it emits no channel `4` position value

#### Scenario: Unmapped Twister encoder is ignored
- **WHEN** a Twister physical encoder has no configured output mapping
- **THEN** the Twister output processor emits no feedback for that physical encoder
- **AND** no blank feedback is required for that physical encoder

#### Scenario: Disconnected Twister brightness remains debounced
- **WHEN** a mapped Twister encoder is processed as disconnected and no relevant UI state or output cache reset occurs before the next process call
- **THEN** the Twister output processor does not emit duplicate brightness-off feedback on the next process call

### Requirement: spm-69 — Processing: rate-aware parameter-group timing
WHEN a parameter group is processed at a sample rate different from its reference rate, THE parameter modulation system SHALL provide pure helpers that convert a reference one-pole alpha `a` to `1 - pow(1 - a, referenceRate / processingRate)` and a positive reference sample interval to `max(1, round(referenceInterval * processingRate / referenceRate))`; and SHALL provide a pre-audio timing-reconfiguration API that updates only `processLiteAlpha`, `targetComputeIntervalSamples`, `uiDisplayCenterAlpha`, and `uiDisplaySpreadAlpha` for an existing group without changing its voice, modulator, scene, parameter-capacity, storage, routing, or value topology.

#### Scenario: Alpha conversion preserves wall-clock response
- **WHEN** a reference alpha is converted from 48 kHz to 192 kHz
- **THEN** four consecutive 192 kHz one-pole updates have the same cumulative response as one 48 kHz update within numeric tolerance

#### Scenario: Interval conversion preserves cadence
- **WHEN** reference interval `16` at 48 kHz is converted to 192 kHz
- **THEN** the returned interval is `64`
- **AND** conversion at non-integer ratios rounds to the nearest positive sample count

#### Scenario: Invalid rates and timing are rejected
- **WHEN** a conversion or timing reconfiguration receives a non-positive or non-finite rate, an alpha outside `[0,1]`, or a zero interval
- **THEN** it reports a coding/configuration error
- **AND** the group remains unchanged

#### Scenario: Reconfiguration preserves topology and values
- **WHEN** a group with registered parameters, scene values, modulation depths, and storage batches receives valid new timing
- **THEN** only its four processing-timing fields change
- **AND** all parameter IDs, pointers, values, routes, storage spans, group shape, and capacity remain unchanged

#### Scenario: Repeated prepare does not compound conversion
- **WHEN** an application prepares the same group at multiple processing rates
- **THEN** it can derive and install each timing configuration from fixed reference values
- **AND** the result does not depend on the previously installed processing rate

#### Scenario: Timing update is allocation-free
- **WHEN** valid timing is installed while audio is stopped
- **THEN** the operation performs no heap allocation and does not rebuild parameter storage

### Requirement: spm-80 — Parameter appearance registration
WHEN `ParameterManager` registers a parameter, THE manager SHALL resolve `ParameterConfig::indicatorColors` against the owning group's voice count by broadcasting the base color for an empty palette, broadcasting one explicit color, accepting exactly one color per voice, and rejecting any other nonzero cardinality atomically; modulation-depth parameters SHALL use their modulation source color as base and inherit their target parameter's resolved indicator colors.

#### Scenario: Invalid indicator cardinality preserves registration state
- **WHEN** a four-voice registration supplies two indicator colors
- **THEN** registration throws before parameter count, group allocation count, name registry, or bank mappings change

### Requirement: spm-81 — UI topology: optional modulator visualizer publication
WHEN a modulation source is registered, THE synth parameter modulation system SHALL allow its `ModulatorMetadata` to carry a nullable non-owning portable `Visualizer*`; SHALL copy that association into the corresponding materialized modulation-depth parameter configuration; SHALL publish it atomically through that parameter's `Parameter::UIState` within the existing snapshot transaction; and SHALL publish null for disconnected cells, ordinary parameters, and modulators configured without a visualizer.

#### Scenario: Modulation-depth cell publishes its source visualizer
- **WHEN** modulator `1` is initialized with a non-null visualizer and a parameter's modulation view materializes depth control `1`
- **THEN** the visible depth control's UI state publishes the same visualizer address
- **AND** no visualizer ownership is transferred to the parameter or UI snapshot

#### Scenario: Null visualizer remains null
- **WHEN** a modulator is initialized without a visualizer and its depth control becomes visible
- **THEN** that control's UI state publishes a null visualizer pointer

#### Scenario: Disconnected cell clears visualizer topology
- **WHEN** a previously populated visible cell is set disconnected
- **THEN** its UI state publishes a null visualizer pointer with the other neutral disconnected fields

#### Scenario: Visualizer topology is not patch state
- **WHEN** parameter values are saved and loaded
- **THEN** no visualizer pointer, visibility flag, bounds, or model address is serialized or restored

### Requirement: spm-71 — MiniApp: standard modulation topology
WHEN MiniApp initializes its modulation topology, THE application SHALL configure its two-voice group for exactly fifteen modulators and its bank slot for all sixteen physical positions; SHALL retain one `StandardModulators<2>` that registers four two-voice ganged random LFOs at indexes `0..3`, constant at `11`, and noise at `14`; SHALL register direct VCO, swapped VCO, and ordinary LFO sources at indexes `4`, `5`, and `6`; SHALL retain and publish the standard bundle's UI states and address-stable visualizers; SHALL render the VCO and ordinary-LFO scopes stacked in a bounded left region and all sixteen physical encoder positions in a row-major `4x4` right grid at the default and `640x480` surface sizes; and SHALL omit a separate main-screen ganged-random panel without adding performer parameters for any standard source.

#### Scenario: MiniApp uses standard random defaults
- **WHEN** MiniApp constructs its standard bundle without overriding random timing
- **THEN** its four random inputs use the waiting means, derived waiting/moving sigmas and internal sigmas, and target sigmas defined by `ssm-3`
- **AND** source `0` uses the 500-millisecond waiting mean and target sigma `0.1`

#### Scenario: MiniApp processes standard sources at audio rate
- **WHEN** MiniApp prepares and processes audio
- **THEN** it prepares the standard bundle at the negotiated processing sample rate
- **AND** processes the bundle once per audio sample before the existing group modulation-value update
- **AND** each standard source publishes outputs in MiniApp voice order

#### Scenario: Application-specific sources move after random sources
- **WHEN** MiniApp registration completes
- **THEN** standard random sources occupy `0..3`, direct VCO occupies `4`, swapped VCO occupies `5`, and ordinary LFO occupies `6`
- **AND** constant occupies `11` and noise occupies `14`
- **AND** all other indexes remain disconnected

#### Scenario: Fifteen modulation cells fit the MIN-16 slot
- **WHEN** the user opens a MiniApp modulation view
- **THEN** physical positions `0..14` preserve all fifteen modulator indexes in order
- **AND** connected indexes expose modulation-depth cells while disconnected indexes expose empty disconnected cells
- **AND** physical position `15` is the return cell
- **AND** group capacity accommodates every connected depth without invalidating existing top-level parameter, page, bank, scene, or gesture topology

#### Scenario: Standard visualizers are address stable
- **WHEN** MiniApp materializes depth cells for standard random, constant, or noise sources
- **THEN** each published visualizer pointer refers to the retained standard bundle's corresponding visualizer
- **AND** no processor, adapter row, UI state, or visualizer is reconstructed during audio or UI refresh

#### Scenario: Main screen uses bounded scopes and a complete encoder grid
- **WHEN** the MiniApp surface builds at its default `900x560` size or at `640x480`
- **THEN** the VCO and ordinary-LFO scopes are stacked without overlap inside a bounded left region
- **AND** all sixteen physical encoder positions occupy a bounded row-major `4x4` right grid
- **AND** position `0` is the upper-left cell and position `15` is the lower-right cell

#### Scenario: Empty positions and modulation views preserve physical mapping
- **WHEN** MiniApp publishes a top-level bank or a modulation view in the `4x4` grid
- **THEN** empty top-level positions remain disconnected placeholders
- **AND** a modulation view presents sources `0..14` in physical positions `0..14` and the return cell at position `15`

#### Scenario: Main screen omits the separate random panel
- **WHEN** the MiniApp main surface builds
- **THEN** no separate main-screen ganged-random panel is constructed or rendered
- **AND** standard random, constant, and noise modulation-depth visualizer underlays remain available through their retained visualizers

#### Scenario: Standard metaparameters are not performer state
- **WHEN** MiniApp publishes its pages, banks, encoder mappings, and patch state
- **THEN** standard source timing, indexes, random state, noise state, constant values, and visualizer state are absent from performer controls and serialized parameter values
- **AND** the existing top-level VCO and LFO bank parameter mappings remain unchanged while the slot gains the previously unused physical positions

#### Scenario: Old modulation indexes are not migrated
- **WHEN** MiniApp loads saved values created with its former six-modulator topology
- **THEN** the live code-defined fifteen-source topology remains authoritative
- **AND** no compatibility alias or index translation is applied

### Requirement: spm-72 — Processing: sparse top-level and modulation-route traversal
WHEN a parameter group performs per-sample processing, THE synth parameter modulation system SHALL run phase 1 and phase 2 only for manager-registered top-level parameters, SHALL update materialized local modulation-depth parameters only through the recursive control-rate compute rooted at those top-level parameters, SHALL allow application code to operate between group phases without adding local nodes to either traversal, and SHALL maintain a stable-source active-route permutation whose contiguous active prefix contains every route with non-zero target depth or non-zero current depth still settling toward zero so per-sample depth slew and modulation application do not visit inactive routes.

#### Scenario: Materialized local depth does not add phased work
- **WHEN** a group contains `N` registered top-level parameters
- **AND** any number of local modulation-depth parameters have been materialized beneath them
- **THEN** one group per-sample processing step invokes phase 1 and phase 2 exactly `N` times each
- **AND** invokes both phases zero times on local modulation-depth parameters

#### Scenario: Recursive compute still refreshes local depth state
- **WHEN** a top-level parameter has a materialized modulation-depth subtree
- **AND** the configured target-compute sample arrives
- **THEN** recursive compute evaluates that subtree before deriving the top-level target depths
- **AND** seeds local cached/UI state without adding local nodes to either per-sample processing phase
- **AND** local display center is seeded from the recursively computed value and local display spread is reset to zero at compute cadence rather than filtered per sample

#### Scenario: Inactive routes are outside the active prefix
- **WHEN** a parameter has allocated modulation sources whose current and target depths are zero
- **THEN** those source identities remain available for editing and persistence
- **AND** their routes are outside the active prefix
- **AND** per-sample depth slew and modulation application do not visit them

#### Scenario: Route returning to zero remains active while settling
- **WHEN** an active route's target depth becomes zero while its current depth remains non-zero
- **THEN** the route remains in the active prefix while its one-pole state settles
- **AND** it leaves the active prefix only after current and target depth are zero within the system's modulation-neutral tolerance

#### Scenario: Active permutation preserves source identity
- **WHEN** active routes are reordered or swap-removed inside the active prefix
- **THEN** each route continues to read the modulation source, metadata, UI color, and persisted JSON key belonging to its original modulator index

### Requirement: spm-73 — Gestures: 64-bit sparse selection and activation
WHEN gesture topology is configured or evaluated, THE synth parameter modulation system SHALL support manager gesture counts from zero through 64, SHALL reject counts above 64 before any group is created, SHALL represent manager gesture selection and each parameter scene's active gestures with 64-bit selectors, and SHALL iterate selected or active gestures by set bits rather than scanning configured inactive gesture slots.

#### Scenario: Gesture index boundaries are supported
- **WHEN** a manager is configured with 64 gestures
- **THEN** selection, value, metadata, activation, compute, editing, messaging, persistence, and UI operations accept gesture indices `0`, `31`, `32`, and `63`

#### Scenario: High gesture badges remain distinguishable
- **WHEN** encoder badges render affecting gestures 16 through 63
- **THEN** each badge identifies its original gesture with the distinct one-based numeric label `17` through `64`
- **AND** gesture 63 renders as `64` rather than collapsing to another gesture's label

#### Scenario: More than 64 gestures is rejected
- **WHEN** code attempts to configure a manager with 65 gestures before group creation
- **THEN** configuration fails without changing the previous gesture topology

#### Scenario: Inactive gesture capacity does not add gesture evaluation
- **WHEN** a parameter belongs to a manager configured with 64 gestures
- **AND** none are active for either scene endpoint
- **THEN** parameter compute evaluates zero gesture contributions

#### Scenario: Scene blend iterates the active union
- **WHEN** different gestures are active in the left and right scene endpoints
- **AND** scene blend is between the endpoints
- **THEN** compute and edit distribution iterate the union of the two 64-bit active selectors
- **AND** each effective gesture weight retains the existing endpoint/blend semantics

#### Scenario: Selection and activation remain distinct
- **WHEN** a gesture is selected globally but inactive for a parameter's current scenes
- **THEN** the selection bit participates in gesture arming during an edit
- **AND** the gesture contributes nothing to parameter compute until its per-scene active bit is set

### Requirement: spm-74 — Ownership: neutral local modulation-depth reclamation
WHEN local modulation-depth storage is maintained at a safe control boundary, THE synth parameter modulation system SHALL recursively detach and recycle a local modulation-depth parameter that is a neutral leaf, is not pinned by a live modulation view, and has no non-default state that would affect future editing or persistence; SHALL preserve manager-registered top-level parameter objects and addresses; and SHALL reuse recycled local slots before requesting additional parameter storage.

#### Scenario: Neutral leaf is reclaimed
- **WHEN** a local modulation-depth parameter has neutral/default depth state in every scene
- **AND** it has no child modulation routes, active gestures, non-default latent gesture values, or live modulation-view pin
- **THEN** garbage collection clears the parent's source-index assignment
- **AND** returns the local slot to its group's reusable pool

#### Scenario: Non-neutral state prevents reclamation
- **WHEN** a local modulation-depth parameter is non-neutral in any scene or gesture state preserved by patch JSON
- **THEN** garbage collection retains the parameter and its parent assignment

#### Scenario: Child route prevents reclamation
- **WHEN** a neutral local modulation-depth parameter still owns a child modulation route that cannot itself be reclaimed
- **THEN** garbage collection retains the parent local parameter

#### Scenario: Recursive collection collapses a neutral subtree
- **WHEN** every node in a modulation-depth subtree is a reclaimable neutral leaf after its children are visited
- **THEN** bottom-up garbage collection recycles the complete subtree

#### Scenario: Visible modulation control is pinned
- **WHEN** a local modulation-depth parameter is visible in an open bank modulation view
- **THEN** garbage collection does not detach or recycle it
- **AND** collection may reconsider it after the view closes

#### Scenario: Recycled slot avoids capacity growth
- **WHEN** a neutral local slot has been reclaimed
- **AND** a later edit materializes another local modulation-depth parameter with the same group shape
- **THEN** the group reuses the reclaimed slot before requesting a new storage batch
- **AND** the recycled parameter begins with fully reset default state and resolved metadata for its new parent and source index
- **AND** capacity checks count the recycled slot before requesting new backing storage
- **AND** high-water storage count and storage-local index inspection remain stable while live-local and free-slot counts describe current topology

#### Scenario: Collection preserves patch representation
- **WHEN** a parameter graph is serialized before and after reclaiming only eligible neutral local nodes
- **THEN** both value JSON documents are semantically identical
- **AND** loading either document reconstructs the same non-default modulation graph and parameter outputs

### Requirement: spm-75 — Disconnected sources are empty modulation-view positions
WHEN a bank opens a parameter's modulation view, THE parameter-modulation system SHALL preserve one physical position for every configured modulator index; SHALL expose and materialize a depth parameter only for indexes whose `ModulatorMetadata.connected` is true; SHALL represent every disconnected index with a null bank cell that publishes disconnected UI state and ignores encoder and modifier input; SHALL count only connected missing depths during capacity preflight; and SHALL limit Random Mod selection to connected source indexes.

#### Scenario: Disconnected index stays empty
- **WHEN** a modulation view opens for a group with a disconnected source index
- **THEN** that physical position has no visible parameter and publishes `connected=false`
- **AND** opening the view does not allocate or assign a modulation-depth parameter for that index

#### Scenario: Disconnected position ignores UI input
- **WHEN** the user turns or presses the disconnected position with no modifier, Reset, or Random held
- **THEN** no parameter value, selection, storage request, or topology changes

#### Scenario: Capacity preflight counts connected depths only
- **WHEN** a modulation view has connected and disconnected indexes without existing depth parameters
- **THEN** opening the view requires storage only for the connected missing depths
- **AND** disconnected positions do not prevent the view from opening

#### Scenario: Random Mod excludes disconnected indexes
- **WHEN** Random Mod applies to a parameter whose group contains disconnected source indexes
- **THEN** it can create or change depths only at connected indexes
- **AND** if no source index is connected, it is a no-op without a storage request

#### Scenario: Explicit disconnected depth remains hidden
- **WHEN** programmatic or legacy code has assigned a depth parameter at an index whose source metadata is disconnected
- **THEN** the modulation view still exposes that index as an empty disconnected position
- **AND** the explicit parameter API and stored depth object are otherwise unchanged

### Requirement: spm-76 — Edits: exact absolute scene and gesture distribution
WHEN `Parameter::HandleSetAbsolute(scene, normalizedTarget)` is called with a finite normalized target, THE parameter SHALL clamp the target to `[0, 1]`, map it to the parameter range, arm every selected inactive gesture for the touched scene endpoints by copying the matching parent scene values, rebuild the contribution coefficients after arming, include those newly armed gestures and every already-active gesture with positive effective weight regardless of current selection, and apply a range-constrained minimum-change weighted projection to the distinct contributing scene-center and gesture-value storage locations such that production `ComputeRawCenter(scene)` before target-center slew yields the mapped target within absolute tolerance `1e-5`, every changed latent value remains in range, and unrelated storage remains unchanged. The routed production handler SHALL be `noexcept`, SHALL use no dynamic allocation, SHALL use a fixed-capacity workspace supporting the exact maximum `2 + 2 * 64 = 130` latent locations, and SHALL leave scene centers, gesture values, and gesture-active masks unchanged when internal scene, topology, storage, weight, capacity, or projection invariants reject the edit.

#### Scenario: Endpoint absolute edit
- **WHEN** a parameter has no active positive-weight gesture contribution and scene blend is at the left endpoint
- **AND** `HandleSetAbsolute(scene, 0.75)` is called on a unipolar parameter
- **THEN** the left scene center becomes `0.75` within tolerance `1e-5`
- **AND** the raw scene/gesture center before target-center slew is `0.75` within tolerance `1e-5`
- **AND** the right scene center is unchanged

#### Scenario: Intermediate scene blend reaches the target
- **WHEN** distinct left and right scene centers contribute at an intermediate scene blend
- **AND** `HandleSetAbsolute` receives an in-range target different from the current effective value
- **THEN** it moves the two scene centers in proportion to their effective contribution coefficients with saturation redistribution where required
- **AND** `ComputeRawCenter(scene)` before target-center slew equals the mapped target within tolerance `1e-5`

#### Scenario: First absolute message arms and applies
- **WHEN** a selected gesture is inactive for a parameter at the touched scene endpoints
- **AND** one absolute message targets a value different from the current effective value
- **THEN** that same message activates the gesture and initially copies the applicable parent scene values
- **AND** rebuilds all contribution coefficients after arming because arming may reweight other active gestures and change the pre-solve value
- **AND** includes the newly active gesture in the exact-target projection without swallowing the target
- **AND** `ComputeRawCenter(scene)` before target-center slew equals the mapped target within tolerance `1e-5`

#### Scenario: Active deselected gestures participate
- **WHEN** one or more positive-weight gestures are active for the current scene selection but are not currently selected
- **AND** `HandleSetAbsolute` receives a new target
- **THEN** their gesture values participate according to the existing effective gesture weights
- **AND** `ComputeRawCenter(scene)` before target-center slew equals the mapped target within tolerance `1e-5`

#### Scenario: Saturated contributors redistribute residual
- **WHEN** the unconstrained weighted update would move any contributing latent value outside the parameter range
- **THEN** the handler fixes that value at the approached range endpoint
- **AND** redistributes the remaining residual over unsaturated contributors
- **AND** leaves every latent value in range and reaches the mapped target within tolerance `1e-5`

#### Scenario: Shared scene endpoint is updated once
- **WHEN** the left and right scene endpoints refer to the same scene storage
- **THEN** the solver combines their coefficients before applying the projection
- **AND** does not apply two writes to the same latent location

#### Scenario: Randomized exactness invariant
- **WHEN** deterministic model tests generate valid scene blends, scene centers, gesture activation masks, gesture weights, latent gesture values, parameter ranges, and absolute targets
- **THEN** every production edit's `ComputeRawCenter(scene)` before target-center slew matches an independently computed target within tolerance `1e-5`
- **AND** every latent value remains in range
- **AND** inactive unrelated storage is unchanged

#### Scenario: Invalid internal state is a mutation-free no-op
- **WHEN** the routed absolute handler encounters an invalid scene, backing-storage topology, non-finite or out-of-range relevant latent state, invalid relevant gesture weight, workspace-capacity failure, or rejected projection
- **THEN** it returns without throwing or dynamically allocating
- **AND** scene centers, gesture values, and gesture-active masks are unchanged

### Requirement: spm-77 — Messaging: absolute parameter routing
WHEN a `MessageIn::ParamSetAbsolute(timestamp, slotIx, position, normalizedValue)` reaches `MessageInBus`, THE synth parameter modulation system SHALL treat `normalizedValue` as a parameter-space float target in `[0, 1]` rather than as a raw MIDI velocity, SHALL route it by slot and position through `ParameterManager`, `BankSlot`, and the selected `Bank` to the parameter or modulation-depth control currently visible in that cell's `HandleSetAbsolute`, SHALL use that parameter's owning scene and gesture context, SHALL ignore the edit while any effective modifier is active in the same manner as `ParamIncDec`, and SHALL leave state unchanged when the slot, position, cell, or parameter is not mapped.

#### Scenario: Absolute message reaches visible parameter
- **WHEN** a selected bank maps slot `0` position `2` to a visible parameter
- **AND** the bus applies `ParamSetAbsolute(..., 0, 2, 0.5)` with no modifier active
- **THEN** the mapped parameter handles absolute float target `0.5` unchanged
- **AND** its `ComputeRawCenter(scene)` before target-center slew equals the mapped target within tolerance `1e-5`

#### Scenario: Modulation-depth view receives absolute edit
- **WHEN** a bank's modulation-depth view maps a slot position to a materialized modulation-depth parameter
- **AND** an absolute message addresses that position with no modifier active
- **THEN** the visible modulation-depth parameter receives `HandleSetAbsolute`
- **AND** the hidden top-level parameter is not edited by that message

#### Scenario: Modifier blocks absolute edit
- **WHEN** any effective modifier is active
- **AND** the bus applies a mapped `ParamSetAbsolute` message
- **THEN** it does not perform an absolute edit

#### Scenario: Unmapped absolute address is a no-op
- **WHEN** `ParamSetAbsolute` addresses an absent slot, out-of-range position, disconnected cell, or cell without a parameter
- **THEN** parameter state remains unchanged

### Requirement: spm-78 — MIDI absolute feedback: causal acknowledgement and debounce
WHEN an absolute encoder input mapping accepts raw 7-bit byte `B`, THE synth parameter modulation system SHALL allocate a globally monotonically increasing nonzero runtime epoch, publish that epoch and received byte as the matching controller route's unresolved output expectation before the epoch-bearing `ParamSetAbsolute` float target `(B / 127)^a` becomes visible to `MessageInBus`, where `a = log(0.5) / log(64 / 127)`, process the parameter edit or rejection on the audio thread, record the epoch as processed for the addressed slot position, and publish that processed epoch coherently with the position's normalized pre-modulation scene/gesture raw center; WHILE the published processed epoch precedes the latest expected epoch, absolute output SHALL emit no position feedback and SHALL NOT mutate its position debounce cache; WHEN the processed epoch reaches or passes the expectation, absolute output SHALL convert normalized raw center `x` to byte `round(127 * clamp(x, 0, 1)^(1/a))`, suppress output exactly when that byte equals the received byte, otherwise emit that actual byte once as a correction, and then resume ordinary debounce using final 7-bit bytes; relative encoder input and output SHALL remain outside this protocol and retain post-modulation display feedback.

#### Scenario: Applied absolute input does not echo
- **WHEN** absolute input receives byte `B`, queues epoch `E`, and DSP applies the exact normalized target `(B / 127)^a`
- **AND** UI state publishes processed epoch at least `E` with that raw center
- **THEN** inverse conversion and 7-bit quantization recover byte `B`
- **AND** absolute output emits no position message for `B`
- **AND** records byte `B` for subsequent debounce

#### Scenario: Absolute midpoint outputs MIDI center
- **WHEN** absolute output observes normalized raw knob position `0.5`
- **THEN** inverse conversion produces `64 / 127` before quantization
- **AND** the emitted or debounced MIDI position byte is `64`

#### Scenario: Output debounce remains in the 7-bit domain
- **WHEN** consecutive normalized raw knob positions inverse-convert and quantize to the same 7-bit byte
- **THEN** absolute output emits that byte at most once
- **AND** its position cache stores and compares the 7-bit byte rather than the pre-quantized float

#### Scenario: Output waits for DSP acknowledgement
- **WHEN** absolute input has published expected epoch `E`
- **AND** the latest stable UI snapshot for the route has processed epoch less than `E`
- **THEN** absolute output emits no position message for that route
- **AND** leaves the route's position cache unchanged
- **AND** may continue independent color and brightness feedback

#### Scenario: Rejected absolute input is corrected
- **WHEN** absolute byte `B` with epoch `E` is rejected because a modifier is active or the routed edit cannot apply
- **THEN** the slot position still publishes processed epoch at least `E`
- **AND** if the inverse-curved and quantized actual raw-center byte differs from `B`, output emits the actual byte once even when it equals the pre-input cached value
- **AND** if the actual raw-center byte equals `B`, output emits no unnecessary correction

#### Scenario: Disconnected pending route resolves as blank
- **WHEN** an absolute route becomes disconnected after receiving byte `B` with epoch `E`
- **AND** the disconnected cell publishes processed epoch at least `E` with neutral raw center `0`
- **THEN** resolution leaves the controller's primary encoder/ring at blank byte `0`
- **AND** it suppresses output if `B` was already `0`, otherwise emits one channel-0 blank correction

#### Scenario: Queue failure restores the prior expectation
- **WHEN** an absolute input publishes a tentative expectation and the bounded MIDI input bus rejects its message
- **THEN** the coordinator conditionally restores the route's preceding expectation when no newer input superseded it
- **AND** the failed event does not persistently alter output debounce state
- **AND** output never emits a value derived from treating the failed event as processed

#### Scenario: Rapid input resolves only the latest expectation
- **WHEN** a route receives increasing epochs `E1`, `E2`, and `E3` before UI publication catches up
- **THEN** output remains gated until a stable snapshot has processed epoch at least `E3`
- **AND** resolves against the received byte for `E3` and the actual raw center after that processed prefix
- **AND** does not echo or correct the superseded expectations separately

#### Scenario: Multiple controllers share one cell
- **WHEN** two absolute controllers have independent expected epochs and received bytes for the same slot position
- **AND** the cell publishes a processed epoch and actual raw center covering both events
- **THEN** each controller resolves its own expectation against the inverse-curved and quantized byte for that common actual center
- **AND** the controller whose received byte equals the actual byte suppresses its echo
- **AND** any other controller receives a correction when its byte differs

#### Scenario: Bank or modulation-view change cannot strand acknowledgement
- **WHEN** an absolute message addresses a slot position and the selected bank or modulation-depth view changes before the next output poll
- **THEN** processed-epoch acknowledgement remains associated with the slot position rather than the former parameter object
- **AND** the currently visible cell publishes that acknowledgement with its actual raw center

#### Scenario: Profile rebuild preserves pending coordination
- **WHEN** controller processors rebuild after an absolute expectation is published but before it is resolved
- **THEN** the engine-owned coordinator retains that expectation
- **AND** the rebuilt absolute output processor remains gated until acknowledgement and performs the same suppression-or-correction decision

#### Scenario: Relative feedback remains modulation-aware
- **WHEN** a controller uses either relative encoder mode
- **AND** modulation changes the existing voice-0 display value without moving the scene/gesture center
- **THEN** its encoder output continues to follow the existing post-modulation display value
- **AND** it neither applies the absolute curve nor allocates or waits for an absolute epoch

#### Scenario: Epoch zero remains untracked
- **WHEN** a `ParamSetAbsolute` message is created outside the epoch-allocating absolute-encoder path with epoch `0`
- **THEN** it retains the existing absolute apply-or-reject routing behavior
- **AND** it creates no output expectation and does not advance a slot's processed absolute epoch

#### Scenario: Coordinator capacity exhaustion fails closed
- **WHEN** profile construction cannot reserve one of the coordinator's 4096 runtime-lifetime route records for a newly configured absolute mapping
- **THEN** a matching hardware turn remains consumed as a mapped controller message but does not queue `ParamSetAbsolute`
- **AND** output for that untracked mapping uses ordinary inverse-curved 7-bit debounce without creating or waiting for an expectation
- **AND** no untracked absolute input is applied in a way that can be overwritten by stale feedback

#### Scenario: Unstable snapshot cannot resolve expectation
- **WHEN** output cannot obtain one stable revision containing both processed epoch and raw center
- **THEN** it emits no absolute position feedback from that read
- **AND** leaves the pending expectation and position cache unchanged for retry

#### Scenario: Correction enqueue failure retries
- **WHEN** an acknowledged actual byte differs from the received byte but the MIDI sender rejects the correction enqueue
- **THEN** output leaves the expectation unresolved and the position cache unchanged
- **AND** a later process pass retries the correction

### Requirement: spm-79 — MIDI output: Generic encoder position feedback
WHEN a Generic controller profile contains encoder-turn input mappings and no explicit encoder output, THE synth parameter modulation system SHALL automatically create position feedback from those turn mappings; for each mapping it SHALL emit at most one debounced MIDI CC using exactly the mapping's input channel and CC and the mapped encoder position byte, SHALL emit no color, brightness, animation, SysEx, or auxiliary feedback, SHALL use the causal absolute acknowledgement protocol when the encoder input mode is Absolute, and SHALL use the existing post-modulation display position without epoch coordination in either relative mode; an explicit Twister or WRLD.Bldr encoder output SHALL override automatic Generic feedback.

#### Scenario: Generic output mirrors the full input address
- **WHEN** a Generic turn mapping uses zero-based channel `C` and CC `N`
- **AND** its mapped position requires feedback byte `V`
- **THEN** automatic Generic output emits exactly one CC `(C, N, V)`
- **AND** emits no other MIDI message for that mapping

#### Scenario: Generic absolute output uses causal acknowledgement
- **WHEN** a Generic controller in Absolute mode receives byte `B` on one of its turn mappings
- **THEN** its automatically derived output waits for the mapping's processed epoch
- **AND** suppresses output when the acknowledged raw-center byte equals `B`
- **AND** emits one correction on the same channel and CC when the acknowledged raw-center byte differs
- **AND** retains the pending expectation and cache for retry when correction enqueue fails

#### Scenario: Generic relative output remains modulation-aware
- **WHEN** a Generic controller uses either relative encoder mode
- **AND** modulation changes the mapped post-modulation display position
- **THEN** automatic Generic output emits the changed position on the turn mapping's same channel and CC
- **AND** allocates, waits for, and resolves no absolute epoch

#### Scenario: Explicit specialized output overrides Generic derivation
- **WHEN** a Generic profile contains encoder input and an explicit Twister or WRLD.Bldr encoder output
- **THEN** profile construction creates only the explicit specialized output
- **AND** does not also create automatic Generic CC feedback

#### Scenario: Generic profile without encoder input has no derived output
- **WHEN** a Generic profile has no encoder input
- **THEN** profile construction creates no automatic Generic encoder output

### Requirement: spm-89 — MIDI input: note-addressed controller buttons

WHEN a controller profile configures an encoder push or Generic system-message control address, THE synth parameter modulation system SHALL allow the address
to select CC or note while retaining numeric channel and message-number fields,
SHALL match the configured message type as part of the address, SHALL classify
CC nonzero and note-on positive velocity as press, and SHALL classify CC zero,
note-on zero velocity, and note-off as release.

#### Scenario: Note encoder push emits parameter push

- **WHEN** an encoder push maps note `60` on channel `1` to slot `2` position `3`
- **AND** the input processor receives note-on for channel `1`, note `60`, and positive velocity
- **THEN** it pushes `MessageIn::ParamPush` for slot `2` position `3`

#### Scenario: Note encoder release does not emit another push

- **WHEN** an encoder push maps note `60` on channel `1`
- **AND** the input processor receives raw note-on with zero velocity or note-off with nonzero release velocity for that address
- **THEN** it does not push a parameter command

#### Scenario: Generic note system message emits press and release

- **WHEN** a Generic system-message association maps note `42` on channel `4` to configured press and release messages
- **AND** the processor receives note-on with positive velocity followed by note-off for that address
- **THEN** it emits the configured press message followed by the configured release message

#### Scenario: Zero-velocity note-on is release

- **WHEN** a Generic note system-message association has a configured release message
- **AND** the processor receives note-on with velocity `0` for the mapped channel and note number
- **THEN** it emits the configured release message

#### Scenario: Note system message emits no CC feedback

- **WHEN** a Generic system-message association uses a note control address and has output feedback enabled
- **AND** profile output processors are built and process its derived state
- **THEN** no CC or note feedback message is emitted for that association

#### Scenario: Message type participates in matching

- **WHEN** a profile maps a note address and receives a CC with the same channel and numeric value
- **THEN** that mapping does not consume the CC as its press or release

#### Scenario: Typed addresses round trip

- **WHEN** a profile containing note-addressed encoder pushes and Generic system messages is serialized and loaded
- **THEN** the loaded mappings preserve note message type, channel, and numeric value

#### Scenario: Existing addresses default to CC

- **WHEN** profile JSON contains an existing control address without a message type
- **THEN** loading treats that address as CC and preserves its existing behavior

### Requirement: spm-82 — Message input: grid command model and routing
WHEN external UI or MIDI code sends grid commands, THE synth message system SHALL represent grid press, grid release, grid pressure-change, and grid-select as timestamped `MessageIn` variants; SHALL carry a grid-slot index and signed x/y on cell events, an unsigned MIDI-range velocity on press and pressure-change, and a grid-slot index plus grid index on selection; and SHALL let one `MessageInBus` route parameter messages only to its attached `ParameterManager` and grid messages only to its attached `GridManager` while preserving the existing bounded SPSC timestamp and FIFO contract.

#### Scenario: Grid messages preserve signed coordinates and velocity
- **WHEN** grid-slot `1` receives press `(-1,7,100)`, pressure-change `(-1,7,64)`, and release `(-1,7)` messages
- **THEN** JSON and in-memory round trips preserve slot `1`, x `-1`, y `7`, and both velocity bytes exactly

#### Scenario: Grid selection carries a distinct grid index
- **WHEN** a grid-select message targets grid slot `2` and grid `5`
- **THEN** bus processing requests grid `5` in grid slot `2`
- **AND** no parameter bank selection occurs even if parameter bank or slot `2` or `5` exists

#### Scenario: Mixed messages retain FIFO order
- **WHEN** one bus contains timestamp-visible parameter and grid messages in an interleaved order
- **THEN** it applies each message to the appropriate attached manager in insertion order

#### Scenario: Invalid external grid target is ignored
- **WHEN** a grid message targets a missing manager, slot, grid, or coordinate
- **THEN** bus processing does not throw or mutate either manager

### Requirement: spm-83 — MIDI input: polyphonic pressure mappings
WHEN a MIDI controller profile configures polyphonic pressure, THE synth MIDI system SHALL provide an input mapping from controller channel/note addresses to grid pressure-change messages, SHALL recognize MIDI status `0xA0`, SHALL stamp the shared runtime timestamp and incoming pressure byte into the mapped message, SHALL push matched messages to the shared `MessageInBus`, and SHALL pass unmatched messages to thru exactly once.

#### Scenario: Mapped aftertouch becomes grid pressure
- **WHEN** channel `3` note `42` is mapped to grid slot `1` coordinate `(-1,7)`
- **AND** the processor receives polyphonic aftertouch value `88` at that address
- **THEN** it pushes one `GridPressureChange` for slot `1`, `(-1,7)`, velocity `88` with the runtime timestamp

#### Scenario: Unmapped aftertouch passes through
- **WHEN** a polyphonic-aftertouch message does not match any configured address
- **AND** a thru processor is configured
- **THEN** the processor pushes no grid message
- **AND** passes the original MIDI message to thru exactly once

#### Scenario: Other MIDI statuses are not consumed
- **WHEN** the pressure processor receives note-on, note-off, CC, channel pressure, pitch bend, or realtime MIDI
- **THEN** it pushes no pressure message
- **AND** passes the original MIDI message to thru when thru is configured

#### Scenario: Profile factory chains pressure input
- **WHEN** a controller profile contains encoder, analog, system-button, and polyphonic-pressure input configuration
- **THEN** the profile factory creates one thru chain containing all configured processors with the same message bus and timestamp provider

### Requirement: spm-84 — MIDI profiles: grid mapping persistence and canonical pairing
WHEN controller profiles persist grid mappings, THE synth MIDI system SHALL store grid press/release/feedback as ordinary system-message associations and their polyphonic-pressure behavior as separate pressure-input entries, SHALL serialize and parse every new grid message field and pressure mapping, SHALL accept the previous profile schema with pressure configuration absent, SHALL emit the new profile schema on save, and SHALL provide deterministic normalization keys that preserve canonical semantic round trips including signed coordinates.

#### Scenario: Old profile loads without pressure mappings
- **WHEN** a valid previous-version controller profile has no pressure-input field
- **THEN** the new reader loads it with an empty pressure mapping list
- **AND** preserves its encoder, analog, system-message, and output configuration

#### Scenario: New profile round-trips grid mapping
- **WHEN** a profile contains paired grid press/release/feedback and pressure entries at a negative coordinate
- **THEN** serialize then parse preserves the physical address, grid slot, signed x/y, output-feedback flag, and pressure target

#### Scenario: Normalization is deterministic
- **WHEN** semantically identical grid and pressure mappings are authored in different array orders
- **THEN** normalization produces the same order by logical grid target and physical address

#### Scenario: Instrument schema delegates nested profile version
- **WHEN** an instrument contains a controller using the new profile schema
- **THEN** the instrument and runtime-configuration envelopes parse it through the nested profile reader without changing patch persistence

### Requirement: spm-85 — MIDI output: grid feedback from global UI state
WHEN a system-message output association uses a grid feedback message, THE synth MIDI output system SHALL resolve the addressed cell from the runtime's global UI snapshot without reading the live grid tree, SHALL return the cell RGB plus `isOn=true` when its packed final byte is nonzero and `isOn=false` otherwise, SHALL return off/false for missing or disconnected targets, and SHALL feed that state through the existing controller-specific output processors without changing their MIDI wire protocols.

#### Scenario: Colored grid feedback uses RGB
- **WHEN** a mapped grid cell publishes packed color `(10,20,30,1)`
- **THEN** Launchpad or WRLD.Bldr system output uses RGB `(10,20,30)` through its existing color protocol
- **AND** output info reports `isOn=true`

#### Scenario: Monochrome grid feedback uses on/off
- **WHEN** a mapped grid cell publishes a nonzero RGB color with final byte `0`
- **THEN** output info reports `isOn=false`
- **AND** an existing monochrome system output path can emit its ordinary off state without inferring on/off from RGB

#### Scenario: Missing grid target is blank
- **WHEN** feedback names an out-of-range grid slot or coordinate
- **THEN** output info returns off color and `isOn=false`
- **AND** no live manager or grid object is read

#### Scenario: Existing output protocols remain stable
- **WHEN** equivalent non-grid parameter, bank, scene, gesture, or modifier feedback is processed before and after this change
- **THEN** the emitted MIDI bytes, caching, reset behavior, and output budgets are unchanged

### Requirement: spm-86 — Message input: relative parameter-bank navigation
WHEN relative parameter-bank control is requested, THE synth parameter modulation system SHALL provide `NextParamBank(timestamp, slotIx)` and `PrevParamBank(timestamp, slotIx)` `MessageIn` operations that route through `MessageInBus`, use the addressed slot's current manager-owned bank as their reference, wrap over manager bank indices `0` through `bankCount - 1` when no modifier is active, and apply the effective reset, random, or random-mod modifier to the current bank without changing selection when a modifier is active.

#### Scenario: Next bank advances
- **WHEN** slot `S` currently selects manager bank `B`
- **AND** `B` is not the last bank
- **AND** no modifier is active
- **AND** the bus applies `NextParamBank(..., S)`
- **THEN** slot `S` selects bank `B + 1`

#### Scenario: Next bank wraps
- **WHEN** slot `S` currently selects the last manager bank
- **AND** no modifier is active
- **AND** the bus applies `NextParamBank(..., S)`
- **THEN** slot `S` selects bank `0`

#### Scenario: Previous bank retreats
- **WHEN** slot `S` currently selects manager bank `B`
- **AND** `B` is greater than `0`
- **AND** no modifier is active
- **AND** the bus applies `PrevParamBank(..., S)`
- **THEN** slot `S` selects bank `B - 1`

#### Scenario: Previous bank wraps
- **WHEN** slot `S` currently selects manager bank `0`
- **AND** no modifier is active
- **AND** the bus applies `PrevParamBank(..., S)`
- **THEN** slot `S` selects the last manager bank

#### Scenario: Reset next and previous act on the current bank
- **WHEN** reset is the effective modifier
- **AND** slot `S` currently selects bank `B`
- **AND** the bus applies either `NextParamBank(..., S)` or `PrevParamBank(..., S)`
- **THEN** every top-level parameter mapped by bank `B` is reset through the existing bank-modifier behavior
- **AND** slot `S` still selects bank `B`

#### Scenario: Random next and previous act on the current bank
- **WHEN** random is the effective modifier
- **AND** slot `S` currently selects bank `B`
- **AND** the bus applies either `NextParamBank(..., S)` or `PrevParamBank(..., S)`
- **THEN** every top-level parameter mapped by bank `B` has its visible value randomized through the existing bank-modifier behavior
- **AND** slot `S` still selects bank `B`

#### Scenario: Random-mod next and previous act on the current bank
- **WHEN** random-mod is the effective modifier
- **AND** slot `S` currently selects bank `B`
- **AND** the bus applies either `NextParamBank(..., S)` or `PrevParamBank(..., S)`
- **THEN** every top-level parameter mapped by bank `B` receives the existing geometric modulation randomization
- **AND** slot `S` still selects bank `B`

#### Scenario: Invalid relative navigation is a no-op
- **WHEN** a relative bank message addresses a missing slot, the manager has no banks, the addressed slot has no selected bank, or its selected bank is not manager-owned
- **THEN** bank selection, parameter values, modulation state, scenes, gestures, and modifier state remain unchanged

### Requirement: spm-87 — MIDI profiles: relative bank persistence and stateless feedback
WHEN MIDI controller profiles contain relative parameter-bank messages, THE synth parameter modulation system SHALL serialize and parse `NextParamBank` as `nextParamBank` and `PrevParamBank` as `prevParamBank`, preserve each message's `slotIx` through in-memory and JSON round trips, describe each action with its direction and slot, and report the default off `SystemMessageOutputState` because relative navigation has no persistent selected state.

#### Scenario: Relative bank messages round-trip
- **WHEN** a profile serializes next-bank for slot `2` and previous-bank for slot `3`
- **THEN** its JSON contains `nextParamBank` with `slotIx` `2` and `prevParamBank` with `slotIx` `3`
- **AND** parsing the JSON reconstructs messages equal to the originals

#### Scenario: Relative bank descriptions include direction and slot
- **WHEN** controller configuration describes next-bank and previous-bank messages for slot `4`
- **THEN** the descriptions distinguish next from previous
- **AND** both descriptions identify slot `4`

#### Scenario: Relative bank feedback is stateless
- **WHEN** the system-message output helper evaluates next-bank or previous-bank
- **THEN** it returns off color with `isOn=false`

### Requirement: spm-88 — Tests: relative bank message simulation
WHEN automated tests cover external synth parameter control, THE synth parameter modulation test suite SHALL exercise both relative bank messages through focused unit tests and the deterministic randomized message-bus simulation, model wrapped selection and current-bank modifier behavior independently, and verify production manager, bank, slot, parameter, modulation, modifier, and UI state against the oracle.

#### Scenario: Randomized relative navigation matches the model
- **WHEN** the message-driven randomized simulation interleaves next-bank and previous-bank operations with selection, parameter edits, and reset/random/random-mod state changes
- **THEN** every processed operation leaves production state equal to the independent deterministic model
- **AND** any failure reports the seed and operation trace needed to reproduce it

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

### Requirement: spm-94 — Compute: per-parameter one-way amount modulation target kind

WHEN a `Parameter` is registered, THE `ParameterConfig` SHALL carry a `ModulationTargetKind` field (`kBipolar` or `kOneWayAmount`, defaulting to `kBipolar`) that selects, for a group already configured with modulation blend mode `kAttenuverter` (spm-92), which of the two laws spm-9/spm-10/spm-70 state that specific parameter resolves under: the existing bipolar attenuverter law (`kBipolar`, unchanged from `attenuverter-blend-mode`) or the crossfade one-way amount law (`kOneWayAmount`): the SAME `kCrossfade` crossfade law stated in spm-9, restricted to `depth[route] ∈ [0, 1]`, each route's own source read exactly as stored regardless of `restsAtZero`. The field SHALL have no defined effect for a group configured with modulation blend mode `kCrossfade`. A modulation-depth `Parameter` materialized under `Parameter::EnsureModulationDepth` SHALL inherit its parent's own `modulationTargetKind` (`Parameter::ModulationDepthConfig` propagates it), so a nested depth-of-a-depth resolves under, and randomizes under (see below), the identical target kind as its top-level ancestor, with no additional per-parameter configuration or branching, mirroring spm-92's own nested-inheritance-by-construction. Adding this field SHALL NOT change any existing `ParameterConfig` aggregate-initializer's meaning, since it is a new trailing field with a default (the same reasoning spm-93 gives for its own new trailing field).

`Parameter::RandomizeVisibleValue` SHALL remap its incoming `normalized` draw to `0.5 + 0.5 * clamp(normalized, 0, 1)` before computing its target, when AND ONLY when `id_ == kLocalParameterId` (the existing sentinel distinguishing a local/modulation-depth `Parameter` from a top-level one) AND `config_.modulationTargetKind == kOneWayAmount` — so a freshly-attached one-way amount route's own randomized depth lands in `[0.5, 1]` raw storage and resolves to a depth `OneWayModulationDepthTargetFromKnob` reads as strictly positive except at the single zero-probability draw landing exactly on the boundary, while a `kOneWayAmount` TARGET's own top-level value (`id_` a real registered id, not the sentinel) is drawn from the full, un-remapped range exactly as a `kBipolar` parameter's own value is.

A `Parameter` whose own `modulationTargetKind` is `kOneWayAmount` SHALL floor its own stored scene-centre value (`Parameter::SceneCenter`) at `kNeutralModulationDepthCenter` (operator ruling, 2026-09-28: "cannot be turned below off" is an enforced floor on the stored knob value, not only a read-time floor on the computed depth) on EVERY write path Sheaf provides: `Parameter::HandleIncDec` (encoder tick), `Parameter::HandleSetAbsolute` (absolute set, UI or MIDI), `Parameter::LoadValuesFromJSON` (patch load), and `Parameter::ResetSceneToDefault` (Reset/default, reached by `Parameter::RevertToDefault`, `Parameter::RevertAllToDefault`, and `Parameter::ResetModulationDepthToNeutral`). THE floor SHALL be realized through one construct, NEW `Parameter::EnforceOneWayAmountFloor(std::size_t sceneIx)`, whose entire body is `if (modulationTargetKind == kOneWayAmount) { sceneCenters_[sceneIx] = max(sceneCenters_[sceneIx], kNeutralModulationDepthCenter); }`, called once at the end of each of the four write paths above (once inside `ResetSceneToDefault` covers all three of its own callers). THE floor SHALL have no effect (a no-op through its own guard) for any parameter whose `modulationTargetKind` is `kBipolar`, so no other Sheaf app or parameter is affected. Loading a patch whose stored scene-centre value is below `kNeutralModulationDepthCenter` SHALL leave that value floored at `kNeutralModulationDepthCenter` in memory once `LoadValuesFromJSON` returns, with NO version marker and NO separate migration step — the same construct every other write path calls is what performs it, and `Parameter::ToValueJSON` is unchanged, so a subsequent save serializes the floored value. This floor does NOT reach a value blended in through `Parameter::GestureValue`/gesture-arming at read time (`Parameter::ComputeRawCenter`), which remains outside its scope, and does NOT change `Parameter::SceneCenter`'s own public `float&` accessor, which remains a direct handle to storage exactly as it is for every other parameter.

#### Scenario: A group's blend mode and a parameter's own target kind together select its law
- **WHEN** a `ParameterGroup` is created with modulation blend mode `kAttenuverter`, and two of its `Parameter`s are registered — one with `modulationTargetKind = kBipolar` (the default), one with `modulationTargetKind = kOneWayAmount`
- **THEN** the first resolves under spm-9's `kBipolar` law and the second under spm-9's `kOneWayAmount` law, from the identical group configuration
- **AND** a modulation-depth `Parameter` materialized on the second, and a further depth-of-depth materialized on that depth parameter, both resolve AND randomize under the identical `kOneWayAmount` law, because `ModulationDepthConfig` propagates the field down at each level

#### Scenario: Existing parameters are unaffected
- **WHEN** every `ParameterConfig` registered before this requirement, in `braid-4`, `miniapp`, and every Froggers parameter other than Crispy/Crunchy, is inspected
- **THEN** each reports `modulationTargetKind == kBipolar`, its aggregate-initializer having supplied no value for this new trailing field
- **AND** their resolved values are unchanged by this requirement's addition

#### Scenario: The target kind is read from registered data, never from a name or slot index
- **WHEN** two `Parameter`s are registered at adjacent ids, one `modulationTargetKind = kOneWayAmount` and the other `kBipolar`
- **THEN** `Parameter::ComputeAtDepth` resolves each one's law by reading that parameter's own `config_.modulationTargetKind`, not by comparing its id, name, or registration order against a fixed set
- **AND** swapping which physical id each parameter is registered at (with its own `modulationTargetKind` carried along) produces the identical resolved values, confirming no id or name is hardcoded

#### Scenario: A one-way amount depth child's own Randomize draw is biased into the live half; the target's own value draw is not
- **WHEN** `RandomizeVisibleValue` is called with a fixed input draw of `0.0` (the worst case) once on a `kOneWayAmount` target's own depth child (`id_ == kLocalParameterId`) and once on that same target's own top-level `Parameter` (`id_` a real registered id)
- **THEN** the depth child's own resulting raw stored value is `0.5` (the remapped floor), resolving to depth `0` (the named zero-probability boundary)
- **AND** the top-level target's own resulting raw stored value is `0.0` (un-remapped), exactly as a `kBipolar` parameter's own value-randomize draw would land at the same input

#### Scenario: Turning the encoder left of off, or setting it absolutely below off, floors the stored value at off
- **WHEN** a `kOneWayAmount` parameter's stored scene-centre value is at `kNeutralModulationDepthCenter` (`0.5`) and `HandleIncDec` is called with a negative `delta` large enough that the unfloored target would be `0.3`
- **THEN** the stored scene-centre value after the call is exactly `0.5`, not `0.3`
- **AND** a separate `HandleSetAbsolute` call with `normalizedTarget = 0.1` on a fresh `kOneWayAmount` parameter, once `committed`, leaves the stored scene-centre value at exactly `0.5`, not `0.1`

#### Scenario: A legacy patch value below off loads floored, with no migration; one at or above off loads unchanged
- **WHEN** `LoadValuesFromJSON` loads a `kOneWayAmount` parameter's `sceneCenters` entry of `0.25` (a legacy negative encoding), and separately loads one of `0.75` (a legacy positive encoding)
- **THEN** the `0.25` load leaves the stored value at exactly `0.5` in memory immediately after `LoadValuesFromJSON` returns
- **AND** the `0.75` load leaves the stored value at exactly `0.75`, unchanged
- **AND** no patch schema version field or migration step is introduced; `ToValueJSON` is unmodified, so a subsequent save of the first case serializes `0.5`

#### Scenario: The floor is a no-op for every kBipolar parameter
- **WHEN** `Parameter::EnforceOneWayAmountFloor` is called on a parameter whose `modulationTargetKind` is `kBipolar` (the default) with a stored scene-centre value below `kNeutralModulationDepthCenter`
- **THEN** the stored value is unchanged
- **AND** `braid-4`, `miniapp`, and every Froggers parameter other than Crispy/Crunchy are unaffected by this requirement's addition

### Requirement: spm-96 — Processing: a quiescent parameter skips its per-sample work with identical results
WHEN a top-level parameter's lite step leaves every value it holds unchanged with no route active, THE parameter group SHALL skip that parameter's per-sample smoothing, raw-value computation and UI display smoothing until a function that writes a value the lite step reads wakes it, SHALL keep restoring the cached knob value to the parameter's raw value each sample while skipping, and SHALL produce, at every sample, cached knob values, raw values, UI display centres and UI display spreads bit-identical to the same group with skipping disabled.

#### Scenario: Skipped and unskipped groups agree under every writer
- **WHEN** two identically configured groups, one with `skipQuiescentParameters` true and one false, run the same sequence for thousands of samples: settling, a scene blend move, a knob edit, a modulation route added and removed, a top-level parameter assigned as another's modulation depth, a revert to default, a revert of all parameters, a JSON load, a snap, a depth reset to neutral, a change of processing timing, and an application overwriting cached knob values after phase 1 with values that change and then hold
- **THEN** every top-level parameter's cached knob value, raw value, current center, current scale, current normalization offset, UI display centre and UI display spread are bit-identical between the groups after every sample (current minimums and maximums have no accessor; they feed the change test that ends quiescence)
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `quiescent_skip_matches_unskipped_processing`.

#### Scenario: Settled parameters stop doing per-sample work
- **WHEN** a group with no active routes has run long enough for its smoothers and display state to stop changing
- **THEN** the processing observer's `topLevelQuiescentSkips` counts every visit to its quiescent parameters except one per compute interval, while `topLevelProcessLiteCalls` still counts every visit
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `quiescent_parameters_skip_their_lite_step_until_woken`.

