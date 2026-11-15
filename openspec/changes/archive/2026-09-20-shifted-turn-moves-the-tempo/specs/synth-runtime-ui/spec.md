# Delta — `synth-runtime-ui`

sru-66 is modified as it stands in the active change `shifted-encoder-turns`
(jvictor0/Sheaf#20), which added it. Its body names BPM as a second choice
after Scene Blend and says the choices come from the library's own catalog of
shifted jobs, in the order that catalog declares them; one scenario is added.
Every other clause and scenario is carried forward word for word.

sru-10, which keeps a turn carrying a shifted job out of a block run, is
unchanged: it is written over "a shifted job", not over which one, so a
tempo-shifted turn already presents as its own row.

## MODIFIED Requirements

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
