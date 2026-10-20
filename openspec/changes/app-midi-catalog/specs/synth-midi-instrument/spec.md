# Delta — `synth-midi-instrument`

## MODIFIED Requirements

### Requirement: smi-1 — Model: instrument configuration
WHEN MIDI controller configuration is stored or edited, THE synth system SHALL represent it as a JUCE-free instrument configuration: an ordered collection of controller slots, each holding a unique controller name, a profile kind (`wrldbldr`, `twister`, `launchpad`, `generic`), an Active or Blacklisted disposition, an optional stable controller-wizard id stored as an opaque non-empty string, best-effort preferred input and output endpoint references that each contain an identifier plus device-name pair (empty meaning unconfigured for an Active slot), and profile data as follows: an Active slot SHALL require a controller profile config (the encoder, analog, and system-message association config defined by spm-44), while a Blacklisted slot SHALL require both endpoint references and a non-empty wizard id, SHALL have no runtime-active profile, and SHALL carry either no dormant profile when created by Ignore or the prior Active profile retained as dormant reconfiguration seed data when changed from Active; the instrument configuration SHALL contain no connection state; unique names and ordered iteration SHALL span both dispositions; each kind SHALL declare which config sections it supports (wrldbldr: encoders, system messages, analogs; twister: encoders, system messages; launchpad: system messages only; generic: all sections) and which system-message address/feedback variants it supports (wrldbldr: channel/CC addresses and WRLD.Bldr feedback positions; twister: channel/CC addresses only; launchpad: Launchpad positions only; generic: channel/CC addresses only); an Active profile or dormant Blacklisted profile whose config populates a section its kind does not support, or whose system-message associations carry a kind-unsupported address or feedback variant, SHALL be invalid. Instrument-model validity SHALL NOT depend on whether the current baked wizard registry recognizes an opaque wizard id. A system-message association's press message and an analog input mapping SHALL each be addressed to either one of the library's own message kinds or, through the two additional kinds `AppAction` and `HoldDrill`, one action of a running app's MIDI catalog, identified by an `(appAction, appActionValue)` name/value pair carried alongside the association or mapping; a profile config's optional analog input SHALL support a list of such app-action mappings, `AnalogMidiInConfig::appActions`, addressed the same way as its gesture mappings and independent of them; a profile config SHALL support `openSysEx`, an ordered list of complete MIDI messages associated with no control or system message.

#### Scenario: Controller names are unique
- **WHEN** an Active or Blacklisted controller is added with a name already used by either disposition
- **THEN** the add is rejected and the configuration is unchanged

#### Scenario: Kind reports supported sections
- **WHEN** code queries the section support of an Active `launchpad` controller slot
- **THEN** encoders and analogs are reported unsupported and system messages supported

#### Scenario: Kind constrains active config sections
- **WHEN** an Active `launchpad` slot is given a profile config containing encoder mappings
- **THEN** the slot is reported invalid and cannot be committed

#### Scenario: Kind constrains active system-message address variants
- **WHEN** an Active `launchpad` slot's system-message association carries a WRLD.Bldr feedback position, or an Active `twister` slot's association carries a Launchpad position
- **THEN** the slot is reported invalid and cannot be committed

#### Scenario: Blacklisted record retains wizard identity
- **WHEN** a Twister pair is ignored before a profile is generated
- **THEN** its Blacklisted slot retains Twister kind, its non-empty wizard id, and both endpoint references
- **AND** it requires no dormant profile

#### Scenario: Active-to-Blacklisted retains dormant profile
- **WHEN** a wizard-associated Active slot is changed to Blacklisted
- **THEN** its complete prior profile is retained as dormant reconfiguration seed data
- **AND** that profile is never runtime-active while the disposition remains Blacklisted

#### Scenario: Configuration is connection-independent
- **WHEN** an instrument configuration is inspected after its devices are unplugged
- **THEN** stored slots, dispositions, kinds, wizard ids, active or dormant profile configs, and endpoint identifiers are unchanged

#### Scenario: Ordered slots preserve UI order
- **WHEN** Active and Blacklisted slots are added in a given order and the configuration is saved and reloaded
- **THEN** iteration yields every slot in the same order

#### Scenario: An association targets an app action by name and value, not by index
- **WHEN** a system-message association's press is `AppAction`
- **THEN** the association carries the app action's name and value
- Check: `instrument_tests.cpp: AssociationJsonRoundTripsAppActionStringsButNotIndex`

#### Scenario: Analog app-action mappings sit beside gesture mappings, independently addressed
- **WHEN** an analog input config carries both gesture mappings and app-action mappings
- **THEN** a CC matching a gesture mapping's control pushes a gesture message
- **AND** a CC matching an app-action mapping's control pushes an `AppAction` message instead
- Check: `instrument_tests.cpp: AnalogMidiInProcessorPushesAppActionForMatchingControlAndGestureOtherwise`

### Requirement: smi-2 — Persistence: instrument JSON
WHEN an instrument configuration is serialized, THE synth system SHALL write a JSON object with a schema identifier, schema version, and a `controllers` array whose entries carry controller name, profile kind, Active or Blacklisted disposition, optional stable wizard id, and preferred input/output endpoint references (identifier and device-name pairs); Active entries SHALL carry the profile config serialized with the existing spm-52 helpers and SHALL allow the wizard id to be absent for manual or migrated records; Blacklisted entries SHALL carry a non-empty opaque wizard id and both endpoint references and SHALL allow the profile field to be absent only for an ignored candidate or to carry dormant seed data; loading SHALL reject unknown kinds, unknown dispositions, malformed or empty wizard-id values, duplicate names, Active entries without a valid kind-compatible profile, Blacklisted entries without a non-empty wizard id or both valid endpoint references, and active or dormant profiles invalid under the smi-1 kind rules (unsupported sections or kind-unsupported system-message address/feedback variants), in every case without mutating the target configuration; loading SHALL preserve an unknown but well-formed wizard id without consulting the baked registry; the reader SHALL accept the preceding instrument schema by treating every legacy entry as Active with no wizard id and requiring its existing profile exactly as before; the earlier single-`midiProfile` format remains unsupported. A system-message association whose press targets `AppAction` SHALL serialize its `appAction`/`appActionValue` name/value pair as JSON string fields and SHALL NOT serialize its resolved catalog index; an association targeting any other kind SHALL carry neither field. An analog app-action mapping SHALL serialize the same way, alongside `AnalogMidiInConfig`'s existing `gestures`/`sceneBlend` fields. A profile config's `openSysEx` SHALL serialize as an array of byte arrays and SHALL be absent or empty on a document written before the field existed, in which case loading SHALL populate an empty list rather than failing.

#### Scenario: New instrument round-trips both dispositions
- **WHEN** an instrument containing Active and Blacklisted slots is serialized and reloaded
- **THEN** every name, kind, disposition, wizard id, endpoint reference, active or dormant profile, and ordered position round-trips losslessly

#### Scenario: Existing multi-kind coverage remains
- **WHEN** an instrument with a wrldbldr, a twister, and two launchpad Active controllers is serialized and reloaded
- **THEN** every slot's name, kind, endpoint identifier and device-name pairs, and profile config round-trip losslessly in order

#### Scenario: Previous instrument schema loads active without wizard identity
- **WHEN** a valid preceding-schema instrument document contains controller entries without disposition or wizard id
- **THEN** every entry loads as Active with its existing profile and endpoint references
- **AND** no wizard lifecycle action is inferred from kind alone

#### Scenario: Unknown kind rejects load
- **WHEN** instrument JSON contains a controller entry with kind `"theremin"`
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Unknown disposition rejects load
- **WHEN** instrument JSON contains disposition `"paused"`
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Unknown wizard id remains loadable and opaque
- **WHEN** instrument JSON contains an otherwise-valid Active or Blacklisted entry whose non-empty wizard id is not in the baked registry
- **THEN** the entry loads and its wizard id round-trips unchanged
- **AND** no wizard association is inferred by the instrument model

#### Scenario: Active record requires profile
- **WHEN** instrument JSON contains an Active entry without a profile or with a kind-incompatible profile
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Blacklisted ignored record can omit profile
- **WHEN** instrument JSON contains a Blacklisted entry with a non-empty wizard id, kind, and both endpoint identities but no profile
- **THEN** it loads as an inert ignored record

#### Scenario: Blacklisted dormant profile is validated
- **WHEN** instrument JSON contains a Blacklisted entry whose dormant profile violates its retained kind
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Duplicate name rejects load
- **WHEN** Active and Blacklisted entries use the same controller name
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Unsupported section rejects load
- **WHEN** instrument JSON contains an Active or dormant `launchpad` profile with encoder mappings
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Kind-incompatible address variant rejects load
- **WHEN** instrument JSON contains an Active or dormant `launchpad` profile whose system-message association carries a WRLD.Bldr position
- **THEN** the load fails and the target configuration is unchanged

#### Scenario: Legacy single-profile document loads with the section ignored
- **WHEN** a patch document contains the old single `midiProfile` section and no `midiInstrument` section
- **THEN** the patch load succeeds (spp-2) with the legacy section tolerated and not applied as patch state
- **AND** the current instrument configuration is unchanged

#### Scenario: App-action association round-trips its name and value, never its index
- **WHEN** an association targeting `AppAction` is serialized and reloaded
- **THEN** its `appAction` and `appActionValue` strings round-trip
- **AND** the reloaded association's resolved index reads as its unresolved default, since the index was never written
- Check: `instrument_tests.cpp: AssociationJsonRoundTripsAppActionStringsButNotIndex`

#### Scenario: A non-app-action association carries neither app-action field
- **WHEN** an association targeting a library message kind is serialized
- **THEN** its JSON contains no `appAction` or `appActionValue` key
- Check: `instrument_tests.cpp: AssociationJsonOmitsAppActionKeysForNonAppActionPress`

#### Scenario: Analog app-action mappings round-trip alongside gestures and scene blend
- **WHEN** an analog input config with gestures, a scene-blend address, and app-action mappings is serialized and reloaded
- **THEN** every gesture, the scene-blend address, and every app-action mapping's control/name/value round-trip
- Check: `instrument_tests.cpp: AnalogMidiInConfigJsonRoundTripsAppActions`

#### Scenario: Connect-time SysEx messages round-trip byte for byte
- **WHEN** a profile config with two `openSysEx` messages is serialized and reloaded
- **THEN** both messages round-trip with every byte unchanged, in order
- Check: `instrument_tests.cpp: ControllerProfileJsonRoundTripsOpenSysExByteForByte`

#### Scenario: A document written before openSysEx existed loads with an empty list
- **WHEN** profile JSON has no `openSysEx` key
- **THEN** loading succeeds and the profile's `openSysEx` is empty
- Check: `instrument_tests.cpp: ControllerProfileJsonWithoutOpenSysExKeyReadsBackEmpty`

### Requirement: smi-8 — Live edits: config changes rebuild processors
WHEN the instrument configuration is edited through the configuration UI, THE message thread SHALL apply the committed edit to the engine's live instrument configuration in a way that cannot race the audio thread's patch-message application (the sar-7 block-boundary patch drain remains the only audio-side writer, and UI-edit application SHALL be serialized against it), SHALL rebuild the affected controller's MIDI processor slot and reconcile connections through the existing shared path, SHALL construct Active processors from Active profiles, and SHALL construct only an explicit drop/no-op input processor with no terminal realtime processor, output processors, thru processors, or sender sink for a Blacklisted slot; WHEN a patch or runtime-configuration load changes the instrument configuration, the existing patch/configuration message flow SHALL apply it followed by the same message-thread rebuild and reconciliation, so UI edits and loads converge on one rebuild path; changing Active to Blacklisted or deleting an Active slot SHALL close affected endpoints, while generating or reconfiguring a slot as Active SHALL make its processors and endpoints live without restart. WHEN the running app declares a MIDI catalog, rebuilding a controller's processors SHALL first resolve every `AppAction` system-message association and analog mapping of a copy of that controller's persisted config against the catalog's current actions by `(appAction, appActionValue)`: a row the catalog has gets its resolved index filled in on the copy; a row the catalog does not have is dropped from the copy and logged by name; in every case the controller's persisted config, as committed or loaded, is left unmodified, so a later catalog version that recognizes a currently-unresolved action recovers that row on its next rebuild.

#### Scenario: Mapping edit takes effect
- **WHEN** the user changes an Active encoder mapping's target slot position and commits
- **THEN** the next matching hardware CC drives the newly mapped position

#### Scenario: UI edits and configuration loads share the rebuild path
- **WHEN** an instrument change arrives from the configuration UI and another from a patch or runtime-configuration load
- **THEN** both trigger the same processor rebuild and reconciliation path

#### Scenario: Edits do not race the audio thread
- **WHEN** a UI instrument edit commits while a patch-load message is pending on the patch input bus
- **THEN** the live instrument configuration observes serialized application with no concurrent mutation

#### Scenario: Added active controller becomes live
- **WHEN** the wizard adds an Active controller with generated profile and present devices
- **THEN** reconciliation connects it and its processors are active without restart

#### Scenario: Blacklisted processor chain drops everything
- **WHEN** a Blacklisted slot's explicit drop input processor receives an ordinary, SysEx, or realtime MIDI message during or after a rebuild window
- **THEN** it emits no parameter, grid, clock, or transport message
- **AND** the slot has no terminal realtime, thru, output, or sender-sink route

#### Scenario: Reconfigure activates a blacklisted record
- **WHEN** a valid wizard profile replaces a Blacklisted slot and changes it to Active
- **THEN** the generated processors are installed
- **AND** reconciliation opens each stored endpoint present under the Active matching rules

#### Scenario: A known app action resolves and dispatches; an unknown one is dropped from the rebuilt copy only
- **WHEN** a controller's persisted config has one system-message association whose app action the running catalog has and one whose app action it does not
- **THEN** the rebuilt processor dispatches the known action on its matching input and has no association left to match the unknown one's address
- **AND** the persisted instrument snapshot still carries both associations, including the unresolved one
- Check: `engine_tests.cpp: engine_rebuild_resolves_app_action_rows_and_drops_unknown_ones`

## ADDED Requirements

### Requirement: smi-13 — App catalog: forwarding and message-thread dispatch
WHEN a running app declares a MIDI catalog (`HasMidiCatalog<App>`), THE synth system SHALL, on the audio thread, push an `AppAction` message onto the app-action output bus instead of applying it locally, and SHALL push an `AppEncoderPress` in place of the library's own `HandlePress` handling whenever the catalog names an encoder-press action; on the message thread, THE synth system SHALL drain that bus once per tick and dispatch each entry as a `ui::Action` to the app's own portable surface: an `AppAction` entry's action name and value come from the catalog's matching action, with the value taken as the action's own stored value, or, when that action declares an analog range, as the control's normalized value rescaled into that range; an `AppEncoderPress` entry's action name is the catalog's encoder-press action and its value is the bank position, as a string. An app that declares no catalog SHALL see this system entirely inert: `AppAction`/`ParamPush` messages are handled exactly as they are today.

#### Scenario: A plain and an analog-ranged app action both dispatch correctly
- **WHEN** the audio thread pushes one `AppAction` for a plain catalog action and one for an analog-ranged action with a mid-range control value
- **THEN** the message thread dispatches the plain action with its stored value
- **AND** dispatches the ranged action with its value rescaled into the action's declared range
- Check: `engine_tests.cpp: engine_dispatches_catalog_app_actions_to_surface`

#### Scenario: An action index outside the catalog dispatches nothing
- **WHEN** an `AppAction` message carries an index past the end of the catalog's action list
- **THEN** the message thread dispatches nothing for it
- Check: `engine_tests.cpp: engine_app_action_out_of_range_dispatches_nothing`

#### Scenario: An encoder press is forwarded to the catalog's action instead of opening modulation
- **WHEN** the catalog names a non-empty encoder-press action and an encoder push arrives
- **THEN** the message thread dispatches that action with the pressed position as its value
- **AND** the library's own modulation-view drill-in does not open
- Check: `engine_tests.cpp: engine_forwards_encoder_press_to_catalog_action_instead_of_opening_modulation_view`

#### Scenario: An empty encoder-press action leaves today's behavior unchanged
- **WHEN** the catalog's encoder-press action is empty and an encoder push arrives
- **THEN** no app action is dispatched
- **AND** the library's own modulation-view drill-in opens, as it does for an app with no catalog
- Check: `engine_tests.cpp: engine_encoder_press_without_catalog_forwarding_opens_modulation_view_as_today`

### Requirement: smi-14 — Hold Drill: momentary drill-in gate on a held button
WHEN a system-button mapping's press targets `HoldDrill`, THE synth system SHALL hold per-profile state — a held flag and one drilled flag per encoder-turn mapping — set by that button's press and release and consumed only by that same profile's encoder input processor, never placed on the message bus; the button's press SHALL set held and clear every drilled flag, and its release SHALL clear held, without pushing any bus message either way. WHILE held is set, THE synth system SHALL, for the first turn on each encoder-turn mapping, push a `ParamPush` for that mapping's slot and position and mark it drilled, and SHALL drop every subsequent turn on an already-drilled mapping during the same hold without pushing any message or applying any relative or absolute value change; on release, every mapping SHALL resume its ordinary relative or absolute turn behavior on its next turn. An encoder's push mapping SHALL be unaffected by Hold Drill.

#### Scenario: A held turn drills once; a plain turn resumes after release
- **WHEN** the drill button is held, the same mapping is turned twice, then the button is released and the mapping is turned again
- **THEN** the first held turn pushes one `ParamPush` for that mapping
- **AND** the second held turn on the same mapping pushes nothing
- **AND** the turn after release applies as an ordinary relative turn
- Check: `instrument_tests.cpp: HoldDrillTurnPushesOnceThenPlainTurnAfterRelease`

#### Scenario: Each distinct mapping drills once per hold
- **WHEN** the drill button is held and two different encoder mappings are each turned once
- **THEN** each mapping pushes exactly one `ParamPush`, in turn order
- Check: `instrument_tests.cpp: HoldDrillDrillsEachTurnedKnobOnceDuringOneHold`

#### Scenario: Absolute-mode feedback is skipped while drilling and resumes after release
- **WHEN** an absolute-mode encoder is turned while the drill button is held, then again after release
- **THEN** the held turn pushes only a `ParamPush`, with no absolute-value message
- **AND** the turn after release pushes an ordinary `ParamSetAbsolute`
- Check: `instrument_tests.cpp: HoldDrillOnAbsoluteEncoderSkipsAbsoluteFeedbackUntilRelease`

#### Scenario: A new hold clears every prior drilled flag
- **WHEN** a mapping is drilled during one hold, the button is released, held again, and the same mapping is turned again
- **THEN** the second hold's turn on that mapping pushes another `ParamPush`
- Check: `instrument_tests.cpp: HoldDrillResetsDrilledFlagsOnEachNewHold`

### Requirement: smi-15 — Connect-time SysEx output
WHEN a controller profile config carries `openSysEx` messages, THE synth system SHALL send every listed message, in declared order, exactly once immediately after the controller's output processor is constructed and again exactly once after every subsequent `Reset()` of that output, and SHALL send nothing between one such send and the next `Reset()`. A profile config with an empty `openSysEx` list SHALL construct no output processor for it.

#### Scenario: Messages send once after construction, then again only after Reset
- **WHEN** a connect-time output processor is constructed with one message, processed twice, reset, and processed again
- **THEN** exactly one send occurs before the reset and exactly one more after it, both carrying the same message bytes
- Check: `instrument_tests.cpp: OpenSysExMidiOutProcessorSendsOnceThenWaitsForReset`

#### Scenario: Multiple messages send in declared order
- **WHEN** a connect-time output processor holds two messages and is processed
- **THEN** both are sent, in the order they were declared
- Check: `instrument_tests.cpp: OpenSysExMidiOutProcessorSendsMultipleMessagesInOrder`

#### Scenario: An empty list adds no output processor
- **WHEN** a profile config's `openSysEx` is empty
- **THEN** the constructed profile has no output processor for it
- Check: `instrument_tests.cpp: CreateMidiControllerProfileOmitsOpenSysExOutputWhenEmpty`
