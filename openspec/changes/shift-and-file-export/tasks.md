# Tasks — `shift-and-file-export`

Builds run under `nice`, `-j2`.

## 0. Hygiene — step zero

- [x] 0.1 **S** Sweep `include/synth/` and `src/` by concept: "where a held
      modifier lives", "where an app-action index is resolved", "what the row
      dropdown offers", "how a combo field is emitted". Expected: one modifier
      (Hold Drill), one resolve site (`Engine.hpp:1040`), one dropdown builder
      (`MidiConfigViewModel.cpp:500`), three combo branches
      (`ControllersPageUI.hpp:2666-2725`). Report found versus changed.
- [x] 0.2 **H** `git status --short` clean on `app-midi-catalog`; cut branch
      `shift-and-file-export` from it.

## 1. The kind

- [x] 1.1 **H** Append `Shift` after `HoldDrill` in `MessageIn::Type`
      (`include/synth/ParameterModulation.hpp:978`) and `UISystemMessage`
      (`include/synth/MidiConfigViewModel.hpp:227`). Add
      `MessageIn::Shift(std::uint64_t, bool held)` beside `HoldDrill`
      (`:1044`; body beside `src/ParameterModulation.cpp:4071-4078`). Update
      the two kind-count comments (`include/synth/MidiConfigBlocks.hpp:75`,
      `src/MidiConfigBlocks.cpp:26`) to 26.
- [x] 1.2 **H** Build the library once before editing with its output
      captured (`nice make -j2 build/libsynth.a 2>&1 | grep -c "not handled in
      switch"`, expected 0: `-Wall` carries `-Wswitch` and there is no
      `-Werror`, `Makefile:2`). Visit every switch in the proposal's family table and add
      the `Shift` case beside `HoldDrill` with the same disposition: name
      `"shift"` (`src/MidiController.cpp:233-234`, `:288-289`); feedback
      exclusion (`:1856`); JSON (`:2411`, `:2483`); bus `Apply` no-op
      (`src/ParameterModulation.cpp:4239`); wizard switches
      (`src/ControllerWizard.cpp:185`, `:396`); view-model arg switches
      (`src/MidiConfigViewModel.cpp:50`, `:92`, `:123`), kind mapping
      (`:188-189`), press `Shift(0, true)` and release `Shift(0, false)`
      (`:241-242`, `:276-277`), library-produced choice labelled "Shift"
      (`:507-508`), row label "shift on"/"shift off" (`:726`); sort key
      (`src/MidiConfigBlocks.cpp:119-122`). Rebuild with the same grep: a
      count above 0 names a site this list missed; fix it and report the
      count found versus the table.

## 2. The model and the processor

- [x] 2.1 **H** `MidiControllerSystemMessageAssociation`
      (`include/synth/MidiController.hpp:907-917`): add
      `std::optional<MessageIn> shiftedPress;`, `std::string shiftedAppAction;`,
      `std::string shiftedAppActionValue;` after the existing pair, with a
      comment saying what a shifted press is.
      `SystemButtonMidiAssociation` (`:368-373`): add `shiftedPress`.
- [x] 2.2 **S** `struct ShiftState { bool held = false; };` beside
      `HoldDrillState` (`:249`). `MidiControllerProfileResult` (`:930-938`)
      gains `std::unique_ptr<ShiftState> shift;`.
      `SystemButtonMidiInProcessor` (`:377-395`) takes `ShiftState* shift =
      nullptr` after `holdDrill`; member `shift_`.
- [x] 2.3 **S** `src/MidiController.cpp`: create the state at `:2960-2961`'s
      pattern; copy `shiftedPress` at `:2990-2996`; pass `shift` at `:2998`.
      In `Process` (`:940-966`): after the Hold Drill branch, `if
      (association->press.type == Shift) { if (shift_) shift_->held =
      isPress; return; }`; then `if (isPress) { PushStamped(shift_ &&
      shift_->held && association->shiftedPress ? *association->shiftedPress
      : association->press); return; }`. Release unchanged.
- [x] 2.4 **H** JSON (`:2589-2600`, `:2636-2662`): write `shiftedPress` (null
      when absent) after `release`; write the two shifted strings only when
      the shifted press is `AppAction`; read all three tolerating absence.
- [x] 2.5 **S** `Engine.hpp:1040-1052`: inside the resolved branch, if
      `it->shiftedPress` is `AppAction`, resolve by
      `(shiftedAppAction, shiftedAppActionValue)`; on failure log once by
      name and `it->shiftedPress.reset()` on the copy. Update the comment
      (`:1033-1039`).

## 3. The Controllers page

- [x] 3.1 **S** `MidiMappingRowVM::Field::ShiftAction`
      (`include/synth/MidiConfigViewModel.hpp`, declared last, after
      `GridYMax`: a field's token is its integer, so appending keeps every
      existing token), width 150
      (`include/synth/ControllersPageUI.hpp:561-563`), short label and
      column header "Shift" (`FieldShortLabel`/`ColumnHeadersForGroup`).
      Appended to an individual system row's fields (`src/MidiConfigViewModel.cpp:
      1063-1067`) unless the row's own kind is Shift or HoldDrill.
- [x] 3.2 **S** `ShiftCatalog()` on the view model: entry 0 "(none)", then
      every `MessageCatalog()` entry whose kind takes no argument
      (`UISystemMessageHasArg`, `:97-127`) or is `AppAction`, excluding Shift
      and HoldDrill. `ShiftChoiceIndex(controllerIx, section, rowIx)` shaped
      like `UISystemMessageIndex` (`:1828-1856`), matching by kind and, for
      `AppAction`, by the shifted name/value pair. The list is derived once
      inside `SetMessageCatalog` (`:541-543`) into a `shiftCatalog_` member;
      `ShiftCatalog()` returns it.
- [x] 3.3 **S** Edit path (`:2690-2705`'s shape) for `ShiftAction`: index 0
      clears `shiftedPress` and the strings; otherwise `shiftedPress =
      PressForUISystemMessage(choice.message, *association)` and, for
      `AppAction`, the index and the pair from the choice. Flushes through the
      session path like every field (sru-11).
- [x] 3.4 **S** `MakeUISystemMessageChoices` (`:514-522`): skip actions with
      an analog range. Update the header comment
      (`include/synth/MidiConfigViewModel.hpp:263-272`).
- [x] 3.5 **S** `ControllersPageUI.hpp:2666-2725`: fold the three combo
      branches into one taking `(options, selectedIndex)`; add the
      `ShiftAction` case as a fourth caller reading `vm.ShiftCatalog()` and
      `vm.ShiftChoiceIndex(...)`.

## 4. The file export seam

- [x] 4.1 **H** `include/synth/AppConcepts.hpp:69-82`: `struct FileExport`
      (fileName, mediaType, bytes, note) and `concept HasFileExports`
      (`app.TakePendingFileExport()` returns `std::optional<FileExport>`),
      with a comment in the shape of `HasMidiCatalog`'s.
- [x] 4.2 **S** `include/synth/Engine.hpp`: `SetFileExportHandler`
      storing a `std::function<void(FileExport)>`; at the end of
      `MessageThreadTick` (`:497-535`), `if constexpr (HasFileExports<App>)`
      loop `TakePendingFileExport()` and call the handler, or `INFO` the
      dropped name when none is installed.
- [x] 4.3 **S** `include/synth/browser/BrowserRuntime.hpp`: install the
      handler in the constructor (queue of `FileExport`); `DequeueFileExport()`
      beside `ConsumePersistenceDirty` (`:778-783`); ABI
      `synth_browser_dequeue_file_export(runtime, out)` in the extern list
      (`:1509-1553`) and `browser/cpp/BrowserRuntimeAbi.cpp`, returning 0 for
      none and 1 with a struct of name pointer/size, media-type pointer/size,
      bytes pointer/size that stays valid until the next dequeue.
- [x] 4.4 **S** `browser/src/worker.ts`: a `dequeueFileExport` module
      binding in the shape of `dequeueMidiAction` (`:370-385`); the
      `message-tick` case (`:619-622`) drains it after the tick and emits
      each export through `emitStatus` (`:478`) as `{ type: "file-export",
      fileName, mediaType, bytes }`, the response type declared beside
      `page-status` (`:46`). `browser/src/main.ts`: the worker client
      forwards `file-export` to its status handlers beside `page-status`
      (`:129-131`) and its reply listener ignores it the same way
      (`:136-138`); the direct client (`:89-121`) needs nothing, its
      `emitStatus` already reaches the handlers. `SynthBrowserApp`'s status
      subscription (`:183`): on `file-export`, Blob, object URL, anchor with
      `download = fileName`, click, revoke; everything else to
      `renderStatus` as today. `browser/src/protocol.ts` if the response
      union lives there rather than `worker.ts`.
- [x] 4.5 **S** `browser/tests/fixtures/cpp/FakeBrowserApp.hpp`: the fixture
      app gains `TakePendingFileExport` and an action that queues a small
      export, so a Playwright spec (`browser/tests/file-export.spec.ts`,
      in the shape of the existing specs) can click it and assert a download
      with the right name, type and bytes.

## 5. Checks

- [x] 5.1 **S** `tests/instrument_tests.cpp`, beside the Hold Drill cases
      (`:460-560`): `ShiftHeldSwapsPressForShiftedPressAndReleaseClearsIt`
      (Shift down, button with shifted press → shifted message; Shift up →
      ordinary message; a button without a shifted press is unaffected
      while held); `ShiftAndHoldDrillAreIndependent`;
      `AssociationJsonRoundTripsShiftedPressAndTreatsAbsentAsNone` beside
      `:1524`.
- [x] 5.2 **S** `tests/engine_tests.cpp`, beside `:3070`:
      `engine_rebuild_resolves_shifted_app_action_and_clears_an_unknown_one`
      (a shifted press resolving to an action other than index 0 dispatches
      that action; an unknown shifted action leaves the row with its ordinary
      press and no shifted press on the rebuilt copy; the snapshot still
      carries both strings).
- [x] 5.3 **S** `tests/viewmodel_tests.cpp`: `MakeUISystemMessageChoicesOffersShiftLikeHoldDrill`;
      `MakeUISystemMessageChoicesSkipsAnalogRangedActions` over
      `MakeAnalogFakeAppCatalog` (`:4068-4076`); `SystemRowsExposeShiftFieldExceptOnShiftAndHoldDrillRows`;
      `ShiftFieldEditCommitsShiftedPressAndNoneClearsIt`.
- [x] 5.4 **S** `tests/portable_ui_tests.cpp:3010`: the fits gate with a Shift
      column on every system row of every kind. Its fixture catalog
      (`:3070-3074`) gains `UISystemMessage::Shift` in `libraryKinds`, as the
      app's does in task 2.1; the 24-choice assertion (`:3376`) then holds as
      six kinds plus eighteen actions, and its message says so. `tests/controllers_page_ui_tests.cpp`:
      one case that the Shift combo renders and commits through
      `kMappingFieldCommit`.
- [x] 5.4b **S** `tests/engine_tests.cpp`: a test app with `HasFileExports`
      queues one export; after a tick the installed handler received it and
      the app's queue is empty; with no handler the export is taken and
      logged, not retained. `tests/browser_runtime_contract_tests.cpp`: give
      `ValidApp` (`:194`) the hook and an action that queues an export; the
      dequeue ABI returns 0 with nothing queued, and with two queued returns
      1, 1, then 0, in order (sbw-12's scenario).
- [x] 5.5 **H** Positive control for each new assertion: flip one expected
      value, watch the case go red, restore. Report the red line. `rm` the
      binary before each rebuild in a break/restore sequence.
- [x] 5.6 **H** Run by path: `instrument_tests`, `engine_tests`,
      `viewmodel_tests`, `portable_ui_tests`, `controllers_page_ui_tests`,
      `controller_wizard_tests`, `browser_runtime_contract_tests`. The carried
      96 kHz deadline failures are not this change's. Then the browser
      build and the new Playwright spec. The JUCE Controllers page build and the browser runtime
      build must compile: the browser command buffer serializes the page
      tree, not the association, so no browser-side schema changes
      (`include/synth/browser/`, grepped for the pair: none). Browser steps,
      from `browser/`: `npm run build`, `npm run check:generic-runtime`,
      `nice make -j2 browser-fixture-app` (the fixture app is wasm, rebuilt
      the way `browser-audio-input-test` does it, `browser/Makefile:44-45`),
      then `npx playwright test tests/file-export.spec.ts --workers=1`. The
      JUCE side: `nice make -j2 -C apps/miniapp` compiles the runtime shell
      and the Controllers page (`Makefile:311-312`).

## 6. Spec

- [x] 6.1 Spec deltas in `specs/` applied; every scenario names a check that
      passes now.

## 7. Deliver

- [x] 7.1 Commit on this branch, push to the fork, open the next pull
      request against upstream `main`.
- [x] 7.2 After the pull request, frogg3rs moves its submodule pin.
