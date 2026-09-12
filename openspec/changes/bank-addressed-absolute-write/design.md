# Design — bank-addressed-absolute-write

Anchors verified 2026-08-20 at Sheaf `fix-out-of-tree-app-gaps` working tree
(80c4eab8).

## The coupling, traced

- `ParameterManager::HandleSetAbsolute(slotIx, position, normalizedTarget,
  absoluteEpoch)` (`ParameterModulation.cpp:3478-3488`) resolves `position`
  to a `PhysicalEncoderId` through the addressed slot (`BankSlotAt(slotIx)`
  + `BankSlot::ResolvePosition`, `:2969-2975`), gates the write on
  `GetCurrentModifier() == Modifier::None`, and dispatches through
  `BankSlot::HandleSetAbsolute` (`:2963-2967`) to whichever bank
  `selectedBank_` currently names; it then unconditionally calls
  `BankSlot::RecordProcessedAbsoluteEpoch(position, absoluteEpoch)`
  (`:2977-2982`), regardless of whether the modifier gate let the write
  through.
- `BankSlot::HandleSetAbsolute` (`:2963-2967`) only fires through
  `BankSlot::Owns` (`:2934-2936`), which requires `selectedBank_ !=
  nullptr`; there is no slot-level entry point that names a bank other
  than the one currently selected.
- `Bank::HandleSetAbsolute` (`:2660-2666`) resolves the encoder through
  `Bank::FindVisibleCell` (`:2758-2765`), which scans `visible_` — the
  bank's current page. `visible_` starts equal to `topLevel_` but
  `Bank::OpenModulationView` (`:2813-2859`) clears it and refills it with
  up to `numModulators` depth cells plus the selected parameter itself
  (`:2841-2857`) whenever that bank is drilled into a parameter's
  modulation view; `Bank::Deselect` (`:2693-2708`) is what restores
  `visible_ = topLevel_`. So a write through the existing path, while a
  bank happens to be showing modulation, lands on whatever depth cell (or
  the selected parameter) occupies that physical position in `visible_` —
  spm-77's own "Modulation-depth view receives absolute edit" scenario
  documents that as correct for a slot-originated write, because the
  operator's own encoder gesture is by construction aimed at whatever they
  currently have drilled into.
- `topLevel_` (`Bank`'s member, `:654`) is the bank's own mapping and is
  never cleared by `OpenModulationView` — only `visible_` is swapped out —
  so a lookup against `topLevel_` instead of `visible_` reaches the real
  parameter regardless of what the bank currently shows.

## Change

Four additive members, all in `ParameterModulation.hpp`/`.cpp`:

- `Bank::HandleSetAbsoluteOnTopLevel(PhysicalEncoderId, const SceneState&,
  float)` — same body shape as `Bank::HandleSetAbsolute`, but resolves the
  encoder through a new private `FindTopLevelCell` that scans `topLevel_`
  (mirroring `FindVisibleCell`'s scan of `visible_`) instead of
  `FindVisibleCell`. Delegates to the resolved cell's parameter's own
  `HandleSetAbsolute` exactly as the existing method does.
- `MessageIn::Type::ParamSetAbsoluteOnBank` (appended after the existing
  enumerators — see "Enum placement" below) plus a matching
  `MessageIn::ParamSetAbsoluteOnBank(timestamp, bankIx, slotIx, position,
  normalizedValue, absoluteEpoch = 0)` factory, declared and defined next
  to `ParamSetAbsolute` (its semantic sibling), built the same way
  `ParamSetAbsolute`'s factory is (`:3786-3796`). `bankIx` is an existing
  `MessageIn` field (already used by `SelectParamBank`); no new field is
  needed beyond it.
- `ParameterManager::HandleSetAbsoluteOnBank(bankIx, slotIx, position,
  normalizedTarget, absoluteEpoch = 0)` — resolves `position` to a
  `PhysicalEncoderId` through `BankSlotAt(slotIx)` +
  `BankSlot::ResolvePosition` (the encoder layout is the slot's; that part
  is intentionally identical to the slot-addressed path), then writes
  through `BankAt(bankIx)`'s new `HandleSetAbsoluteOnTopLevel`. Does not
  call `BankSlot::SelectBank`, touch `selectedBank_`, or go through
  `BankSlot::Owns` — the addressed slot is consulted only for its encoder
  layout (`ResolvePosition`) and its epoch bookkeeping
  (`RecordProcessedAbsoluteEpoch`), never for which bank owns the write.
  Null-checks both the slot and the bank and returns without effect if
  either is missing, matching `HandleSetAbsolute`'s own
  slot/position-resolution guard.
- `MessageInBus::Apply` case for the new type, dispatching to the above,
  guarded by `manager_ != nullptr` (matching the majority shape of the
  switch's other cases, e.g. `ParamPush`, `SelectParamBank` — the adjacent
  `ParamSetAbsolute` case is the switch's one exception to that guard, per
  its own comment explaining that modifier gating and epoch
  acknowledgement are deliberately downstream; this new case has no such
  downstream modifier gate to defer to, so it follows the switch's general
  pattern instead of that one neighbor).

### Two behaviors carried over deliberately, one dropped deliberately

- **No modifier gate.** `ParameterManager::HandleSetAbsolute` gates on
  `GetCurrentModifier() == Modifier::None` because that path's input is an
  operator's own encoder gesture, and a modifier reinterprets what that
  gesture means (reset/randomize instead of set). A bank-addressed write
  did not come from the encoders — gating it on a modifier some operator
  happens to be holding would silently swallow a write that has nothing to
  do with that operator's gesture. `HandleSetAbsoluteOnBank` applies the
  write unconditionally.
- **Epoch recording carried over unchanged.** The slot-addressed path
  calls `RecordProcessedAbsoluteEpoch` regardless of the modifier gate
  (`:3486`, outside the `if` at `:3483`); `HandleSetAbsoluteOnBank` calls
  the same method on the same addressed slot for the same reason — epoch
  tracking is a debounce/ordering record keyed by slot position,
  independent of whichever bank a given write happened to target, and it
  early-returns on epoch `0` (`:2978`), so it costs nothing for callers
  that never pass one.

### Enum placement

`MessageIn::Type` has no explicit ordinal-stability contract, but
`MidiConfigBlocks.hpp:75`'s `SystemMessageSortKey::typeOrder` is
documented as keying on "`MessageIn::Type`'s declaration order
(`ParamIncDec=0` .. `PrevParamBank=21`)," and that comment's own range
shows the file's established practice already: `NextParamBank`/
`PrevParamBank` sit at the very end of the enum's declaration (ordinals
20-21) despite their factory functions being declared earlier, next to
`SelectParamBank`. `ParamSetAbsoluteOnBank` follows that precedent —
appended after `PrevParamBank` — so every existing enumerator keeps its
current ordinal and `typeOrder`'s existing comparisons are provably
unaffected; only the factory declaration/definition sit next to
`ParamSetAbsolute` for readability.

## Scope note: switches over `MessageIn::Type` elsewhere

The repository has roughly a dozen other exhaustive `switch
(message.type)`-shaped statements with no `default:` label, outside the
two files this change touches (`MessageInBus::Apply` is the only one
inside them) — e.g. `MidiController.cpp`'s `MessageTypeName`/
`ParseMessageType`/JSON round-trip, `MidiConfigViewModel.cpp`'s several,
`MidiConfigBlocks.cpp`'s `ComputeSystemMessageSortKey`/
`ClassifyBlockable`, `ControllerWizard.cpp`'s
`TwisterMessageForAssociation`, and the test-only equivalence switches in
`blocks_tests.cpp`/`viewmodel_tests.cpp`. None of them will be reached
with `Type::ParamSetAbsoluteOnBank` by any existing caller — nothing
outside this change constructs that value — so leaving them as they are is
additive-safe: `-Wall -Wextra -Wpedantic` (`projects/synth/Makefile:2`, no
`-Werror`) will warn on the now-incomplete coverage but the build will not
fail and no existing behavior changes for any input those functions can
currently receive. Those surfaces were then classified rather than left warning. The message
serialisers -- `ToJSON`, `FromJSON` and the round-trip equivalence helper --
handle any `MessageIn`, so a type absent from them cannot round-trip at all;
the new type joins their fall-through groups, and every field including
`bankIx` is already written after the switch, so no new field handling is
needed. `MessageTypeName` and its parser are a total name mapping and gain
both directions. `SystemMessageOutputInfo::Evaluate` computes feedback for a
mapped controller, and a bank-addressed programmatic write is not mappable to
a controller, so it joins the group that yields no feedback -- an explicit
exclusion rather than an omission. Two range comments naming `PrevParamBank`
as the last enumerator are corrected; `TypeOrder` static_casts the enum and
already ordered the new type correctly.

Confirmed against the actual build: only 5 of these actually
warned in this incremental run — the 4 in `MidiController.cpp` and the 1
in `blocks_tests.cpp` — because `MidiController.o`'s Makefile rule lists
`include/synth/ParameterModulation.hpp` as an explicit prerequisite
(`projects/synth/Makefile:72`) while `MidiConfigViewModel.o`,
`MidiConfigBlocks.o`, and `ControllerWizard.o`'s rules (`:93`, `:96`, `:75`)
list only their own direct headers and do not chase the transitive include
back to `ParameterModulation.hpp`, so `make` reused their existing object
files unchanged. That is a pre-existing property of this Makefile's
hand-written dependency rules, not something this change introduces or
relies on: those three files' own switches are exactly as incomplete as
the five that did warn, and will warn identically the next time something
forces them to recompile (editing them directly, or `make clean`).
`viewmodel_tests.cpp`'s three switches (`:3936`, `:3951`, `:4221`) and
`MidiConfigBlocks.cpp:992` already carry a `default:` label, so none of
those four warn regardless of whether they recompile.

## Testing

The full existing `projects/synth` suite staying green and unmodified proves
only that four purely-additive members leave every existing behavior
byte-identical. It is not proof that the new path works — nothing called it
until this change's own cases were added. Those cases cover the spec delta's
scenarios directly: a write reaching a bank the slot has not selected, the
slot's selection surviving it, and the write landing on the top-level
parameter rather than a depth cell when the target bank is showing
modulation. Coverage of a real consumer arrives with the change that adds the
point where directly exercising the new path stops being coverage of a
currently-uncalled primitive and starts being coverage of real behavior.

## Risks

- Leaving the dozen other `MessageIn::Type` switches uncovered means a
  future change to any of them (or one that flips on `-Werror`) will need
  to account for `ParamSetAbsoluteOnBank`; flagged above rather than fixed
  here, since fixing them is unrelated to and much larger than this
  change's own scope.
- `HandleSetAbsoluteOnBank`'s null-checks on the slot and the bank make an
  unmapped address a silent no-op, exactly like the slot-addressed path's
  own unmapped-address behavior (spm-77's "Unmapped absolute address is a
  no-op" scenario) — consistent, but worth naming since a caller that
  mistypes a `bankIx` gets no signal.
