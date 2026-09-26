# Delta — `synth-midi-instrument`

## ADDED Requirements

### Requirement: smi-launchpad-model — A launchpad profile records its model

WHEN a controller profile of the launchpad kind is stored or edited, THE synth system SHALL record on the profile itself which Launchpad model the row addresses, defaulting to Launchpad X, and SHALL treat that record as the answer to which model the row is for — including for a profile carrying no mappings at all, which has no position to be read off. The model SHALL persist with the profile; a profile read from JSON without it SHALL take the model of its first association carrying a grid position, or Launchpad X when it has none, so a record stored before the field existed keeps the model its pads already imply. The model SHALL be part of what distinguishes one profile from another, so a row pointed at another model no longer matches the preset that created it. Each association SHALL keep carrying its own model, which is what the runtime routes on; the recorded model SHALL be what every newly added row, block and grid cell is stamped from, so the two agree by construction rather than by a validity rule.

#### Scenario: An empty profile still knows its model

- **WHEN** a launchpad profile carrying no associations records Launchpad Mini MK3
- **THEN** it reads back as a Launchpad Mini MK3 profile
- **AND** an association added to it carries a Launchpad Mini MK3 position

#### Scenario: A stored record keeps the model its pads imply

- **WHEN** an instrument stored before the model was recorded is read back, holding a launchpad profile whose associations carry Launchpad Pro MK3 positions
- **THEN** the profile reads back as a Launchpad Pro MK3 profile
- **AND** an empty launchpad profile stored the same way reads back as Launchpad X

#### Scenario: The model survives a round trip

- **WHEN** an instrument holding a Launchpad Mini MK3 profile is serialized and reloaded
- **THEN** the reloaded profile records Launchpad Mini MK3
