# Proposal — `block-end-fields-show-the-last-control`

This change modifies the promoted sru-28 and adds sru-67
(`shifted-encoder-turns` added sru-66). It leaves sru-10 as
`shifted-encoder-turns` modifies it (Decisions, "sru-10 and sru-28"). It is
written on the branch `shifted-encoder-turns`, at that branch's tip
`ee679e48`, and ships in the same pull request: the branch carries two
openspec changes. Paths are relative to `projects/synth/`, and code is named
by symbol. Its spec delta is in this change's `specs/`.

## Why

A block row on the Controllers page stands for a run of controls. Its end
field shows the block model's exclusive end, one past the last control the
block uses, and a value typed into it is stored as that exclusive end. The
columns are headed "End CC", "End X", "End Y", "X Max" and "Y Max". A MIDI
Fighter Twister row's sixteen encoders on CCs 0 to 15 read "Start CC 0, End
CC 16", and CC 16 is none of its encoders. A player who types 15, the last
encoder's CC, keeps fifteen turns and silently loses the sixteenth.

The player needs every block field that names an end to show, and accept,
the last control the block uses, and every block field's header to say what
the field does.

On the base:

- The block model stores half-open ranges: `EncoderBlock`, `AnalogBlock`,
  `SystemBlock` (the generic cc run and the WRLD.Bldr and Launchpad x/y
  rectangle) and `GridBlock` each hold `[start, end)`. A run of sixteen
  turns on CCs 0 to 15 reconstructs to a block with `endCc` 16
  (`ReconstructEncoderBlocksMergesSixteenConsecutiveIntoOneBlock` asserts
  it).
- An x range only runs upward: every cell count and expansion refuses
  `endX <= startX`. A y range runs either way: a rectangle's rows run from
  `startY` toward `endY`, refused only when the two are equal. The default
  WRLD.Bldr bank-select block runs downward, rows 3 then 2, stored as
  `startY` 3 and `endY` 1 (`RoundTripDefaultWrldBldrProfileEncodersAndSystemMessages`
  asserts it).
- `BlockFieldValue`, the view model's read path for a block row's fields,
  returns each stored end as it is. `ApplyEncoderBlockField`,
  `ApplyAnalogBlockField`, `ApplySystemBlockField` and `ApplyGridBlockField`,
  the write path, store a typed end as it is. No code between them adds or
  subtracts one.
- A CC end accepts 0 to 128, and an x or y end any `int`.
- `FieldShortLabel` gives each field one header, and the page's one render
  path, `include/synth/ControllersPageUI.hpp`, which the JUCE runtime and
  the browser runtime both include, draws it above the column.
- Blocks exist only in the view model and the block model. No serializer
  names them, so no saved document holds a block.

## What Changes

- **One translation, in the block model.** NEW `BlockLastFromEnd` and NEW
  `BlockEndFromLast` in `include/synth/MidiConfigBlocks.hpp` convert between
  a range's stored exclusive end and the last control it covers, given the
  range's start and whether the range may run downward. The y direction rule
  they need, which `VisitRectangle` and `ExpandGridBlock` each compute today,
  is defined once as NEW `BlockRowDirection` in `src/MidiConfigBlocks.cpp`,
  and both call it.
- **The page shows the last control.** `BlockFieldValue` returns
  `BlockLastFromEnd` for `Field::BlockEndCc` (encoder, analog and generic
  system blocks), `Field::BlockEndX`, `Field::BlockEndY`, `Field::GridXMax`
  and `Field::GridYMax`.
- **A typed value is the last control.** The four `Apply*BlockField`
  functions store `BlockEndFromLast` of a typed end. A typed start y
  (`Field::BlockStartY`, `Field::GridYMin`) keeps the last y the page shows:
  the edit reads the last from the old start and end, then stores the end
  that the new start and that last give.
- **What an end field accepts.** A CC end accepts 0 to 127. An x or y end
  accepts an integer -2147483647 to 2147483646, one range for both axes: a
  y end's exclusive end can move either direction (`last + 1` or
  `last - 1`), so both bounds keep it inside what an `int` holds; an x end,
  which only ever computes `last + 1`, takes the same range for
  consistency. Whether a coordinate is on the controller's grid stays the
  per-cell check expansion already makes, as for a start field. A last
  below its start on a range that only runs upward is held in the open
  section and refused on commit, as today.
- **Headers.** "End CC", "End X" and "End Y" become "Last CC", "Last X" and
  "Last Y". A grid block's "X Min", "X Max", "Y Min" and "Y Max" become
  "Start X", "Last X", "Start Y" and "Last Y". A Note-addressed
  encoder-push or generic system block carries NEW `Field::BlockStartNote`
  and NEW `Field::BlockEndNote`, headed "Start Note" and "Last Note", in
  place of `Field::BlockStartCc` and `Field::BlockEndCc`. An analog block
  carries NEW `Field::BlockStartGesture`, headed "Start Gesture", in place
  of `Field::BlockStartArg`. `Field::BlockStartArg` keeps "Start Arg" and
  heads system blocks only. The page's header-row rule is unchanged: each
  header still comes from `FieldShortLabel(field)` alone.
- **Refusals name what the page shows.** The field refusals and the
  expansion refusals an end edit can reach name the start and last, not the
  stored `endCc`, `endX` or exclusive range (listed under Decisions).
- **Tests whose typed ends change meaning are rewritten** (Decisions,
  "Tests that change meaning").
- **Comments the change makes false are corrected:** the
  `MidiMappingRowVM::Field` comments that call a block's CC pair `[start,end)`
  and say a grid block adds exclusive maxima, and the comment above
  `SafeValueForRow` in `tests/viewmodel_tests.cpp` that describes end fields
  by exclusive-end semantics.

## Decisions

**Where the translation lives.** In the block model, as `BlockLastFromEnd`
and `BlockEndFromLast`, beside the structs whose convention they state.
`BlockFieldValue` (seven end reads) and the four `Apply*BlockField` functions
(seven end writes and two start-y writes) call them, and nothing else in the
view model adds or subtracts one. The four summary builders are unaffected:
the page draws none of their output, so translating them serves no story. The
model
owns the convention: the `SystemBlock` comment defines `endX = maxX + 1` and
`endY = lastY + d`, and the direction rule already exists there twice
(`VisitRectangle`'s `yDir`, `ExpandGridBlock`'s `yDirection`). A copy in the
view model would be a third definition of the direction. FOUND 2 copies of
the direction rule, CHANGED 2 to call `BlockRowDirection`; reconstruction's
`rowDir` derives the direction from the run's second row, not from a stored
end, and is not a copy.

**What "last" means for a range that runs downward.** On each range the last
control is the one the block covers farthest from its start. On a range
that runs upward, which is every CC range and every x range, that is
`end - 1`. On a y range it is `end - d`, where `d` is +1 when `end` is above
`start` and -1 when it is below. A single row's last row is its start row.
Read the other way: a last y at or above the start y stores `end = last + 1`,
which for `last == start` is reconstruction's single-row form
(`rowDir = height == 1 ? 1 : yDir`), and a last y below the start y stores
`end = last - 1`. The default WRLD.Bldr bank-select block, stored `startY` 3
and `endY` 1, reads start y 3 and last y 2.

A typed start y keeps the last y the page shows. On the base, a start edit
stores the start and keeps the stored end, so on that bank-select block a
typed start y of 1 stores `startY == endY == 1`, which `ExpandSystemBlock`
refuses as "system block y range must be non-empty (endY != startY)"; a page
that shows the last row reads that edit as start y 1 and last y 2, two rows.
Keeping the shown last makes the edit mean what the page shows. A start x or start cc
edit needs no such step: an upward range's end does not depend on its start.

**Which headers are false, and what replaces them.** A column's header is
`FieldShortLabel(field)`, one per field, and the page emits a new header
row when the row group or the row's field list changes.

| Header | Why it is false | Becomes |
|---|---|---|
| "End CC", "End X", "End Y" | the field holds one past the last control | "Last CC", "Last X", "Last Y" |
| grid "Y Min", "Y Max" | a grid block's rows may run downward (`ExpandGridBlock` steps from `startY` toward `endY`), where the start is the largest row | "Start Y", "Last Y" |
| grid "X Max" | the field holds one past the largest column | "Last X", with "X Min" becoming "Start X", so a grid block's header reads like a 2-D system block's; `GridBlock`'s comment calls it the same address form |
| "Start CC" and the CC end on a Note-addressed encoder-push block or generic system block | the fields set note numbers | "Start Note" and "Last Note", on NEW `Field::BlockStartNote` and NEW `Field::BlockEndNote` |
| "Start Arg" on an analog block | the field sets the block's first gesture, which sru-10 names "start gesture" | "Start Gesture", on NEW `Field::BlockStartGesture` |

"X Max" is not false because of direction: no block's x range runs
downward (Evidence). The grid button shares `Field::GridXMin` and
`Field::GridYMin` with the grid block, so its header reads "Start X" and
"Start Y" where it read "X Min" and "Y Min". Both describe a range on a
one-cell row; the grid button is not a block row, so this header is
unaffected by O1's ruling. Heading it "X" and "Y" instead is outside this
story (Open, "Outside this story, named to the operator").

**What each end field accepts.**

- A CC or note end: an integer 0 to 127, refused otherwise with "last cc
  must be an integer 0-127" on a CC field or "last note must be an integer
  0-127" on a note field. The base's 128 was the exclusive end one past CC
  127.
- An x or y end: an integer -2147483647 to 2147483646, refused otherwise
  with "last x must be an integer from -2147483647 to 2147483646" (or
  "last y"), one range for both axes. On a y range, which can run either
  way, the two excluded values are the ones whose exclusive end an `int`
  cannot hold (`last + 1` above `INT_MAX`, `last - 1` below `INT_MIN`); an x
  range only ever computes `last + 1`, so only the upper exclusion is
  load-bearing for it, and it keeps the same range as y so there is one
  accepted range across both axes. A coordinate off the controller's grid
  is refused by the per-cell shape checks expansion already makes, as for a
  start field.
- A last below its start on a range that only runs upward: the open
  section keeps the typed value and the commit is refused with the page's
  "Warning:" status, as any invalid block edit is today (sru-11's open
  session; `BlockEditOverlappingExistingSceneButtonRefused` asserts the
  kept edit). The reason names the start and last. Reasons an end edit can
  reach, and what they become:

| Where | On the base | Becomes |
|---|---|---|
| `ApplyEncoderBlockField`, `ApplyAnalogBlockField`, `ApplySystemBlockField` (`Field::BlockEndCc`, NEW `Field::BlockEndNote`) | "end cc must be an integer 0-128" | "last cc must be an integer 0-127" on a CC field, "last note must be an integer 0-127" on a note field |
| `ApplySystemBlockField` (`Field::BlockEndX`, `Field::BlockEndY`) | "end x must be an integer", "end y must be an integer" | "last x must be an integer from -2147483647 to 2147483646", same for "last y" |
| `ApplyGridBlockField` (`Field::GridXMax`, `Field::GridYMax`, `Field::GridXMin`, `Field::GridYMin`) | "x max", "y max", "x min", "y min" must be an integer | "last x", "last y", "start x", "start y"; the last fields carry the range above |
| `ApplyGridButtonField` (`Field::GridXMin`, `Field::GridYMin`) | "x min", "y min" must be an integer | "start x", "start y", matching the shared header |
| `ExpandEncoderBlock`, `ExpandAnalogBlock`, `ExpandSystemBlock` (generic) | "... cc range must be non-empty (endCc > startCc)" | "encoder block's last cc is below its start cc", and "analog block's", "system block's"; on a Note-addressed block, "encoder block's last note is below its start note" and "system block's last note is below its start note" |
| `ExpandSystemBlock` (2-D) | "system block x range must be non-empty (endX > startX)" | "system block's last x is below its start x" |
| `ExpandGridBlock` | "grid block rectangle must have non-empty exclusive ranges" | "grid block's last x is below its start x" when x is empty; "grid block y range is empty" when y is |

"system block y range must be non-empty (endY != startY)" stays: no page
edit reaches it once the translation never stores `endY == startY`, and it
describes a struct built that way directly.

**Tests that change meaning.** Found by grepping every test file for the
end fields (Evidence); `tests/blocks_tests.cpp` and `tests/portable_ui_tests.cpp`
name none, and the first builds block structs directly, whose storage does
not change. `tests/controllers_page_ui_tests.cpp` names no end field
either, but its grid section's field-label check, in `main()`, asserts the
portable tree's visible text against the base's literal grid headers ("x
min", "x max", "y min", "y max"), which the header rename (Decisions,
"Which headers are false") makes false; that check is also in the table
below. Each test below is run unmodified against the new code and recorded
red before it is rewritten; each rewritten assertion that names a last
control is recorded red against the base.

| Test | What changes | Rewrite, and how it is shown |
|---|---|---|
| `BlockEditAllOrNothingRefusalLeavesConfigUnchanged` (`tests/viewmodel_tests.cpp`) | it types end cc 0 on a block starting at cc 0 to force an empty range; 0 is now a valid last, a one-cell block, so the edit commits | it forces the refusal with a start cc above the shown last cc; unmodified, it goes red on the new code |
| `BlockEditOverlappingExistingSceneButtonRefused` | the second typed edit -- itself refused, like the first, for the duplicate address -- now stores endY 1, not 0 (BlockEndFromLast), so its kept-but-refused summary reads "(0,0)..(8,0)" on the base | it asserts "(0,0)..(8,1)"; that assertion is red on the base |
| `GenericSystemBlocksRandomizedOpenSessionOracle` | its oracle reads and writes `Field::BlockEndCc` as the exclusive end | the oracle keeps exclusive ends inside and converts at the view-model boundary (reads compare with `endCc - 1`, writes send `newEnd - 1`); red on the base |
| `GridInvalidRectangleAndDuplicatePairEditsRefuseAtomically` | x max 0 on a block starting at x 0 is now a valid one-column block; its repair value 2.0 is now a three-column block | it types last x -1 to force the refusal and repairs with last x 1; unmodified, it goes red on the new code |
| `RunGridSimulation` (`juce/ControllersPageSimulationTests.cpp`) | its invalid-rectangle step types the block's x min as x max, now a valid one-column block | it types x min minus one; unmodified, it goes red on the new code |
| `EveryEditableFieldOnEveryDefaultProfileRowSucceeds` through `SafeValueForRow` | writes back each field's shown value, so it still passes; the comment above `SafeValueForRow` becomes false | the comment only |
| grid section's field-label check (`tests/controllers_page_ui_tests.cpp`, `main()`) | it checks the portable tree's visible text for the base's grid headers "x min", "x max", "y min", "y max" | it checks "start x", "last x", "start y", "last y" instead; unmodified, it goes red on the new code |

**sru-10 and sru-28.** sru-10 states the block model — the reconstructed
blocks' fields, "end cc exclusive" and the "exclusive-end x/y rectangle" —
which this change keeps, so it needs no modification.
`shifted-encoder-turns` modifies sru-10 on this branch; this change carries
no sru-10 text, so the two changes archive in either order without one
dropping the other's clause. sru-28's body says the page exposes each grid
range "with exclusive maxima", which the page no longer does, so sru-28 is
modified: the page exposes a grid block's start and last on each range and
the model keeps its maxima exclusive; every scenario is carried forward
word for word and one is added. What the page shows for every block's ends
is the new sru-67.

## Open

**O1. Headers of fields whose meaning differs between rows.** A header
belongs to a field, and one header row heads every consecutive row with the
same field list. An encoder-push block and a generic system block carry the
address-type field; a CC block and a Note block of the same form have the
same field list and share a header, so `Field::BlockStartCc` and
`Field::BlockEndCc` head CC numbers and note numbers at once.
`Field::BlockStartArg` is an analog block's first gesture and a system
block's first argument.

**Ruled: one header per field, true on every row it heads; where a field's
meaning differs by a block's form or address type, the row carries its own
field.** NEW `Field::BlockStartGesture`, NEW `Field::BlockStartNote` and NEW
`Field::BlockEndNote`, appended in that order after `Field::ShiftAction`.
An encoder-push or generic system block whose `controlType` is Note carries
`Field::BlockStartNote` and `Field::BlockEndNote` in place of
`Field::BlockStartCc` and `Field::BlockEndCc`; an analog block carries
`Field::BlockStartGesture` in place of `Field::BlockStartArg`.
`Field::BlockStartArg` keeps "Start Arg" and heads system blocks only. The
page's header-row rule is unchanged: a header still comes from
`FieldShortLabel(field)` alone, and a new header row starts only when a
row's field list changes.

**Outside this story, named to the operator.** Individual push and system
rows still head a note number "CC" (`Field::Cc`, on
`EncoderPushEditableFields` and the Generic system schema); this is a
separate change, fixable by the same token split. The grid button, sharing
`Field::GridXMin` and `Field::GridYMin` with the grid block, keeps "Start
X" / "Start Y" rather than "X" / "Y"; both are range words on a one-cell
row. A system block's first argument keeps "Start Arg" rather than a
per-message header ("Start Scene", "Start Bank"); "Start Arg" is already
true, and the row's own "Type" column names the message.

Task 2 records the ruling, before task 6 runs, and writes the matching
sru-67 scenario.

**O2. The cost of a rectangle far off the controller's grid.** The new end
fields reach this defect: a last typed millions past its start asks
expansion for that many cells before any per-cell check runs, found while
tracing what an end field accepts. `VisitRectangle` enumerates
`width * height` cells before `ExpandSystemBlock` reports an off-grid
coordinate, and computes `endX - startX` in `int`; `ExpandGridBlock`
reserves `width * height` elements before checking any cell, which the
start fields already allow today.

**Ruled fixed here.** Both expansions check the two corner cells against
the controller's grid through the existing shape checks (the WRLD.Bldr 0-7
bound and `LaunchpadShapeSupports`, both rectangles) before enumerating or
reserving. Task 2 records the ruling. Task 10 runs.

## Data flow

**Read.** Persisted profile config → section opened →
`ReconstructEncoderBlocks` / `ReconstructAnalogBlocks` /
`ReconstructSystemBlocks` / `ReconstructGridMappings` → block with an
exclusive end in the open section → `MidiConfigViewModel::RowFieldValue` →
`BlockFieldValue` → `BlockLastFromEnd(start, end, mayRunDownward)` → the
page's text field under the `FieldShortLabel` header.

**Write.** Typed text → `ControllersPageSurface::HandleMappingFieldCommit`
→ `MidiConfigViewModel::ApplyMappingEdit` → the row's `Apply*BlockField` →
`BlockEndFromLast(start, last, mayRunDownward)` into the open section's
block (a start y edit first reads the shown last, then stores the end the
new start and that last give) → `FlushSectionPresentationToSlot` →
`Expand*Block` → persisted profile config. A refusal returns its reason to
`HandleMappingFieldCommit`, which shows "Refused:" (field refusals, open
section unchanged) or "Warning:" (expansion refusals, open section keeps
the edit).

## Delivery

The end fields change meaning: a value that was an exclusive end is now the
last control. No saved document changes, since blocks are never
serialized, and the persisted mappings a block expands to are the same for
the same cells. An app author who has never heard of frogg3rs reads the
same "Start CC 0, End CC 16" on a sixteen-encoder controller and would want
this change.

This change is written on the branch `shifted-encoder-turns` and ships in
that branch's pull request against jvictor0/Sheaf `main`, whose description
carries step-by-step testing instructions for both changes. Like
`shifted-encoder-turns`, it stays active until upstream merges the stack.

## Evidence

Run at `ee679e48`, the tip of `shifted-encoder-turns` this change is
written on.

The read path returns each stored end, and the write path stores each typed
end, with its accepted range:

```
$ git grep -n -E "out = static_cast<double>\((encoderBlock|analogBlock|systemBlock|gridBlock)->end" ee679e48 -- projects/synth/src/MidiConfigViewModel.cpp
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1573:                out = static_cast<double>(encoderBlock->endCc);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1594:                out = static_cast<double>(analogBlock->endCc);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1618:                out = static_cast<double>(systemBlock->endCc);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1627:                out = static_cast<double>(systemBlock->endX);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1630:                out = static_cast<double>(systemBlock->endY);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1660:                out = static_cast<double>(gridBlock->endX);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1666:                out = static_cast<double>(gridBlock->endY);

$ git grep -n -E "block\.end(Cc|X|Y) = static_cast|ApplyGridCoordinate\(block\.end|end (cc|x|y) must" ee679e48 -- projects/synth/src/MidiConfigViewModel.cpp
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2101:                validationError = "end cc must be an integer 0-128";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2104:            block.endCc = static_cast<std::uint8_t>(value);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2143:                validationError = "end cc must be an integer 0-128";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2146:            block.endCc = static_cast<std::uint8_t>(value);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2192:                validationError = "end cc must be an integer 0-128";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2195:            block.endCc = static_cast<std::uint8_t>(value);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2216:                validationError = "end x must be an integer";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2219:            block.endX = static_cast<int>(value);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2224:                validationError = "end y must be an integer";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2227:            block.endY = static_cast<int>(value);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2316:            return ApplyGridCoordinate(block.endX, value, "x max", validationError);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2320:            return ApplyGridCoordinate(block.endY, value, "y max", validationError);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:3844:        block.endCc = static_cast<std::uint8_t>(std::min<std::size_t>(
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:3864:        block.endCc = static_cast<std::uint8_t>(std::min<std::size_t>(
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:3968:            block.endCc = static_cast<std::uint8_t>(
```

The last three are `AddBlock`'s defaults, which set a new block's stored end
directly and are not field edits. No end in the view model is offset by
one; the same pattern finds the block model's `endCc - 1`, so it can match:

```
$ git grep -n -E "end(Cc|X|Y) *[-+] *1" ee679e48 -- projects/synth/src/MidiConfigViewModel.cpp; echo "exit=$?"
exit=1

$ git grep -n -E "end(Cc|X|Y) *[-+] *1" ee679e48 -- projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:276:    if (block.endCc - 1 > 127 || block.startCc > 127) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:310:    if (block.endCc - 1 > 127 || block.startCc > 127) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:493:        if (block.endCc - 1 > 127) {
```

The headers, the page's use of them, the header-row rule, the warning
status, and the one render path both runtimes include:

```
$ git grep -n -E "\"(Start CC|End CC|Start X|Start Y|End X|End Y|X Min|X Max|Y Min|Y Max)\"" ee679e48 -- projects/synth
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:421:            return "Start CC";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:423:            return "End CC";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:431:            return "Start X";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:433:            return "Start Y";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:435:            return "End X";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:437:            return "End Y";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:449:            return "X Min";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:451:            return "X Max";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:453:            return "Y Min";
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:455:            return "Y Max";

$ git grep -n -E "FieldShortLabel\(field\)|\*previousFields != rows\[rowIx\]\.editableFields|SetStatus\(\"Warning: \" \+ reason\)" ee679e48 -- projects/synth/include/synth/ControllersPageUI.hpp
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:1693:            SetStatus("Warning: " + reason);
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2235:                        FieldShortLabel(field),
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2255:                FieldShortLabel(field),
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2368:                                             FieldShortLabel(field),
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2432:                        *previousFields != rows[rowIx].editableFields)

$ git grep -n "#include \"synth/ControllersPageUI.hpp\"" ee679e48 -- projects/synth/include/synth/RuntimeMainComponent.hpp projects/synth/include/synth/browser/BrowserRuntimeMainServices.hpp
ee679e48:projects/synth/include/synth/RuntimeMainComponent.hpp:4:#include "synth/ControllersPageUI.hpp"
ee679e48:projects/synth/include/synth/browser/BrowserRuntimeMainServices.hpp:3:#include "synth/ControllersPageUI.hpp"
```

The y direction rule's two copies (reconstruction's `rowDir` is derived from
the run), and the x-only-upward rule at every site:

```
$ git grep -n -E "yDir =|yDirection =|rowDir =" ee679e48 -- projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:368:    const int yDir = (startY < endY) ? 1 : -1;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:650:    const int yDirection = block.endY > block.startY ? 1 : -1;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:1210:        int yDir = 0;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:1217:                yDir = delta;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:1255:            const int rowDir = height == 1 ? 1 : yDir;

$ git grep -n -E "endX <= (block\.)?startX|endY == (block\.)?startY" ee679e48 -- projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:214:    if (endX <= startX || endY == startY) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:365:    if (endX <= startX || endY == startY) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:431:        if (block.endX <= block.startX) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:435:        if (block.endY == block.startY) {
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:628:    if (block.endX <= block.startX || block.endY == block.startY) {
```

A block edit changes the open section's own block, which is why an invalid
edit stays visible, and the expansion reasons it can reach:

```
$ git grep -n -E "PresentationRow& presentationRow = presentation\.rows\[rowIx\];" ee679e48 -- projects/synth/src/MidiConfigViewModel.cpp
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1699:    const PresentationRow& presentationRow = presentation.rows[rowIx];
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2613:    PresentationRow& presentationRow = presentation.rows[rowIx];
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:3289:    const PresentationRow& presentationRow = presentation.rows[rowIx];

$ git grep -n -E "Apply(Encoder|Analog|System|Grid)BlockField\(\*" ee679e48 -- projects/synth/src/MidiConfigViewModel.cpp
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2626:            fieldValid = ApplyEncoderBlockField(*encoderBlock, field, value, validationError);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2628:            fieldValid = ApplyAnalogBlockField(*analogBlock, field, value, validationError);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2630:            fieldValid = ApplySystemBlockField(*systemBlock, field, value, validationError);
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:2632:            fieldValid = ApplyGridBlockField(*gridBlock, field, value, validationError);

$ git grep -n -E "range must be non-empty|rectangle must have non-empty" ee679e48 -- projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:273:        SetReason(reason, "encoder block cc range must be non-empty (endCc > startCc)");
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:307:        SetReason(reason, "analog block cc range must be non-empty (endCc > startCc)");
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:432:            SetReason(reason, "system block x range must be non-empty (endX > startX)");
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:436:            SetReason(reason, "system block y range must be non-empty (endY != startY)");
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:490:            SetReason(reason, "system block cc range must be non-empty (endCc > startCc)");
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:629:        SetReason(reason, "grid block rectangle must have non-empty exclusive ranges");
```

A push block may be Note-addressed (only turn blocks refuse Note), and it
carries the address-type field, as the generic system block does (O1):

```
$ git grep -n -E "encoder turn blocks must use CC addresses|row\.editableFields\.insert\(row\.editableFields\.begin\(\), Field::AddressType\)" ee679e48 -- projects/synth/src
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:269:        SetReason(reason, "encoder turn blocks must use CC addresses");
ee679e48:projects/synth/src/MidiConfigViewModel.cpp:1500:                    row.editableFields.insert(row.editableFields.begin(), Field::AddressType);
```

The width arithmetic and allocation O2 is about:

```
$ git grep -n -E "static_cast<std::size_t>\(endX - startX\)|static_cast<std::size_t>\(std::abs\(endY - startY\)\)|coordinatesValid = false;|scratch.systemMessages.reserve\(count\)" ee679e48 -- projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:217:    const std::size_t width = static_cast<std::size_t>(endX - startX);
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:218:    const std::size_t height = static_cast<std::size_t>(std::abs(endY - startY));
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:369:    const std::size_t width = static_cast<std::size_t>(endX - startX);
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:370:    const std::size_t height = static_cast<std::size_t>(std::abs(endY - startY));
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:445:                        coordinatesValid = false;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:468:                        coordinatesValid = false;
ee679e48:projects/synth/src/MidiConfigBlocks.cpp:648:    scratch.systemMessages.reserve(count);
```

Blocks are never serialized; the files naming them are the two models and
tests. The page draws no row summary (its `.label` reads are catalog
options and a dot id):

```
$ git grep -l -w -E "EncoderBlock|AnalogBlock|SystemBlock|GridBlock" ee679e48 -- projects/synth
ee679e48:projects/synth/include/synth/MidiConfigBlocks.hpp
ee679e48:projects/synth/include/synth/MidiConfigViewModel.hpp
ee679e48:projects/synth/juce/ControllersPageSimulationTests.cpp
ee679e48:projects/synth/src/MidiConfigBlocks.cpp
ee679e48:projects/synth/src/MidiConfigViewModel.cpp
ee679e48:projects/synth/tests/blocks_tests.cpp
ee679e48:projects/synth/tests/controllers_page_ui_tests.cpp
ee679e48:projects/synth/tests/viewmodel_tests.cpp

$ git grep -n "\.label" ee679e48 -- projects/synth/include/synth/ControllersPageUI.hpp
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2099:                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2121:                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2170:                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
ee679e48:projects/synth/include/synth/ControllersPageUI.hpp:2685:                            const std::string labelId = dotId + ".label";
```

The tests that type or read an end field, by enumerator:

```
$ git grep -n -E "Field::(BlockEndCc|BlockEndX|BlockEndY|GridXMax|GridYMax)" ee679e48 -- projects/synth/tests projects/synth/juce
ee679e48:projects/synth/juce/ControllersPageSimulationTests.cpp:605:                                                           synth::MidiMappingRowVM::Field::GridXMax, xMax),
ee679e48:projects/synth/juce/ControllersPageSimulationTests.cpp:610:                                               synth::MidiMappingRowVM::Field::GridXMax) +
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1559:        case Field::BlockEndCc:
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1571:        case Field::BlockEndX:
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1573:        case Field::BlockEndY:
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1590:        case Field::GridXMax:
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1592:        case Field::GridYMax:
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1629:        case Field::BlockEndCc: {
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1636:        case Field::BlockEndX: {
ee679e48:projects/synth/tests/viewmodel_tests.cpp:1643:        case Field::BlockEndY: {
ee679e48:projects/synth/tests/viewmodel_tests.cpp:2623:        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::BlockEndCc, 0.0, out, &reason);
ee679e48:projects/synth/tests/viewmodel_tests.cpp:2693:    ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, sceneBlockIx, MidiMappingRowVM::Field::BlockEndY,
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5101:                                          MidiMappingRowVM::Field::BlockEndCc, value));
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5123:                                             MidiMappingRowVM::Field::BlockEndCc, value));
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5216:                                                 MidiMappingRowVM::Field::BlockEndCc,
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5664:                                   MidiMappingRowVM::Field::GridXMax) != rows[0].editableFields.end());
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5666:                                   MidiMappingRowVM::Field::GridYMax) != rows[0].editableFields.end());
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5749:                                      MidiMappingRowVM::Field::GridXMax, 0.0, out, &reason,
ee679e48:projects/synth/tests/viewmodel_tests.cpp:5759:                                     MidiMappingRowVM::Field::GridXMax, 2.0, out, &reason));
```

Lines 1559 to 1592 are `SafeValueFor`'s fallback constants and 1629 to
1643 `SafeValueForRow`, which writes each field's shown value back; 5664 and
5666 check the fields are present. The behaviour these traces predict (the
sixteen-turn block reads 16, a typed 15 keeps fifteen turns, a typed start
y of 1 on the bank-select block is refused) is structural until the
preflight's run confirms it; see tasks.

**Task 10's base timing run.** A program outside the tree, built against
`ee679e48`, timed `ExpandSystemBlock` on a WRLD.Bldr block with x
`[0, 2147483647)` on one row and `ExpandGridBlock` on a Launchpad block with
x `[0, 1073741824)` and y `[0, 1024)`, catching any exception, run twice for
reproducibility:

```
ExpandSystemBlock WRLD.Bldr x[0,2147483647) y[0,1): ok=0 reason="wrldbldr coordinate is outside the 0-7 grid" 3972.5 ms
ExpandGridBlock Launchpad x[0,1073741824) y[0,1024): EXCEPTION std::bad_alloc after 0.0 ms
```

A stall (3972.5 ms) and an exception (`std::bad_alloc`), both on the base,
before the corner check.

**Task 10's fixed-code timing run.** Against the shipped code, typing each of
the scenario's own values through the page's field-commit action, each run
labelled with what it typed:

- A WRLD.Bldr system block's last x, typed `1073741824` on the default
  bank-select block (`attack.cpp`, `which == "wrld"`, row 4,
  `Field::BlockEndX`):
  ```
    type Last X     "1073741824"  -> status "Warning: wrldbldr coordinate is outside the 0-7 grid"  committed=no  (0.0 ms)
  ```
- A Launchpad grid block's last x, then its last y, typed `1073741824` and
  `1024` in turn (`attack.cpp`, `which == "grid"`, controller 1,
  `Field::GridXMax` then `Field::GridYMax`):
  ```
    type Last X     "1073741824"  -> status "Warning: launchpad grid coordinate is outside this controller's grid"  committed=no  (0.0 ms)
    type Last Y     "1024"  -> status "Warning: launchpad grid coordinate is outside this controller's grid"  committed=no  (0.0 ms)
  ```

Every one of these is refused at once, in 0.0 ms, with the off-grid reason.
The base run above shows the stall, in `ExpandSystemBlock` on a WRLD.Bldr
block whose last x is `2147483646` rather than `1073741824`, and the
exception, in `ExpandGridBlock` on a Launchpad block carrying both
`1073741824` and `1024` together as the base run above does; the corner
check removes both.

## Capabilities

### Modified Capabilities
- `synth-runtime-ui`: sru-28 modified, sru-67 added.

## Impact

- `include/synth/MidiConfigBlocks.hpp` and `src/MidiConfigBlocks.cpp` (the
  translation, the direction rule, the expansion reasons, and the corner
  checks (O2 ruled fixed here)), `src/MidiConfigViewModel.cpp`
  (`BlockFieldValue`, the `Apply*BlockField` functions,
  `ApplyGridButtonField`'s reasons,
  `FieldShortLabel`), and
  `include/synth/MidiConfigViewModel.hpp` (the `MidiMappingRowVM::Field`
  comments; NEW `Field::BlockStartGesture`, NEW `Field::BlockStartNote` and
  NEW `Field::BlockEndNote`).
- `include/synth/ControllersPageUI.hpp`: `FieldEditorWidth` (NEW
  `Field::BlockStartNote`, `Field::BlockEndNote` and
  `Field::BlockStartGesture`'s column widths), and `ParseFiniteNumericToken`,
  which now takes whether the field is an integer field (`FieldIsInteger`):
  for one, it trims leading and trailing whitespace and requires the trimmed
  text to be wholly an integer literal, rather than parsing it as a double,
  so a fractional value a double's rounding would turn into a whole number is
  refused instead of silently accepted, and typed text with trailing
  whitespace is accepted like text with leading whitespace. A well-formed
  integer literal too large for `long long`, or one whose magnitude exceeds
  2^53 (past which not every integer is exact in a `double`), sets a new
  out-parameter rather than returning a value. Its caller,
  `HandleMappingFieldCommit`, refuses malformed text in an integer field
  with "value must be an integer", an out-of-range one with "value is out
  of range", and a non-integer field's malformed text with "value must be a
  finite number" as before. Not a story-5 defect: the parser predates this
  change, and story 5's own diff never touched it (its behaviour is fixed
  as a standalone parser fix, layered on after).
- Tests: `tests/blocks_tests.cpp`, `tests/viewmodel_tests.cpp`,
  `tests/controllers_page_ui_tests.cpp`,
  `juce/ControllersPageSimulationTests.cpp`.
- Documentation: `docs/coverage.md`.
