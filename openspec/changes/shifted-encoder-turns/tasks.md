# Tasks — `shifted-encoder-turns`

Every new test is shown to fail with its production change reverted, by the
executor, before the task is reported done, and the report says so. Builds
run under `nice`, `-j2`.

The open task: 10.

- [x] 1. Add the scene-blend increment: NEW `MessageIn::Type::SceneBlendIncDec`
      appended after `Shift`, its factory, NEW `ParameterManager::IncDecSceneBlend`
      beside `ParameterManager::SetSceneBlend`, its `MessageInBus::Apply` case,
      and a case at every site in the proposal's `SetSceneBlend` family.
      Check: NEW `SceneBlendIncrementAddsToTheBlendAndClamps` passes; the
      proposal's Evidence lists each family site as changed or not needed,
      with the reason for the one not needed.
- [x] 2. Add the shifted job to the turn mapping: NEW `EncoderShiftedJob`
      (`None`, `SceneBlend`) as `shiftedJob` on `EncoderMidiMapping`, its JSON
      key written only when set, a missing key read as none, an unknown value
      failing the load, and a push mapping carrying one reported invalid by
      profile validation.
      Check: NEW `EncoderTurnJsonRoundTripsShiftedJobAndRejectsAnUnknownOne`
      and NEW `ProfileWithAShiftedEncoderPushIsInvalid` pass.
- [x] 3. Hand the profile's `ShiftState` to `EncoderMidiInProcessor` and
      handle a turn in the order the proposal gives: Hold Drill, then the
      shifted job while Shift is held, then the turn as on the base. Correct
      the `ShiftState` comment.
      Check: NEW `ShiftHeldTurnPushesSceneBlendIncrementAndReleaseRestoresTheParameter`,
      NEW `ShiftHeldAbsoluteTurnSetsTheSceneBlend` and
      NEW `HoldDrillDrillsAShiftedTurnWhileBothAreHeld` pass, and the existing
      `ShiftAndHoldDrillAreIndependent` still passes.
- [x] 4. Keep a turn with a shifted job out of `ReconstructEncoderBlocks`
      runs, and have a block expand to turns with no shifted job.
      Check: NEW `ReconstructEncoderBlocksKeepsAShiftedTurnOutOfItsBlock`
      passes.
- [x] 5. Controllers page: give Individual encoder-turn rows the Shift field
      with the choices none and Scene Blend, committed through the row's
      existing flush path; show the Shift field on system rows and turn rows
      only when the row dropdown offers Shift, as NEW
      `MessageCatalogOffersShift` answers it. This reaches these sites: the
      turn-row field list, which `BuildSectionRows` and
      `MidiConfigViewModel::GroupColumnFields` both read from NEW
      `EncoderTurnEditableFields`, the way system rows share
      `SystemRowEditableFields`; `ApplyMappingEdit`'s `EncoderMidiMapping`
      branch, whose `ShiftAction` case maps 0 to no shifted job and 1 to
      Scene blend; the `Field::ShiftAction` combo branch in
      `include/synth/ControllersPageUI.hpp`, which for a turn row offers
      NEW `EncoderShiftedJobCatalog()` and selects NEW
      `MidiConfigViewModel::EncoderTurnShiftedJobIndex` instead of using
      `ShiftCatalog()` and `ShiftChoiceIndex()`; and `SystemRowEditableFields`.
      Correct the `Field::ShiftAction` comment ("System row only") and the
      `GroupColumnFields` comment that calls the per-group field tables a
      single source of truth.
      Check: NEW `TurnShiftFieldEditCommitsSceneBlendAndNoneClearsIt`,
      NEW `TurnRowsExposeShiftFieldOnlyWhenShiftIsOffered` and
      NEW `NoShiftFieldWhenTheRowDropdownOffersNoShift` pass. NEW
      `TestTurnRowShiftComboOffersSceneBlendAndCommits` in
      `controllers_page_ui_tests.cpp` passes: on a page built with a catalog
      that offers Shift, a turn row's combo offers exactly none and Scene
      Blend, shows the turn's shifted job as selected, commits a change, and
      shows the committed value after a rebuild. The existing
      `ShiftFieldEditCommitsShiftedPressAndNoneClearsIt`,
      `SystemRowsExposeShiftFieldExceptOnShiftAndHoldDrillRows` and
      `TestSystemMessageShiftFieldRendersAndCommits` pass with a catalog that
      offers Shift; the `GroupColumnFieldsMatches…` parity tests pass with and
      without a Shift-offering catalog; `TestControllersRowFitsWithinFroggersNarrowestHost`
      passes with a shifted turn row in its open states.
- [x] 5a. Correct the comment task 5 added above the shifted turn in
      `TestControllersRowFitsWithinFroggersNarrowestHost`
      (`tests/portable_ui_tests.cpp`), which says "the Shift field this
      change adds": it describes the field by the change that added it
      rather than by what it is. Runs after task 9.
      Check: the comment names the turn row's Shift field without naming a
      change; `TestControllersRowFitsWithinFroggersNarrowestHost` passes.
- [x] 6. Run Sheaf's full `projects/synth` suite, and build and run the
      miniapp runtime target, which that suite does not build.
      Check: pass and fail counts reported per binary as measured; every
      failure is either fixed here or shown to fail identically at the base.
- [x] 7. In `ControllersPageSurface`, configure the view model
      `CommitLifecycleAction` builds exactly as the surface's own view model is
      configured (message catalog, analog action catalog, layouts from
      `ControllersPageCallbacks`), through NEW
      `ControllersPageSurface::ConfigureViewModel`, which both the constructor
      and `CommitLifecycleAction` call. On a successful Restore,
      `HandleRestoreController` drops that controller's cached section rows
      on the surface's own view model through the public NEW
      `MidiConfigViewModel::NoteControllerConfigReplaced`, beside
      `NoteControllerRenamed`, so an open section shows the restored mappings.
      Check: NEW `TestRestoreResolvesAnAppPreset` in
      `controllers_page_ui_tests.cpp` passes: on a page built with a registry
      made from a catalog whose device default has an app-specific wizard id,
      a row carrying that id whose config diverges from the preset is
      restored to the preset by the page's Restore action. It fails with the
      old `CommitLifecycleAction`. No test in this repository covers the
      cache drop; frogg3rs' `twister_row_saved_before_the_shifted_turn_gains_it_on_restore`
      does, and fails without it.
- [x] 8. Rebase the branch onto `e8894727`, the tip of
      `fold-controller-wizard-into-add-row` (#19). That base's Controllers
      page has no Release or Reclaim action, so task 7 keeps only its
      Restore half.
      Check: `git merge-base HEAD e8894727` prints `e8894727…`;
      `TestRestoreResolvesAnAppPreset` is defined in
      `tests/controllers_page_ui_tests.cpp`.
- [x] 9. Rebase the branch onto `f6266560`, the Sheaf commit frogg3rs `main`
      pins at `37c1b9c`, and update every statement in this change that
      names the base (`e8894727`, #19 as the stack tip) to the new one; an
      Evidence block keeps the commit it ran at and says so.
      Check: `git merge-base HEAD f6266560` prints `f6266560…`;
      `openspec validate --strict shifted-encoder-turns` passes.
- [x] 10. After the last edit, run task 6 again on the rebased tree before
      the push.
      Check: pass and fail counts reported per binary as measured, from a run
      started after the last edit; every failure is either fixed here or
      shown to fail identically at `f6266560`.

      Discharged by frogg3rs `frogg3rs-transport-and-shift-ux` task 11.8's
      full run of both suites and the miniapp target, on this exact tree
      (Sheaf `20606fda`); not run separately here.
