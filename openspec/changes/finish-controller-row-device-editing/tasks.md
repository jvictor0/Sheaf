## 1. Remove the Pressure-Mapping Row Editor

- [x] 1.1 Remove the row-level pressure-mapping editor in one edit, so the
      tree never passes through a state where a call site outlives its
      definition: `MidiConfigViewModel`'s `PressureMappingCount`,
      `PressureMappingFieldValue`, `AddPressureMapping`,
      `SetPressureMappingField`, `DeletePressureMapping` and the
      `PressureMappingField` enum; `MirrorPressureMappingChangeIntoOpenSession`;
      the `PressureMappings`, `PressureMappingsHeading`, `PressureMappingRow`,
      `PressureMappingField`, `PressureMappingDelete` and `PressureMappingAdd`
      node ids; the `kPressureMappingFieldCommit`, `kPressureMappingDelete` and
      `kPressureMappingAdd` actions and their handlers; the rendered
      `scroll.Column` block; and `PressureMappingFieldToken` and
      `ParsePressureMappingFieldToken`, whose only call sites are inside that
      block and which are dead the moment it goes.
      Restore `SectionPresentation::hiddenPressureMappings`'s original comment
      (never rendered or edited; carried verbatim through a System Messages
      flush), and correct the claim in `ControllersPageUI.hpp` that pressure
      mappings render unconditionally on every kind, which this task makes
      false.
      Check: the library and its test binaries build and link with every symbol
      above gone, and a case-insensitive search for `pressuremapping` across
      `projects/synth` returns no reference to any removed row-editor symbol —
      every remaining hit belongs to the grid-cell pressure path, to
      `hiddenPressureMappings`, or names the row editor only to assert its
      absence (`ControllersPageScreenshotHarness.cpp`'s
      `RenderState6_NoPressureMappingRowEditor`, which renders and verifies
      that no such editor exists — a hit on its own name is not a
      reintroduction). Stated as that property rather than as a list of
      allowed hits, which a growing grid-cell path makes stale.
      `SlotValidForKindValidatesPressureMappingsWithoutExposingANewKindSection`
      in `instrument_tests.cpp` exercises `SlotValidForKind` directly, never
      called the removed entry points, and passes unmodified.
- [x] 1.2 Remove the ten `TestPressureMapping*` functions
      (`TestPressureMappingShowsGridAttachedAndOrphanedThenEditCommits`,
      `TestPressureMappingAddAndDelete`,
      `TestPressureMappingAddStillSucceedsWhenNoteZeroIsTakenByPickingTheLowestFreeNote`,
      `TestPressureMappingAddedWhileSystemMessagesOpenSurvivesALaterGridAdd`,
      `TestPressureMappingEditedWhileSystemMessagesOpenSurvivesALaterGridAdd`,
      `TestPressureMappingXRefusesAWholeNumberOutsideIntRange`,
      `TestPressureMappingSlotXAndYReadAndWriteTheirOwnFieldNotAnother`,
      `TestPressureMappingFieldsRefuseChannelNoteSlotOutOfRangeAndNonNumericText`,
      `TestPressureMappingDeleteRemovesTheGivenIndexNotAlwaysFirst`,
      `TestPressureMappingShownOnEveryKindNotJustWrldBldr`) and their call
      sites from `controllers_page_ui_tests.cpp`.
      Check: the file defines no function and contains no call whose name
      refers to a pressure mapping, and the rest of its tests build and pass
      with those ten removed.
- [x] 1.3 Confirm on the harness: a WRLD.Bldr row's expanded editor shows no
      "Pressure mappings" heading, with a working grid mapping row present in
      the same image.
      Check: delivery gate state 6's image shows the absence only — a grid
      row holding a paired pressure mapping renders pixel-identical to one
      without, since the grid button carries no pressure-specific control or
      label text, so no screenshot can show that half surviving. That half is
      carried instead by the grid tests (`MidiMappingRowVM::Field`'s own
      comment and the grid round-trip tests in `viewmodel_tests.cpp`), which
      pass.

- [x] 1.4 Correct two comments in the directories this change touches that are
      false independently of the removal:
      `ControllersPageUI.hpp`'s comment on `kControllerDeviceWidth`, which says
      the constant clears every listed device name while the check it cites
      measures only the library's own names — say what the check actually
      measures; and the construction-path comments in
      `ControllerWizardDiscoveryCache.hpp` and `MidiConfigViewModel.hpp`
      claiming a cache or registry nobody configures behaves exactly as it did
      before, which stopped being true when the fallback registry grew from one
      device to three.
      Check: each edited comment states what the code does now, and names no
      commit, task, plan or document.

## 2. Cover Committing to a Connect Message Other Than the First

- [x] 2.1 Add a test: a row holds two connect messages; committing a valid
      edit to message index 1 updates index 1 and leaves index 0's stored
      bytes unchanged.
      This is the one uncovered path. `TestConnectMessageDeleteRemovesTheGivenIndexNotAlwaysFirst`
      already deletes index 1 on a two-message row, so deletion by index is
      covered; committing by index is not.
      The handler path is correct: on a two-message row, committing to
      index 1 gives `stored[0]=F0 01 F7` unchanged and `stored[1]=F0 55 F7`
      edited. The test is therefore a regression guard over behaviour that
      works, not a fix.
      Check: after dispatching `kConnectMessageCommit` with value
      `"0:1:<new hex>"`, `harness.instrument.controllers[0].config.openSysEx[0]`
      equals its pre-edit value and `openSysEx[1]` equals the edited bytes.
- [x] 2.2 `HandleConnectMessageCommit`'s arity guard tightened from
      `parts.size() < 3` to `< 2`, a real behaviour change: an edit value of
      `"<controllerIx>:<messageIx>:"` (nothing after the second colon)
      tokenises to 2 parts (`Split`'s `std::getline` loop drops the trailing
      empty token). Under the old `< 3` floor that shape returned before ever
      calling `SetConnectMessage` or `SetStatus` — an edit that committed
      nothing and showed nothing, indistinguishable from success. Under the
      new `< 2` floor it reaches `SetConnectMessage` with an empty hex text,
      which is refused there with a visible `Refused: ...` status, the same
      as any other malformed edit.
      Check: `TestConnectMessageEditCommitsValidAndRefusesInvalidUnchanged`
      (`controllers_page_ui_tests.cpp`) dispatches `"0:0:"` after a known-good
      commit and asserts `harness.commits` does not advance and
      `surface.StatusText()` starts with `"Refused"` — proving the empty
      commit is refused visibly, not dropped silently.
- [x] 2.3 Check `Commit()`'s return value in the three connect-message
      handlers (`HandleConnectMessageCommit`, `HandleConnectMessageDelete`,
      `HandleConnectMessageAdd`), which previously called it unchecked --
      a host rejection there silently reported success, unlike every other
      commit path in this file. Introduce `kHostRejectedCommitStatus` for the
      refusal text this shares with the wizard/add paths, then sweep the rest
      of the header for the same shape of duplicated literal and consolidate
      the six other families found (`Refused: invalid controller identity`,
      `Refused: generated controller record is invalid`,
      `Refused: controller record changed; refresh and try again`,
      `Refused: current controller state is unavailable`, the runtime-config
      save-failure text, and the wizard's controller-profile-generation-
      failure text) into named constants the same way.
      Check: `grep -c` for each of the seven literal strings above in
      `ControllersPageUI.hpp` returns 0 outside their own constant
      definitions, and the miniapp JUCE test binaries and the JUCE-free
      suite build and pass with the constants in place. A behavioural test
      (`TestConnectMessageHandlersRefuseVisiblyWhenHostRejectsCommit`,
      `controllers_page_ui_tests.cpp`) drives each of the three handlers with
      the harness's `commitSucceeds = false` knob and asserts the commit
      attempt is refused visibly; reverting one handler's return-value check
      makes that test fail (`terminating due to uncaught exception of type
      std::runtime_error`), confirmed and restored to green.

## 3. Fix the Browser E2E Spec's Device Assertions

- [x] 3.1 Fix the manually-added-record test's `.device` assertion, which
      expects the old kind-identity string. The row that test builds adds
      through Custom and binds no endpoints, so it renders neither a preset
      device name nor a stored endpoint label.
      Check: the assertion names what that row actually renders for an
      unresolved wizard id with no bound input, confirmed by running the
      test, not by reading the renderer.
- [x] 3.2 Update the add flow that test drives. `runtime.controllers.add_name`
      and `runtime.controllers.add_kind` no longer exist; add through the
      current Preset combo. `runtime.controllers.add_button` does still exist
      and is the current add row's Add button — keep it.
      Check: no locator in the test targets `add_name` or `add_kind`, the
      locator for `add_button` still resolves, and every assertion downstream
      of the add flow matches a Custom row's actual fields.
- [x] 3.3 Run the suite and report its result against the state this change
      inherited: ten of seventeen tests fail for a reason that predates this
      work, where the add controls were removed without every reference to
      them being enumerated. `npm ci` in `projects/synth/browser` is approved
      by the operator.
      Check: the run's own pass/fail counts are reported as measured. Tests
      this change did not touch are not claimed as fixed, and a suite still
      short of green is reported as still short of green.
      Measured: `npx playwright test tests/fake-app.e2e.spec.ts --workers=1`
      reports 8 passed, 9 failed, of 17. The one test those tasks touch,
      "controller wizard actions are absent on a manually added record," is
      among the 8 passing. The remaining 9 failures predate this work (the
      removed `add_name`/`add_kind` controls, never fully enumerated when
      they were deleted) and this change does not touch or claim to fix
      them.

## 4. Delivery

- [x] 4.1 Capture the six Controllers-page states named in the proposal's
      Delivery Gate and share them for the operator's review before this
      merges to main.
- [x] 4.2 Remove the uncommitted scratch measurement function from
      `ControllersPageSimulationTests.cpp` before delivery.
      Check: `git status` in the submodule shows no scratch artifact, and
      `git diff` contains no function labelled scratch.

## Cut from this change, with the evidence that cut it

- **Widening the device-label width check.** The premise was that the check
  measures only the library's own fallback names and might miss a longer real
  one. Measured against the same glyph technique and the same allowance the
  existing cases use: the longest real device name renders at 141.1px and the
  bound-device fallback label at 221.6px, against an allowance of 308px. The
  instrument was proven live first — one character measured 6.9px against 552.8px
  for eighty. There is no defect here to catch, the proposed literal would have
  been a third unsynchronised copy of a name owned by another repository, and
  the widening step it carried was impossible anyway: the two header-width
  `static_assert`s it promised to keep already hold with zero slack, so any
  widening breaks them.
- **A test for editing or deleting a connect message at an index that does not
  exist.** Indices reach the handler from the rendered row, so no player can
  produce one that is out of range. The refusal a player can actually reach —
  text that is not a complete SysEx message — is already covered.
- **A comment recording which commit broke the browser suite.** A comment
  states what the code does, not what happened to it.
