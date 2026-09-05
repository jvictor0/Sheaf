# Proposal — `app-midi-catalog`

## Why

An app's Controllers page has always offered the library's own vocabulary
of MIDI targets — inc/dec, absolute, the reset/random modifiers, bank
navigation, scene select — and the one device the library ships a wizard
for, the MIDI Fighter Twister. An app that defines its own actions (its own
menu commands, transport controls, drill-in gestures) had no way to make
those actions MIDI-mappable: the Controllers page's message dropdown, the
wizard registry, and patch persistence all worked from a fixed, library-only
vocabulary. An app with its own preferred controllers — devices it ships
day-one defaults for — had no way to offer a ready-made layout for them
either; every device, including one the app knows how to fully address out
of the box, had to be hand-mapped through the low-level per-mapping editor.
frogg3rs is the first consumer of the mechanism this change adds.

## What Changes

- **App catalog.** A new optional app hook, `HasMidiCatalog<App>`
  (`app.MidiCatalog()` returning a `MidiAppCatalog`), lets an app declare
  the actions it wants MIDI-mappable, which of the library's own message
  kinds it still wants offered, whether one of its actions is what an
  encoder press should drill into, whether its saved patches should carry
  MIDI mappings, and the device defaults it ships. The catalog is read once
  at engine construction; an app that declares none sees exactly today's
  behavior.
- **Two new message kinds.** `MessageIn::Type` and `UISystemMessage` each
  gain `AppAction` and `HoldDrill`, appended after the existing
  enumerators so no ordinal moves.
- **Dispatch.** On the audio thread, an `AppAction` message and — only when
  the catalog names an encoder-press action — a `ParamPush` are forwarded
  over `ParameterMessageOutBus` instead of being handled locally.
  `MessageThreadTick` drains that bus and dispatches each as a `ui::Action`
  to `app.PortableSurface()`: an app action's value is either its own
  stored value or, for an analog-ranged action, the control's normalized
  value rescaled into the action's range; an encoder press's value is the
  bank position.
- **Association and analog persistence.** A system-message association and
  an analog mapping can each carry an `appAction`/`appActionValue` pair
  instead of a library kind. Both persist as strings; the runtime index
  they resolve to is never persisted, because an app's action list can
  reorder between runs. `Engine::RebuildMidiProcessors` resolves every
  `AppAction` row of a controller's profile against the running catalog on
  a copy of that controller's config: a resolved row gets its index filled
  in, an unresolved row is dropped from the copy and logged by name, and
  the persisted config is never touched — a later app version that knows
  the action gets the row back. This sits beside, and does not change,
  `ParseMessageType`'s existing fail-closed policy for an unknown message
  *type*: a whole file with an unrecognized type still fails to load;
  dropping is only ever a per-row response to an unrecognized app *action*.
- **Hold Drill.** `MessageIn::Type::HoldDrill` names a button's press and
  release, exactly as Hold Reset uses its own toggle pair. The held/drilled
  state lives in the controller profile's input chain (`HoldDrillState`,
  one per profile), not on the message bus: while the button is held, the
  first turn on each encoder mapping pushes a `ParamPush` for that mapping
  and is remembered as drilled; every further turn on the same mapping
  during that hold is dropped; release clears both the held flag and every
  drilled flag, so the next hold starts fresh and the knob resumes normal
  turn behavior (relative or absolute) once released.
- **Connect-time SysEx output.** `MidiControllerProfileConfig` gains
  `openSysEx`, a list of complete MIDI messages sent once whenever the
  controller's output opens or reopens (once per `Reset()`, in declared
  order), independent of any mapped control. Any device profile can use
  it; nothing about it is specific to one manufacturer.
- **Controllers page catalog wiring.** The message dropdown offered for
  system-message and Generic-controller rows, and the target list offered
  for an analog row's app-action combo, are built from the app's catalog
  (library kinds it keeps plus its own actions) when the app supplies one,
  and default to the unchanged library-only list otherwise.
- **Per-controller Layout combo.** Every controller row's lifecycle
  controls gain a "Layout" combo whose options are the wizard registry's
  display names plus "Custom." Its value reflects the slot's stored
  `wizardId` (or "Custom" when none of the registry's descriptors match).
  Choosing a named layout regenerates that descriptor's profile against
  the slot's own name/endpoints, installs it, commits the instrument, and
  saves the runtime configuration — one action, no intermediate form.
  Choosing "Custom" clears `wizardId` and leaves the stored config
  untouched. Any mapping edit committed on the slot (an edited field, an
  add, a delete, a block edit, a Launchpad variant change) also clears
  `wizardId`, so the combo reads "Custom" again as soon as the installed
  layout's config is no longer what is actually mapped. This combo
  replaces the Active-record "Reconfigure" action; the Blacklisted-record
  "Configure" action (reactivating an inert record by reopening its
  wizard form, seeded from any retained dormant profile) is unchanged.
- **Registry.** `MakeControllerWizardRegistry(catalog)` replaces the
  library's former static registry: it returns one descriptor per app
  device default when the catalog supplies any, else the library's single
  Twister descriptor. Discovery walks whichever registry it is given in
  order and classifies a present device pair under the first descriptor
  whose aliases match, unchanged from today. An app default's descriptor
  wraps a wizard with an empty, always-valid form: generating from it
  installs the default's own stored config, with only the name and
  endpoints filled in from where the pair was found or is being placed.
- **Patch files.** `BuildPatchJSON`/`LoadPatchJSON`/`ApplyPatchMessage`
  gain a `carryInstrument` flag the engine passes from
  `catalog.patchCarriesMappings`. With it set, a saved patch is schema
  version 2 and carries a `midiInstrument` section; a load parses that
  section into a separate out-parameter without touching the live
  instrument, and the engine applies it later, on the message thread,
  through the ordinary instrument-edit path. With the flag unset (the
  default, and every app without a catalog), a saved patch stays schema
  version 1 with no such section, byte-for-byte what a parameter-only
  patch has always written, and a version-2 section present in a file —
  however it got there — is never even parsed.
- **Controller row fits its host.** Every controller row's header lays out
  as two 36 px lines instead of one: identity — disclosure, name, device
  display name, status dots, the Layout combo, and, for a Launchpad, the
  Variant combo — on line one, and ports and lifecycle — MIDI in, MIDI out,
  a "Rename to" draft, Rename, Delete, and Blacklist — on line two, so the
  header's minimum width stays under a narrow host's content instead of
  running past it. The row's device label and the add row's selector now
  read the device's display name (`MidiProfileKindDisplayName`) while the
  persisted config keeps the profile-kind token; the add row's caption
  reads "Device"; the endpoint selectors read "MIDI in" and "MIDI out";
  and one legend, ahead of the first row, shows a coloured dot in each
  endpoint-status colour before "online", "offline", and "not set". A new
  `FitsWithinViolations` criterion checks every node's rectangle, folded
  over its ancestor chain, against the page's actual content bounds — the
  check `ContainmentViolations` alone cannot make, since a row inside a
  scroll area that grows to fit it still passes containment while running
  past the surface — and the Controllers fixture asserts it empty for a
  Twister, a Generic, a Launchpad, and a Blacklisted row at 900 px.

## What Does NOT Change

For an app that declares no `MidiCatalog()`: the offered message list is
the same fixed library vocabulary as before; the MF Twister wizard is the
only device the discovery registry recognizes; and patch files stay schema
version 1 with no `midiInstrument` section. Two things are true for every
app regardless of whether it declares a catalog: the Layout combo appears
on every controller row (offering, for a catalog-less app, the library's
Twister layout plus Custom), and the Active-record "Reconfigure" action is
gone — every app now regenerates an active layout through the combo.

## Testing

- App catalog / `HasMidiCatalog` construction wiring: `engine_tests.cpp`
  (`engine_dispatches_catalog_app_actions_to_surface`).
- `AppAction`/`HoldDrill` message kinds, JSON name round-trip: covered
  through the association and instrument persistence tests below and
  `MidiController.cpp`'s `MessageTypeName`/`ParseMessageType` table.
- Dispatch (audio-thread forward, message-thread `ui::Action`, value
  formatting, encoder-press forwarding): `engine_tests.cpp`
  (`engine_dispatches_catalog_app_actions_to_surface`,
  `engine_app_action_out_of_range_dispatches_nothing`,
  `engine_forwards_encoder_press_to_catalog_action_instead_of_opening_modulation_view`,
  `engine_encoder_press_without_catalog_forwarding_opens_modulation_view_as_today`).
- Association/analog persistence and rebuild resolution:
  `instrument_tests.cpp`
  (`AssociationJsonRoundTripsAppActionStringsButNotIndex`,
  `AssociationJsonOmitsAppActionKeysForNonAppActionPress`,
  `AnalogMidiInConfigJsonRoundTripsAppActions`), `engine_tests.cpp`
  (`engine_rebuild_resolves_app_action_rows_and_drops_unknown_ones`).
- Hold Drill: `instrument_tests.cpp`
  (`HoldDrillTurnPushesOnceThenPlainTurnAfterRelease`,
  `HoldDrillDrillsEachTurnedKnobOnceDuringOneHold`,
  `HoldDrillOnAbsoluteEncoderSkipsAbsoluteFeedbackUntilRelease`,
  `HoldDrillResetsDrilledFlagsOnEachNewHold`).
- Connect-time SysEx output: `instrument_tests.cpp`
  (`ControllerProfileJsonRoundTripsOpenSysExByteForByte`,
  `ControllerProfileJsonWithoutOpenSysExKeyReadsBackEmpty`,
  `CreateMidiControllerProfileOmitsOpenSysExOutputWhenEmpty`,
  `OpenSysExMidiOutProcessorSendsOnceThenWaitsForReset`,
  `OpenSysExMidiOutProcessorSendsMultipleMessagesInOrder`).
- Controllers page message/analog-action catalog wiring:
  `viewmodel_tests.cpp` (`MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`,
  `ViewModelOffersAppCatalogChoicesThroughMessageCatalog`,
  `ViewModelLayoutsDefaultsToTheLibraryTwisterOnlyRegistry`,
  `SystemMessageRowsDescribeAppActionAndHoldDrillSemantics`,
  `SystemMessageRowFromAppActionChoiceRoundTripsRowIdentity`,
  `MakeAnalogAppActionChoicesReturnsOnlyAnalogRangeActions`,
  `AddAndCommitAnalogAppActionRowWritesAppActionsWithoutTouchingGestures`,
  `SecondAnalogAppActionRowOnSameAddressIsRejectedLikeADuplicateGesture`,
  `EmptyAnalogActionCatalogOffersNoAppActionAddRow`).
- Layout combo: `controllers_page_ui_tests.cpp`
  (`TestLayoutComboOffersLayoutNamesThenCustom`,
  `TestChoosingALayoutInstallsItsConfigAndSetsWizardId`,
  `TestChoosingCustomClearsWizardIdWithoutTouchingConfig`), and
  `viewmodel_tests.cpp` (`SetLaunchpadVariantClearsWizardId`) for the
  wizardId-clearing behavior shared with every other slot-mutating edit.
- Registry / discovery / app-default wizard: `controller_wizard_tests.cpp`
  (`MakeControllerWizardRegistryWithEmptyCatalogReturnsTheOneTwisterDescriptor`,
  `MakeControllerWizardRegistryWithAppDefaultsReturnsOneDescriptorPerDefault`,
  `AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`,
  `DiscoveryWithAppRegistryClassifiesDeviceByFirstDefaultsInputAlias`).
- Blacklisted-record Configure (reconfigure lifecycle, unchanged):
  `controllers_page_ui_tests.cpp`
  (`TestConfigureSeedsFromDormantDataForBlacklistedRecords`).
- Patch schema version 2 / `carryInstrument` / deferred apply:
  `engine_tests.cpp`
  (`engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings`,
  `engine_patch_load_leaves_instrument_untouched_when_catalog_does_not_carry_mappings`,
  `engine_ignores_version_two_midi_instrument_section_when_catalog_does_not_carry_mappings`).
