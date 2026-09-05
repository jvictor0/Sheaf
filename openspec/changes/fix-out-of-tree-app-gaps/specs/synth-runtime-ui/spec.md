# Delta — synth-runtime-ui

## ADDED Requirements

### Requirement: sru-59 — Chrome: deadline meter is labeled

THE runtime sidebar's deadline meter SHALL render its percentage with a
`CPU ` label so the figure is identifiable without prior knowledge.

#### Scenario: labeled rendering

- **WHEN** the sidebar renders the deadline meter at any value
- **THEN** the rendered text carries the `CPU ` prefix before the percentage

### Requirement: sru-60 — Components: caption placement before or after

`ControlStyle` SHALL carry a caption placement of Before or After, defaulting
to Before, and WHEN placement is After, THE builder SHALL emit the caption
label after its control within the same implicit row, with caption id
derivation and text sync identical in both placements.

#### Scenario: default placement unchanged

- **WHEN** a control declares a caption without a placement
- **THEN** the caption label precedes the control exactly as before this
  change

#### Scenario: trailing caption

- **WHEN** a control declares caption placement After
- **THEN** the caption label follows the control in the emitted node order,
  and its derived id and sync behavior match the leading form

### Requirement: sru-61 — Tooling: boundary check survives empty discovery on macOS bash

THE UI-boundary check script SHALL complete under macOS system bash 3.2.57
when any dynamically-built discovery array is empty, and SHALL evaluate its
discovery-floor diagnostics before any expansion of the discovered sets, so
an empty discovery reports the intended failure instead of aborting the
shell.

#### Scenario: empty discovery reaches the floor diagnostic

- **WHEN** the script runs under `/bin/bash` 3.2.57 and app-producer
  discovery yields zero headers
- **THEN** the script reports the discovery-floor failure through its own
  `fail` path and does not abort with an unbound-variable error

#### Scenario: populated run unchanged

- **WHEN** the script runs with normal discovery results
- **THEN** its checks and exit status are unchanged from before this change
