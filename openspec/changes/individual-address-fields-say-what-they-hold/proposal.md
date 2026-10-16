# Proposal — `individual-address-fields-say-what-they-hold`

This change is written on the branch `shifted-encoder-turns`, on top of that
branch's tip `ea0b3844`, alongside the two openspec changes already active on
it (`shifted-encoder-turns` itself and `block-end-fields-show-the-last-control`,
which ships in the same pull request). Paths are relative to
`projects/synth/`, and code is named by symbol. Its spec delta is in this
change's `specs/`.

## Why

`block-end-fields-show-the-last-control` gave a Note-addressed block its own
`Field::BlockStartNote`/`Field::BlockEndNote`, headed "Start Note"/"Last
Note", because a block's number pair sets note numbers on that row and CC
numbers on a sibling row sharing the same header. That change's Open section
named the same defect one level down and left it for a separate change:

> Individual push and system rows still head a note number "CC"
> (`Field::Cc`, on `EncoderPushEditableFields` and the Generic system
> schema); this is a separate change, fixable by the same token split.

An individual encoder-push row and an individual Generic system-message row
each carry a Note/CC selector (sru-27); switching it to Note makes the row's
own number field hold a note number, 0-127, on the row's control. That field
is still `Field::Cc`, headed "CC" (`FieldShortLabel`). A player who switches
a push row to Note and types `60` sees "CC 60" over a note.

An encoder turn row, an analog gesture row and an analog app-action row also
show `Field::Cc`, but none of them carries a Note/CC selector -- a turn
mapping is refused outside CC by `ProfileConfigValidForKind` ("encoder turns
must use CC control addresses"), and neither analog row ever gets an
`AddressType` field. Their "CC" header stays true.

## What Changes

- **One field for the individual number, split by address type**, mirroring
  the block pattern's O1 ruling ("where a field's meaning differs by a
  block's form or address type, the row carries its own field"): NEW
  `Field::Note`, appended after `Field::BlockEndNote`. An individual encoder
  push row and an individual Generic system row carry `Field::Note` in place
  of `Field::Cc` when the row's own control is Note-addressed;
  `Field::Cc` keeps "CC" and heads every row that is actually CC-addressed
  (both address-typed rows when CC-selected, and every row with no address
  selector: turns, analog gestures, analog app actions).
- **Headed "Note".** `FieldShortLabel(Field::Note)` returns "Note".
  `FieldIsInteger(Field::Note)` is true, same as `Field::Cc`.
  `FieldEditorWidth` gives it the same 66px `Field::Cc` gets ("Note", 4
  characters, is shorter than "Start Note", already measured to fit at 66px
  in `block-end-fields-show-the-last-control` task 6).
- **No value translation.** Unlike a block's end field, an individual row
  has no start/end pair and no exclusive-end convention to translate:
  `Field::Note` reads and writes the same `.control.cc` storage `Field::Cc`
  does, unchanged.
- **Refusals name what the page shows.** An out-of-range value is refused
  with "note must be an integer 0-127" on `Field::Note`, "cc must be an
  integer 0-127" on `Field::Cc` -- both still accept 0-127, only the wording
  changes, matching the block end fields' own start/end refusal split.
- **`EncoderPushEditableFields` and `SystemRowEditableFields` choose the
  field from the row's live control type**, the same as
  `EncoderBlockEditableFields`/`SystemBlockEditableFields` already choose
  `BlockStartNote`/`BlockEndNote` from a block's `controlType`. The one
  caller building a table for a not-yet-added row (`GroupColumnFields`)
  passes CC, a fresh row's own default (`MidiControlAddress::type`'s member
  initializer).
- **The existing per-row header-row mechanism needs no change.** The page
  already starts a new header row whenever a row's field list changes
  (`*previousFields != rows[rowIx].editableFields`, `ControllersPageUI.hpp`);
  this change is what makes a push or Generic row's own field list change
  with its address type, and the header mechanism picks that up unmodified,
  the same way it already does for a block's field list.

## Decisions

**Scope: only the two rows the sibling change named.** An encoder turn row
and both analog row kinds keep `Field::Cc` unconditionally: none of them
ever carries `Field::AddressType`, so their own `Field::Cc` is never given a
Note value to mislabel. Confirmed by the same validation the sibling change
already cites for turns, and by the field tables for both analog kinds
carrying no `AddressType` entry (Evidence).

**`GroupColumnFields`'s fresh-row preview stays CC.** It answers "what would
a first added row in this group show," and a freshly added row is
CC-addressed until its own `AddressType` is switched -- the same default
`AddSingle` gives it. Passing anything else would show a header the row
itself won't carry until edited.

## Data flow

**Read.** Persisted profile config → `EncoderMidiMapping.control` /
`MidiControllerSystemMessageAssociation.control` → `BuildSectionRows` picks
`Field::Note` or `Field::Cc` from `control.type` →
`MidiConfigViewModel::RowFieldValue` reads `control.cc` for either → the
page's text field under the `FieldShortLabel` header.

**Write.** Typed text → `ControllersPageSurface::HandleMappingFieldCommit` →
`MidiConfigViewModel::ApplyMappingEdit` (refused up front if the row's
current `editableFields` doesn't carry the field, e.g. `Field::Cc` on a
Note-addressed row) → `mapping->control.cc` / `association->control->cc`,
same as `Field::Cc` writes today → refusal reason names "note" or "cc" by
which field was given.

## Delivery

Ships on branch `shifted-encoder-turns`, in the same pull request as the
other two changes already active on it. No persisted-format change: the
field split is presentation-only, over the same `MidiControlAddress`
(`channel`, `cc`, `type`) every row already stores.

## Evidence

Run at `ea0b3844`.

The two rows the sibling change named, and their unconditional `Field::Cc`
header today:

```
$ git grep -n -E "EncoderPushEditableFields\(\)|SystemAddressField::Cc|SystemRowEditableFields" -- projects/synth/src/MidiConfigViewModel.cpp
projects/synth/src/MidiConfigViewModel.cpp:1106:std::vector<Field> SystemRowEditableFields(MidiProfileKind kind,
projects/synth/src/MidiConfigViewModel.cpp:1133:            case SystemAddressField::Cc:
projects/synth/src/MidiConfigViewModel.cpp:1154:// SystemRowEditableFields above. Shift is included only when the app's
projects/synth/src/MidiConfigViewModel.cpp:1169:std::vector<Field> EncoderPushEditableFields() {
projects/synth/src/MidiConfigViewModel.cpp:1537:                    row.editableFields = EncoderPushEditableFields();
projects/synth/src/MidiConfigViewModel.cpp:1551:                    SystemRowEditableFields(slot.kind, *association, messageCatalogOffersShift_);
projects/synth/src/MidiConfigViewModel.cpp:4161:            return EncoderPushEditableFields();
projects/synth/src/MidiConfigViewModel.cpp:4175:        return SystemRowEditableFields(instrument_.controllers[controllerIx].kind, association,
```

Only encoder pushes may be Note-addressed; encoder turns are refused outside
CC at both the individual-mapping and the block level:

```
$ git grep -n -E "encoder turns must use CC control addresses|encoder turn blocks must use CC addresses" -- projects/synth/src
projects/synth/src/MidiConfigBlocks.cpp:335:        SetReason(reason, "encoder turn blocks must use CC addresses");
projects/synth/src/MidiController.cpp:3640:                return Fail(reason, "encoder turns must use CC control addresses");
```

`FieldShortLabel(Field::Cc)` returns "CC" unconditionally on the base, with
three callers in `ControllersPageUI.hpp`. `RowFieldValue` and
`ApplyMappingEdit` each switch on `Field::Cc` in four branches, one per
row-data variant (`EncoderMidiMapping`, `AnalogMidiMapping`,
`AnalogAppActionMapping`, `MidiControllerSystemMessageAssociation`); the
`EncoderMidiMapping` and `MidiControllerSystemMessageAssociation` branches
are the two this change touches, and both key off the same `Field::Cc`
regardless of `control.type` today:

```
$ git grep -n -A1 "case Field::Cc:" -- projects/synth/src/MidiConfigViewModel.cpp
projects/synth/src/MidiConfigViewModel.cpp:322:        case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-323-        case Field::SlotIx:
--
projects/synth/src/MidiConfigViewModel.cpp:395:        case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-396-            return "CC";
--
projects/synth/src/MidiConfigViewModel.cpp:1783:            case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-1784-                out = static_cast<double>(mapping->control.cc);
--
projects/synth/src/MidiConfigViewModel.cpp:1801:            case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-1802-                out = static_cast<double>(mapping->control.cc);
--
projects/synth/src/MidiConfigViewModel.cpp:1816:            case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-1817-                out = static_cast<double>(mapping->control.cc);
--
projects/synth/src/MidiConfigViewModel.cpp:1849:            case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-1850-                if (!association->control.has_value()) {
--
projects/synth/src/MidiConfigViewModel.cpp:2743:                case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-2744-                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
--
projects/synth/src/MidiConfigViewModel.cpp:2788:                case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-2789-                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
--
projects/synth/src/MidiConfigViewModel.cpp:2817:                case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-2818-                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
--
projects/synth/src/MidiConfigViewModel.cpp:2874:                case Field::Cc:
projects/synth/src/MidiConfigViewModel.cpp-2875-                    if (!association->control.has_value()) {
```

Neither analog row kind ever carries `Field::AddressType` (only the encoder
push branch and the system-row schema loop insert it):

```
$ git grep -n -E "row\.editableFields = \{Field::Channel, Field::Cc" -- projects/synth/src/MidiConfigViewModel.cpp
projects/synth/src/MidiConfigViewModel.cpp:1544:                row.editableFields = {Field::Channel, Field::Cc, Field::GestureIx};
projects/synth/src/MidiConfigViewModel.cpp:1547:                row.editableFields = {Field::Channel, Field::Cc, Field::AppAction};
```

The only other test file naming these rows checks solely the CC-addressed
instance:

```
$ git grep -ln -E "EncoderPushEditableFields|SystemRowEditableFields|RowGroup::EncoderPush" -- projects/synth/tests projects/synth/juce
projects/synth/tests/controllers_page_ui_tests.cpp
projects/synth/tests/viewmodel_tests.cpp
```

Behavior confirmed by the executor's own run (`viewmodel_tests`, task 4): a
push row's `AddressType` switch to Note is shown to leave its number field
absent from `Field::Note` against the unfixed code (red), and present,
reading the unchanged number under "Note", against the fix (green); see the
task's report for the exact runs.

## Capabilities

### Added Capabilities
- `synth-runtime-ui`: sru-68 added.

## Impact

- `include/synth/MidiConfigViewModel.hpp`: NEW `Field::Note`; one stale
  comment fixed (an example call in `GroupColumnFields`'s doc comment named
  `EncoderPushEditableFields()` with no argument after its signature grows
  one).
- `src/MidiConfigViewModel.cpp`: `FieldIsInteger`, `FieldShortLabel`,
  `EncoderPushEditableFields` (now takes the row's `MidiControlType`),
  `SystemRowEditableFields` (branches on the row's own `control.type`), the
  two call sites of each, `RowFieldValue`'s two individual-row branches
  (`EncoderMidiMapping`, `MidiControllerSystemMessageAssociation`),
  `ApplyMappingEdit`'s same two branches.
- `include/synth/ControllersPageUI.hpp`: `FieldEditorWidth`.
- Tests: `tests/viewmodel_tests.cpp` (extends
  `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`,
  `FieldIsIntegerTrueForIndexAndCoordinateFields`,
  `FieldShortLabelIsNonEmptyAndDistinctPerField`, and the exhaustive
  `SafeValueFor`/`SafeValueForRow` helpers). `tests/controllers_page_ui_tests.cpp`
  also names `RowGroup::EncoderPush` and both individual rows (Evidence), but
  only ever against the CC-addressed instance (its own
  `requireTypeCombo(..., "0")` assertions), so it is unchanged; run to
  confirm, not edited.
