# Delta — `synth-runtime-ui`

**Added 2026-08-19** (frogg3rs live-site smoke finding: browser sliders
carry no numeric value while the JUCE backend shows one for the same
node).

## ADDED Requirements

### Requirement: sru-59 — Slider value readout in every backend
WHEN a `Slider` node renders, THE backend SHALL present the node's
current numeric value as a visible readout alongside the control,
updating live during drags and on wire value changes, formatted from the
node's own range and step so that all backends present the same text for
the same node state. The readout SHALL render within the node's
wire-set bounds and SHALL NOT intercept pointer input intended for the
slider.

#### Scenario: Both backends show the same number
- **WHEN** the same Slider node state renders in the JUCE and browser
  backends
- **THEN** both present the same formatted value text

#### Scenario: The readout tracks a drag
- **WHEN** the user drags the slider in any backend
- **THEN** the readout updates continuously during the drag, not only
  after release or on the next frame
