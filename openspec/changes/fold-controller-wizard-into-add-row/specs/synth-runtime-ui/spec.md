# Delta — `synth-runtime-ui`

## MODIFIED Requirements

### Requirement: sru-4 — Controllers page: list, state, and adding
WHEN the Controllers page is open, THE runtime library SHALL list every active and blacklisted controller record in instrument order, showing its name, hardware kind, and disposition. Active records SHALL also show each endpoint as online, offline, or unconfigured when its stored reference is empty; show the actual input and output choices as the present devices plus the stored reference when absent; preserve the existing low-level mapping editor; offer Delete on the header's ports line and, as the first row of the expanded editor, a Name field and Rename button; and offer Restore (sru-60) when their persisted wizard id resolves in the current registry and their stored config no longer matches that preset's generated profile. A rename SHALL keep the row's expanded editor and its open sections open under the renamed row's new name, and a controller re-added under a name a deleted record previously held SHALL NOT inherit that prior record's expanded or open-section state. Blacklisted records SHALL show a Released badge and their stored endpoint labels and offer Delete, which removes the record, and SHALL expose no Rename, no disclosure, no live endpoint selectors or mapping editor, and no action that returns them to Active. The page SHALL offer no action that creates a blacklisted record, and SHALL open no configuration-wizard page, form, or chooser. Under the heading "Available controllers", the page SHALL show once each connected input and output pair that no record claims and whose port names match a preset's aliases, by its input and output port names and the display name of every preset that matches it, with no action of its own; SHALL show "No connected controller is waiting to be set up" when there is none; and SHALL list every other connected input and output that no record claims as "Other inputs: …" and "Other outputs: …". The page SHALL preserve the add row, which creates a named active controller from the chosen Preset option: a registry descriptor, named after that preset (with the smallest free suffix beginning with ` 2` when the name is taken), seeded from its generated profile, and with its endpoints bound to the first connected pair no record claims whose port names match that preset's own aliases, when one is present; or Custom (sru-63). WHEN the player has not chosen a Preset option, the add row SHALL show the first matching preset of the first device listed under "Available controllers", or the registry's first descriptor when that list is empty. The page SHALL commit device selections and lifecycle actions through instrument editing and reconciliation rather than opening or closing handlers directly.

#### Scenario: Connection state is visible
- **WHEN** one active mapped controller is connected and another active controller's device is unplugged
- **THEN** the page shows the first online with its device names and the second offline

#### Scenario: Manual and legacy profiles retain generic editing
- **WHEN** an active record has no persisted wizard id
- **THEN** its Rename, Delete, endpoint-selection, and low-level mapping controls remain available
- **AND** Restore is not offered, since it requires a resolved wizard id
- Check: `controllers_page_ui_tests.cpp: TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence`,
  `TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`

#### Scenario: Unknown opaque wizard id remains recoverable
- **WHEN** a stored Active or Blacklisted record carries a well-formed wizard id that does not resolve in the current registry
- **THEN** the record remains visible; an Active record still offers Rename, in its expanded editor, and Delete, and a Blacklisted record still offers Delete
- **AND** Restore is not offered
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`

#### Scenario: Connected devices waiting to be set up are status only
- **WHEN** a device pair whose port names match two presets is connected and no record claims it, and another connected input matches no preset
- **THEN** "Available controllers" lists the pair once, by its port names and both presets' display names, with no button
- **AND** the other input is listed as "Other inputs: <its name>"
- **AND** a recognized device with only its input connected is listed under "Other inputs", not as a waiting device
- **AND** with nothing waiting, the block reads "No connected controller is waiting to be set up"
- Check: `controllers_page_ui_tests.cpp: TestConnectedNotSetUpListsDevicesWithoutActions`

#### Scenario: The add row starts on a waiting device's preset
- **WHEN** the player has not chosen a Preset option and a recognized device pair is waiting under "Available controllers"
- **THEN** the add row's Preset shows that device's first matching preset
- **AND** pressing Add installs that preset bound to that device's ports
- **AND** once the player chooses a Preset option, the add row keeps showing the player's choice
- Check: `controllers_page_ui_tests.cpp: TestAddRowStartsOnTheFirstWaitingDevicesPreset`

#### Scenario: Add controller installs the chosen preset
- **WHEN** the user presses Add without changing the Preset combo
- **THEN** an active controller is added, named after and seeded from the combo's currently displayed preset
- **AND** its endpoints bind to an unclaimed connected pair whose port names match that preset's aliases when one is available, and otherwise read "(none)"
- **AND** that holds for every preset that matches the pair, including one discovery did not assign it to
- Check: `controllers_page_ui_tests.cpp: TestAddFromPresetWithNoDeviceInstallsTheDefaultPresetWithNoneEndpoints`,
  `TestAddFromPresetWithMatchingOnlinePairBindsBothEndpoints`, `TestAddBindsAConnectedDeviceForEveryPresetItMatches`

#### Scenario: Add Custom seeds an empty Generic record
- **WHEN** the user selects Custom and presses Add
- **THEN** an active Generic-kind controller is added, named after the Custom entry's label, with no generated mapping, no bound endpoints, and no wizard id
- Check: `controllers_page_ui_tests.cpp: TestAddCustomGenericYieldsAnEmptyGenericRecord`

#### Scenario: Device choice triggers reconnect
- **WHEN** the user assigns a present input device to an active controller
- **THEN** the preference is stored and reconciliation opens that device for the controller

#### Scenario: Rename rejects duplicates
- **WHEN** the user renames an active record to a name already used by another record
- **THEN** the rename is refused and the prior name remains

#### Scenario: Delete active profile closes hardware
- **WHEN** the user deletes an active connected controller
- **THEN** the record is removed through the instrument commit path
- **AND** reconciliation closes its endpoints and removes its processor/sender routing

#### Scenario: A rename keeps the expanded editor and its open sections open
- **WHEN** an active row is expanded, one of its sections is opened, and the record is renamed
- **THEN** the row stays expanded and that section stays open under the new name, its cached presentation carried over rather than rebuilt
- **AND** renaming a collapsed row leaves it collapsed
- Check: `viewmodel_tests.cpp: RenameOfExpandedRowKeepsSectionPresentationOpen`, `RenameOfCollapsedRowLeavesItCollapsed`

#### Scenario: A same-name re-add after delete does not inherit the deleted row's state
- **WHEN** a controller is deleted while expanded with an open section, and a controller is later added under that same name
- **THEN** the view model gives the re-added name a fresh, fully collapsed entry rather than the deleted record's open state
- **AND** the page then opens the added row as sru-65 describes
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

### Requirement: sru-60 — Controllers page: wizard identity persists; Restore reinstalls a diverged preset
WHEN an active controller row's mapping is edited, added, deleted, or block-edited, THE runtime library SHALL leave the slot's stored `wizardId` unchanged, so a row keeps the identity of the preset that created it regardless of any later edit. WHEN a row's persisted wizard id resolves in the current registry and the slot's stored config no longer serializes identically to that descriptor's freshly generated profile, THE runtime library SHALL offer Restore on the row; choosing it SHALL regenerate that descriptor's profile from the slot's own current name and endpoint references and replace the slot's kind and config, leaving the slot's name, endpoint references, `wizardId`, and disposition unchanged, and SHALL commit the instrument and save the runtime configuration as one action with no intermediate form. THE runtime library SHALL NOT offer Restore when the row has no persisted wizard id, when that id does not resolve in the current registry, or when the stored config already matches the resolved descriptor's generated profile.

#### Scenario: A mapping edit no longer clears the wizard id
- **WHEN** a preset-installed slot's mapping is edited and committed
- **THEN** the slot's `wizardId` is unchanged
- **AND** Restore becomes offered once the edited config no longer matches the preset's generated profile
- Check: `viewmodel_tests.cpp: ApplyMappingEditKeepsWizardIdProvenance`;
  `controllers_page_ui_tests.cpp: TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence`

#### Scenario: Restore is absent until the row actually diverges
- **WHEN** a preset-installed row's stored config still matches what its preset generates
- **THEN** Restore is not offered
- Check: `controllers_page_ui_tests.cpp: TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence`

#### Scenario: Restore reinstalls the preset without disturbing identity or endpoints
- **WHEN** the user presses Restore on a row whose config has diverged from its resolved preset
- **THEN** the slot's kind and config become that preset's freshly generated profile
- **AND** the row's name, both endpoint references, and disposition are unchanged
- **AND** Restore disappears from the row once it matches its preset again
- Check: `controllers_page_ui_tests.cpp: TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence`

#### Scenario: Restore requires a resolved preset
- **WHEN** a row has no persisted wizard id, or a wizard id that does not resolve in the current registry
- **THEN** Restore is not offered, regardless of how far the stored config would otherwise diverge from any descriptor
- Check: `controllers_page_ui_tests.cpp: TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence`

### Requirement: sru-2 — Sidebar: tabs and deadline readout
WHEN the sidebar renders, THE runtime library SHALL show Audio, Controllers, Sync, and File entries that open their pages in the content host, SHALL show a warning marker on the Controllers entry exactly while the cached scw-2 classification contains at least one available wizard candidate, and SHALL show a max-recent-deadline readout displaying the maximum audio callback load percentage over a rolling window of recent UI frames, updated on the UI timer. Candidate classification SHALL be recomputed from the cached device snapshot after a device-list change and after every successful instrument commit, including while the Controllers page is closed, but SHALL NOT force device enumeration or reconciliation when the device list is unchanged.

#### Scenario: Tabs open their pages
- **WHEN** the user activates the Controllers entry
- **THEN** the Controllers page opens in the content host

#### Scenario: Sync tab opens its page
- **WHEN** the user activates the Sync entry in JUCE or Chrome
- **THEN** the portable Sync page opens in the content host

#### Scenario: Deadline readout holds recent peaks
- **WHEN** a single audio callback spikes the load percentage and later callbacks are cheap
- **THEN** the readout continues to display the spike value until it leaves the rolling window

#### Scenario: Available controller shows warning
- **WHEN** the background MIDI refresh observes a present wizard-recognized pair whose endpoints are unclaimed
- **THEN** the Controllers sidebar entry shows a warning marker without requiring the Controllers page to be open

#### Scenario: Added controller clears warning
- **WHEN** the last available candidate is added
- **THEN** the warning marker clears on the next portable UI refresh

#### Scenario: A claimed recognized pair never warns
- **WHEN** a recognized present pair has either endpoint claimed by an active or blacklisted record
- **THEN** it is not an available candidate
- **AND** it does not contribute to the Controllers warning marker

#### Scenario: Successful lifecycle commits refresh cached classification
- **WHEN** adding, deleting, or restoring a controller successfully commits an instrument change
- **THEN** candidate availability and the warning marker are recomputed against the cached device snapshot without waiting for another device-list change

### Requirement: sru-5 — Controllers page: expandable config sections
WHEN a controller row's config section is used, THE runtime library SHALL provide an expandable config area, which starts collapsed for a row loaded from a configuration and starts open with every section it lists, exactly as sru-65 describes, for a row the player just added; that area contains collapsible submenus — encoders, system messages, and analogs/gestures — that match the area's own start state and are omitted entirely when the controller's kind does not support them; the submenus SHALL present the controller's mappings as the block presentation of sru-10/sru-11 (block rows for uniform runs, individual rows otherwise, config-level rows for encoder mode, turn step, and scene blend), SHALL edit them through the view model (encoder channel/CC to parameter slot and position; kind-schema system-message addresses to press/release message-ins per sru-8; analog channel/CC to gesture index and scene blend), SHALL expose scrollable content with distinct visible viewport bounds and content extent so every row remains reachable in JUCE and portable backends, and SHALL remain usable with multiple controllers each carrying dozens of mappings; committed edits apply through the live-edit rebuild path (smi-8).

#### Scenario: Config starts collapsed
- **WHEN** the Controllers page opens, so every row is one loaded from the persisted configuration
- **THEN** every controller's config section and submenus are collapsed

#### Scenario: Unsupported submenus are skipped
- **WHEN** a launchpad controller's config is expanded
- **THEN** no encoders or analogs submenu is shown
- **AND** the system messages submenu is shown

#### Scenario: Mapping lists scroll
- **WHEN** a controller has more mappings than fit in the visible page
- **THEN** the mapping lists expose a content extent larger than the visible viewport
- **AND** the page can scroll to every mapping row and keep it reachable and editable

#### Scenario: Committed edit reaches the hardware path
- **WHEN** the user edits a system-message association or a block row and commits it
- **THEN** the live instrument configuration is updated and the controller's processors are rebuilt

#### Scenario: Uniform runs present as blocks
- **WHEN** the default WRLD.Bldr controller's encoders submenu is expanded
- **THEN** the 16 turn mappings present as one turn block row and the 16 push mappings as one push block row rather than 32 individual rows

### Requirement: sru-6 — File page: patch commands
WHEN the File page is open, THE runtime library SHALL present the patch commands (new, save, save-as, load) in a polished page layout with a current-patch identity header, runtime patch-root/path context, and last command/browser status, replacing the former shell chrome row; Save with no current patch directory SHALL fall through to the in-app Save As flow; Save As and Load SHALL open the dedicated in-app patch-browser viewer rooted at the runtime-owned `patches/` directory rather than an operating-system file explorer.

#### Scenario: File page carries patch identity
- **WHEN** a patch is saved-as or loaded
- **THEN** the File page shows that patch's name and the command status in the page header/status area

#### Scenario: First save falls through
- **WHEN** the user presses Save before any patch directory exists
- **THEN** the in-app Save As flow opens instead of an error

#### Scenario: Patch browser stays under patches root
- **WHEN** the user browses, saves-as, or loads from the File page
- **THEN** every selectable or creatable patch directory is resolved under the runtime-owned `patches/` directory
- **AND** the UI exposes no arbitrary absolute filesystem picker

#### Scenario: File page layout remains intentional while idle
- **WHEN** no Save As or Load browser viewer is open
- **THEN** the File page still shows the patch identity header, command strip, and status/empty-state region without leaving a rough blank or cramped chooser area

### Requirement: sru-12 — Configuration pages: Back saves runtime configuration
WHEN the user dismisses the Audio or Sync page with Back, THE runtime library SHALL save the current runtime configuration before returning to the application view. Leaving the Controllers page by Back SHALL NOT save the runtime configuration, since every committed edit on that page is already saved (sru-64).

#### Scenario: Audio Back saves configuration
- **WHEN** the user changes the audio device selection and presses Back on the Audio page
- **THEN** the runtime saves a configuration document containing the current audio device state
- **AND** returns to the application view

#### Scenario: Sync Back saves configuration
- **WHEN** the user changes a clock/transport send or receive toggle or PPQN and presses Back on the Sync page
- **THEN** the runtime applies the sync configuration through its audio-safe handoff, saves it in runtime configuration, and returns to the application view

#### Scenario: File Back does not save runtime configuration
- **WHEN** the user presses Back on the File page
- **THEN** the runtime returns to the application view without writing runtime configuration solely because the File page was dismissed

### Requirement: sru-15 — Controllers page: DOM-friendly semantic presentation
WHEN the Controllers page is rendered, THE runtime UI layer SHALL derive a DOM-friendly semantic tree from the JUCE-free `MidiConfigViewModel`, preserving the page's existing controller list, endpoint selection, connection state display, add-controller flow, expandable config sections, mapping rows, block rows, add/delete affordances, validation/refusal status, focus-safe refresh, and commit-through-`EditInstrument` behavior; the JUCE backend SHALL render that semantic tree without reducing current functionality, SHALL NOT treat the current dense Controllers page look and feel as a visual-parity target, and MAY improve spacing, visual layout grouping, labels, and control presentation without relaxing sru-11's row/block presentation-stability requirements.

#### Scenario: Controller workflows are preserved
- **WHEN** the user adds a controller, selects input/output endpoints, expands sections, edits mapping fields, adds or deletes individual rows, adds or deletes block rows, or changes launchpad variant controls
- **THEN** each accepted action is applied through the existing `MidiConfigViewModel` edit APIs and committed through the runtime's instrument edit path
- **AND** refused actions show a status reason without mutating the live instrument

#### Scenario: Connection and rebuild refresh survives the refactor
- **WHEN** MIDI connection state changes, a patch load changes the instrument, or an out-of-band MIDI processor rebuild occurs
- **THEN** the Controllers page marks its semantic tree dirty and refreshes from the current instrument snapshot and connection state
- **AND** active text editing is not clobbered before the edit commits or focus is released

#### Scenario: Empty groups remain editable
- **WHEN** a controller section has no existing mappings in an addable group
- **THEN** the semantic tree still exposes the group's add affordance and, where supported, block-add affordance
- **AND** adding the first row creates any absent profile-config container needed by the view model

#### Scenario: JUCE renderer may improve presentation
- **WHEN** the JUCE backend renders the Controllers page semantic tree
- **THEN** it may replace the current dense component layout with clearer visual layout grouping, spacing, headers, and form controls rather than preserving the current appearance for its own sake
- **AND** every capability required by `sru-4` through `sru-11` remains available and testable

### Requirement: sru-59 — Controllers page: app message and analog-action catalog
WHEN the Controllers page builds the message dropdown for a system-message or Generic-controller row, or the target combo for an analog row's app-action choice, THE runtime library SHALL offer the app's own catalog when the running app declares one — the library message kinds the catalog keeps, in order, followed by one entry per app action, in catalog order, for the row dropdown; the catalog's analog-ranged actions, in catalog order, for the analog target combo — and SHALL offer the unchanged library-only message list, and no analog-action target combo, when the app declares no catalog. The row dropdown SHALL also list a system-message row's own stored kind, by name, when the application catalog lacks it, and SHALL show it as the row's current value; choosing any other kind replaces it. An app-action row's identity SHALL be the pair of its action name and value, never its resolved index.

#### Scenario: App with no catalog sees the unchanged library list
- **WHEN** the running app declares no `MidiCatalog()`
- **THEN** the message dropdown offered on the Controllers page is exactly the fixed library list it has always been
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`

#### Scenario: App catalog choices are library kinds then app actions
- **WHEN** the running app declares a catalog naming some library kinds to keep and some app actions
- **THEN** the offered message list contains exactly those library kinds, in order, followed by the app's actions, in order
- Check: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`,
  `ViewModelOffersAppCatalogChoicesThroughMessageCatalog`

#### Scenario: A row's own stored kind is offered when the catalog lacks it
- **WHEN** a System Messages row's stored kind is not among the application catalog's offered kinds
- **THEN** the row's message dropdown also lists that stored kind, by name, and shows it as the current value
- **AND** choosing any other kind replaces it
- Check: `controllers_page_ui_tests.cpp: TestSystemMessageRowShowsAStoredKindTheCatalogLacks`

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

### Requirement: sru-61 — Controllers page: the controller row fits the host
WHEN an active or blacklisted controller row is presented, THE runtime library SHALL lay out its header as two lines of 36 px each. An active row's first line SHALL hold its identity controls — disclosure, name, and the device's display name — and its second line SHALL hold a status dot immediately before each of the MIDI in and MIDI out combos, then Delete and, when its wizard id resolves and its stored config no longer matches that preset, Restore. A blacklisted row's first line SHALL hold its name, kind, and Released badge, and its second line SHALL hold its two stored-endpoint labels followed by Delete. THE runtime library SHALL keep every control's node id unchanged by the reflow, and every node of the Controllers page SHALL lie inside the surface's content bounds at any app width of at least the header's minimum width, 724 px.

#### Scenario: The page fits a 900-wide host with its widest rows
- **WHEN** the Controllers page lists a Twister, a Generic, a Launchpad, and a Blacklisted controller, each with device names as long as "Midi Fighter Twister (offline)", built at content bounds 900 by 620
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: The fits-within gate has a working positive control
- **WHEN** the same collapsed rows are built 124 px narrower than the header's 724 px minimum, at 600 px wide
- **THEN** `FitsWithinViolations` reports at least one violation, proving the gate can actually fail rather than always passing
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: A blacklisted row lays out on its own two lines, with no disclosure or live editor
- **WHEN** a blacklisted controller record is listed
- **THEN** its name, kind, and Released badge sit on line one, and its two stored-endpoint labels and Delete sit on line two
- **AND** it has no disclosure control and no live endpoint selectors
- Check: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`,
  `TestControllersSectionsNestThroughLibraryContainers`; `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

#### Scenario: The page fits a 900-wide host in every open state
- **WHEN** the same page has the Generic row expanded with Encoders (a Turn and a Push row added), System Messages (a row added) and Analogs (a Gesture and an App action row added) open, the Launchpad row expanded with System Messages open, and the Twister row expanded with Encoders open
- **THEN** every node's rectangle, folded over its ancestor chain, lies inside those bounds after each step, and the Generic system row's Message combo offers the 24 choices the app catalog supplies
- Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`

### Requirement: sru-62 — Controllers page: device names, port captions and legend
WHEN the Controllers page builds an active row's device label, THE runtime library SHALL show `ControllerDeviceLabel`: the resolved wizard descriptor's own display name (for example, "MIDI Fighter Twister") when the row's stored wizard id still resolves against the page's layouts, else the MIDI input device the row is bound to, while the persisted config keeps the profile-kind token; the add row's selector SHALL be captioned "Preset," offering the wizard registry's descriptor display names, in registry order, followed by exactly one Custom entry; the endpoint selectors SHALL be captioned "MIDI in" and "MIDI out," each preceded on its row by a status dot the same width as the legend's own dot; a blacklisted row's stored-endpoint labels SHALL read "MIDI in: " and "MIDI out: " ahead of the stored name and identifier; and the section heading SHALL carry one legend, ahead of the first controller row, showing a coloured dot in each of the three `EndpointStatusColor` colours before the words "online", "offline", and "not set".

#### Scenario: An active row's device label shows the resolved preset's display name; the persisted config keeps the token
- **WHEN** a Twister slot's row is built from a preset whose wizard id resolves
- **THEN** its device label reads "MIDI Fighter Twister"
- **AND** the persisted config still writes "twister"
- Check: `controllers_page_ui_tests.cpp: TestControllerDeviceLabelsIdentifyThePresetOrBoundInputDevice`, `instrument_tests.cpp: KindNameRoundTrip`

#### Scenario: The add row offers the registry plus one Custom entry
- **WHEN** the add row's Preset combo is built
- **THEN** it lists the current registry's descriptors, in registry order, followed by exactly one entry labelled Custom
- **AND** choosing Custom and pressing Add installs an active Generic-kind controller with no generated mapping and no bound endpoints
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

## ADDED Requirements

### Requirement: sru-64 — Controllers page: every committed edit is saved
WHEN an action on the Controllers page commits a changed instrument, THE runtime library SHALL save the runtime configuration as part of that same action, so the edit survives a reload however the player leaves the page. This covers adding a controller from a preset or as Custom, choosing an input or output port, choosing a Launchpad Variant, editing a mapping field, adding or deleting a mapping entry or block, adding, editing, or deleting a connect message, Rename, Delete, and Restore. WHEN the save fails, THE runtime library SHALL leave the committed edit in place and show the status that the runtime configuration could not be saved. Leaving the Controllers page by Back SHALL NOT save on the page's behalf, since its edits are already saved.

#### Scenario: Edits survive a reload without Back
- **WHEN** the player adds a preset, adds a Custom controller, chooses its input port, edits a mapping field, adds a mapping entry, adds, edits, and deletes a connect message, renames a row, restores a diverged row, and deletes a row, without pressing Back
- **THEN** after each of those actions the saved runtime configuration already holds its result
- Check: `browser_runtime_contract_tests.cpp: TestControllersPageSavesEachCommittedEdit`

#### Scenario: A failed save keeps the edit and says so
- **WHEN** a committed edit's save fails
- **THEN** the instrument keeps the edit
- **AND** the page shows the status that the runtime configuration could not be saved
- Check: `controllers_page_ui_tests.cpp: TestSaveFailureKeepsTheCommittedEditAndReportsIt`

#### Scenario: Back does not save for the Controllers page
- **WHEN** the runtime asks whether leaving a page by Back saves the configuration
- **THEN** it answers no for Controllers and yes for Audio and Sync
- Check: `contract_tests.cpp`, its `RuntimePageBackSavesConfiguration` assertions

### Requirement: sru-65 — Controllers page: a newly added row opens
WHEN the player adds a controller from the add row, from a preset or as Custom, THE runtime library SHALL show the new row expanded with every one of its sections open, and the row's disclosure and each section's toggle SHALL collapse them as they do for any row. A row that appears any other way, such as loading a configuration, SHALL start collapsed.

#### Scenario: An added preset shows its mappings
- **WHEN** the player adds a preset
- **THEN** the new row is expanded, every section it lists is open, and the preset's mapping entries are in the page's tree
- Check: `controllers_page_ui_tests.cpp: TestAddedRowOpensWithEverySectionOpen`

#### Scenario: The player can still collapse it
- **WHEN** the player presses the added row's disclosure, or one of its section toggles
- **THEN** the row collapses, or that section closes
- Check: `controllers_page_ui_tests.cpp: TestAddedRowOpensWithEverySectionOpen`

#### Scenario: An added Custom row opens too
- **WHEN** the player adds Custom
- **THEN** the new row is expanded with every section it lists open
- Check: `controllers_page_ui_tests.cpp: TestAddedRowOpensWithEverySectionOpen`

## REMOVED Requirements

### Requirement: sru-32 — Controllers page: three-click configuration wizard flow
**Reason**: The add row is the one way to set up a controller. It binds a connected device's ports and installs its preset through the same wizard machinery. The wizard page it duplicated was empty for application presets, and edits happen on the row.
**Migration**: Choose the device's preset on the add row (it starts on a connected device's preset) and press Add; edit the row it opens.

### Requirement: sru-33 — Controllers page: portable wizard backend parity
**Reason**: No wizard form, chooser, or blacklist control remains to render. The Controllers page's remaining controls come from the same portable tree as every other portable page.
**Migration**: None; nothing replaces the removed controls.

### Requirement: sru-34 — Portable UI: semantic enabled state
**Reason**: The Configuration Wizard button was the only control any page disabled. With it removed, no production code sets a portable node's `enabled` field to false, so the capability this requirement described has nothing left to exercise.
**Migration**: None; no page disables a control.
