# Delta — `synth-runtime-ui`

## MODIFIED Requirements

### Requirement: sru-4 — Controllers page: list, state, and adding
WHEN the Controllers page is open, THE runtime library SHALL list every active and blacklisted controller record in instrument order, showing its name, hardware kind, and disposition. Active records SHALL also show each endpoint as online, offline, or unconfigured when its stored reference is empty; show the actual input and output choices as the present devices plus the stored reference when absent; preserve the existing low-level mapping editor; offer Delete on the header's ports line and, as the first row of the expanded editor, a Name field and Rename button; and offer a Preset combo (sru-60) and, when their persisted wizard id resolves in the current registry, Blacklist. A rename SHALL keep the row's expanded editor and its open sections open under the renamed row's new name, and a controller re-added under a name a deleted record previously held SHALL start fully collapsed regardless of that prior record's state. Blacklisted records SHALL show their stored endpoint labels and expose Remove and, when their wizard id resolves in the current registry, Configure, but SHALL expose no Rename, no disclosure, and no live endpoint selectors or mapping editor. The page SHALL list currently available wizard candidates separately with Configure and Ignore actions, SHALL preserve the add row that creates a named active controller from the chosen Preset option — a wizard registry descriptor or a per-device-kind Custom entry — named after that preset or kind and seeded from its generated profile, or an empty profile of the chosen kind for Custom, and SHALL commit device selections and lifecycle actions through instrument editing and reconciliation rather than opening or closing handlers directly.

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
- **AND** the Preset combo is offered, defaulting to Custom
- **AND** Blacklist is not offered
- Check: `controllers_page_ui_tests.cpp: TestLayoutComboOffersLayoutNamesThenCustom`

#### Scenario: Unknown opaque wizard id remains recoverable
- **WHEN** a stored Active or Blacklisted record carries a well-formed wizard id that does not resolve in the current registry
- **THEN** the record remains visible; an Active record still offers Rename, in its expanded editor, and Delete, and a Blacklisted record still offers Remove
- **AND** Blacklist and Configure are not offered
- **AND** an Active record's Preset combo is still offered, reading Custom since no registry descriptor matches its wizard id
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`,
  `TestLayoutComboReadsCustomForUnresolvedWizardId`

#### Scenario: Add controller installs the chosen preset
- **WHEN** the user presses Add without changing the Preset combo
- **THEN** an active controller is added, named after and seeded from the combo's currently displayed preset
- **AND** its endpoints bind to a connected device matching that preset when one is available, and otherwise read "(none)"
- Check: `controllers_page_ui_tests.cpp: TestAddFromPresetWithNoDeviceInstallsTheDefaultPresetWithNoneEndpoints`,
  `TestAddFromPresetWithMatchingOnlinePairBindsBothEndpoints`

#### Scenario: Add Custom seeds an empty record of the chosen device kind
- **WHEN** the user selects a "Custom (<device display name>)" option and presses Add
- **THEN** an active controller of that kind is added, named after the kind's display name, with no generated mapping, no bound endpoints, and no wizard id
- Check: `controllers_page_ui_tests.cpp: TestAddCustomGenericYieldsAnEmptyGenericRecord`

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
- **WHEN** the user renames an active record to a name already used by another record
- **THEN** the rename is refused and the prior name remains

#### Scenario: Delete active profile closes hardware
- **WHEN** the user deletes an active connected controller
- **THEN** the record is removed through the instrument commit path
- **AND** reconciliation closes its endpoints and removes its processor/sender routing

#### Scenario: Blacklist retains a dormant wizard profile
- **WHEN** the user blacklists a wizard-associated active controller
- **THEN** its record changes to Blacklisted while retaining its prior profile as dormant reconfiguration seed data
- **AND** reconciliation closes both endpoints and the row exposes no active mapping or endpoint controls

#### Scenario: A rename keeps the expanded editor and its open sections open
- **WHEN** an active row is expanded, one of its sections is opened, and the record is renamed
- **THEN** the row stays expanded and that section stays open under the new name, its cached presentation carried over rather than rebuilt
- **AND** renaming a collapsed row leaves it collapsed
- Check: `viewmodel_tests.cpp: RenameOfExpandedRowKeepsSectionPresentationOpen`, `RenameOfCollapsedRowLeavesItCollapsed`;
  `juce/ControllersPageSimulationTests.cpp: RunControllerWizardParitySimulation`

#### Scenario: A same-name re-add after delete starts fully collapsed
- **WHEN** a controller is deleted while expanded with an open section, and a controller is later added under that same name
- **THEN** the re-added row starts fully collapsed, with every one of its sections collapsed, not inheriting the deleted record's open state
- Check: `viewmodel_tests.cpp: SameNameReaddAfterDeleteStartsFullyCollapsed`

#### Scenario: A header change rebuilds every binary that reaches it, including one built from two translation units
- **WHEN** `include/synth/ControllersPageUI.hpp` changes
- **THEN** building any of them rebuilds it from the changed source rather than reporting it up to date — `portable_ui_tests`, `controllers_page_ui_tests`, `runtime_main_component_tests`, `browser_audio_device_tests`, and `browser_runtime_contract_tests`
- **AND** `browser_runtime_contract_tests` rebuilds on a change to a header reached by EITHER of its two translation units. Both reach `ControllersPageUI.hpp`, so that header alone cannot distinguish them; a single compiler invocation over two sources writes one depfile recording only the last source, which would have dropped the test unit's own headers. `BrowserRuntimeAbi.cpp` therefore compiles to its own object with its own dependency list
- Check: `Makefile`: `DEPFLAGS := -MMD -MP` on the five test-binary rules and on the split
  `$(BUILD_DIR)/BrowserRuntimeAbi.o` rule, with `-include $(wildcard $(BUILD_DIR)/*.d)` at the bottom.
  Generated lists replace the hand-written ones, so this cannot drift. Proven by a two-leg positive control
  run once: touching `ControllersPageUI.hpp` rebuilds the binary where the committed Makefile reported it up
  to date, and touching `include/synth/browser/BrowserAppEntry.hpp` — reached by the test unit and not by the
  ABI unit — rebuilds it too.

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

### Requirement: sru-60 — Controllers page: per-controller Preset combo
WHEN an active controller row is presented, THE runtime library SHALL offer a Preset combo whose options are the current wizard registry's descriptor display names, in registry order, followed by "Custom," SHALL set its current value to the option whose descriptor id equals the slot's stored `wizardId`, or "Custom" when none does. Choosing a named option SHALL generate that descriptor's profile from the slot's own name and endpoints, replace the slot's kind, config, and `wizardId`, commit the instrument, and save the runtime configuration, all as one action with no intermediate form. Choosing "Custom" SHALL clear the slot's `wizardId` and change nothing else. THE runtime library SHALL also clear a slot's `wizardId` whenever any mapping edit — a field commit, an add, a delete, a block edit, or a Launchpad variant change — is committed on that slot, so the combo reads "Custom" once the installed layout's generated config no longer matches what is mapped.

#### Scenario: Combo lists layouts then Custom
- **WHEN** the wizard registry holds two descriptors and the slot's `wizardId` matches neither
- **THEN** the Preset combo offers both descriptors' display names, in registry order, followed by Custom
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
- **AND** the Preset combo subsequently reads Custom
- Check: `controllers_page_ui_tests.cpp: TestChoosingALayoutInstallsItsConfigAndSetsWizardId`

#### Scenario: Any other slot-mutating edit clears the wizard id the same way
- **WHEN** a Launchpad slot's variant is changed
- **THEN** its `wizardId` is cleared, the same as a mapping field commit, add, delete, or block edit would clear it
- Check: `viewmodel_tests.cpp: SetLaunchpadVariantClearsWizardId`

### Requirement: sru-61 — Controllers page: the controller row fits the host
WHEN an active or blacklisted controller row is presented, THE runtime library SHALL lay out its header as two lines of 36 px each. An active row's first line SHALL hold its identity controls — disclosure, name, the device's display name, the Preset combo, and, for a Launchpad slot, the Variant combo — and its second line SHALL hold a status dot immediately before each of the MIDI in and MIDI out combos, then Delete and, when its wizard id resolves, Blacklist. A blacklisted row's first line SHALL hold its name, kind, and Blacklisted badge, and its second line SHALL hold its two stored-endpoint labels followed by Configure, when its wizard id resolves, and Remove. THE runtime library SHALL keep every control's node id unchanged by the reflow, and every node of the Controllers page SHALL lie inside the surface's content bounds at any app width of at least the header's minimum width, 740 px.

#### Scenario: The page fits a 900-wide host with its widest rows
- **WHEN** the Controllers page lists a Twister, a Generic, a Launchpad, and a Blacklisted controller, each with device names as long as "Midi Fighter Twister (offline)", built at content bounds 900 by 620
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: The fits-within gate has a working positive control
- **WHEN** the same collapsed rows are built 140 px narrower than the header's 740 px minimum, at 600 px wide
- **THEN** `FitsWithinViolations` reports at least one violation, proving the gate can actually fail rather than always passing
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: A Launchpad row keeps its Variant combo on the identity line
- **WHEN** a Launchpad slot's row is built
- **THEN** the Variant combo sits on line one, to the right of the Preset combo, at the same line height
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: A blacklisted row lays out on its own two lines, with no disclosure or live editor
- **WHEN** a blacklisted controller record is listed
- **THEN** its name, kind, and Blacklisted badge sit on line one, and its two stored-endpoint labels, Configure (when its wizard id resolves), and Remove sit on line two
- **AND** it has no disclosure control and no live endpoint selectors
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`,
  `TestControllersSectionsNestThroughLibraryContainers`; `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: The page fits a 900-wide host in every open state
- **WHEN** the same page has the Generic row expanded with Encoders (a Turn and a Push row added), System Messages (a row added) and Analogs (a Gesture and an App action row added) open, the Launchpad row expanded with System Messages open, and the Twister row expanded with Encoders open
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds after each step, and the Generic system row's Message combo offers the 24 choices the app catalog supplies
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

### Requirement: sru-62 — Controllers page: device names, port captions and legend
WHEN the Controllers page builds an active row's device label or the add row's Preset selector, THE runtime library SHALL show the device display name (`MidiProfileKindDisplayName`: WRLD.Bldr, MF Twister, Launchpad, Generic) while the persisted config keeps the profile-kind token; the add row's selector SHALL be captioned "Preset," offering the wizard registry's descriptor display names, in registry order, followed by one "Custom (<device display name>)" entry per device kind; the endpoint selectors SHALL be captioned "MIDI in" and "MIDI out," each preceded on its row by a status dot the same width as the legend's own dot; a blacklisted row's stored-endpoint labels SHALL read "MIDI in: " and "MIDI out: " ahead of the stored name and identifier; and the section heading SHALL carry one legend, ahead of the first controller row, showing a coloured dot in each of the three `EndpointStatusColor` colours before the words "online", "offline", and "not set".

#### Scenario: An active row's kind label uses the display name; the persisted config keeps the token
- **WHEN** a Twister slot's row is built
- **THEN** its kind label reads "MF Twister"
- **AND** the persisted config still writes "twister"
- Check: `instrument_tests.cpp: KindDisplayNameCoversEveryKind`, `controllers_page_ui_tests.cpp: TestControllerKindLabelsShowTheCombinedDisplayNames`

#### Scenario: A Custom preset option names its device kind by display name
- **WHEN** the add row's Preset combo is built
- **THEN** it offers one "Custom (<device display name>)" entry per device kind, after the registry descriptors, in the fixed Generic, MF Twister, Launchpad, WRLD.Bldr order
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: The add row and the endpoint selectors read their captions
- **WHEN** the Controllers page builds the add row and an active row's endpoint selectors
- **THEN** the add row's selector is captioned "Preset" and the endpoint selectors are captioned "MIDI in" and "MIDI out"
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: Each port's status dot precedes its own combo
- **WHEN** an active row's ports line is built
- **THEN** a status dot sits immediately before each of the MIDI in and MIDI out combos, sized like the legend's own dot
- Check: `controllers_page_ui_tests.cpp: main`

#### Scenario: A blacklisted row keeps its stored endpoint labels under the same wording
- **WHEN** a blacklisted record is listed
- **THEN** its two endpoint labels carry the stored device name and identifier under the "MIDI in: " / "MIDI out: " wording
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`

#### Scenario: A status legend precedes the first controller row
- **WHEN** the Controllers page lists at least one controller
- **THEN** a legend node naming all three endpoint statuses is present ahead of the first row, each word preceded by its status colour's dot
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`
