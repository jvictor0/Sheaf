# Tasks — `block-end-fields-show-the-last-control`

Every new test is shown to fail against the behaviour it guards, by the
executor, before the task is reported done, and the report says so: for a
test of the page's end fields, against the base's read and write path
(`BlockLastFromEnd` and `BlockEndFromLast` replaced by returning their `end`
and `last` arguments unchanged); for a test of a header or a reason, against
the base's text. The executor deletes the test binary before each rebuild in
a break-and-restore sequence. A test not named in these tasks that goes red
is reported, never edited. Builds run under `nice`, `-j2`, one build or
suite at a time.

The open tasks: 1 to 11, task 7 dropped (nothing draws block summaries) and
task 12 discharged by frogg3rs task 11.8. Tasks 1 and 2 run first; task 3
onward runs after the preflight over both trees that frogg3rs' change
schedules.

- [x] 1. Hygiene sweep of the files this change edits
      (`include/synth/MidiConfigBlocks.hpp`, `src/MidiConfigBlocks.cpp`,
      `include/synth/MidiConfigViewModel.hpp`, `src/MidiConfigViewModel.cpp`,
      `include/synth/ControllersPageUI.hpp`, `tests/blocks_tests.cpp`,
      `tests/viewmodel_tests.cpp`, `tests/controllers_page_ui_tests.cpp`,
      `juce/ControllersPageSimulationTests.cpp`, `docs/coverage.md`), fixed
      in this change: dead code, names and paths that do not resolve, and
      comments or documentation that are false. Comments that name a
      planning label are rewritten only where this change edits the comment.
      Found while writing this change, and fixed here:
      - the `MidiMappingRowVM::RowGroup` comment in
        `include/synth/MidiConfigViewModel.hpp` points to
        `ColumnHeadersForGroup()`, which nothing defines; the header strings
        come from `FieldShortLabel` alone;
      - the comment above `ApplyEncoderBlockField` says it applies "a system
        Block row's field edit to a scratch copy of `block`"; it heads four
        functions, and `ApplyMappingEdit` passes them the open section's own
        block;
      - the comment above `BlockFieldValue` says the summary builders share
        it "indirectly ... via their own direct field access"; they do not
        call it;
      - `docs/coverage.md` heads the grid coverage "`sru-26` and `sru-27`";
        the grid requirements are sru-28 and sru-29.
      Check: the report lists FOUND and CHANGED per item and names each file
      swept; `git grep -n -w ColumnHeadersForGroup -- projects/synth` prints
      nothing; Sheaf's full `projects/synth` suite, run by binary after the
      fixes, matches its counts before them.
- [x] 2. The operator's rulings, written here before task 3 starts:
      - O1 ruled: one header per field, true on every row it heads; where a
        field's meaning differs by a block's form or address type, the row
        carries its own field. NEW `Field::BlockStartGesture`, NEW
        `Field::BlockStartNote` and NEW `Field::BlockEndNote`, appended in
        that order after `Field::ShiftAction`. Headers: `Field::BlockStartCc`
        "Start CC", `Field::BlockEndCc` "Last CC", `Field::BlockStartNote`
        "Start Note", `Field::BlockEndNote` "Last Note",
        `Field::BlockStartGesture` "Start Gesture"; `Field::BlockStartArg`
        keeps "Start Arg" and heads system blocks only. An encoder-push or
        generic system block whose `controlType` is Note carries the two
        note fields in place of `Field::BlockStartCc` and
        `Field::BlockEndCc`; an analog block carries
        `Field::BlockStartGesture` in place of `Field::BlockStartArg`. The
        page's header-row rule is unchanged. This task adds sru-67's
        scenario "Note and analog blocks head their fields by what they
        set" and replaces the delta's O1 paragraph with one sentence
        stating this ruling. Individual rows' "CC" over a note number and
        the grid button's range headers are named to the operator as
        outside this story.
      - O2 is ruled fixed here: task 10 runs.
      Check: this task names the O1 ruling; tasks 4, 5 and 6 name the three
      new fields; sru-67 holds the O1 scenario;
      `openspec validate --strict block-end-fields-show-the-last-control`
      passes.
- [x] 3. Block model: NEW `BlockLastFromEnd(start, end, mayRunDownward)` and
      NEW `BlockEndFromLast(start, last, mayRunDownward)`, declared in
      `include/synth/MidiConfigBlocks.hpp` with a comment stating what "last"
      means (the covered control farthest from the start; `end - 1` on a
      range that runs upward; `end - d` on a y range, `d` being +1 when end
      is above start and -1 when below; a last y at or above the start
      stores `last + 1`, one below stores `last - 1`). NEW
      `BlockRowDirection(start, end)` in `src/MidiConfigBlocks.cpp` is the
      y direction rule, and `VisitRectangle` and `ExpandGridBlock` call it in
      place of their own `yDir` and `yDirection`. The expansion reasons the
      proposal lists under "What each end field accepts" change to its new
      text; `ExpandGridBlock`'s combined empty-range check splits into the x
      and y reasons it lists.
      Check: NEW `BlockLastAndEndTranslateEveryRangeBothWays` in
      `tests/blocks_tests.cpp` passes: on an upward range, end 16 reads last
      15 and last 15 stores end 16; on a y range, (start 3, end 1) reads last
      2, (start 3, last 2) stores end 1, (start 3, last 3) stores end 4 and
      (start 3, last 5) stores end 6; every pair round-trips. It fails with
      both functions returning their argument unchanged. The existing
      `tests/blocks_tests.cpp` cases pass unchanged.
      `git grep -n -E "yDir =|yDirection =" -- projects/synth/src/MidiConfigBlocks.cpp`
      prints only the reconstruction's `yDir = 0` and `yDir = delta` lines.
- [x] 4. Read path: `BlockFieldValue` returns `BlockLastFromEnd` for
      `Field::BlockEndCc` (encoder, analog and generic system blocks,
      upward), `Field::BlockEndX` and `Field::GridXMax` (upward), and
      `Field::BlockEndY` and `Field::GridYMax` (either way). It also reads
      `Field::BlockEndNote` through `BlockLastFromEnd`, `Field::BlockStartNote`
      as the start, and `Field::BlockStartGesture` as the analog block's
      `startGestureIx`.
      Check: NEW `BlockEndFieldsShowTheLastControlTheyCover` in
      `tests/viewmodel_tests.cpp` passes: through `RowFieldValue`, an analog
      gesture block on ccs 20..23 reads 23, a generic scene-select block on
      ccs 40..47 reads 47, the default WRLD.Bldr scene-select block reads
      last x 7 and last y 6, the default WRLD.Bldr bank-select block reads
      start y 3 and last y 2, a grid block over `[0,2) x [0,1)` reads last x
      1 and last y 0, a Launchpad grid block over `[0,8) x [-1,7)` reads
      start x 0, last x 7, start y -1 and last y 6, and a Note push block on
      notes 60..61 reads last note 61. It fails against the base's read
      path.
- [x] 5. Write path: the four `Apply*BlockField` functions store
      `BlockEndFromLast` of a typed end. `ApplyEncoderBlockField` and
      `ApplySystemBlockField` take `Field::BlockStartNote` and
      `Field::BlockEndNote` as they take `Field::BlockStartCc` and
      `Field::BlockEndCc`. `ApplyAnalogBlockField` takes
      `Field::BlockStartGesture` into `startGestureIx` in place of
      `Field::BlockStartArg`, keeping the refusal "start gesture index must
      be a non-negative integer". A CC or note end accepts an integer 0 to
      127, and an x or y end accepts -2147483647 to 2147483646. The field
      refusals take the text the proposal lists, including
      `ApplyGridButtonField`'s; a note field's refusal says "start note" or
      "last note" where a CC field's says "start cc" or "last cc". A typed
      `Field::BlockStartY` or `Field::GridYMin` reads the block's last y from
      its current start and end, stores the new start, and stores the end
      `BlockEndFromLast` gives for the new start and that last y.
      Check: these NEW cases in `tests/viewmodel_tests.cpp` pass, each red
      against the base's write path:
      - `TypingABlocksLastControlKeepsEveryControl`: on a row whose sixteen
        turns on ccs 0..15 form one block, typing 15 into the end field
        commits sixteen turns on ccs 0..15;
      - `LastYRunsTowardTheTypedRowFromTheStart`: the sru-67 scenario "A
        typed last y sets the direction from the start", reading the
        committed rows in expansion order;
      - `EditingAStartYKeepsTheShownLastY`: the sru-67 scenario "Editing the
        start y keeps the last y"; on the base the edit is refused;
      - `EndFieldsRefuseValuesNoBlockCanEndOn`: 127 is accepted in a CC end
        field; 128 is refused with "last cc must be an integer 0-127" and
        2147483647 in an x end field with its range, each with
        `presentationChanged` false, and 128 in a Note push block's
        last-note field is refused with "last note must be an integer
        0-127";
      - `BlockEndRefusalsNameStartAndLast`: a CC end below its start leaves
        the typed last in the open section (`presentationChanged` true, the
        field reading the typed value) and the refusal names "last cc" and
        "start cc"; the same for a 2-D system block's x and a grid block's
        x, and for a Note push block, which names "last note" and "start
        note";
      - NEW `AnalogBlockStartGestureWritesThroughItsOwnField`: on an analog
        block over ccs 20..23 with first gesture 0, typing 4 into
        `Field::BlockStartGesture` commits gestures 4..7 on ccs 20..23, and
        `Field::BlockStartArg` is refused on that row.
      Each new assertion is red with the new field's case removed.
- [x] 6. First, at Froggers' narrowest host, render an analog block and a
      Note-addressed push block and record whether "Start Gesture", "Start
      Note" and "Last Note" show in full in the column width, not
      truncated. If any is cut off, stop and report before the rest of this
      task runs.
      Headers and fields: `FieldShortLabel` returns "Last CC" for
      `Field::BlockEndCc`, "Last X" for `Field::BlockEndX` and
      `Field::GridXMax`, "Last Y" for `Field::BlockEndY` and
      `Field::GridYMax`, "Start X" for `Field::GridXMin`, "Start Y" for
      `Field::GridYMin`, "Start Note" for NEW `Field::BlockStartNote`,
      "Last Note" for NEW `Field::BlockEndNote`, and "Start Gesture" for
      NEW `Field::BlockStartGesture`, the three appended in that order
      after `Field::ShiftAction`. "Start CC" and "Start Arg" are unchanged.
      `EncoderBlockEditableFields` (push) and `SystemBlockEditableFields`
      (generic) give a Note-addressed block the two note fields in place of
      the CC pair; `AnalogBlockEditableFields` gives
      `Field::BlockStartGesture` in place of `Field::BlockStartArg`.
      `FieldIsInteger` returns true for the three new fields;
      `ControllersLayout::FieldEditorWidth` gives `Field::BlockStartNote` and
      `Field::BlockEndNote` the width of `Field::BlockStartCc` (66px), and
      `Field::BlockStartGesture` 70px: real glyph measurement
      (`juce::GlyphArrangement` against `pagestyle::kDefaultTextStyle`, the
      font a header Label renders at) gives "Start Gesture" 69.705px, 66px
      truncates it, and 70px is the smallest whole-pixel width that does not;
      `SafeValueFor` and `SafeValueForRow` treat them
      as `BlockStartCc`, `BlockEndCc` and `BlockStartArg`. The header-row
      rule is unchanged.
      Check: the narrowest-host render is recorded, and nothing is cut off,
      before the rest of this task's Check is attempted. NEW
      `BlockFieldHeadersSayWhatTheFieldHolds` in `tests/viewmodel_tests.cpp`
      passes. It asserts each header above, and, through `SectionRows`:
      - a turn block, a CC push block and a CC generic system block carry
        the CC pair;
      - Note push and generic system blocks carry the note pair and no CC
        field;
      - typing Note into a CC push block's `Field::AddressType` switches it
        to the note pair;
      - an analog block carries `Field::BlockStartGesture` and not
        `Field::BlockStartArg`;
      - a generic scene-select block carries `Field::BlockStartArg`.
      Header assertions are red against the base's text, and field-list
      assertions are red with the builders returning the base's lists.
      `portable_ui_tests.cpp: TestControllersRowFitsWithinFroggersNarrowestHost`
      passes.
- [ ] DROPPED 7. Summary strings. A command settled it: no page code draws a
      block row's `label` (`ControllersPageUI.hpp` never reads it; the only
      `row.label` drawn anywhere is `RuntimePages.hpp`'s file-list rows, a
      different row type). No player sees `EncoderBlockLabel`,
      `AnalogBlockLabel`, `SystemBlockLabel` or `GridBlockLabel`'s output, so
      translating it serves no story. The four summary builders are
      unchanged from the base.
- [x] 8. Rewrite the tests whose typed ends change meaning, as the
      proposal's table gives them, and correct the comments the change makes
      false: the `MidiMappingRowVM::Field` comments that call a block's CC
      pair `[start,end)` and say a grid block adds exclusive maxima, and the
      comment above `SafeValueForRow`. Also correct the comment in
      `BlockEditOverlappingExistingSceneButtonRefused` that says the edit
      moves the scene-select block's row from y=6 down to y=0: the edit
      stretches the block's row range, it does not move it. The
      `MidiMappingRowVM::Field` form-list comment in
      `include/synth/MidiConfigViewModel.hpp` lists NEW
      `Field::BlockStartGesture`, `Field::BlockStartNote` and
      `Field::BlockEndNote` under the forms that carry them.
      Check: each of `BlockEditAllOrNothingRefusalLeavesConfigUnchanged`,
      `GridInvalidRectangleAndDuplicatePairEditsRefuseAtomically` and the
      invalid-rectangle step of `RunGridSimulation` is run unmodified against
      the new code and recorded red, then rewritten, and passes;
      `BlockEditOverlappingExistingSceneButtonRefused` and
      `GenericSystemBlocksRandomizedOpenSessionOracle` pass as rewritten and
      are recorded red against the base; `EveryEditableFieldOnEveryDefaultProfileRowSucceeds`
      passes; the comments say what the fields hold and what the edit does.
- [x] 9. Page: the Controllers page's built tree renders each block's end
      field as the last control it covers, headed correctly, through
      `ControllersPageSurface`: on a row whose sixteen turns on ccs 0..15
      form one block, the block's end field renders "15" and its column
      header "Last CC"; on a WRLD.Bldr row, the bank-select block's y fields
      render "3" and "2" under "Start Y" and "Last Y"; and on a Generic row
      with Note pushes on notes 60 and 61 at positions 0 and 1 and Note
      scene-select associations on notes 40 and 41 (the pairs `main`'s
      address-type fixture builds), the push block's number fields render
      "60" and "61" and the system block's "40" and "41", under "Start
      Note" and "Last Note". The operator confirms this by screenshot
      rather than an automated page test.
      Check: operator step: the Delivery Gate screenshot of a Twister row's
      encoder section, its block reading its last CC under 'Start CC'/'Last
      CC'.

      The operator approved the story-5 screenshots in the coordinating
      session.
- [x] 10. `ExpandGridBlock` and `ExpandSystemBlock`'s 2-D branch check the
      start and last corner cells against the controller's grid through the
      existing shape checks before enumerating or reserving: in
      `ExpandGridBlock` the check goes ahead of its `size_t` overflow guard;
      in `ExpandSystemBlock` it goes ahead of
      `StartPlusCountExceedsDomain(block.startArg, block.CellCount())`,
      which computes `endX - startX` in `int`. First, a program outside the
      tree, built against the base, times `ExpandSystemBlock` on a WRLD.Bldr
      block with x `[0, 2147483647)` on one row and `ExpandGridBlock` on a
      Launchpad block with x `[0, 1073741824)` and y `[0, 1024)`, catching
      any exception; its command and output go in the report.
      Check: operator step: task 10's fixed-code timing run, recorded in
      the proposal's Evidence, refuses a WRLD.Bldr system block's last x
      `1073741824` and a Launchpad grid block's last x `1073741824` and
      last y `1024`, each at once, in 0.0 ms, with the off-grid reason; the
      base timing run alongside it shows the stall and the exception the
      corner check removes.
- [x] 11. Once each test named on a "not yet delivered" Check line in this
      change's `specs/` exists, rewrite that line as an ordinary Check line
      naming the file and case in backticks.
      Check: no "not yet delivered" line is left in this change's `specs/`;
      `openspec validate --strict block-end-fields-show-the-last-control`
      passes.

      A non-author read found four sru-67 clauses no named test backs (last
      y typed 3 and 1; the row order after a start-y edit; a Note block's
      end field values; a switch back to CC), and one clause ("setting the
      push block's address type to CC...") left in its original scenario
      after being copied into a new one. The four untested clauses' scenarios
      are removed rather than carried as "not yet delivered": their
      behaviour is already stated in sru-67's requirement body (the last-y
      direction rule and the start-y edit both there already), so a
      scenario is kept only where a named test asserts it, and the CC-switch
      clause is removed from its original scenario. No "not yet delivered"
      line is left; `openspec validate --strict` passes.
- [x] 12. Discharged by frogg3rs `frogg3rs-transport-and-shift-ux` task
      11.8's full run (both full suites, run once, on the exact tree to be
      committed); not run separately here.
