# Proposal — `attenuverter-blend-mode`

Paired with the frogg3rs change `modulation-depth-attenuverter`
(`openspec/changes/modulation-depth-attenuverter/` in the frogg3rs repo),
which holds every task for both trees (Sheaf tasks are prefixed S) and the
design trace this proposal's claims rest on. This directory holds only this
proposal and the spec delta.

Read in full before dispatch: `omni-rule.md` at the Desktop root (the
frogg3rs `CLAUDE.md` also points here). It binds this proposal: every claim
below carries a file:symbol trace: this proposal was produced under it, and
its dispatch briefs quote the two lines it requires — the observable a
named agent loses if the round is skipped, and the decision waiting on it.

## Why

A `frogg3rs` player turning a modulation-depth knob today watches the
target parameter's own knob lose authority as depth rises: `Parameter::
ComputeAtDepth` (`External/Sheaf/projects/synth/src/ParameterModulation.cpp:2251-2356`)
derives `weightSum = Σ|targetDepth[route]|` across a parameter's active
modulation routes (lines 2308-2311), sets `targetCenterScales_[voiceIx] =
max(0, 1 - weightSum)` (2313-2316, an if/else equivalent to that max), and
at `weightSum ≥ 1` on a single full-depth route the center scale reaches
exactly `0` — `GetRaw` (`:1257-1266`) and `TargetValue` (`:2395-2404`) then
read `center * centerScale + normalizationOffset + Σ depth·modSource`, so
the knob's own value is read with weight `0` and the result is the
modulation source alone. This is a **crossfade between knob and source**,
promoted as spm-9 ("Compute: signed modulation normalization") and spm-70
("Parameters: unipolar core value domain"), and it is a general-purpose
Sheaf capability: `braid-4` (`apps/braid-4/Braid4Core.hpp:122-139`, three
`ParameterGroup`s) and `miniapp` (`apps/miniapp/MiniAppCore.hpp:91-98`) both
build on the identical `Parameter::ComputeAtDepth` path, and neither app nor
the library has a stated defect against the crossfade law itself.

The frogg3rs operator ruled (2026-09-28) that Froggers' own modulation
depth must instead be an **attenuverter**: the knob keeps full authority at
every depth setting, and each modulation source is read bipolar around its
own midpoint and adds `depth × swing` on top of the knob's value, several
sources' swings adding independently and the total clamped to the
parameter's range with no renormalization between sources. That is a
different law than spm-9/spm-70 assert, and the ruling scopes it to a
per-`ParameterGroup` opt-in so `braid-4` and `miniapp` are unaffected unless
their own owners choose it — this proposal is the library-side half of that
scope decision; the frogg3rs-side opt-in itself, and every test/doc change
outside this library, are the frogg3rs change's own tasks.

## What changes

The two laws differ by exactly the per-voice block `ComputeAtDepth` runs
after populating `targetDepths_` (`:2307-2343`) — nothing upstream
(`ModulationDepthTargetFromKnob`, `:include/synth/ParameterModulation.hpp:116-124`,
the depth knob's own centre-neutral exponential curve) or downstream
(`GetRaw`/`TargetValue`'s shared `center·centerScale + offset + Σdepth·modSource`
expression, and `Parameter::PopulateUIState`'s generic normalized-to-display
conversion, `:1304-1361`) needs to change. Working the algebra through
`GetRaw`'s own expression shows the attenuverter law is reachable through
that SAME expression, with a different `centerScale`/`normalizationOffset`
pair and no depth renormalization:

- crossfade (today, default): `centerScale = max(0, 1-weightSum)`;
  `normalizationOffset = -Σ min(0, depth)`; depths divided by `weightSum`
  when `weightSum > 1`.
- attenuverter (new, opt-in): `centerScale = 1` always; depths are never
  renormalized; `normalizationOffset = -0.5 · Σ depth[route]`. Substituting
  into `GetRaw`'s expression: `center·1 + (-0.5Σdepth) + Σdepth·modSource =
  center + Σ depth·(modSource - 0.5)` — the knob at full weight, plus each
  source read bipolar around its own midpoint (`modSource - 0.5`, range
  `±0.5`), scaled by that route's signed depth, summed independently, and
  `GetRaw`'s own trailing `ClampToRange` (`:1265`, `:2403`) is the ruled
  clamp to the parameter's range with no renormalization between sources.
  A negative depth inverts because `depth·(modSource-0.5)` flips sign with
  `depth` — no separate branch needed.
- Reachable ring range (spm-10's law) follows the same substitution: each
  route's term ranges over `[-0.5·|depth|, +0.5·|depth|]` independently of
  the source's live value or the depth's sign, so the attenuverter reach is
  `Σ 0.5·|depth[route]|` and `min/max = ClampToRange(center ∓ reach)` —
  no `weightSum > 1` full-range special case is needed (the ordinary clamp
  already bounds it).

The mode lives on `ParameterGroupConfig` (`include/synth/ParameterModulation.hpp:195-207`),
the one struct every `CreateGroup` call already configures voice/modulator/
scene counts through; `RangeKind` (`:38-41`, per-parameter `Unipolar`/
`Bipolar`) is the nearest stylistic sibling for a small config-carried enum.
A modulation-depth Parameter is always materialized into its parent's own
`group_` (`Parameter::EnsureModulationDepth`, `:src/ParameterModulation.cpp:1882-1896`,
calling `group_.CreateLocalParameter`; `Parameter::AssignModulationDepth`,
`:1853-1866`, refuses a cross-group assignment outright at `&parameter->
Group() != &group_`), so a nested depth-of-depth Parameter shares its
parent's exact `ParameterGroup` and therefore its parent's configured mode
automatically — the ruling's "nested depths use the same law as their
parent's mode" holds by construction, with no new branching for recursion.

## A second operator ruling: not every source rests at 0.5 (spm-93)

The algebra above substitutes a uniform `0.5` for every route's source
midpoint. The operator's before-code audit (`scratchpad/attenuverter-audit.md`,
Finding A) traced this against every one of frogg3rs's 15 registered
modulation sources and found it false for 4 of them: `app/FroggersModulation.hpp:465`'s
own comment distinguishes "`0.5f == NormalizeBipolarToUnit(0.0f)`" (11
sources' own neutral) from "`0.0f == a resting envelope follower's own
floor`" (the other 4: VCO1/VCO2/VCO3 envelope follower and the external-
audio envelope follower). Under the uniform-0.5 substitution, a silent
oscillator or input on one of those 4 sources would still pull its assigned
target away from the knob's own value by up to half the depth's magnitude —
not "no effect," which contradicts the attenuverter's own point.

The operator ruled (2026-09-28) that the distinction is per-SOURCE data, not
a hardcoded index — and then, after re-reading this proposal's OWN first
attempt at that fix (which read a silent envelope follower's full depth as
`+1.0`, not `+0.5`), ruled again that an envelope follower is not a separate
case with its own arithmetic. There is ONE law, stated once, for every
source:

```
signal[route] = restsAtZero(route) ? modulatorSource[route]
                                    : (2 * modulatorSource[route] - 1)
value = clamp(knob + Σ_route depth[route] * signal[route] * 0.5, range)
```

`signal` decodes a route's own stored, always-`[0,1]` `modulatorSource` to
its real reading, `0` meaning "doing nothing": a source whose own rest value
is `0.5` (every registered source today) decodes bipolar around its own
midpoint, `2s-1 ∈ [-1,1]`; a source whose own rest value is `0` (an envelope
follower) decodes unchanged, `s ∈ [0,1]`. The SAME `* 0.5` then applies to
either category, so a bipolar source swings the full `±0.5` at full depth
and full signal, and a follower swings only up to `+0.5` — never `+1` — at
full depth and full signal; a resting source of either kind contributes `0`.

NEW `ModulatorMetadata::restsAtZero` (a defaulted boolean, `false` unless a
source registers otherwise) carries the per-source flag. Realized through
the SAME `GetRaw`/`TargetValue` expression every mode already uses (no
change to that expression): a route's effective depth is `restsAtZero ?
0.5*depth : depth` — `ComputeAtDepth` halves a `restsAtZero` route's own
stored depth in place (`targetDepths_[route] *= 0.5f`), mirroring the
`kCrossfade` branch's own existing in-place `/= weightSum` renormalization
at the same point — and its rest point is `restsAtZero ? 0 : 0.5`; spm-9's
offset and spm-10's min/max formulas both generalize from the hardcoded
`0.5` to this per-route effective-depth/rest-point pair, reducing exactly to
the original single-category derivation when every route is
`restsAtZero = false`. NEW spm-93 states the unified law and this
realization in full, including that adding the field changes no existing
source's behavior (it is a new trailing struct field with a default, so
every one of the 15 existing frogg3rs `ModulatorMetadata{...}`
aggregate-initializers, and any in `braid-4`/`miniapp`, compiles and behaves
unchanged). Which four call sites in frogg3rs set `restsAtZero = true` is
that repo's own task (`2.5` in `modulation-depth-attenuverter`'s tasks.md),
not this library's.

## What does not change

- `ModulationDepthTargetFromKnob`'s curve (the depth knob's own
  centre-neutral exponential knob-to-depth mapping) — confirmed upstream of
  and untouched by this proposal's math.
- `GetRaw`, `TargetValue`, `Parameter::PopulateUIState`'s bipolar/display
  conversion, `Modulators::ApplyActive`'s dot product, and
  `EncoderDrawStateFromParameter`/`AppendArcWithSwitchGaps`
  (`include/synth/EncoderDraw.hpp:306-339`, `:741-752`) — all read whatever
  normalized `[0,1]` center/min/max/depths the per-mode block in
  `ComputeAtDepth` produces, generically.
- Randomize (`spm-63`), persistence (`spm-50`/`spm-51`), revert (`spm-13`),
  and the neutral-leaf reclamation GC (`spm-74`) all operate on the depth
  knob's own raw `[0,1]` commanded value, which `ModulationDepthTargetFromKnob`
  maps identically regardless of which mode reads the resulting depth — none
  of those requirements' text or numbers change.
- `braid-4` and `miniapp`'s `CreateGroup` calls do not set the new field, so
  they default to `kCrossfade` and are byte-for-byte unaffected.

## Impact

`External/Sheaf/projects/synth/include/synth/ParameterModulation.hpp`
(`ParameterGroupConfig`, `ModulatorMetadata`), `External/Sheaf/projects/synth/src/ParameterModulation.cpp`
(`Parameter::ComputeAtDepth`), `External/Sheaf/projects/synth/tests/parameter_modulation_tests.cpp`.
`apps/braid-4/`, `apps/miniapp/` are read for their `CreateGroup` call sites
only (Impact confirms they are unedited).

## Other active changes this overlaps

The other thirteen changes active at the fork point (app-operator-runs,
block-end-fields-show-the-last-control, browser-slider-value-readout,
finish-controller-row-device-editing, fix-out-of-tree-app-gaps,
fix-task-analyzer-plan-derived-tasks, fold-controller-wizard-into-add-row,
gate-browser-midi-on-controllers, individual-address-fields-say-what-they-hold,
launchpad-model-on-the-row, rework-controllers-block-editing,
shorten-deadline-readout-window, unbounded-patch-serialization-arena) name
none of the symbols or the spec this change edits (`ComputeAtDepth`,
`ParameterGroupConfig`, `ModulatorMetadata`, the centre-scale and offset
accessors, `synth-parameter-modulation`), so they do not overlap it. The
ones below do.

Enumerated from `External/Sheaf/openspec/changes/*` (excluding `archive/`),
by content:

- `bank-addressed-absolute-write` (spm-90, `BankSlot`/`Bank::FindVisibleCell`
  addressing) and `app-commands-on-the-bus` (spm-22 MODIFIED, spm-91/spm-95
  ADDED, message-bus storage watermark) both touch
  `synth-parameter-modulation` but neither's own diff (per its proposal/
  design/tasks text) names `ComputeAtDepth`, `ParameterGroupConfig`,
  `GetRaw`, or `PopulateUIState` — no line-level overlap found. This
  proposal claims requirement IDs spm-92 (new) plus MODIFIED spm-4, spm-9,
  spm-10, spm-70; `bank-addressed-absolute-write` already claims spm-90,
  `app-commands-on-the-bus` already claims spm-22/spm-91/spm-95 — no ID
  collision.
- `ui-state-before-audio`: its own tasks.md (section 1, all four tasks
  checked `[x]`, only its postflight/PR section 2 still open) already
  refactored `PopulateUIState` to per-field relaxed-atomic stores without
  changing the values it publishes. This proposal does not edit
  `PopulateUIState`'s body, only what feeds `currentMinValues_`/
  `currentMaxValues_`/`currentCenter_` upstream in `ComputeAtDepth`, so the
  two changes touch adjacent but disjoint code; because `ui-state-before-
  audio`'s code is already written and only awaiting its own PR, whichever
  of the two lands first in the Sheaf branch history, the other rebases
  onto it (task S1.4 in the frogg3rs change's tasks.md).
- `app-midi-catalog` and `shift-and-file-export`: touch `synth-midi-instrument`
  only; their tasks.md files mention `synth-parameter-modulation` paths
  only in passing file lists unrelated to modulation compute. No overlap.
- `shifted-encoder-turns`: already merged into the pinned commit history
  (the frogg3rs change's base per its own delivery note); no longer active
  against the tip this proposal branches from.

## Delivery

Branched from the current stack tip (`8d08d24c` at the time this was
written; the frogg3rs change's task S0.0 re-reads the actual tip before
branching, the same check `frogg3rs-android-app` task S0.0 ran), pushed to
the fork `daguilarc/Sheaf`, and opened as the next sequential pull request
against `jvictor0/Sheaf` `main`. The frogg3rs `External/Sheaf` pin moves to
that branch's tip once its own tests are green (frogg3rs change task S1.4).
