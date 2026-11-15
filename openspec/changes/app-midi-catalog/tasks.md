# Tasks — app-midi-catalog

## 1. Implementation

- [x] 1.1 `HasMidiCatalog<App>` concept and `MidiAppCatalog`/`MidiAppAction`/
      `MidiAppDeviceDefault` types.
      Files: `include/synth/AppConcepts.hpp`, `include/synth/MidiAppCatalog.hpp`.
      Test: `engine_tests.cpp: engine_dispatches_catalog_app_actions_to_surface`.
- [x] 1.2 `MessageIn::Type::AppAction`/`HoldDrill` and `UISystemMessage::AppAction`/
      `HoldDrill`, appended after the existing enumerators.
      Files: `include/synth/ParameterModulation.hpp`,
      `include/synth/MidiConfigViewModel.hpp`, `src/MidiController.cpp`
      (`MessageTypeName`/`ParseMessageType`).
      Test: `instrument_tests.cpp: AssociationJsonRoundTripsAppActionStringsButNotIndex`.
- [x] 1.3 Audio-thread forwarding: `ParameterMessageOut::AppAction`/
      `AppEncoderPress`, `MessageInBus::SetAppActionOut`, `MessageInBus::Apply`'s
      `AppAction`/`ParamPush`/`HoldDrill` cases, `Engine`'s constructor wiring
      of `uiBus_`/`midiBus_`.
      Files: `include/synth/ParameterModulation.hpp`, `src/ParameterModulation.cpp`,
      `include/synth/Engine.hpp`.
      Test: `engine_tests.cpp: engine_dispatches_catalog_app_actions_to_surface`,
      `engine_app_action_out_of_range_dispatches_nothing`,
      `engine_forwards_encoder_press_to_catalog_action_instead_of_opening_modulation_view`,
      `engine_encoder_press_without_catalog_forwarding_opens_modulation_view_as_today`.
- [x] 1.4 Message-thread dispatch: `MessageThreadTick` drains `AppAction`/
      `AppEncoderPress` and dispatches `ui::Action`s to `app_.PortableSurface()`,
      with analog-range value formatting.
      File: `include/synth/Engine.hpp`.
      Test: `engine_tests.cpp: engine_dispatches_catalog_app_actions_to_surface`.
- [x] 1.5 Association `appAction`/`appActionValue` fields and JSON, index not
      persisted.
      Files: `include/synth/MidiController.hpp`, `src/MidiController.cpp`.
      Test: `instrument_tests.cpp: AssociationJsonRoundTripsAppActionStringsButNotIndex`,
      `AssociationJsonOmitsAppActionKeysForNonAppActionPress`.
- [x] 1.6 `AnalogAppActionMapping`, `AnalogMidiInConfig::appActions`, analog
      dispatch branch, JSON.
      Files: `include/synth/MidiController.hpp`, `src/MidiController.cpp`.
      Test: `instrument_tests.cpp: AnalogMidiInConfigJsonRoundTripsAppActions`,
      `AnalogMidiInProcessorPushesAppActionForMatchingControlAndGestureOtherwise`.
- [x] 1.7 `Engine::ResolveAppActionsAgainstCatalog`: resolves app-action rows
      of a copy of each controller's config against the catalog in
      `RebuildMidiProcessors`, drops unresolved rows from the copy with one
      log line, leaves the persisted config untouched.
      File: `include/synth/Engine.hpp`.
      Test: `engine_tests.cpp: engine_rebuild_resolves_app_action_rows_and_drops_unknown_ones`.
- [x] 1.8 `HoldDrillState`, held/drilled semantics in
      `EncoderMidiInProcessor::Process` and `SystemButtonMidiInProcessor::Process`,
      one instance per profile via `MidiControllerProfileResult::holdDrill`.
      Files: `include/synth/MidiController.hpp`, `src/MidiController.cpp`.
      Test: `instrument_tests.cpp: HoldDrillTurnPushesOnceThenPlainTurnAfterRelease`,
      `HoldDrillDrillsEachTurnedKnobOnceDuringOneHold`,
      `HoldDrillOnAbsoluteEncoderSkipsAbsoluteFeedbackUntilRelease`,
      `HoldDrillResetsDrilledFlagsOnEachNewHold`.
- [x] 1.9 `MidiControllerProfileConfig::openSysEx` and
      `OpenSysExMidiOutProcessor` (sends once after construction and after
      every `Reset()`), JSON.
      Files: `include/synth/MidiController.hpp`, `src/MidiController.cpp`.
      Test: `instrument_tests.cpp: ControllerProfileJsonRoundTripsOpenSysExByteForByte`,
      `ControllerProfileJsonWithoutOpenSysExKeyReadsBackEmpty`,
      `CreateMidiControllerProfileOmitsOpenSysExOutputWhenEmpty`,
      `OpenSysExMidiOutProcessorSendsOnceThenWaitsForReset`,
      `OpenSysExMidiOutProcessorSendsMultipleMessagesInOrder`.
- [x] 1.10 `MakeUISystemMessageChoices`/`MakeAnalogAppActionChoices`,
      `MidiConfigViewModel::SetMessageCatalog`/`SetAnalogActionCatalog`,
      Controllers page callbacks wired from the app catalog (library default
      when the app supplies none).
      Files: `include/synth/MidiConfigViewModel.hpp`, `src/MidiConfigViewModel.cpp`,
      `include/synth/ControllersPageUI.hpp`.
      Test: `viewmodel_tests.cpp: MakeUISystemMessageChoicesOrdersLibraryKindsThenActions`,
      `ViewModelOffersAppCatalogChoicesThroughMessageCatalog`,
      `SystemMessageRowsDescribeAppActionAndHoldDrillSemantics`,
      `SystemMessageRowFromAppActionChoiceRoundTripsRowIdentity`,
      `MakeAnalogAppActionChoicesReturnsOnlyAnalogRangeActions`,
      `AddAndCommitAnalogAppActionRowWritesAppActionsWithoutTouchingGestures`,
      `SecondAnalogAppActionRowOnSameAddressIsRejectedLikeADuplicateGesture`,
      `EmptyAnalogActionCatalogOffersNoAppActionAddRow`.
- [x] 1.11 `MakeControllerWizardRegistry(catalog)` replacing the static
      `ControllerWizardRegistry()`; `AppDefaultControllerWizard`/
      `AppDefaultConfigForm`; every registry caller and
      `ControllerWizardDiscoveryCache::SetRegistry` updated to take the
      vector.
      Files: `include/synth/ControllerWizard.hpp`, `src/ControllerWizard.cpp`,
      `include/synth/ControllerWizardDiscoveryCache.hpp`.
      Test: `controller_wizard_tests.cpp:
      MakeControllerWizardRegistryWithEmptyCatalogReturnsTheOneTwisterDescriptor`,
      `MakeControllerWizardRegistryWithAppDefaultsReturnsOneDescriptorPerDefault`,
      `AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`,
      `DiscoveryWithAppRegistryClassifiesDeviceByFirstDefaultsInputAlias`.
- [x] 1.12 Per-controller Layout combo: `BuildLayoutOptions`,
      `Actions::kControllerLayout`/`HandleControllerLayout` (install + commit
      + save, or clear `wizardId` for Custom); `Actions::kControllerReconfigure`
      and its button removed for Active records.
      File: `include/synth/ControllersPageUI.hpp`.
      Test: `controllers_page_ui_tests.cpp: TestLayoutComboOffersLayoutNamesThenCustom`,
      `TestChoosingALayoutInstallsItsConfigAndSetsWizardId`,
      `TestChoosingCustomClearsWizardIdWithoutTouchingConfig`.
- [x] 1.13 `slot.wizardId.reset()` on every slot-mutating edit
      (`ApplyMappingEdit`, `DeleteRow`, `AddSingle`, `AddBlock`,
      `SetLaunchpadVariant`).
      File: `src/MidiConfigViewModel.cpp`.
      Test: `controllers_page_ui_tests.cpp: TestChoosingALayoutInstallsItsConfigAndSetsWizardId`
      (mapping-field-commit branch), `viewmodel_tests.cpp: SetLaunchpadVariantClearsWizardId`.
- [x] 1.14 `BuildPatchJSON`/`LoadPatchJSON`/`ApplyPatchMessage` gain
      `carryInstrument`/`loadedInstrument`; schema version 2 with
      `midiInstrument` only when requested; `Engine` stages a loaded
      instrument and applies it on the message thread through
      `EditInstrument`.
      Files: `include/synth/PatchPersistence.hpp`, `src/PatchPersistence.cpp`,
      `include/synth/Engine.hpp`.
      Test: `engine_tests.cpp: engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings`,
      `engine_patch_load_leaves_instrument_untouched_when_catalog_does_not_carry_mappings`,
      `engine_ignores_version_two_midi_instrument_section_when_catalog_does_not_carry_mappings`.
- [x] 1.15 Build + run `cd projects/synth && nice make -j2 test`; confirm the
      suite is green apart from the two known pre-existing 96kHz-deadline
      failures.
      Verified complete per this submodule's working-tree test run logs
      (`run_engine_tests.log`, `run_instrument_tests.log`,
      `run_parameter_modulation_tests.log`, `run_miniapp_system_tests.log`,
      `run_braid4_system_tests.log`).
- [x] 1.16 Two-line controller header: `kControllerHeaderHeight` becomes two
      36 px lines, identity on line one and ports/lifecycle on line two, for
      both Active and Blacklisted rows; the header width constants become
      the maximum of the two lines.
      File: `include/synth/ControllersPageUI.hpp`.
      Test: `controllers_page_ui_tests.cpp: TestControllersSectionsNestThroughLibraryContainers`,
      `TestControllerRowsStayReadableWithLargeLists`,
      `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`.
- [x] 1.17 Device display names and captions: `MidiProfileKindDisplayName`
      beside `MidiProfileKindName`, used by the row label and the add row's
      selector; the add row's caption "Kind" becomes "Device"; the endpoint
      selectors are captioned "MIDI in" and "MIDI out" (a blacklisted row's
      stored labels read "MIDI in: " / "MIDI out: "); the rename draft gains
      the caption "Rename to".
      Files: `include/synth/MidiController.hpp`, `src/MidiController.cpp`,
      `include/synth/ControllersPageUI.hpp`.
      Test: `instrument_tests.cpp: KindDisplayNameCoversEveryKind`,
      `controllers_page_ui_tests.cpp: TestControllerKindLabelsShowTheCombinedDisplayNames`,
      `TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`, `main`.
- [x] 1.18 Status legend: one legend row, `NodeIds::kStatusLegend`, ahead of
      the first controller row, with a coloured dot in each
      `EndpointStatusColor` colour before "online", "offline", and "not set".
      File: `include/synth/ControllersPageUI.hpp`.
      Test: `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`.
- [x] 1.19 Browser overlay sizing: the combo box's `<select>` and the text
      field's `<input>` fill their node's box (width and height 100%,
      border-box) and a select's text clips to it
      (`browser/public/synth-browser.css`); spec'd as sprs-18; checked by
      the select-fills-wrapper assertion in `browser/tests/ui-backend.spec.ts`.
- [x] 1.20 Fits-within criterion: `FitsWithinViolations(tree, bounds)`
      beside `ContainmentViolations`, folded over each node's ancestor
      chain; the Controllers fixture builds a Twister, a Generic, a
      Launchpad, and a Blacklisted row at 900x620 content bounds and asserts
      the violation list is empty.
      Files: `tests/support/VisualCriteria.hpp`, `tests/portable_ui_tests.cpp`.
      Test: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`.
- [x] 1.21 Fits-within over the open states: the Controllers fixture
      dispatches the page's own actions (expand, open section, add row)
      for the Generic, Launchpad and Twister rows and asserts zero
      violations after each step; the Generic system row's Message combo
      offers the app catalog's 24 choices.
      File: `tests/portable_ui_tests.cpp`.
      Test: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`.
- [x] 1.22 `browser/tests/ui-backend.spec.ts` runs in CI: added to the
      build job's "Run local cross-origin publication gates" step of
      `.github/workflows/synth-browser-pages.yml`, the invocation that
      runs against the built bundle, so the select-fills-wrapper
      assertion (sprs-18) is checked on every push to main.
      File: `.github/workflows/synth-browser-pages.yml`.
- [x] 1.23 Move the Name row into the expanded editor and re-key a rename
      across it: the header's rename field and button are gone (a
      blacklisted row, which has no expanded editor, now has no Rename
      control at all); the expanded editor's first row holds a Name field
      and Rename button; `MidiConfigViewModel::NoteControllerRenamed(from,
      to)` re-keys `expandState_` and `presentations_` by their old name so
      a rename keeps the row expanded and its open sections open, and the
      page calls it on the rename success path, before the next
      `RefreshOnTick` rebuild would otherwise erase the old name's cache
      entries.
      Files: `include/synth/MidiConfigViewModel.hpp`,
      `src/MidiConfigViewModel.cpp`, `include/synth/ControllersPageUI.hpp`.
      Test: `viewmodel_tests.cpp: RenameOfExpandedRowKeepsSectionPresentationOpen`,
      `RenameOfCollapsedRowLeavesItCollapsed`, `SameNameReaddAfterDeleteStartsFullyCollapsed`,
      `controllers_page_ui_tests.cpp: TestControllerLifecycleActionsUseTheNormalCommitAndSavePath`,
      `juce/ControllersPageSimulationTests.cpp: RunControllerWizardParitySimulation`.
- [x] 1.24 Remove the standalone Controllers harness app: it duplicated
      the production `ControllersPageSurface` path the simulation tests
      already exercise. Deleted `apps/controllers_harness/` (`Info.plist`,
      `Makefile`, `README.md`) and `juce/ControllersHarnessApp.cpp`;
      dropped the harness's carve-out from `check_ui_boundary.sh`'s
      `BACKEND_EXCLUDED_FROM_ALL` list and its entry from `docs/coverage.md`.
      Files: `scripts/check_ui_boundary.sh`, `docs/coverage.md`.
- [x] 1.25 Generated header dependency lists: `DEPFLAGS := -MMD -MP`,
      applied to the `portable_ui_tests`, `runtime_main_component_tests`,
      `controllers_page_ui_tests`, `browser_runtime_contract_tests`, and
      `browser_audio_device_tests` rules; `browser/cpp/BrowserRuntimeAbi.cpp`
      split into its own `$(BUILD_DIR)/BrowserRuntimeAbi.o` object rule
      (also under `DEPFLAGS`) so its translation unit gets its own depfile.
      Both translation units reach `ControllersPageUI.hpp`, and one compiler
      invocation over two sources writes a single depfile recording only the
      last of them, so without the split the test unit's own headers would
      go unrecorded; `-include
      $(wildcard $(BUILD_DIR)/*.d)` added at the bottom of the Makefile so
      a header edit rebuilds every binary that reaches it.
      File: `Makefile`.
- [x] 1.26 Rewrote sru-4, sru-60, sru-61, and sru-62 in
      `specs/synth-runtime-ui/spec.md` against the page as it now stands:
      the Name row moved into the expanded editor and a rename re-keying
      its UI state; the per-controller combo recaptioned Layout to Preset
      (sru-60); the two-line header's per-row composition, its 724 px
      minimum width, and the blacklisted row's loss of Rename (sru-61);
      and the add row's Preset combo, per-port status dots, and their
      "Preset" caption (sru-62).
      File: `specs/synth-runtime-ui/spec.md`.

## 2. Postflight and delivery

- [ ] 2.1 Postflight against these artifacts: implementation versus
      proposal, plus a duplication pass over the whole diff.
- [ ] 2.2 Commits append to the branch this submodule is pinned to
      (`app-midi-catalog`), which is the branch of the open upstream pull
      request.
