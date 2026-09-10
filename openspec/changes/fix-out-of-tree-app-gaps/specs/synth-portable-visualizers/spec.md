# Delta — synth-portable-visualizers

## ADDED Requirements

### Requirement: spv-9 — Visualizers: ganged-random-LFO background opt-out

`GangedRandomLfoVisualizer` SHALL accept a construction-time background
opt-out; WHEN constructed with the background disabled, THE visualizer SHALL
emit no full-cell background fill and no midline while emitting voice traces
unchanged, and WHEN constructed without the parameter, THE visualizer SHALL
draw exactly today's background.

#### Scenario: default background unchanged

- **WHEN** the visualizer is constructed as existing callers construct it
- **THEN** its command stream is identical to the pre-change stream,
  including the background fill and midline

#### Scenario: opted out

- **WHEN** the visualizer is constructed with the background disabled
- **THEN** the command stream contains no background fill and no midline,
  and the voice traces are identical to the default stream's traces
