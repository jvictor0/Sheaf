# Proposal — `one-way-amount-modulation`

Paired with the frogg3rs change `one-way-amount-modulation`
(`openspec/changes/one-way-amount-modulation/` in the frogg3rs repo), which
holds every task for both trees (Sheaf tasks are prefixed S, S1.1-S1.7 and
S2.1-S2.2 in that file's own `tasks.md`) and the design trace this
proposal's claims rest on (`design.md` in that same directory). This
directory holds only this proposal and the spec delta.

Read in full before dispatch: `omni-rule.md` at the Desktop root (the
frogg3rs `CLAUDE.md` also points here, and this repository's own `CLAUDE.md`
requires escalating rather than working around broken agentic
infrastructure). It binds this proposal: every claim below carries a
file:symbol trace, produced under it.

## Why

`froggers-sheaf-parameter-model`'s own attenuverter requirement (the prior
`attenuverter-blend-mode` change, `2ff35940`, this branch's own current tip)
gave every Froggers parameter — Crispy and Crunchy included — the SAME
bipolar law: a modulation source can swing a target's value both up and
down around its own knob, and the depth knob itself runs signed, off at
centre with a negative half. That is the right law for a real (sound)
parameter, where "the knob's own commanded value" is a meaningful centre to
swing around in either direction. Crispy and Crunchy are not that kind of
parameter: they are amount controls (0 = no scramble, 1 = maximum scramble,
frogg3rs's own `MANUAL.md`), and a "negative scramble amount" has no floor
below zero scramble to express — the bipolar law's own negative half is
present but functionally inert for them, clamping to the same `0` a
depth of exactly `0` already gives. The frogg3rs operator ruled (2026-09-28)
that an app SHOULD be able to mark a specific parameter as taking one-way
modulation instead — every route only adds, the depth knob runs one-sided —
without touching the bipolar law every other Froggers parameter, `braid-4`,
and `miniapp` keep.

## What changes

One new per-parameter field and the two Sheaf functions that read it; the
frogg3rs proposal's own `design.md` states the full law and every
substitution this proposal's spec delta cites rather than repeats.

- `ParameterConfig` (`include/synth/ParameterModulation.hpp`) gains a new
  field, `modulationTargetKind` (type `ModulationTargetKind`, new enum
  `kBipolar`/`kOneWayAmount`, default `kBipolar`) — a NEW trailing field, so
  every existing `ParameterConfig{...}` aggregate-initializer in this
  repository and every app built on it keeps its own meaning unchanged, the
  same pattern `ModulatorMetadata::restsAtZero`'s own addition (spm-93,
  `attenuverter-blend-mode`) already established for exactly this reason.
- `Parameter::ComputeAtDepth` (`src/ParameterModulation.cpp`), inside its
  existing `kAttenuverter` branch, reads `config_.modulationTargetKind` on
  the parameter being resolved and, for `kOneWayAmount`, substitutes a
  different, simpler law through the SAME `centerScale`/`normalizationOffset`
  expression the `kBipolar` branch already uses: `restPoint = 0` for every
  route (never `0.5`, and never derived from that route's own
  `restsAtZero`), and each route's own depth read through a NEW sibling
  curve, `OneWayModulationDepthTargetFromKnob`, instead of the existing
  `ModulationDepthTargetFromKnob`.
- `Parameter::ModulationDepthConfig` propagates `modulationTargetKind` from
  a parent to the depth `Parameter` it creates, so a nested depth-of-a-depth
  inherits its ancestor's target kind by construction — the same
  inheritance-by-construction spm-92 already established for blend mode.
- `Parameter::RandomizeVisibleValue` gains one target-kind-aware remap,
  gated on `id_ == kLocalParameterId` (the existing sentinel this file
  already uses to tell a local/depth `Parameter` from a top-level one) so
  only a one-way DEPTH child's own random draw is biased, never the
  top-level parameter's own value draw.
- A new private helper, `Parameter::EnforceOneWayAmountFloor(std::size_t
  sceneIx)`, floors a `kOneWayAmount` parameter's stored scene-centre value
  at `kNeutralModulationDepthCenter` and is a no-op for every other
  parameter. Enumerated by grep across every site that writes
  `sceneCenters_`: `Parameter::HandleIncDec` (through
  `ApplySceneDistribution`), `Parameter::HandleSetAbsolute` (through
  `detail::ProjectAbsoluteTarget`'s own write-back), `Parameter::LoadValuesFromJSON`,
  and `Parameter::ResetSceneToDefault` (the one function
  `RevertToDefault`/`RevertAllToDefault`/`ResetModulationDepthToNeutral` all
  call) each gain one call to it — operator ruling (2026-09-28): "cannot be
  turned below off" is a stored-value floor enforced on every write path,
  not only a read-time clamp on the computed depth. `design.md`'s own "The
  hard floor" section states the full trace and why one new construct,
  called from four sites, is the right shape rather than repeating the
  clamp at each.

This is a general-purpose Sheaf capability, exactly as `attenuverter-blend-mode`
itself was: the target kind is read from data an app registers on a specific
`Parameter` (`ParameterConfig`'s own field), never from a name, slot index,
or any other identity Sheaf itself would have to hardcode. `braid-4` and
`miniapp` do not set the new field and are unaffected.

## What does not change

- `ModulationDepthTargetFromKnob` itself, for every `kBipolar` parameter —
  untouched; `OneWayModulationDepthTargetFromKnob` is a new sibling, not an
  edit to it.
- `GetRaw`, `TargetValue`, `Parameter::PopulateUIState`'s bipolar/display
  conversion, `Modulators::ApplyActive`'s dot product, and
  `EncoderDrawStateFromParameter`/`AppendArcWithSwitchGaps`
  (`include/synth/EncoderDraw.hpp`) — all read whatever normalized `[0,1]`
  center/min/max/depths the per-mode, per-target-kind block in
  `ComputeAtDepth` produces, generically, exactly as `attenuverter-blend-mode`
  already established for its own two blend modes.
- `ModulatorMetadata`/`restsAtZero` and every existing source registration —
  a one-way target reads a route's `connected` flag and its raw
  `modulatorSource` value only, never `restsAtZero`; adding
  `modulationTargetKind` changes no existing `ModulatorMetadata` or
  `ParameterConfig` aggregate-initializer's meaning, for the same reason
  spm-93 gave for its own new trailing field.
- Randomize (spm-63), persistence (spm-50/spm-51), Revert (spm-13), and the
  neutral-leaf reclamation GC (spm-74) all continue to operate on the depth
  knob's own raw `[0,1]` commanded value. Each of `RandomizeVisibleValue`
  (its own INPUT remap), `LoadValuesFromJSON`, and `ResetSceneToDefault`
  (the floor) gains one additive, target-kind-gated call; `ToValueJSON`,
  the GC's own neutral-detection (`kNeutralModulationDepthCenter`, unchanged),
  and every `kBipolar` parameter's own read of any of these are byte-for-byte
  unaffected, since each new call is a no-op through its own `if` guard for
  every parameter that is not `kOneWayAmount`.
- `braid-4` and `miniapp`'s `CreateGroup`/`ParameterConfig` calls do not set
  the new field, so they default to `kBipolar` and are byte-for-byte
  unaffected.

## Impact

`include/synth/ParameterModulation.hpp` (`ModulationTargetKind`,
`ParameterConfig`), `src/ParameterModulation.cpp` (`Parameter::ComputeAtDepth`,
`Parameter::ModulationDepthConfig`, `Parameter::RandomizeVisibleValue`, NEW
`Parameter::EnforceOneWayAmountFloor`, and its four call sites —
`Parameter::HandleIncDec`, `Parameter::HandleSetAbsolute`,
`Parameter::LoadValuesFromJSON`, `Parameter::ResetSceneToDefault`),
`tests/parameter_modulation_tests.cpp`. `apps/braid-4/`, `apps/miniapp/` are
read for their `CreateGroup`/`ParameterConfig` call sites only (Impact
confirms they are unedited).

## Other active changes this overlaps

Enumerated from `External/Sheaf/openspec/changes/*` (excluding `archive/`)
at this branch's own base, `2ff35940`: none exist beside `archive/` at this
commit. The Android change (`phone-width-composition-and-record-permission`,
`jvictor0/Sheaf#25`) is a separate, later commit
(`dc5a500` in the frogg3rs worktree's own history, ahead of this branch's
base) rebased onto this branch's new tip after delivery, per the Delivery
section below — not concurrent with this branch's own base, so no
line-level overlap exists to trace against it today; S2.2 in the frogg3rs
`tasks.md` is where that rebase's own cleanliness is checked, once #25's
owner performs it.

## Delivery (operator ruling, 2026-09-28)

This is NOT a new branch or a new pull request. This proposal's commits go
directly onto the branch `attenuverter-blend-mode` already carries
(currently `2ff35940`, open and unmerged as `jvictor0/Sheaf#24`) — pushing
that branch updates #24 in place. #24's own description is refreshed to
state it now carries both the attenuverter law it already shipped and this
proposal's one-way amount mode (frogg3rs task S2.1). `jvictor0/Sheaf#25`
(the Android change, currently stacked after #24) is rebased onto #24's new
tip afterward — that rebase belongs to #25's own owner, not to this
proposal; this proposal's only obligation toward it is landing cleanly
enough to rebase onto, checked by frogg3rs task S2.2. The frogg3rs
`External/Sheaf` pin moves to this branch's new tip once its own tests are
green (frogg3rs task 2.1), and frogg3rs `main` is fast-forwarded onto that
(frogg3rs task 3.1) — never opened as a pull request against this
repository, since frogg3rs consumes this repository as a pinned submodule,
not the reverse.
