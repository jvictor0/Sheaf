The executor's deliverable is a report; code changes are a side effect of it.
A conflict between this file, `proposal.md`, the spec delta and the code is
reported and stops that task. Every new assertion is shown to fail with its
production change reverted, by the executor, before the task is reported done.
A test that is red before the task starts is reported, never edited. Build and
run one binary at a time; C++ builds run at `-j2` under `nice`. Nothing is
installed and nothing is fetched: Mathlib is already present at
`projects/synth/.lake` (a local copy), and `make formal-test` needs
`export PATH="$HOME/.elan/bin:$PATH"` first; with that it passed offline on this
tree before the change (180 theorems, 11 sources). Paths are relative to
`projects/synth` unless they say frogg3rs.

## 0. Trace

- [ ] 0.1 Confirm `proposal.md`'s trace table against `ComputeAtDepth` and
      everything it calls. Report every read the table does not cover, and
      every site that writes `modulationDepths_` or `assignedDepthCount_`,
      with whether it already calls `Wake()`. Stop on any uncovered read.
      Check: the report lists each site with file and function.

## 1. Lean law

- [ ] 1.1 Add `Sheaf.second_compute_fixed_point` and
      `Sheaf.retired_frames_match_full` to `formal/Sheaf/Behavior.lean`, stated
      as in `proposal.md` ("The law"), with `step k (t, p) = (g k t, h k (g k t))`
      over arbitrary types, with the key / carried / derived partition as a
      named definition whose comment says it is a judgment ruled by the lead
      on 2026-11-11 from `proposal.md`'s trace table and checked against the
      code by the native differential tests, and with both theorems stated
      over that definition; proved without `sorry` or new axioms, using only
      what that file already imports. Add both to `formal/Sheaf/Audit.lean`
      (`#print axioms`) and to `formal/requirements.json` (theorem list and
      statement pins) in the positions the gate requires; pinned statements
      cannot contain `:=`, and the gate's escape-hatch scan bans the words it
      lists in `scripts/check_parameter_model.py`. Done before any task-2 edit,
      while the source fingerprints still match.
      Check: `make formal-test` passes and prints both names; deleting either
      theorem, or editing either statement without its pin, makes it fail.

## 2. Retirement

- [ ] 2.1 Implement "The C++" section of `proposal.md` exactly: write epoch
      (every listed site, including the local-only increments), epoch
      sampling, key, armed (including the early return), retirable, the group
      change, the 64-modulator bound, `retireFixedPointComputes`,
      `SetRetireFixedPointComputesForTests`, `topLevelRetiredComputes`. Update
      the spm-96 comments in `ParameterModulation.hpp`
      (`skipQuiescentParameters`) and `ParameterGroup::ProcessSamplePhase1` so
      they state the retired case.
      Check: tasks 2.2 and 2.3.
- [ ] 2.2 Give `quiescence::Rig` constructor parameters for the compute
      interval and for `retireFixedPointComputes`; existing callers keep
      interval 16 and retirement off, so the spm-96 tests still compare
      against full `Compute()`. "Compute-sample visits" below means
      `topLevelProcessLiteCalls` divided by the run's interval. Add
      `fixed_point_compute_retirement_matches_full_compute`: the spec delta's
      first spm-97 scenario, a rig with retirement on against a rig with
      retirement and the quiescent skip off, at intervals 1 and 16, comparing
      after every sample every public const accessor of every top-level
      parameter and of every depth child reachable from them. Positive control
      written now: `topLevelRetiredComputes` exceeds half of the compute-sample
      visits in each run. Red controls,
      each with only that one guard removed from production, each making this
      test fail (report which sample and parameter): the fresh raw-centre
      compare; the metadata masks; the child `activeRouteCount_ == 0`
      condition; the child-is-local condition; the `ProcessLitePhase1` epoch
      increment; the epoch increment in each of `AssignModulationDepth`,
      `CollectNeutralChildren` and `ClearModulationDepths`; the local-only
      increment in `ReplaceCachedKnobValue`; the route-changed flag; the
      `targetCenter_` unchanged condition. Give `quiescence::Rig` a
      blend-mode constructor parameter (existing callers keep their current
      mode) and add one run, at interval 1, in attenuverter mode, whose
      sequence flips `Metadata(source).restsAtZero` on a routed source after
      the rig has settled and flips it back later, with no `Wake()`; the
      `restsAtZero` mask red control must fail on that run. A guard whose removal leaves the
      test green is reported, with the event added to the sequence that would
      exercise it, or with the trace showing the guard is unreachable.
- [ ] 2.3 Add `settled_parameters_retire_their_compute_until_written` (the
      second spm-97 scenario) on the seven-parameter rig at interval 1 with
      retirement on: after 48000 samples, 160 more samples retire all
      7 × 160 visits; after `RevertAllToDefault` on parameter 2, the next
      sample retires exactly 6. Update
      `quiescent_parameters_skip_their_lite_step_until_woken` to the MODIFIED
      spm-96 scenario: the existing `visits - 7 * 10` assertion on a rig with
      retirement off, and `visits` on a rig with it on.
      Check: both pass; with the group change reverted, the retired count
      is 0 and the new assertions fail.
- [ ] 2.4 Run every `projects/synth` test binary. Report each test that
      fails, with the observer counter or value it asserts. Do not edit it.

## 3. Froggers

- [ ] 3.1 In frogg3rs `app/FroggersModulation.hpp`, add the test seam the
      proposal describes (`SetLaunchSaltForTests`, read only by the default
      constructor), and the noise-source seeding it describes (seeded
      constructor only while `TestLaunchSalt()` is set).
      Check: task 3.2's determinism control.
- [ ] 3.2 In frogg3rs `app/FroggersParameterModelTests.cpp`, add
      `froggers_compute_retirement_matches_full_compute`, the third spm-97
      scenario: set `FroggersModulationSlate::SetLaunchSaltForTests(0x5EED)`,
      build three `SynthRig<FroggersApp>` rigs (48 kHz, 256 frames, own
      scratch data paths), then clear the override; rigs B and C call
      `SetRetireFixedPointComputesForTests(false)` on
      `Application().Parameters().Group()`. Same sequence on all three:
      `StartAt(0)`; settle 16 blocks; six Randomize All presses 40 blocks
      apart; a knob turn through the surface; a scene blend sweep over 40
      blocks; transport stop for 40 blocks and start; then 40 idle blocks.
      After every block: every output sample, and every top-level parameter's
      `CachedKnobValue(0)`, equal bit for bit between A and B and between B
      and C. Attach one `ParameterProcessingObserver` per rig with
      `SetProcessingObserverForTests` on its group after construction; the
      counters are cumulative, so window counts are differences of snapshots
      taken at the window's first and last block. Positive control: A's
      `topLevelRetiredComputes` exceeds half of its
      `topLevelProcessLiteCalls` over the whole run. Over the final 40 idle
      blocks only: B's `localRecursiveComputeCalls` is greater than 0, and
      A's is at most one tenth of B's. If B and C disagree, or either
      idle-window assertion fails on the implementation as written, stop and
      report the numbers; do not weaken any comparison or threshold.
      Check: passes; with the fresh raw-centre compare removed from Sheaf
      production it fails; with the group's retired branch calling
      `Parameter::ProcessSamplePhase1` instead of `ProcessLitePhase1` the
      idle-window assertion fails.
- [ ] 3.3 Run every frogg3rs app test binary against this Sheaf tree. Report
      failures; do not edit them.

## 4. Fingerprints

- [ ] 4.1 Do not refresh the `sources` fingerprints in
      `formal/requirements.json`. Report the gate's mismatch output; the lead
      refreshes them after the after-code audit reviews the three files.

## 5. After-code repairs (ruled by the lead 2026-11-12 from the after-code audit)

The audit reports are in the lead's scratchpad: `after/diff-vs-proposal.md`,
`after/attack-A.md`, `after/attack-B.md`, `after/repetition-coverage.md`. Read
the finding named in each task before you start it. The same rules as tasks
0–4 apply.

- [ ] 5.1 Held depth-slot spans (attack-A F1). Do not change behaviour. First
      determine, by a differential in scratch, whether the spm-96 quiescent
      skip alone (retirement off) already misses a write through a
      `TargetDepthSlots`/`CurrentDepthSlots` span held across samples; report
      that result. Then document on both non-const overloads that the span is
      valid for writing only until the next processed sample, because the
      `Wake()` happens when the span is handed out, and that a later write
      must reacquire the span. Remove the "every write" overclaim from the
      `writeEpoch_` and `retireFixedPointComputes` comments, naming the held
      span and direct `ProcessLite*` calls on a depth child as the uncovered
      writes.
- [ ] 5.2 Comments (diff-vs-proposal). `ComputeWouldChangeNothing` states that
      it is a sufficient condition, not an equivalence. Correct the Froggers
      test's "the one random input" comment and the comment above the slate's
      default constructor so that they name the noise seed.
- [ ] 5.3 Repetition (repetition-coverage). Make the local-only epoch bump in
      `ReplaceCachedKnobValue` and `RestoreLastRawKnobValues` one private
      helper. Put the depth-child head shared by `RecordComputeForRetirement`
      and `ComputeWouldChangeNothing` into one predicate. Define
      `kRetirementMaskBits` from `std::numeric_limits<std::uint64_t>::digits`.
      Above `FreshActiveRouteMasks`, add a comment stating that it must read
      exactly the metadata fields `ComputeAtDepth` reads. In the tests, build
      the event ladders (`Rig::Event`, `RetirementEvent`, `RestsAtZeroEvent`)
      as one table, parameterized only by what differs, and replace
      `SameState` with `CompareParameters`. Report each count found against
      each count changed. Show that every Rig-based test still passes, with
      the same counters as before the repair.
- [ ] 5.4 Source indices beyond the rig's two (attack-B F1). Give `Rig` a
      modulator-count constructor parameter; existing callers keep 2. Add a
      differential run at 64 modulators, interval 1, in attenuverter mode,
      with routes on sources 9 and 63. Flip `connected` and then `restsAtZero`
      on each of them after settling, without `Wake()`. Red controls:
      `if (sourceIx < 8)` in either mask, and `<< (sourceIx % 2)`; each must
      fail.
- [ ] 5.5 Skip-off combination (attack-B F2). Add the run in which the
      retiring group has `skipQuiescentParameters = false` and the full group
      has both features off, and the run in which both have the skip on and
      only the retiring group retires. Red control: attack-B's mutant that
      skips `ProcessLitePhase1` when the skip is off must fail.
- [ ] 5.6 Above 64 modulators (attack-B F3). On a 65-modulator rig at interval
      1, after 48000 settling samples, `topLevelRetiredComputes` is 0. Red
      control: `kRetirementMaskBits = 200` must fail.
- [ ] 5.7 Retired means not computed (attack-B F4). Amended 2026-11-12: the
      settled rig has no depth children, so the counter cannot move there.
      Instead, extend task 5.4's 64-modulator run, whose routes give
      parameters depth children, with 48000 samples of no writes after its
      last flag flip, then 160 more. Over those 160 only: the retirement-off
      rig's `localRecursiveComputeCalls` delta is greater than 0, and the
      retirement-on rig's is exactly 0. If the retiring rig's delta is not 0
      on the implementation as written, stop and report the numbers. Red
      control: attack-B's `counted_never_retires` mutant must fail this run.
- [ ] 5.8 Voices (attack-B F7). If `ParameterGroup` supports more than one
      voice, give `Rig` a voice-count parameter. Add a two-voice differential
      run in which the voices' depth smoothing differs, plus a
      `smoothTargetCenter` run. Red controls: `epoch_lite_voice0_only`, and
      dropping the smoothing key component. If the group has one voice only,
      report that, with the trace.
- [ ] 5.9 Lean (attack-B L1, L2). Restate the two theorems so that their
      pinned statements fix `step` and `frames` (for example by stating them
      over the explicit `fun` forms), and so that L2's two definition mutants,
      `frames` ignoring the step and `step` as the identity, fail the gate. Add
      a `contracts` entry tying them to `ComputeAtDepth`'s retirement branch,
      with a `boundary` stating that the partition is a ruled judgment checked
      by the native differentials and that the lite steps between Computes are
      not modelled. Fingerprints stay for the lead.

## 6. Repair-audit fixes (ruled by the lead 2026-11-12 from `after/repair-audit.md`)

Same rules as before. Read the finding named in each task.

- [ ] 6.1 Every source index (F1). In the 64-modulator run, replace the two
      fixed routes with a loop over every source index 0–63. For each index,
      put a route on that source, settle, flip `connected`, then flip
      `restsAtZero`, without `Wake()`, and remove the route. Red controls:
      `% 32`, `% 63` and `< 8` in each mask must fail.
- [ ] 6.2 Both voice skews (F2). Run the two-voice run in both directions.
      Red controls: the voice-0-only bump and the last-voice-only bump must
      both fail.
- [ ] 6.3 Top-level computes (F3). Add `topLevelComputeCalls` to
      `ParameterProcessingObserver`, incremented once per top-level
      `ComputeAtDepth` that is not retired. In the 64-modulator idle window,
      in Froggers' idle window, and in the settled seven-parameter test over
      its 160 retired samples, the retiring rig's delta is exactly 0 and the
      full rig's is greater than 0. If a retiring delta is not 0 on the
      implementation as written, stop and report. Red controls: the
      childless-compute mutant and the 24% throttle must fail in Sheaf.
- [ ] 6.4 Skip-off means unskipped (F4). In every run where a rig has
      `skipQuiescentParameters = false`, that rig's `topLevelQuiescentSkips`
      is 0 at the end of the run. Red control: the mutant that ignores the
      setting must fail.
- [ ] 6.5 Lean (F5). Write `_root_.Nat.repeat` in the theorems and their pins.
      Show that a `Sheaf.Nat.repeat` shadow now fails the build. `ComputeSplit`
      stays unpinned: it is the ruled judgment, and its comment already says
      so. Add "fields beyond key/carried/derived are not constrained" to the
      contracts entry's `boundary`.
- [ ] 6.6 Comments (F6, 5.1).
      - Put the held-span note on both non-const overloads.
      - Remove the sentence blaming spm-96; your own differential cleared it.
      - Remove direct `ProcessSample` from the uncovered-writes lists.
      - Say that direct `ProcessLite*` on a depth child affects only that
        child's `Phase1Quiescent()` and `Phase2WouldChangeNothing()`.
      - Change the test's "through every writer" to name the writers it
        actually drives.
      - Correct the Rig comment about the spm-96 tests.
- [ ] 6.7 Test repetition (§3). Make the three identical ladder methods one
      method taking the schedule. Make the retiring/full rig-pair
      construction (5 found) one helper, and the settle-then-count idiom (3
      found) one helper. Report each count found against each count changed,
      and show that the counters program's output is unchanged.
- [ ] 6.8 The `smoothTargetCenter` key component stays. Its impossibility
      rests on a caller convention (`SnapCurrentToTarget` after
      `ComputeAllParameters`), not on the data's origin. That is the same
      ruling as the `CollectNeutralChildren` bump. Task 5.8's red control for
      it is recorded as unmeetable. Edit nothing for this item.

Recorded 2026-11-12 after round 5: the lead accepts three deviations, each of
which strengthens a check.
- 6.1 routes each source's aliases under `% 32` and `% 63` alongside it. With
  a single route per source, an aliasing mutant cannot fail.
- 6.3 adds a 40000-sample idle window to the 64-modulator run, so that a
  periodic throttle cannot hide between windows.
- 6.3 adds a retiring rig with the skip off to the settled test. With the skip
  on, a settled parameter never reaches the retired branch.

## 7. Round-5 audit fixes (ruled by the lead 2026-11-12)

This round closes the change's test-strength loop. A later audit reports its
findings to the lead; they are not chased in this change.

- [ ] 7.1 Injective masks. Add one run on 64 modulators, at interval 1, in
      attenuverter mode, with a single parameter that has a route on every
      source 0–63 at once and every source's `connected` and `restsAtZero`
      true. Settle it. Then, for each source in turn, without `Wake()`: set
      that source's `connected` false, settle, restore it, settle; then do the
      same with its `restsAtZero`. Because every other source holds true, any
      mapping of sources to bits that is not injective leaves a shared bit
      unchanged and must diverge. Red controls: `sourceIx & ~size_t{1}` and
      `% 61`, each in each mask, must fail. Keep 6.1's run.
- [ ] 7.2 Phase-2 skips. Add `topLevelPhase2Skips` to
      `ParameterProcessingObserver`, counted where the phase-2 quiescent skip
      is taken. `RequireNoSkipsWhenSkipIsOff` also requires it to be 0. Red
      control: removing `config_.skipQuiescentParameters &&` from the phase-2
      skip condition must fail.
- [ ] 7.3 Comments and repetition.
      - Change the `retireFixedPointComputes` wording to "a write through a
        span held across two or more processed samples may be missed".
      - Make the `CurrentDepthSlots` note say that writes through it are
        currently re-seen through the lite step's epoch bump, and that it is
        documented conservatively.
      - Make the test's "Nothing is written after the loop" comment name the
        cached-knob overwrite of parameter 0 in `Rig::Sample`.
      - Make `Rig::RetirementEvent` use `RefreshedEvent` for its shared body.
