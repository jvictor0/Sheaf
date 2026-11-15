# Delta — `synth-runtime-ui`

## MODIFIED Requirements

### Requirement: sru-59 — Controllers page: app message and analog-action catalog
WHEN the Controllers page builds the message dropdown for a system-message or Generic-controller row, or the target combo for an analog row's app-action choice, THE runtime library SHALL offer the app's own catalog when the running app declares one — the library message kinds the catalog keeps, in order, followed by one entry per app action that declares no analog range, in catalog order, for the row dropdown; the catalog's analog-ranged actions, in catalog order, for the analog target combo — and SHALL offer the unchanged library-only message list, and no analog-action target combo, when the app declares no catalog. An app-action row's identity SHALL be the pair of its action name and value, never its resolved index. The `Shift` library kind, when the catalog keeps it, SHALL be offered the way Hold Drill is: a choice the runtime library produces rather than one read from the static catalog.

#### Scenario: App with no catalog sees the unchanged library list
- **WHEN** the running app declares no `MidiCatalog()`
- **THEN** the message dropdown offered on the Controllers page is exactly the fixed library list it has always been
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`

#### Scenario: App catalog choices are library kinds then app actions
- **WHEN** the running app declares a catalog naming some library kinds to keep and some app actions
- **THEN** the offered message list contains exactly those library kinds, in order, followed by the app's actions, in order
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`,
  `ViewModelOffersAppCatalogChoicesThroughMessageCatalog`

#### Scenario: App-action row identity survives a kind change
- **WHEN** a row is set to an app-action choice and then read back
- **THEN** its identity is its action name and value, not a stored index
- Check: `viewmodel_tests.cpp: SystemMessageRowFromAppActionChoiceRoundTripsRowIdentity`

#### Scenario: Only analog-ranged actions appear in the analog target combo
- **WHEN** the app's catalog contains both analog-ranged and non-analog actions
- **THEN** the analog row's target combo offers only the analog-ranged ones
- Check: `viewmodel_tests.cpp: MakeAnalogAppActionChoicesReturnsOnlyAnalogRangeActions`

#### Scenario: Empty analog-action catalog offers no analog app-action row
- **WHEN** the app's catalog has no analog-ranged actions
- **THEN** the analog section offers no app-action add row
- Check: `viewmodel_tests.cpp: EmptyAnalogActionCatalogOffersNoAppActionAddRow`

#### Scenario: Analog app-action row commits without touching gesture rows
- **WHEN** an analog app-action row is added and committed
- **THEN** it is written to `AnalogMidiInConfig::appActions`
- **AND** existing gesture mappings are unchanged
- Check: `viewmodel_tests.cpp: AddAndCommitAnalogAppActionRowWritesAppActionsWithoutTouchingGestures`

#### Scenario: Analog-ranged actions do not appear in the row dropdown
- **WHEN** the app's catalog contains both analog-ranged and non-analog actions
- **THEN** the row dropdown offers only the non-analog ones
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesSkipsAnalogRangedActions`

#### Scenario: Shift is offered when kept
- **WHEN** the app's catalog keeps the `Shift` library kind
- **THEN** the row dropdown offers a "Shift" choice in the kept-kinds run
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOffersShiftLikeHoldDrill`

### Requirement: sru-15 — Controllers page: system message kind and argument editors
WHEN system-message rows or blocks are edited on the Controllers page, THE runtime library SHALL present the message kind as a compact choice independent from that message's semantic argument fields, so message-kind controls contain labels such as "Scene Select", "Bank Select", and "Gesture Select" rather than enumerated labels such as "Scene Select 3"; argument-bearing message kinds SHALL expose their arguments through separate numeric or structured fields that commit through the same edit-session flush path as other row fields. Every individual system-message row whose own kind is neither `Shift` nor `HoldDrill` SHALL expose a Shift field after its argument fields, headed "Shift", offering none and then every choice of the row dropdown whose kind takes no argument, excluding `Shift` and `HoldDrill`; committing it SHALL set or clear the row's shifted press through the same flush path, and its current value SHALL be read back by kind and, for an app action, by the shifted name/value pair.

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
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost` (re-run with the column; task 1.S4.4)

### Requirement: sru-16 — Controllers page: shared system-message editing pipeline
WHEN system-message configuration is implemented for WRLD.Bldr, Launchpad, MF Twister, and Generic controller kinds, THE runtime library SHALL use one shared JUCE-free system-message row, block, coalescing, expansion, validation, and commit pipeline, with per-kind differences supplied by an address schema and address validators rather than by separate kind-specific editor implementations.

#### Scenario: Kinds share semantic message fields
- **WHEN** system-message rows are built for WRLD.Bldr, Launchpad, MF Twister, and Generic controllers
- **THEN** each kind uses the same message-kind and message-argument field definitions for scene select, bank select, and gesture select
- **AND** each kind uses the same Shift field definition
- **AND** only the address fields differ by kind

#### Scenario: Address schema is the variation point
- **WHEN** the shared system-message pipeline builds address fields
- **THEN** WRLD.Bldr receives channel/x/y fields
- **AND** Launchpad receives x/y fields
- **AND** MF Twister receives logical side-button fields
- **AND** Generic receives channel/cc fields

#### Scenario: One edit path commits every kind
- **WHEN** a system-message kind, argument, address, output-feedback, or block field is edited for any supported controller kind
- **THEN** the edit is validated through the shared system-message pipeline
- **AND** the accepted edit flushes through the same edit-session expansion and persisted-config normalization path

#### Scenario: Regression tests cover all kinds through one contract
- **WHEN** the synth test suite verifies system-message editor behavior
- **THEN** the tests exercise all supported controller kinds through common helper expectations for message kind fields, argument fields, add, delete, edit, and block support
- **AND** kind-specific assertions are limited to address field shape and validation
