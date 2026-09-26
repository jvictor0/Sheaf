# Tasks — `individual-address-fields-say-what-they-hold`

Every new or changed assertion is shown to fail against the base's field
lists (`EncoderPushEditableFields`/`SystemRowEditableFields` always
returning `Field::Cc`) before this task is reported done, by the executor,
and the report quotes the red and green runs. Builds run under `nice`,
`-j2`, one at a time; the test binary is deleted before each rebuild.

- [x] 1. Hygiene sweep of the files this change edits
      (`include/synth/MidiConfigViewModel.hpp`, `src/MidiConfigViewModel.cpp`,
      `include/synth/ControllersPageUI.hpp`, `tests/viewmodel_tests.cpp`),
      fixed in this change: names and paths that do not resolve, and
      comments made false by this change. Found while writing this change,
      and fixed here: `MidiConfigViewModel.hpp`'s `GroupColumnFields` doc
      comment names `EncoderPushEditableFields()` with no argument; the
      function now takes the row's `MidiControlType`.
      Check: the report names each file swept; `git grep -n -w
      EncoderPushEditableFields -- projects/synth/include` shows only the
      corrected comment and the declaration.
- [x] 2. NEW `Field::Note`, appended after `Field::BlockEndNote`
      (`MidiConfigViewModel.hpp`). `FieldIsInteger(Field::Note)` is true;
      `FieldShortLabel(Field::Note)` returns "Note"; `FieldEditorWidth` gives
      it `Field::Cc`'s 66px.
      Check: `FieldIsIntegerTrueForIndexAndCoordinateFields` and
      `FieldShortLabelIsNonEmptyAndDistinctPerField` (`viewmodel_tests.cpp`)
      pass with `Field::Note` added to each.
- [x] 3. `EncoderPushEditableFields` takes the row's `MidiControlType` and
      returns `Field::Note` in place of `Field::Cc` when it is Note;
      `SystemRowEditableFields` does the same from the row's own
      `association.control`. Both call sites of each function are updated
      (`BuildSectionRows` passes the row's live control type;
      `GroupColumnFields` passes CC, a fresh row's own default).
      `RowFieldValue` and `ApplyMappingEdit`'s `EncoderMidiMapping` and
      `MidiControllerSystemMessageAssociation` branches read/write
      `Field::Note` exactly as `Field::Cc`, except the out-of-range refusal
      text, which names "note" or "cc" by which field was given.
      Check: NEW assertions in `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`
      (`viewmodel_tests.cpp`) pass: an individual push row and an individual
      Generic system row each show `Field::Cc` headed "CC" while
      CC-addressed, switch to `Field::Note` headed "Note" (same number)
      when switched to Note, refuse `Field::Cc` and an out-of-range
      `Field::Note` value by name while Note-addressed, and revert to
      `Field::Cc` when switched back. Each new assertion is shown red
      against the base (`EncoderPushEditableFields`/`SystemRowEditableFields`
      always returning `Field::Cc`) before this task is reported done.
      `SafeValueFor`/`SafeValueForRow` (test helpers) gain `Field::Note`
      alongside `Field::Cc` so the switch stays exhaustive.
- [x] 4. Build and run `viewmodel_tests` and `controllers_page_ui_tests`
      (both link `MidiConfigViewModel.cpp`/`ControllersPageUI.hpp`;
      `blocks_tests.cpp` includes neither, confirmed by
      `grep -n "#include" tests/blocks_tests.cpp`, so it is not rebuilt for
      this change). `build/*.o` and `build/libsynth.a` are deleted first,
      since this change edits headers.
      Check: both binaries build clean under `-Wall -Wextra -Wpedantic`
      with no new warning; `viewmodel_tests` reports 165 `[PASS]` and 0
      `[FAIL]`; `controllers_page_ui_tests` reports "controllers_page_ui_tests
      passed", exit 0.
- [x] 5. `openspec validate --strict individual-address-fields-say-what-they-hold`
      passes.
