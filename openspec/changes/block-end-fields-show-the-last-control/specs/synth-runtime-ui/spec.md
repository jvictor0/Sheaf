# Delta — `synth-runtime-ui`

sru-28 is modified as promoted; no active change modifies it. Its body's
clause "SHALL expose the target grid-slot index and the existing
kind-specific signed physical x/y range with exclusive maxima" now says the
page exposes a grid block's ranges as their start and last while the block
model keeps each maximum exclusive. It gains one scenario, "A grid block's
fields show its last row and column". Every other clause and scenario is
carried forward word for word.

sru-67 is added; `shifted-encoder-turns` added sru-66.

sru-10 is not modified. It states the block model, whose exclusive ends this
change keeps. `shifted-encoder-turns` modifies sru-10 on the same branch, and
this change carries no sru-10 text, so the two changes archive in either
order without one dropping the other's clause.

O1 is ruled: a block's number and gesture fields are headed by field token
— NEW `Field::BlockStartNote`, NEW `Field::BlockEndNote` and NEW
`Field::BlockStartGesture` give a Note-addressed block and an analog block
their own headers, and the page's header-row rule is unchanged.

Every scenario below is backed by a named test or an operator step; none
carries a "not yet delivered" Check line.

## MODIFIED Requirements

### Requirement: sru-28 — Controllers page: grid buttons and blocks
WHEN a WRLD.Bldr or Launchpad controller's system mappings target button-grid messages, THE runtime UI SHALL present each exact press/release/feedback plus polyphonic-pressure pair as one user-visible grid button or maximal rectangular grid block, SHALL expose the target grid-slot index and the existing kind-specific signed physical x/y coordinates, a grid block's as the start and last of each of its ranges (sru-67) while the block model keeps each range's maximum exclusive, SHALL map physical `(x,y)` directly to the same logical grid coordinate, SHALL always derive press, release, pressure-change, and feedback mappings together on commit, SHALL offer no toggle or aftertouch controls, and SHALL preserve unknown pressure mappings invisibly and losslessly through open-section editing and persistence.

#### Scenario: Grid block expands all input behavior
- **WHEN** the user commits a Launchpad grid block for slot `1` over `[0,8) x [-1,7)`
- **THEN** the profile receives one system association per coordinate with paired `GridPress` and `GridRelease` and grid feedback
- **AND** receives one matching derived polyphonic-pressure mapping per coordinate
- **AND** every range maximum is treated as exclusive

#### Scenario: Single grid button hides MIDI mechanics
- **WHEN** one exact grid mapping is presented individually
- **THEN** the row identifies it as one grid button with controller address and target grid slot
- **AND** it exposes no toggle, note number, MIDI status, aftertouch, or pressure-mapping row

#### Scenario: Exact mappings reconstruct to one block
- **WHEN** canonical system and pressure config contains a uniform rectangular run of matching grid mappings
- **THEN** collapsing and reopening the section reconstructs the maximal grid block
- **AND** expanding that block reproduces the canonical underlying mappings exactly

#### Scenario: Negative coordinates round-trip
- **WHEN** a grid block includes y `-1` and has exclusive maximum y `7`
- **THEN** its row, edit session, expansion, JSON persistence, and reconstruction preserve the signed coordinates

#### Scenario: Grid mappings are always momentary
- **WHEN** a grid button or block is added or edited
- **THEN** the view model generates press and release messages as a pair
- **AND** offers no toggle variant

#### Scenario: Orphan pressure data is preserved but hidden
- **WHEN** externally authored profile config contains a pressure mapping that does not exactly pair with a grid system association
- **THEN** the Controllers page shows no aftertouch line item for it
- **AND** opening, editing an unrelated row, committing, and saving preserves that mapping unchanged

#### Scenario: Pair edit is atomic
- **WHEN** a grid block edit would create an invalid controller address, duplicate address, invalid signed rectangle, or unrepresentable logical target
- **THEN** the commit is refused with a reason
- **AND** neither its system associations nor pressure mappings change

#### Scenario: A grid block's fields show its last row and column
- **WHEN** a Launchpad grid block over `[0,8) x [-1,7)` is shown on the Controllers page
- **THEN** its x and y fields read start `0`, last `7`, start `-1` and last `6`
- **AND** they are headed "Start X", "Last X", "Start Y" and "Last Y"
- Check: `viewmodel_tests.cpp` `BlockEndFieldsShowTheLastControlTheyCover` and `BlockFieldHeadersSayWhatTheFieldHolds`

## ADDED Requirements

### Requirement: sru-67 — Controllers page: a block's end fields show the last control it covers
WHEN the Controllers page shows a block row, THE runtime UI SHALL show in every field that names the end of one of the block's ranges the last control the block covers on that range, and SHALL read a value typed into such a field as that last control; the block model keeps its exclusive ends (sru-10), and the page SHALL translate between the two through one definition the block model provides. On a range that runs upward the last control SHALL be the one before the exclusive end; on a row range that runs downward it SHALL be the row after the exclusive end; a single row's last row SHALL be its start row. A last y typed above the start y SHALL make the rows run upward to it, one typed below SHALL make them run downward to it, and one equal to the start y SHALL give a single row; editing a block's start y SHALL keep the last y the page shows. A CC or note end field SHALL accept an integer 0 to 127, and an x or y end field an integer from -2147483647 to 2147483646, one range for both axes that keeps a y end's either-direction exclusive end inside what an `int` holds and that an x end, whose exclusive end only ever adds one, shares for consistency, with the controller's grid checked by expansion as for a start field. A last below its start on a range that only runs upward SHALL be kept in the open section and refused on commit (sru-11), with a reason that names the start and the last as the page heads them. A block's number fields SHALL be headed "Start CC" and "Last CC" on a CC-addressed block and "Start Note" and "Last Note" on a Note-addressed encoder-push or generic system block, an x/y block's end fields "Last X" and "Last Y", a grid block's x and y fields "Start X", "Last X", "Start Y" and "Last Y", and an analog block's first gesture "Start Gesture"; every block field's header SHALL say what the field sets on every row it heads.

#### Scenario: A sixteen-turn run shows and keeps its last CC
- **WHEN** a config maps ccs 0..15 on one channel to slot 0 positions 0..15 as turns and its encoder section is open
- **THEN** the turn block's end field reads `15` under the header "Last CC"
- **AND** typing `15` into it commits sixteen turns on ccs 0..15
- Check: `viewmodel_tests.cpp` `TypingABlocksLastControlKeepsEveryControl`; operator step: the Delivery Gate screenshot of a Twister row's encoder section, its block reading its last CC under 'Start CC'/'Last CC'

#### Scenario: Every block form shows its last control
- **WHEN** an analog gesture block on ccs 20..23, a generic scene-select block on ccs 40..47, a WRLD.Bldr scene-select block over x 0..7 on row 6, and a grid block over `[0,2) x [0,1)` are shown
- **THEN** their end fields read `23`, `47`, last x `7` and last y `6`, and last x `1` and last y `0`
- Check: `viewmodel_tests.cpp` `BlockEndFieldsShowTheLastControlTheyCover`

#### Scenario: A rectangle whose rows run downward shows its last row
- **WHEN** the default WRLD.Bldr bank-select block (banks 0..7 on row 3, banks 8..15 on row 2) is shown
- **THEN** its y fields read start `3` and last `2`
- Check: `viewmodel_tests.cpp` `BlockEndFieldsShowTheLastControlTheyCover`

#### Scenario: A typed last y sets the direction from the start
- **WHEN** a WRLD.Bldr row holding only a scene-select block two columns wide that starts at row 3 has its last y set to `5`
- **THEN** the block covers rows 3, 4 and 5 in that order
- Check: `viewmodel_tests.cpp` `LastYRunsTowardTheTypedRowFromTheStart`

#### Scenario: Editing the start y keeps the last y
- **WHEN** the default WRLD.Bldr row's bank-select block, with start y `3` and last y `2`, has its start y set to `1`
- **THEN** its last y still reads `2`
- Check: `viewmodel_tests.cpp` `EditingAStartYKeepsTheShownLastY`

#### Scenario: An end field accepts only a control a block can end on
- **WHEN** `127` and then `128` are typed into a CC end field, and `2147483647` into an x end field
- **THEN** `127` is accepted
- **AND** `128` and `2147483647` are refused with a reason that names the field and the range it accepts, and the open section is unchanged
- Check: `viewmodel_tests.cpp` `EndFieldsRefuseValuesNoBlockCanEndOn`

#### Scenario: A last below its start is kept and refused by name
- **WHEN** a CC end field is given a value below its block's start cc
- **THEN** the open section shows the typed last
- **AND** the commit is refused with a reason that names the last cc and the start cc
- **AND** on a Note-addressed block the reason names the last note and the start note
- Check: `viewmodel_tests.cpp` `BlockEndRefusalsNameStartAndLast`

#### Scenario: Headers say what the fields hold
- **WHEN** a turn block, a WRLD.Bldr scene-select block and a grid block are shown
- **THEN** their end columns are headed "Last CC", "Last X" and "Last Y"
- **AND** the grid block's x and y columns are headed "Start X", "Last X", "Start Y" and "Last Y"
- Check: `viewmodel_tests.cpp` `BlockFieldHeadersSayWhatTheFieldHolds`; operator step: the Delivery Gate screenshot of a Twister row's encoder section, its block reading its last CC under 'Start CC'/'Last CC'

#### Scenario: Note and analog blocks head their fields by what they set
- **WHEN** a Note-addressed encoder-push block and a Note-addressed generic scene-select block are shown, and an analog gesture block is shown
- **THEN** the push and scene-select blocks' number columns are headed "Start Note" and "Last Note"
- **AND** the analog block's first-gesture column is headed "Start Gesture"
- Check: `viewmodel_tests.cpp` `BlockFieldHeadersSayWhatTheFieldHolds`; operator step: the Delivery Gate screenshot of a Twister row's encoder section, its block reading its last CC under 'Start CC'/'Last CC'

#### Scenario: An end far off the grid is refused at once
- **WHEN** a Launchpad grid block's last y is typed `1024`, or a WRLD.Bldr
  system block's last x is typed `1073741824`
- **THEN** the commit is refused at once with the off-grid reason
- **AND** neither an exception nor a multi-second stall occurs
- Check: operator step: task 10's fixed-code timing run, recorded in the proposal's Evidence, refuses a WRLD.Bldr system block's last x `1073741824` and a Launchpad grid block's last y `1024` (and last x `1073741824`), each at once, in 0.0 ms, with the off-grid reason; the base timing run alongside it shows the stall and the exception the corner check removes
