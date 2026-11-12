# Delta — `synth-parameter-modulation`

## ADDED Requirements

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
