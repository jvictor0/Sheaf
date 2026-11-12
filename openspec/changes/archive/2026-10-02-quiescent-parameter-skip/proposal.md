# Proposal — `quiescent-parameter-skip`

## Why

A `simpleperf` profile of frogg3rs's audio thread on a Galaxy S20 FE, at idle,
puts about 68% of it in this library's per-sample parameter code. Every sample,
`ParameterGroup::ProcessSamplePhase1/2` run the lite step of all 91 top-level
parameters: every smoother steps, `GetRaw()` recomputes the knob value, and the
UI display centre and spread (which drive `EncoderDraw`'s motion blur) step.
With the default patch, 85 of the 91 have no active route and nothing moving:
their lite step changes nothing, 48,000 times a second.

## What changes

Measured first (frogg3rs default patch, idle, real engine): after one second,
96.7% of top-level lite steps leave every value they touch bit-identical, about
88 of the 91 parameters every sample.

- A top-level parameter becomes **quiescent** when, with no route active, a
  full lite step (phase 1 and phase 2) left every value it touches
  bit-identical, compared bitwise (`std::bit_cast`, so NaN and signed zero
  compare exactly): `currentCenter_`, every voice's current scale, offset,
  min and max, and every voice's UI display centre and spread. Both steps are
  pure functions of that state, the targets, the group's alphas and the cached
  knob value phase 2 reads, so an identical next step changes nothing.
- While quiescent, the **group's** per-sample pass skips the parameter's lite
  steps, with two exceptions that keep it exact:
  - phase 1 still restores each voice's cached knob value to the raw value the
    parameter last computed, because an application may overwrite that cache
    after phase 1 (`ReplaceCachedKnobValue`, frogg3rs's fuego stage) and reads
    it again after phase 1 next sample;
  - phase 2 runs again whenever the cached knob value it reads differs,
    bitwise, from the one its last no-change step read; any phase 2 step that
    changes a value ends quiescence.
- `Compute()` keeps running on its `targetComputeIntervalSamples` schedule for
  every parameter, quiescent or not: it is the only source of target changes
  and has side effects of its own (depth children, gesture visits, route
  pruning). Every `ComputeAtDepth` call on a parameter, at any depth, **wakes**
  it, so a quiescent parameter still runs one full lite step per compute
  interval (about 1 in 16 samples) and a depth snap at recursion > 0 is never
  missed.
- Other writers of a value the lite step reads also wake the parameter:
  `SnapCurrentToTarget`, `SeedCachedKnobAndUiDisplayState`, `RevertToDefault`,
  `RevertAllToDefault`, `ResetLocalForReuse`, `ResetModulationDepthToNeutral`,
  `EnsureRouteActive`, `RemoveActiveRoute`, `PruneNeutralActiveRoutes`, the
  non-const `CurrentDepthSlots` and `TargetDepthSlots`, construction, and
  `ParameterGroup::ConfigureProcessingTiming` (every parameter of the group).
- New per-voice storage, in the group's arenas like every other per-voice
  value (`ParameterStorageBatch`, `MakeParameterStorageBatch`, both `Parameter`
  constructors): the raw value last computed, and the knob value phase 2 last
  read without changing anything.
- `Parameter::ProcessSamplePhase1/2` called directly keep their behaviour; the
  skip lives only in `ParameterGroup::ProcessSamplePhase1/2`.
- Observer: `topLevelProcessLiteCalls` keeps counting every visit, as today; a
  new `topLevelQuiescentSkips` counts skipped visits.
- `ParameterGroupConfig::skipQuiescentParameters` (default true) lets a test run
  the unskipped path beside the skipped one.

## Impact

`include/synth/ParameterModulation.hpp`, `src/ParameterModulation.cpp`,
`tests/parameter_modulation_tests.cpp`. Textual overlap, no semantic one, with
the active change `app-commands-on-the-bus` in the storage-batch code.

## Delivery

Branch `quiescent-parameter-skip`, from the tip of
`phone-width-composition-and-record-permission` (jvictor0/Sheaf#25), pushed to
the fork and opened as the next sequential pull request; frogg3rs pins it after
measuring it on the phone.
