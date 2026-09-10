# Delta — `synth-runtime-ui`

## ADDED Requirements

### Requirement: sru-launchpad-variant — Controllers page: the row chooses its Launchpad model

WHEN a controller row of the launchpad kind is listed, THE runtime library SHALL offer on that row a Variant selector holding every Launchpad model the library addresses, showing the model the row's profile records, and SHALL offer it on no other kind. Choosing a model SHALL rewrite every grid position on that row to the chosen model and commit through the instrument edit path; where an existing position lies outside the chosen model's grid shape the whole change SHALL be refused with a status naming the position, leaving the row as it was. A row with no mappings SHALL take the chosen model just the same, and rows and blocks added afterwards SHALL be seeded from the recorded model rather than from a position read out of the mappings.

#### Scenario: An empty custom launchpad row takes a model

- **WHEN** the operator adds a Custom (Launchpad) controller and chooses Launchpad Mini MK3 on its Variant selector
- **THEN** the row records Launchpad Mini MK3
- **AND** a row added to it afterwards carries a Launchpad Mini MK3 position

#### Scenario: A model with a smaller grid refuses a position it cannot hold

- **WHEN** a row recording Launchpad Pro MK3 carries a position in the column the Pro MK3 alone has, and the operator chooses Launchpad X
- **THEN** the change is refused, the status names that position, and the row still records Launchpad Pro MK3

#### Scenario: Only launchpad rows offer the selector

- **WHEN** a Twister row and a Generic row are listed beside a launchpad row
- **THEN** only the launchpad row shows a Variant selector
