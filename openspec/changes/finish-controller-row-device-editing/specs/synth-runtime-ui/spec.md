## ADDED Requirements

### Requirement: sru-63 — Controllers page: row device label, Custom entry, and connect-time messages

WHEN the Controllers page builds an active or blacklisted row's device label, THE runtime library SHALL show the display name of the descriptor its persisted wizard id resolves against in the current registry, or, when no wizard id resolves, the row's bound MIDI input's stored endpoint label, or the page's no-input placeholder when the row binds no input. WHEN the page builds the add row's Preset combo, THE runtime library SHALL offer the current registry's descriptors, in registry order, followed by exactly one Custom entry; choosing Custom SHALL add an active controller of the Generic kind, named after the Custom entry's own label, with no generated mapping and no bound endpoints. WHEN a row's expanded editor is open, THE runtime library SHALL show every connect-time SysEx message the row's config holds as editable hex text, regardless of the row's kind or whether its wizard id resolves; SHALL let the operator add an already-valid empty message and delete any stored message; and SHALL refuse an edit whose text is not whitespace-separated hex byte pairs forming one complete SysEx message (leading `0xF0`, trailing `0xF7`, every byte between them a data byte `0x00`-`0x7F`), leaving the stored message and every other row's config unchanged on refusal.

#### Scenario: A resolved row's label is the preset's device name

- **WHEN** an active row's persisted wizard id resolves against the current
  registry
- **THEN** its device label reads that descriptor's display name
- Check: `controllers_page_ui_tests.cpp: TestControllerDeviceLabelsIdentifyThePresetOrBoundInputDevice`, `TestControllerDeviceLabelForAppCatalogDeviceShowsItsName`

#### Scenario: An unresolved row's label is its bound input

- **WHEN** an active row has no persisted wizard id, or one that does not
  resolve against the current registry
- **THEN** its device label reads its bound MIDI input's stored endpoint label
- Check: `controllers_page_ui_tests.cpp: TestControllerDeviceLabelForUnresolvedWizardIdShowsBoundDevice`

#### Scenario: The add row offers the registry plus one Custom entry

- **WHEN** the add row's Preset combo is built
- **THEN** it lists the current registry's descriptors, in registry order,
  followed by exactly one entry labelled Custom
- **AND** choosing Custom and pressing Add installs an active Generic-kind
  controller with no generated mapping and no bound endpoints
- Check: `controllers_page_ui_tests.cpp: TestAddPresetDropdownListsRegistryDescriptorsThenOneCustomEntry`, `TestAddCustomGenericYieldsAnEmptyGenericRecord`

#### Scenario: A row's connect messages are shown and editable

- **WHEN** a row's expanded editor is open
- **THEN** every connect-time SysEx message it holds is shown as hex text,
  with Add and per-message Delete controls
- **AND** committing hex text that is not a complete, valid SysEx message
  refuses, leaving every stored connect message unchanged
- **AND** committing an entirely empty field refuses visibly (a status
  starting "Refused"), the same as any other malformed edit, rather than
  silently doing nothing
- Check: `controllers_page_ui_tests.cpp: TestConnectMessageShowsOnAnAbletonStyleRowsExpandedConfiguration`, `TestConnectMessageEditCommitsValidAndRefusesInvalidUnchanged`, `TestConnectMessageAddAndDelete`

#### Scenario: Committing to a later connect message leaves the earlier one alone

- **WHEN** a row holds two connect messages and a valid edit is committed to
  the second
- **THEN** the second message holds the edited bytes and the first is
  unchanged
- Check: `controllers_page_ui_tests.cpp: TestConnectMessageCommitAtIndexOneLeavesIndexZeroUnchanged`
