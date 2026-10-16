# Delta — `synth-runtime-ui`

sru-10 is modified as promoted. Its body gains one clause, that a turn
carrying a shifted job never joins a run, and one scenario, "A shifted turn
ends its run"; every other clause and scenario is carried forward word for
word.

sru-15 here is "Controllers page: system message kind and argument editors",
modified as it stands in the active change `shift-and-file-export`
(jvictor0/Sheaf#14), which gave it the Shift field. It is not the promoted
sru-15, "Controllers page: DOM-friendly semantic presentation", which
`fold-controller-wizard-into-add-row` modifies: two requirements carry the
id. Its body makes the Shift field conditional on the row dropdown offering
`Shift` and adds the sentence for when it does not; its "still fits" Check
drops a task number; it gains one scenario, "An app that cannot map Shift
shows no Shift field". Every other clause and scenario is carried forward
word for word.

sru-66 is added; `fold-controller-wizard-into-add-row` (jvictor0/Sheaf#19)
added sru-64 and sru-65.

## MODIFIED Requirements

### Requirement: sru-10 — Controllers page: block model
WHEN a section is expanded, THE runtime library SHALL reconstruct, via pure JUCE-free functions, a block presentation of the sorted config that is minimal under the reconstruction's canonical traversal (x ascending within a row, row direction ±1 in y, row-major): encoder blocks (channel, start cc, end cc exclusive, slot, start position) for maximal runs (length ≥ 2) of constant slot/channel with consecutive positions and ccs at constant offset, where a turn that carries a shifted job (sru-66) never joins a run and presents as an individual row, separately for turns and pushes; analog gesture blocks (channel, start cc, end cc exclusive, start gesture) analogously; system blocks only for scene-select, bank-select, and gesture-select messages — presented per the kind's address form (generic: channel + cc run; wrldbldr: channel + exclusive-end x/y rectangle whose end row may be above or below its start row; launchpad: the same rectangle form without channel; twister: never blocked) — for maximal runs of the same type (and same bank slot for bank-select) with consecutive arguments, consistent release pattern (paired set-false for gesture-select, absent otherwise), feedback equal to press on every cell, and a constant output-feedback flag, fitting greedy rectangles of ≥ 2 cells with the y direction fixed by the run's second row; a block SHALL expand to exactly its equivalent individual configs (argument = start + cell index in traversal order; wrldbldr cells derive control cc from position with the block channel authoritative; feedback = press; output-feedback = the block's flag), and reconstruction composed with expansion SHALL round-trip: expanding a reconstructed presentation reproduces the sorted config exactly for every config (duplicate addresses or duplicate messages simply fail the run checks and pass through as individual rows), and reconstructing the expansion of any block reconstruction can itself produce yields that block back; mappings fitting no block — including all non-blockable message types and scene blend — SHALL present as individual rows.

#### Scenario: Encoder run reconstructs to one block
- **WHEN** a config maps ccs 0..15 on one channel to slot 0 positions 0..15 as turns
- **THEN** the encoders presentation contains one turn block (channel, cc 0..16 exclusive, slot 0, start position 0)

#### Scenario: Bank rectangle reconstructs on WRLD.Bldr
- **WHEN** the default WRLD.Bldr profile's bank selectors (banks 0..7 on row y=3, banks 8..15 on row y=2, feedback = press throughout) are reconstructed
- **THEN** the system presentation contains one bank-select block spanning x 0..7 from start row 3 to end row 2 with start argument 0

#### Scenario: Broken run splits minimally
- **WHEN** one cell in the middle of an otherwise uniform scene-select run has a non-consecutive argument
- **THEN** reconstruction emits blocks/rows covering each maximal uniform piece and an individual row for the outlier

#### Scenario: Non-blockable messages stay individual
- **WHEN** a config contains reset/random modifier associations and a scene blend assignment
- **THEN** they present as individual rows regardless of adjacency

#### Scenario: Expansion round-trips
- **WHEN** any reconstructed presentation's blocks are expanded and merged with its individual rows
- **THEN** the result equals the sorted input config exactly

#### Scenario: Block commit is all-or-nothing
- **WHEN** a block edit produces an expansion where any cell fails validation (address out of shape, argument out of domain, duplicate address)
- **THEN** the commit is refused with a reason and the config is unchanged

#### Scenario: A shifted turn ends its run
- **WHEN** a config maps ccs 0..15 on one channel to slot 0 positions 0..15 as turns and the turn at position 15 carries a shifted job
- **THEN** the encoders presentation contains one turn block (channel, cc 0..15 exclusive, slot 0, start position 0) and one individual turn row for cc 15
- Check: `blocks_tests.cpp: ReconstructEncoderBlocksKeepsAShiftedTurnOutOfItsBlock`

### Requirement: sru-15 — Controllers page: system message kind and argument editors
WHEN system-message rows or blocks are edited on the Controllers page, THE runtime library SHALL present the message kind as a compact choice independent from that message's semantic argument fields, so message-kind controls contain labels such as "Scene Select", "Bank Select", and "Gesture Select" rather than enumerated labels such as "Scene Select 3"; argument-bearing message kinds SHALL expose their arguments through separate numeric or structured fields that commit through the same edit-session flush path as other row fields. When the row dropdown offers `Shift`, every individual system-message row whose own kind is neither `Shift` nor `HoldDrill` SHALL expose a Shift field after its argument fields, headed "Shift", offering none and then every choice of the row dropdown whose kind takes no argument, excluding `Shift` and `HoldDrill`; committing it SHALL set or clear the row's shifted press through the same flush path, and its current value SHALL be read back by kind and, for an app action, by the shifted name/value pair. When the row dropdown does not offer `Shift`, no system-message row SHALL expose a Shift field, since no button on that page can be mapped to hold it.

<!-- RESTATES-EXCEPT
task 1.S4.4
  keeps: re-run with the column
-->

#### Scenario: Scene select separates kind from scene index
- **WHEN** a system-message row sends scene select for scene index 3
- **THEN** the row presents "Scene Select" as the message kind
- **AND** presents scene index `3` in a separate argument field
- **AND** no dropdown option for that row is labeled "Scene Select 3"

#### Scenario: Bank select separates kind from bank arguments
- **WHEN** a system-message row sends bank select for slot 0 bank 7
- **THEN** the row presents "Bank Select" as the message kind
- **AND** presents the slot and bank arguments in separate argument fields
- **AND** no dropdown option for that row is labeled "Bank Select 7"

#### Scenario: Gesture select separates kind from gesture index
- **WHEN** a system-message row sends gesture select for gesture index 4
- **THEN** the row presents "Gesture Select" as the message kind
- **AND** presents gesture index `4` in a separate argument field
- **AND** no dropdown option for that row is labeled "Gesture Select 4"

#### Scenario: Message argument edit preserves the open session
- **WHEN** the user changes a system-message argument field while the section stays expanded
- **THEN** the edit mutates the targeted session row and flushes the expanded persisted config
- **AND** the section does not re-coalesce until it is closed and reopened

#### Scenario: The Shift field sets and clears a shifted press
- **WHEN** a system-message row's Shift field is set to an app action and then to none
- **THEN** the row's persisted association first carries that shifted press with its name/value pair and then carries none
- **AND** the section stays open across both edits
- Check: `viewmodel_tests.cpp: ShiftFieldEditCommitsShiftedPressAndNoneClearsIt`

#### Scenario: Shift and Hold Drill rows have no Shift field
- **WHEN** a system-message row's own kind is Shift or Hold Drill
- **THEN** its editable fields contain no Shift field
- Check: `viewmodel_tests.cpp: SystemRowsExposeShiftFieldExceptOnShiftAndHoldDrillRows`

#### Scenario: The page still fits with the Shift column
- **WHEN** the Controllers page is built at 900 px wide with every kind's system section open and a row in each
- **THEN** every node lies inside the content bounds
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost` (re-run with the column)

#### Scenario: An app that cannot map Shift shows no Shift field
- **WHEN** the row dropdown is built from a catalog that does not offer Shift, and a system-message section with an Individual row is opened
- **THEN** no row's editable fields contain a Shift field
- Check: `viewmodel_tests.cpp: NoShiftFieldWhenTheRowDropdownOffersNoShift`

## ADDED Requirements

### Requirement: sru-66 — Controllers page: an encoder turn row's Shift field
WHEN the row dropdown offers `Shift`, every individual encoder-turn row on the Controllers page SHALL expose a Shift field after its address and target fields, headed "Shift", offering none and then every shifted job the library declares, in the order it declares them, which is Scene Blend and then BPM; committing it SHALL set or clear that turn's shifted job (smi-17) through the same edit-session flush path as the row's other fields, and its current value SHALL be read back from the turn's shifted job. A turn that carries a shifted job SHALL never be folded into an encoder block row, so its Shift field is on screen whenever its section is open; a block row SHALL expand to turns with no shifted job. Encoder push rows SHALL have no Shift field. When the row dropdown does not offer `Shift`, no encoder-turn row SHALL expose a Shift field.

#### Scenario: The turn Shift field sets and clears a shifted job
- **WHEN** an encoder-turn row's Shift field is set to Scene Blend and then to none
- **THEN** the row's persisted turn mapping first carries shifted job Scene blend and then carries none
- **AND** the section stays open across both edits
- Check: `viewmodel_tests.cpp: TurnShiftFieldEditCommitsSceneBlendAndNoneClearsIt`

#### Scenario: The turn Shift field offers BPM and commits it
- **WHEN** an encoder-turn row's Shift field is read and then set to BPM
- **THEN** its choices are none, Scene Blend and BPM, in that order
- **AND** the row's persisted turn mapping carries shifted job Tempo
- Check: `controllers_page_ui_tests.cpp: TestTurnRowShiftComboOffersBpmAndCommits`

#### Scenario: A shifted turn stays out of its block
- **WHEN** sixteen contiguous turns are reconstructed and the last one carries a shifted job
- **THEN** the first fifteen form one block row and the last is an Individual row
- Check: `blocks_tests.cpp: ReconstructEncoderBlocksKeepsAShiftedTurnOutOfItsBlock`

#### Scenario: Only turn rows get the field, and only when Shift is offered
- **WHEN** an encoder section with an Individual turn row and an Individual push row is opened with a dropdown that offers Shift, and again with one that does not
- **THEN** the turn row carries a Shift field in the first case only
- **AND** the push row carries none in either case
- Check: `viewmodel_tests.cpp: TurnRowsExposeShiftFieldOnlyWhenShiftIsOffered`

#### Scenario: The page still fits with a shifted turn row open
- **WHEN** the Controllers page is built at 900 px wide with an encoder section open that holds a turn row with a Shift field
- **THEN** every node lies inside the content bounds
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost` (re-run with a shifted turn row in its open states)
