# Delta — `synth-runtime-ui`

## MODIFIED Requirements

### Requirement: sru-4 — Controllers page: list, state, and adding
WHEN the Controllers page is open, THE runtime library SHALL list every active and blacklisted controller record in instrument order, showing its name, hardware kind, and disposition. Active records SHALL also show each endpoint as online, offline, or unconfigured when its stored reference is empty; show the actual input and output choices as the present devices plus the stored reference when absent; preserve the existing low-level mapping editor; offer Rename and Delete; and offer a Layout combo (sru-60) and, when their persisted wizard id resolves in the current registry, Blacklist. Blacklisted records SHALL show their stored endpoint labels, expose Rename and Remove from blacklist, expose Configure only when their wizard id resolves in the current registry, and expose no live endpoint selectors or mapping editor. The page SHALL list currently available wizard candidates separately with Configure and Ignore actions, SHALL preserve the add ("+") action that creates a named active controller of a chosen kind seeded from that kind's default profile, and SHALL commit device selections and lifecycle actions through instrument editing and reconciliation rather than opening or closing handlers directly.

#### Scenario: Connection state is visible
- **WHEN** one active mapped controller is connected and another active controller's device is unplugged
- **THEN** the page shows the first online with its device names and the second offline

#### Scenario: Blacklisted record is visibly inert
- **WHEN** a blacklisted controller record is listed
- **THEN** its row shows a Blacklisted badge and its stored endpoint labels
- **AND** it exposes no mapping disclosure or live endpoint selectors

#### Scenario: Manual and legacy profiles retain generic editing
- **WHEN** an active record has no persisted wizard id
- **THEN** its Rename, Delete, endpoint-selection, and low-level mapping controls remain available
- **AND** the Layout combo is offered, defaulting to Custom
- **AND** Blacklist is not offered
- Check: `controllers_page_ui_tests.cpp: TestLayoutComboOffersLayoutNamesThenCustom`

#### Scenario: Unknown opaque wizard id remains recoverable
- **WHEN** a stored Active or Blacklisted record carries a well-formed wizard id that does not resolve in the current registry
- **THEN** the record remains visible and offers its non-wizard Rename and Delete or Remove-from-blacklist actions
- **AND** Blacklist and Configure are not offered
- **AND** an Active record's Layout combo is still offered, reading Custom since no registry descriptor matches its wizard id
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`,
  `TestLayoutComboReadsCustomForUnresolvedWizardId`

#### Scenario: Add controller
- **WHEN** the user presses "+", names the controller, and picks the twister kind
- **THEN** an active controller seeded with the default twister profile appears in the list and in the instrument configuration

#### Scenario: Device choice triggers reconnect
- **WHEN** the user assigns a present input device to an active controller
- **THEN** the preference is stored and reconciliation opens that device for the controller

#### Scenario: Ignore creates a visible blacklist row
- **WHEN** the user activates Ignore for an available controller
- **THEN** its available row is replaced by a persisted Blacklisted row
- **AND** neither endpoint is opened

#### Scenario: Remove from blacklist restores availability
- **WHEN** the user removes a blacklisted record while its recognized pair remains present and unclaimed
- **THEN** the inert record is removed
- **AND** the pair returns to Available controllers and restores the sidebar warning

#### Scenario: Rename rejects duplicates
- **WHEN** the user renames any active or blacklisted record to a name already used by another record
- **THEN** the rename is refused and the prior name remains

#### Scenario: Delete active profile closes hardware
- **WHEN** the user deletes an active connected controller
- **THEN** the record is removed through the instrument commit path
- **AND** reconciliation closes its endpoints and removes its processor/sender routing

#### Scenario: Blacklist retains a dormant wizard profile
- **WHEN** the user blacklists a wizard-associated active controller
- **THEN** its record changes to Blacklisted while retaining its prior profile as dormant reconfiguration seed data
- **AND** reconciliation closes both endpoints and the row exposes no active mapping or endpoint controls

## ADDED Requirements

### Requirement: sru-59 — Controllers page: app message and analog-action catalog
WHEN the Controllers page builds the message dropdown for a system-message or Generic-controller row, or the target combo for an analog row's app-action choice, THE runtime library SHALL offer the app's own catalog when the running app declares one — the library message kinds the catalog keeps, in order, followed by one entry per app action, in catalog order, for the row dropdown; the catalog's analog-ranged actions, in catalog order, for the analog target combo — and SHALL offer the unchanged library-only message list, and no analog-action target combo, when the app declares no catalog. An app-action row's identity SHALL be the pair of its action name and value, never its resolved index.

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

### Requirement: sru-60 — Controllers page: per-controller Layout combo
WHEN an active controller row is presented, THE runtime library SHALL offer a Layout combo whose options are the current wizard registry's descriptor display names, in registry order, followed by "Custom," SHALL set its current value to the option whose descriptor id equals the slot's stored `wizardId`, or "Custom" when none does. Choosing a named option SHALL generate that descriptor's profile from the slot's own name and endpoints, replace the slot's kind, config, and `wizardId`, commit the instrument, and save the runtime configuration, all as one action with no intermediate form. Choosing "Custom" SHALL clear the slot's `wizardId` and change nothing else. THE runtime library SHALL also clear a slot's `wizardId` whenever any mapping edit — a field commit, an add, a delete, a block edit, or a Launchpad variant change — is committed on that slot, so the combo reads "Custom" once the installed layout's generated config no longer matches what is mapped.

#### Scenario: Combo lists layouts then Custom
- **WHEN** the wizard registry holds two descriptors and the slot's `wizardId` matches neither
- **THEN** the Layout combo offers both descriptors' display names, in registry order, followed by Custom
- **AND** its current value is Custom
- Check: `controllers_page_ui_tests.cpp: TestLayoutComboOffersLayoutNamesThenCustom`

#### Scenario: Choosing a layout installs it in one action
- **WHEN** the user selects a named layout option
- **THEN** the slot's kind, config, and `wizardId` become that descriptor's generated result
- **AND** the instrument is committed and the runtime configuration is saved exactly once
- Check: `controllers_page_ui_tests.cpp: TestChoosingALayoutInstallsItsConfigAndSetsWizardId`

#### Scenario: Choosing Custom clears the wizard id only
- **WHEN** the user selects Custom on a slot with a stored `wizardId`
- **THEN** the slot's `wizardId` is cleared
- **AND** its stored config is unchanged, byte for byte
- Check: `controllers_page_ui_tests.cpp: TestChoosingCustomClearsWizardIdWithoutTouchingConfig`

#### Scenario: A mapping edit clears the installed wizard id
- **WHEN** a layout-installed slot's mapping is edited and committed
- **THEN** the slot's `wizardId` is cleared
- **AND** the Layout combo subsequently reads Custom
- Check: `controllers_page_ui_tests.cpp: TestChoosingALayoutInstallsItsConfigAndSetsWizardId`

#### Scenario: Any other slot-mutating edit clears the wizard id the same way
- **WHEN** a Launchpad slot's variant is changed
- **THEN** its `wizardId` is cleared, the same as a mapping field commit, add, delete, or block edit would clear it
- Check: `viewmodel_tests.cpp: SetLaunchpadVariantClearsWizardId`

### Requirement: sru-61 — Controllers page: the controller row fits the host
WHEN an active or blacklisted controller row is presented, THE runtime library SHALL lay out its header as two lines of 36 px: the first line holds the row's identity controls — disclosure, name at 200 px, the device's display name, status dots, the Layout combo, and, for a Launchpad slot, the Variant combo — and the second line holds its ports and lifecycle controls — MIDI in, MIDI out, a rename field captioned "Rename to", Rename, Delete, and Blacklist. THE runtime library SHALL keep every control's node id unchanged by the reflow, and every node of the Controllers page SHALL lie inside the surface's content bounds at any app width of at least the header's minimum width, 890 px.

#### Scenario: The page fits a 900-wide host with its widest rows
- **WHEN** the Controllers page lists a Twister, a Generic, a Launchpad, and a Blacklisted controller, each with device names as long as "Midi Fighter Twister (offline)", built at content bounds 900 by 620
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: A Launchpad row keeps its Variant combo on the identity line
- **WHEN** a Launchpad slot's row is built
- **THEN** the Variant combo sits on line one, to the right of the Layout combo, at the same line height
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: A blacklisted row lays out on the same two lines
- **WHEN** a blacklisted controller record is listed
- **THEN** its name, kind, and Blacklisted badge sit on line one and its two stored-endpoint labels and lifecycle controls sit on line two
- Check: `controllers_page_ui_tests.cpp: TestControllersSectionsNestThroughLibraryContainers`,
  `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: The page fits a 900-wide host in every open state
- **WHEN** the same page has the Generic row expanded with Encoders (a Turn and a Push row added), System Messages (a row added) and Analogs (a Gesture and an App action row added) open, the Launchpad row expanded with System Messages open, and the Twister row expanded with Encoders open
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds after each step, and the Generic system row's Message combo offers the 24 choices the app catalog supplies
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

### Requirement: sru-62 — Controllers page: device names, port captions and legend
WHEN the Controllers page builds an active row's device label or the add row's device selector, THE runtime library SHALL show the device display name (`MidiProfileKindDisplayName`: WRLD.Bldr, MF Twister, Launchpad, Generic) while the persisted config keeps the profile-kind token; the add row's selector SHALL be captioned "Device"; the endpoint selectors SHALL be captioned "MIDI in" and "MIDI out", and a blacklisted row's stored-endpoint labels SHALL read "MIDI in: " and "MIDI out: " ahead of the stored name and identifier; and the section heading SHALL carry one legend, ahead of the first controller row, showing a coloured dot in each of the three `EndpointStatusColor` colours before the words "online", "offline", and "not set".

#### Scenario: Row and add-selector labels use the display name; the persisted config keeps the token
- **WHEN** a Twister slot's row and the add row's Device selector are built
- **THEN** both read "MF Twister"
- **AND** the persisted config still writes "twister"
- Check: `instrument_tests.cpp: KindDisplayNameCoversEveryKind`, `controllers_page_ui_tests.cpp: TestControllerKindLabelsShowTheCombinedDisplayNames`

#### Scenario: The add row and the endpoint selectors read their new captions
- **WHEN** the Controllers page builds the add row and an active row's endpoint selectors
- **THEN** the add row's selector is captioned "Device" and the endpoint selectors are captioned "MIDI in" and "MIDI out"
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: A blacklisted row keeps its stored endpoint labels under the same wording
- **WHEN** a blacklisted record is listed
- **THEN** its two endpoint labels carry the stored device name and identifier under the "MIDI in: " / "MIDI out: " wording
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`

#### Scenario: A status legend precedes the first controller row
- **WHEN** the Controllers page lists at least one controller
- **THEN** a legend node naming all three endpoint statuses is present ahead of the first row, each word preceded by its status colour's dot
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`
