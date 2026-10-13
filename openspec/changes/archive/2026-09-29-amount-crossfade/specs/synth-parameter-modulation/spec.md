# Delta — `synth-parameter-modulation`

Every clause the promoted requirements below already assert for the
`kCrossfade` case and for the `kBipolar` case under `kAttenuverter` is carried
forward verbatim, as is every scenario that documents either of those two
cases. The `kOneWayAmount` case changes law (add-only to crossfade), so every
scenario that stated the old add-only law's own numbers is superseded outright
by a new scenario stating the crossfade law's numbers instead, rather than
edited bullet-by-bullet in place; no `RESTATES-EXCEPT` block is used, because
no bullet is dropped from within any scenario this delta still carries under
its original title — only whole superseded scenarios are dropped, which the
promoted-requirement check reports as a note, not a defect.

## MODIFIED Requirements

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
