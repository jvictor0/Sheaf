Every task serves the two stories in `proposal.md`. A new test named here must
go red when the production change it covers is reverted: revert it, watch the
test fail, restore it. Record both outputs in the report.

## 1. Save every committed edit

- [x] 1.1 `ControllersPageSurface::Commit` calls `saveRuntimeConfiguration`
      after a successful `commitInstrument`, and sets the page status in one
      place, as `design.md` D1 says:
      - `Commit` takes the handler's success text;
      - it sets `kHostRejectedCommitStatus` on a refusal,
        `kRuntimeConfigSaveFailedStatus` when the save fails after a
        successful commit, and the handler's text otherwise.

      Every handler passes its text into `Commit` and sets no status after it.
      Delete `SaveCommittedWizardAction` and its call sites.
      Check: new `browser_runtime_contract_tests.cpp:
      TestControllersPageSavesEachCommittedEdit`. After each action in sru-64's
      first scenario, with no Back, the saved configuration file already holds
      that action's result.
      Check: new `controllers_page_ui_tests.cpp:
      TestSaveFailureKeepsTheCommittedEditAndReportsIt`, which replaces
      `TestWizardSaveFailureDoesNotRollbackCommittedInstrument`. With a save
      callback that always fails, a mapping-field edit stays committed and the
      status reads `kRuntimeConfigSaveFailedStatus`. Every path sets its status
      through `Commit`, so one path is enough.
- [x] 1.2 `RuntimePageBackSavesConfiguration` returns false for Controllers and
      stays true for Audio and Sync. In each test that asserts Controllers Back
      saves, remove or flip only that assertion, so it keeps testing Audio and
      Sync:
      - `contract_tests.cpp`'s assertion;
      - `engine_tests.cpp:
        engine_runtime_page_back_policy_saves_config_for_audio_and_controllers_only`.
        Rename it for Audio and Sync;
      - `runtime_main_component_tests.cpp:
        TestBackFromConfigurationPageSavesRuntimeConfiguration`;
      - `browser_runtime_contract_tests.cpp:
        TestControllersUseLatestBridgeSnapshotCommitEditsAndSaveOnBack`. Its
        last assertion expects a save on Back; it goes. Rename the test to
        match what it still checks.

      Check: all four pass.

## 2. Open a newly added row

- [x] 2.1 After `HandleAddController` commits, from a preset or as Custom, set
      the new row's view-model expand state to expanded, with every section it
      lists open. Use the existing toggle state, so the row's disclosure and
      section toggles collapse it. Rows that appear any other way keep starting
      collapsed.
      Check: new `controllers_page_ui_tests.cpp:
      TestAddedRowOpensWithEverySectionOpen`, covering sru-65's three
      scenarios.
- [x] 2.2 Code that adds a row and then presses its disclosure or a section
      toggle to open it now closes it instead. Change each case to rely on the
      row opening on Add, and keep what it asserts about the opened row:
      - `visual-criteria.spec.ts`, "the controller row's Encoders group header
        meets the structural criteria across a range of widths";
      - `ControllersPageSimulationTests.cpp`, `RunManualRecordSimulation`;
      - `fake-app.e2e.spec.ts`, "controller wizard actions are absent on a
        manually added record", if task 7.1 keeps it;
      - `juce/ControllersPageScreenshotHarness.cpp`, states 4 and 5 and its
        extra check. Correct its comment about toggling the row's editor open.

      Search the C++, JUCE and browser suites for any case not listed here, and
      list every case in the report.
- [x] 2.3 `visual-criteria.spec.ts`'s `seedControllers` adds 12 rows, and its
      criteria measure collapsed rows. With rows opening on Add, it presses each
      new row's disclosure once after Add, so every criterion measures the rows
      it measured before, and its assertions stay as they are.

## 3. Connected devices as status; Add binds the chosen preset

- [x] 3.1 Make the "Available controllers" block read-only status, as `design.md` D3 lists it:
      - each waiting device once, by its port names and the name of every
        preset that matches it;
      - "No connected controller is waiting to be set up" when none is waiting;
      - "Other inputs: …" and "Other outputs: …" for every other connected port.

      No node under the block dispatches an action. Update
      `controllers_page_ui_tests.cpp:
      TestDiscoveryRendersPortableAvailableRowsAndDiagnostics`, which asserts
      "Available controllers" and `runtime.controllers.available.0.configure`
      by string id, to the new block.
      Check: new `controllers_page_ui_tests.cpp:
      TestConnectedNotSetUpListsDevicesWithoutActions`. It covers:
      - a device matching two presets, listed once with both names;
      - a recognized device with only one port present, listed under Other;
      - a port no preset matches, listed under Other;
      - no action anywhere in the block.
- [x] 3.2 `EffectiveAddPresetId` returns the player's chosen draft when there is
      one. Otherwise it returns the first matching preset of the first waiting
      device, and otherwise the registry's first descriptor.
      Check: new `controllers_page_ui_tests.cpp:
      TestAddRowStartsOnTheFirstWaitingDevicesPreset`.
- [x] 3.3 Add binds the first unused connected pair whose port names match the
      chosen preset's own aliases, whichever descriptor discovery assigned the
      pair to.
      Check: new `controllers_page_ui_tests.cpp:
      TestAddBindsAConnectedDeviceForEveryPresetItMatches`. Its catalog has two
      presets that share aliases, as frogg3rs's two APC40 mkII presets do. With
      one matching pair connected:
      - adding the second preset binds both ports;
      - a later add of the first preset leaves both ports "(none)", because
        the pair is in use.

      With 3.3 reverted, the first add reads "(none)".
- [x] 3.4 A System Messages row's kind combo also lists the row's own stored
      kind, by name, when the application catalog lacks it, and shows it as the
      current value (`design.md` D3). Choosing any other kind replaces it.
      Check: new `controllers_page_ui_tests.cpp:
      TestSystemMessageRowShowsAStoredKindTheCatalogLacks`. It uses a catalog
      without the Hold kinds, as frogg3rs's is, and adds the WRLD.Bldr preset.
      Each System Messages row then shows its stored kind (Hold Reset, Hold
      Random, Hold Random Mod, Hold Gesture Select), not "Param Inc/Dec".
- [x] 3.5 A System Messages row added by hand (`MidiConfigViewModel::AddSingle`,
      `RowGroup::System`) keeps starting on Scene Select at the next free scene
      when the application's message choices (`MessageCatalog()`) include Scene
      Select. When they do not, as in frogg3rs, it starts on the first entry of
      `MessageCatalog()`, the way an added analog app-action row starts on
      `AnalogActionCatalog().front()`.
      - Today the row stores the library's Scene Select even where the
        application does not offer it, so the combo showed a job the row did
        not have. With 3.4 alone, it would show a Scene Select choice the
        application never offers.
      - Library applications keep today's behaviour, which
        `viewmodel_tests.cpp`'s `AddSingleCommitNormalizes` and
        `BlockEditOverlappingExistingSceneButtonRefused` pin.

      Check: `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`
      passes unchanged, including its assertion that the hand-added Generic
      system row's Message combo offers 24 choices. With 3.5 reverted and 3.4
      in place, that assertion fails with 25. `viewmodel_tests` passes
      unchanged.

## 4. Remove the wizard's entry points; one Delete

- [x] 4.1 Remove the Configuration Wizard button, the wizard page (form and
      chooser, with Back, Cancel, Submit and Ignore), and the connected-device
      list's Configure and Ignore. Remove their node ids, their actions (and
      their entries in `kControllersActions`), their handlers, and the session
      state: `WizardSession`, `ExistingWizardTarget`, `m_wizardSession`,
      `m_wizardChooserOpen`, and the chooser status.
- [x] 4.2 Remove Release from active rows and Configure from released rows,
      with their actions, handlers, and `MidiConfigViewModel::BlacklistController`.
      - `MidiConfigViewModel::DeleteController` accepts a record of either
        disposition.
      - A released row keeps its Released badge and stored port labels, and
        offers Delete, dispatching `Actions::kControllerDelete`.
      - `RemoveFromBlacklist`, `Actions::kControllerRemoveBlacklist`,
        `NodeIds::ControllerRemoveBlacklist` and `hasCompleteEndpointPair` go.
      - Recompute the row width constants as `design.md` D4 says.
      - The Blacklisted disposition, its dormant config, its persistence and
        its reconciliation stay.

      Check: `TestControllersRowFitsWithinFroggersNarrowestHost` and
      `RunDeviceLabelWidthCheck` pass.
- [x] 4.3 Remove what only the removed user interface reached, as `design.md` D5
      lists:
      - the config forms' `ui::Surface` membership, with their `BuildTree`,
        `BuildSubtree`, `DispatchAction` and `SetActionHandler`;
      - `TwisterFormLayout`;
      - reconfigure seeding, `ConfigForm`'s seed parameter (its callers,
        including `tests/instrument_tests.cpp`, call `ConfigForm()`) and
        `ExtractMfTwisterWizardSeed`;
      - `ReconfigureWarning`;
      - the anonymous-namespace helpers in `ControllerWizard.cpp` that only
        removed code reaches;
      - the `controller-wizard.` prefix in `RuntimeMainComponent`'s
        `IsControllersAction`.

      Delete each only after tracing its invocations (bare name, qualified name,
      virtual dispatch, build files) to none, and put the trace output in the
      report. `Validate` stays.

## 5. A relaunch restores the setup the player last had

- [x] 5.1 Under `patchCarriesMappings`, the patch loaded at launch restores its
      parameters and leaves the instrument the runtime configuration restored
      (`design.md` D7).
      Check: new `engine_tests.cpp` test with `patchCarriesMappings` set:
      - save a patch;
      - edit the instrument and save the runtime configuration;
      - start a second engine on the same data root.

      After its first `MessageThreadTick`, the live instrument is the edited
      one and the parameters are the patch's. With 5.1 reverted, the instrument
      is the patch's.
- [x] 5.2 A patch opened from the File page still applies its instrument under
      `patchCarriesMappings`. Once that instrument is applied, the runtime
      configuration is saved.
      Check: new `engine_tests.cpp` test. Open a patch whose instrument differs
      from the live one and tick. The runtime configuration file then holds the
      patch's instrument, and a second engine on the same data root starts with
      it.
- [x] 5.3 Record in the runtime configuration the patch version file last opened
      or saved, relative to the patches root:
      - Load, Save, Save As and Save As (overwrite) set the record and save the
        configuration;
      - New records that no patch is open.

      At launch:
      - open the recorded version, restoring its sound only (5.1);
      - after New, open no patch;
      - with no record, open the newest saved version as today, including its
        instrument under `patchCarriesMappings`, then record that version and
        save the configuration;
      - with the recorded file gone, open the newest saved version, restoring
        its sound only, and record it.

      Check: new `engine_tests.cpp` tests for the two cases a player meets:
      - an older version opened, then relaunch opens that version;
      - no record (a player upgrading): relaunch opens the newest version with
        its instrument, and the relaunch after that keeps a Controllers edit
        made in between.
- [x] 5.4 In the browser, every runtime configuration write the engine makes
      marks persistence dirty, through one signal the browser runtime reads on
      each tick (`design.md` D7). Trace the three places that set the flag
      today, and remove each one the signal makes redundant.
      Check: frogg3rs's task 3.2, which opens a patch and reloads the built
      site.

## 6. Remove Revert

- [x] 6.1 Remove what `design.md` D8 lists:
      - the File page's Revert button, with its action and node id;
      - the Revert branches in `RuntimeFileService` and
        `juce/RuntimePagesJuce.hpp`;
      - each host's `revertPatch` callback, and `Runtime::RevertPatch`;
      - `PatchManager::RevertPatch`.

      Grep case-insensitively for `revert` across `projects/synth` and classify
      every hit. Change each caller of `PatchManager::RevertPatch` (in
      `SynthRig.hpp`, `engine_tests.cpp`, `rig_tests.cpp` and
      `parameter_modulation_tests.cpp`) as D8 says: to `LoadPatch` or
      `NewPatch` where it reloads or resets, or delete it with its test where
      Revert is that test's subject. Record each one and why.

## 7. Tests of removed behaviour

- [x] 7.1 Delete tests whose subject is removed behaviour. Edit tests that mix
      removed and kept assertions so they keep only the kept ones. The compiler
      finds every test that still names a removed symbol. For each one, record
      whether it was deleted or edited, and why. Kept behaviour (Add, Restore,
      Delete, Rename, discovery, generation, Load, New, Save) keeps its tests.
      This includes:
      - the wizard tests in `fake-app.e2e.spec.ts`;
      - `ControllersPageSimulationTests.cpp`'s wizard parity and refusal
        simulations, and its `IsWizardModalTree`, which the kept random-action
        simulation reaches through `VerifyTreeAndRenderer`;
      - the four wizard entries in `RuntimeShellSessionTests.cpp`'s
        structural-action list.
- [x] 7.2 `visual-criteria.spec.ts`'s "the wizard chooser and form meet the
      structural criteria" drives the wizard page, so it goes with the tests of
      7.1. It was also the only test where the sidebar badge rendered, so its
      `badgesSeen > 0` pin goes with it. Correct the comments that name the
      wizard test or `runtime.controllers.wizard.warning`.

## 8. Docs and specs

- [x] 8.1 Update `projects/synth/README.md`:
      - Rewrite the wizard section. The registry, discovery and generation stay
        documented as what the add row and Restore run on.
      - The lifecycle subsection describes the page as it now is: "Connected,
        not set up", the add row starting on a waiting device's preset, rows
        opening on Add, every edit saved as made, and released records offering
        Delete.
      - The Back-saves line names Audio and Sync only.
      - The File page and startup patch text follow `design.md` D7 and D8.
- [x] 8.2 Update `docs/coverage.md` so its rows name the requirements and tests
      as they now stand:
      - Remove the rows for sru-32, sru-33, scw-4 and sru-34.
      - Correct the sru-12 and sru-4 rows, and add a row for sar-8 naming the
        tests from group 5.
      - Correct every row that names Revert.
- [x] 8.3 Correct every comment the change makes false, in any file, including:
      - `MidiController.hpp`'s comment naming `WizardCandidateToken`;
      - `scripts/check_ui_boundary.sh`'s "The wizard's tree is built here";
      - the comments citing sru-33 in `PortableJuceBackendTests.cpp` and
        `RuntimePagesJuceTests.cpp`;
      - `IsControllersAction`'s comment about wizard-step actions.
- [x] 8.4 Extend the deltas so that every requirement is true once this change
      lands:
      - `synth-runtime-ui`:
        - MODIFIED sru-12 (Back saves for Audio and Sync);
        - MODIFIED sru-5 (a newly added row opens, and rows loaded from a
          configuration start collapsed);
        - MODIFIED sru-2, whose scenarios name configuring, ignoring,
          blacklisting and removing from the blacklist;
        - REMOVED sru-34, because the Configuration Wizard button is the only
          control any page disables;
        - MODIFIED sru-61, sru-62 and sru-59 as `app-midi-catalog` adds them,
          by their full headers: Release, Configure and Reclaim on line two;
          one Custom entry per device kind; and the kind combo also listing a
          row's own stored kind (D3);
        - MODIFIED sru-6 and the sru-15 scenario that name Revert.
      - `synth-controller-wizards`: MODIFIED scw-2 (no wizard submission, and
        its registry statement and test names true as the code stands), and
        its Purpose section.
      - `synth-midi-instrument`: MODIFIED smi-8 (no wizard add and no
        Reconfigure).
      - `synth-app-runtime`: MODIFIED sar-8 and the startup-patch scenarios,
        per `design.md` D7; sar-13; and the "restored by revert/new" line.
      - `synth-portable-runtime-shell`: MODIFIED sprs-3.
      - `synth-patch-persistence`: MODIFIED spp-6 and spp-8.

      Every scenario a MODIFIED requirement drops is named in its reason.
      Check: `openspec validate fold-controller-wizard-into-add-row --strict`
      passes. Then grep `openspec/specs` and this change's deltas,
      case-insensitively, for Release, Configure, Reclaim, "Configuration
      Wizard", "wizard page", Revert and "Back saves". Put the output in the
      report, with each remaining match either stating something true after
      this change or superseded by a delta here.

## 9. Verify

- [x] 9.1 From `projects/synth`, run each of these, at most `-j2`, under `nice`
      and in the foreground:
      - build every host test binary that `test:` lists with `make -B`, then run
        each by path, because `make test` stops early at the carried 96 kHz
        deadline tests, which fail on this Mac before and after;
      - `make -B test-wasm32`;
      - `make -B -C apps/miniapp test`, which builds and runs the JUCE binaries,
        including `ControllersPageSimulationTests` and
        `RuntimeShellSessionTests`;
      - the browser `npm test`.

      From the repository root, run `make openspec-check` and
      `openspec validate --all --strict`. Report counts against the same runs
      at `f73d4202`.

## 10. Delivery

The operator approves this change by screenshots before anything is pushed.

- [x] 10.1 Finish `gate-browser-midi-on-controllers`' tasks 4.2 to 4.6 with
      their evidence, commit on that branch, push it to the fork, and
      fast-forward this branch onto it.
- [x] 10.2 Execute frogg3rs's `carry-fold-controller-wizard-into-add-row` against
      this change's uncommitted tree, then run the one postflight over both.
- [x] 10.3 After the postflight, commit this change on
      `fold-controller-wizard-into-add-row`, without pushing it yet. frogg3rs
      then pins that commit.
- [x] 10.4 Take screenshots from a local frogg3rs site build in Google Chrome,
      with the operator's controllers attached. Cover:
      - **Story 1.** The page with a device waiting and the add row on its
        preset; the row it adds, open with its mappings; that row collapsed;
        one mapping changed; the page reloaded without Back, with the change
        still there.
      - **Story 1 after a saved patch.** Save a patch, change a mapping, reload:
        the change is still there, and the patch's sound is loaded.
      - **Story 2.** A Custom row with its ports picked and a hand-entered
        mapping, after a reload.
      - **The File page** with no Revert.
      - **A second preset device**, if one is attached.
      - **The two MIDI Fighter Twister diagrams** from frogg3rs's task 2.4.

      Send them to the operator and wait for approval.
- [x] 10.5 After approval, push and open the pull request:
      - If the Sheaf branch of `frogg3rs-transport-and-shift-ux`
        (`shifted-encoder-turns`) is already on the fork, first rebase this
        branch onto its tip and re-run 9.1.
      - Push from the main checkout's Sheaf git directory
        (`frogg3rs/External/Sheaf`, remote `fork`) to `daguilarc/Sheaf`.
      - Open the next pull request against `jvictor0/Sheaf:main`, stating which
        pull request it stacks on. Its description carries the two stories,
        what was measured on the live site, and step-by-step testing
        instructions.
      - Add the delivery-record commit naming the pull request, and push it.
- [x] 10.6 Finish frogg3rs's delivery (its tasks), pinning this branch's tip.
