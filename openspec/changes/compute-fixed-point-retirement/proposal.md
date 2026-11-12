# Proposal — `compute-fixed-point-retirement`

## Why

Diego runs frogg3rs on Android, where the DSP meter has already been seen
pinned at 100%. After the 192 kHz engine landed, the desktop benchmark
(M2, headless, -O2, 48 kHz / 256 frames, default patch plus 12 Randomize All
presses, three interleaved rounds) measured mean callback CPU at about 51% for
the old 48 kHz engine, 64% for the 192k engine with the previous converter, and
71% for what shipped. A `sample` profile of the shipped build puts 7874 of 19759
`FroggersAppCore::ProcessBlock` samples (about 40%) in
`ParameterGroup::ProcessSamplePhase1`, nearly all of it under
`Parameter::ComputeAtDepth`.

Froggers configures `targetComputeIntervalSamples = 1`
(`FroggersAppCore` `ConfigureProcessingTiming` call), so every matrix frame is a
compute sample: every top-level parameter runs a full `Compute()` 48,000 times a
second, and spm-96's quiescent skip (`ParameterGroup::ProcessSamplePhase1`,
gated on `!computeSample`) never fires in Froggers, because no sample is a
non-compute sample (and each `ComputeAtDepth` also calls `Wake()`). spm-96 was measured at an interval of 16.

Measured first, on a throwaway copy of the shipped tree instrumented to snapshot
every value `Compute()` touches (target and current centres, scales, offsets,
minimums, maximums, depths, cached knobs, last raw values, UI display state,
route arrays and counts, scalar-leaf state, recursively through depth children)
before and after each top-level `Compute()`:

- steady state, default patch and 12 randomized patches: 100.00% of
  4,193,280 top-level Computes per second-long window left every one of those
  values bit-identical, including 100.00% of the 1.2–3.8 million per window on
  parameters with active routes;
- positive controls on the same instrument: a direct `SceneCenter` write with
  no `Wake()` produced 2134 changing Computes over 40 blocks and then 0; a
  scene-blend sweep produced 12265; idle produced 0.

So almost every Compute in Froggers is a fixed point of a deterministic step.
The Lean model already states the law that makes skipping such a step exact
(`Sheaf.quiescence_preserves_step`, `Sheaf.Schedule.unaffected_node_can_skip`);
nothing in Sheaf uses it for `Compute()`.

## What changes

A top-level parameter's `Compute()` on a compute sample is **retired** — not
run — when the group can show, from values it reads fresh that sample plus a
write epoch, that the full `Compute()` would leave every value it writes
bit-identical. Retirement lives only in `ParameterGroup::ProcessSamplePhase1`,
beside spm-96's skip; `Parameter::Compute`, `ComputeAllParameters`, snaps and
every other caller still compute in full.

### The law (Lean, `formal/Sheaf/Behavior.lean`)

Model one `ComputeAtDepth` call as `step k (t, p) = (g k t, h k (g k t))`:
`k` is everything the call reads that it does not write, `t` is the state it
both reads and writes (`targetCenter_`, the route structure
`activeRouteCount_` / `routeSourceIndices_` / `sourceRoutePositions_`, and
`currentDepths_`), and `p` is everything else it writes, a function of `k` and
the new `t` alone. Two new theorems:

1. `second_compute_fixed_point`: if `s1 = step k s0` and `g k s1.1 = s1.1`, then
   `step k s1 = s1`. So two consecutive Computes with the same `k`, no write
   between them, and the second leaving `t` unchanged, prove the second was a
   fixed point, without comparing any of `p`.
2. `retired_frames_match_full`: if `step k s = s`, then for every `n`, running
   `n` frames whose key stays `k` with every step retired gives the same state
   as running all `n` steps.

Both are pinned in `formal/requirements.json` like every audited statement.
They prove the abstract law over pure logic. Which C++ fields are `k`, `t` and
`p` is a judgment — a mapping onto the C++ code, ruled by the lead on
2026-11-11 from the trace table below — so it gets a named Lean definition with
a comment saying that, and both theorems are stated over that definition so
the dependence shows wherever they are used. The result reads: given the field
partition, retirement is exact. The native differential tests are the
correspondence check that carries it to the compiled path.

### The C++ (`Parameter`, `ParameterGroup`)

- **Write epoch.** New `std::uint64_t writeEpoch_` on `Parameter`, incremented
  by `Wake()` and by every write to `modulationDepths_` or `assignedDepthCount_`
  that does not already call `Wake()`: `AssignModulationDepth`,
  `CollectNeutralChildren` and `ClearModulationDepths`, plus any further site
  task 0.1 finds. `ProcessLitePhase1` increments it when it changes any
  `currentDepths_` value's bits. `ReplaceCachedKnobValue` and
  `RestoreLastRawKnobValues` increment it only on a local parameter
  (`id_ == kLocalParameterId`; `kLocalParameterId` is defined in
  `ParameterModulation.cpp`, so `RestoreLastRawKnobValues`' body moves from the
  header to that file): a depth child's full `Compute()` reseeds its
  cached knob, while depth-0 `Compute()` does not read it, and the
  rate-separated bridge calls `ReplaceCachedKnobValue` on every top-level root
  every engine sample.
- **Epoch sampling.** The epoch is not part of the key. Each `ComputeAtDepth`
  call reads `writeEpoch_` as its first statement, before its own `Wake()`
  (epoch at start), and records `writeEpoch_` as its last action, after every
  internal `Wake()` including `PruneNeutralActiveRoutes`' (epoch after).
- **Key.** At the end of every `ComputeAtDepth` call (both the scalar-leaf early
  return and the full path) the parameter records: the bits of
  `ClampToRange(ComputeRawCenter(scene), range)`; two `std::uint64_t` masks over
  the active routes' sources — `Metadata(source).connected` and
  `Metadata(source).restsAtZero`; and the `recursionDepth` and
  `smoothTargetCenter` arguments. A group whose `numModulators` exceeds 64
  never retires (decided once, at group construction).
- **Armed.** A call arms the parameter iff: a previous call was recorded; this
  call's epoch at start equals the previous call's epoch after; this call's key
  equals the previous call's key; `targetCenter_` has the same bits at the end
  of the call as at its start; no route was activated or removed during the
  call (a flag set by `EnsureRouteActive` when it activates and by
  `RemoveActiveRoute`); and every non-null depth child is a local parameter
  (`kLocalParameterId`) with `activeRouteCount_ == 0` that is armed after this
  call. The scalar-leaf early return writes neither `targetCenter_` nor routes
  and has no depth children, so it arms on the epoch and key conditions alone;
  settled scalar-leaf depth children, the common case in Froggers, take that
  path (a depth child with children of its own takes the full path). By theorem
  1 an armed parameter's last call was a fixed point.
- **Retirable.** `ComputeWouldChangeNothing(scene, depth, smooth)` (const) holds
  iff the parameter is armed, `writeEpoch_` equals the last epoch after,
  `depth` and `smooth` equal the recorded ones, a fresh
  `ClampToRange(ComputeRawCenter(scene), range)` has the recorded bits, the two
  fresh masks over the active routes equal the recorded ones, and every
  non-null depth child is local, has `activeRouteCount_ == 0`, and is retirable
  at `depth + 1`. By theorem 2 retiring it is exact.
- **Group.** In `ParameterGroup::ProcessSamplePhase1`, on a compute sample with
  `config_.retireFixedPointComputes` true, a top-level parameter for which
  `ComputeWouldChangeNothing(Manager().Scene(), 0, true)` holds does not run
  `Compute()`; that sample is then treated as a non-compute sample for it, so
  spm-96's skip applies: restore cached knobs when phase-1 quiescent, otherwise
  call `ProcessLitePhase1()` directly. It must not call
  `Parameter::ProcessSamplePhase1`, which computes again on a compute sample.
- `ParameterGroupConfig::retireFixedPointComputes` (default true) and
  `ParameterGroup::SetRetireFixedPointComputesForTests(bool)` let a test run the
  full path beside the retired one, including inside an application whose group
  config it does not construct.
- Observer: new `topLevelRetiredComputes` counts retired compute visits.
- `localRecursiveComputeCalls` and `scalarLeafCacheHits` count less when
  computes retire. `activeGestureVisits` is incremented in `ComputeRawCenter`,
  which the retirement check also calls, and `activeRouteVisits` only in
  `ProcessLitePhase1`. Tests that assert any of them are reported by the
  executor, not edited, unless this change's tasks name them.
- Test seam for frogg3rs determinism: source 6
  (`FroggersModulationSlate::gangedRandomLfo6_`) is seeded from
  `std::random_device` in the slate's default constructor and has no reseed. A
  static test-only override,
  `FroggersModulationSlate::SetLaunchSaltForTests(std::optional<std::uint32_t>)`,
  is read only by that default constructor
  (`TestLaunchSalt().value_or(std::random_device{}())`); unset, behaviour is
  unchanged. The slate's noise source (`noiseProcessor_`,
  `synth::NoiseModulatorProcessor`) also seeds itself from `std::random_device`
  (`NoiseInitializationSeed()`, `DspNoise.hpp`) independently of the salt
  (found by the executor in task 3.2, amended 2026-11-12). The slate's salted
  constructor therefore builds it with the existing seeded constructor
  `NoiseModulatorProcessor(1, seed)` when, and only when, `TestLaunchSalt()` is
  set, with `seed = (std::uint64_t{*TestLaunchSalt()} << 32) ^
  0x9E3779B97F4A7C15ULL`; otherwise it keeps `NoiseInitializationSeed()`. Unset,
  behaviour is unchanged.

### Trace: why the key covers every input

Every read in `ComputeAtDepth` (and the `ComputeRawCenter`, `GetRaw`,
`ModulationDepthTargetForKind`, `EnsureRouteActive`, `RemoveActiveRoute`,
`PruneNeutralActiveRoutes`, `SeedCachedKnobAndUiDisplayState` it calls), and
what covers it:

| Read | Covered by |
|---|---|
| scene endpoints and blend, own scene centres, gesture values and masks, group gesture values | fresh `ComputeRawCenter` bits (its only consumer in `ComputeAtDepth` is `rawCenter`); `SceneCenter`/`GestureValue` return references writable with no `Wake()`, so these are compared fresh, never trusted to the epoch |
| `targetCenter_` | `t`; armed requires unchanged bits; writers outside `Compute` call `Wake()` |
| `group_.Config()` alphas, `numVoices`, `numModulators`, `modulationBlendMode` | `Config()` is const; `ConfigureProcessingTiming` wakes every top-level parameter |
| own `config_` (`range`, `modulationTargetKind`) | `ResetLocalForReuse` wakes |
| `modulationDepths_`, `assignedDepthCount_` | write epoch (new increments where no `Wake()`) |
| a depth child's cached knob and UI display state (its full `Compute()` reseeds them) | write epoch: `ReplaceCachedKnobValue` / `RestoreLastRawKnobValues` increment it on local parameters |
| route structure, `currentDepths_` | `t`; `EnsureRouteActive`/`RemoveActiveRoute`/`PruneNeutralActiveRoutes` and the non-const depth-slot accessors wake; `ProcessLitePhase1`'s smoothing increments the epoch on change; inside `Compute` a change implies an activation or removal, which sets the flag |
| `targetDepths_` | written before read inside the call (`p`); `TargetDepthSlots` wakes |
| modulator metadata `connected`, `restsAtZero` | fresh masks over active routes; `Modulators::Metadata` returns a mutable reference with no notification |
| depth child's `GetRaw`, `scalarLeafPrepared_`, `preparedScalarDepthTarget_`, `config_` | child retirable recursively; child `activeRouteCount_ == 0` makes `GetRaw` read no modulator value (it adds `Modulators::ApplyActive` over active routes) |
| a depth child that is a top-level parameter | never arms its parent: its lite steps and cached knob change between computes, and the parent's depth-1 `Compute` reseeds them |

The executor confirms this table against the code before writing anything and
reports any read it does not cover.

## Not in scope

Freezing settled root filters in `RateSeparatedParameters` and skipping
unrouted source filters (proposed alongside this change) are separate changes.

## Impact

- `projects/synth/include/synth/ParameterModulation.hpp`,
  `projects/synth/src/ParameterModulation.cpp`
- `projects/synth/tests/parameter_modulation_tests.cpp`
- `projects/synth/formal/Sheaf/Behavior.lean`, `projects/synth/formal/Sheaf/Audit.lean`,
  `projects/synth/formal/requirements.json` (theorem list, statement pins, and
  the three re-reviewed source fingerprints: the two C++ files and frogg3rs
  `app/FroggersModulation.hpp`)
- frogg3rs `app/FroggersModulation.hpp` (test seam),
  `app/FroggersParameterModelTests.cpp` (application-level differential test)
  and the frogg3rs Sheaf pin
- Other active changes touching the same files (bank-addressed-absolute-write,
  app-commands-on-the-bus, shifted-encoder-turns, shift-and-file-export in
  Sheaf; engine-192k-matrix-48k and conversion-alias-rejection in frogg3rs for
  the formal gate): their code is already in this tree; the overlap is merge
  and fingerprint-line risk only.
- braid-4 and miniapp run the same group code; their results are bit-identical
  by the same argument and their existing tests are the check.
