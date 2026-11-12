# Delta — `synth-parameter-modulation`

## ADDED Requirements

### Requirement: spm-97 — Processing: a fixed-point Compute is retired with identical results
WHEN, on a compute sample, a top-level parameter's last `Compute()` was a fixed point and nothing it reads has changed since — its fresh raw centre, its active routes' modulator `connected` and `restsAtZero` flags, its write epoch, and recursively the same for every depth child, each a local parameter with no active route — THE parameter group SHALL NOT run that `Compute()`, SHALL treat that sample as a non-compute sample for spm-96's skip, and SHALL produce, at every sample, every top-level and depth-child value bit-identical to the same group with retirement disabled.

#### Scenario: Retired and full groups agree under every writer
- **WHEN** two identically configured groups, one with `retireFixedPointComputes` true and one with both it and `skipQuiescentParameters` false, run the same sequence at compute intervals 1 and 16: settling, a scene blend move, a knob edit with no `Wake()`, a route added and returned to neutral, a modulator's `connected` and `restsAtZero` flags flipped and restored, a depth child given a route of its own whose source holds still and then moves, a local and a top-level parameter assigned as another's depth, a parameter's depths cleared, neutral children collected, a revert, a revert of all, a JSON load, a compute-and-snap, a depth reset to neutral, a depth-slot write, a change of processing timing, an application overwriting a top-level parameter's cached knob values after phase 1, and a local depth child's cached knob overwritten
- **THEN** every public const accessor of every top-level parameter and every depth child agrees bit for bit between the groups after every sample
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `fixed_point_compute_retirement_matches_full_compute`.

#### Scenario: Settled parameters stop computing
- **WHEN** a group at compute interval 1 has settled
- **THEN** the processing observer's `topLevelRetiredComputes` counts every top-level visit, and after a writer wakes one parameter the next sample retires every visit but that one
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `settled_parameters_retire_their_compute_until_written`.

#### Scenario: Froggers sounds the same with retirement
- **WHEN** two Froggers applications built with the same test launch salt, one with retirement disabled, settle, take six Randomize All presses, a knob turn, a scene blend sweep and a transport stop and start
- **THEN** every output sample and every top-level parameter's cached knob value agree bit for bit, two applications with retirement disabled also agree (the determinism control), and the retiring application retires more than half its compute visits
- Check: frogg3rs `app/FroggersParameterModelTests.cpp`, `froggers_compute_retirement_matches_full_compute`.

#### Scenario: The retirement law is proved and pinned
- **WHEN** the formal gate runs
- **THEN** `Sheaf.second_compute_fixed_point` and `Sheaf.retired_frames_match_full` check with only the allowed axioms and match their pinned statements
- Check: `make formal-test` (`scripts/check_parameter_model.py`).

## MODIFIED Requirements

### Requirement: spm-96 — Processing: a quiescent parameter skips its per-sample work with identical results
WHEN a top-level parameter's lite step leaves every value it holds unchanged with no route active, THE parameter group SHALL skip that parameter's per-sample smoothing, raw-value computation and UI display smoothing until a function that writes a value the lite step reads wakes it, SHALL keep restoring the cached knob value to the parameter's raw value each sample while skipping, and SHALL produce, at every sample, cached knob values, raw values, UI display centres and UI display spreads bit-identical to the same group with skipping disabled. A compute sample on which spm-97 retires the parameter's `Compute()` counts as a non-compute sample for this skip.

#### Scenario: Skipped and unskipped groups agree under every writer
- **WHEN** two identically configured groups, one with `skipQuiescentParameters` true and one false, run the same sequence for thousands of samples: settling, a scene blend move, a knob edit, a modulation route added and removed, a top-level parameter assigned as another's modulation depth, a revert to default, a revert of all parameters, a JSON load, a snap, a depth reset to neutral, a change of processing timing, and an application overwriting cached knob values after phase 1 with values that change and then hold
- **THEN** every top-level parameter's cached knob value, raw value, current center, current scale, current normalization offset, UI display centre and UI display spread are bit-identical between the groups after every sample (current minimums and maximums have no accessor; they feed the change test that ends quiescence)
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `quiescent_skip_matches_unskipped_processing`.

#### Scenario: Settled parameters stop doing per-sample work
- **WHEN** a group with no active routes has run long enough for its smoothers and display state to stop changing
- **THEN** with `retireFixedPointComputes` false, the processing observer's `topLevelQuiescentSkips` counts every visit to its quiescent parameters except one per compute interval; with it true, every visit; and `topLevelProcessLiteCalls` still counts every visit
- Check: `projects/synth/tests/parameter_modulation_tests.cpp`, `quiescent_parameters_skip_their_lite_step_until_woken`.
