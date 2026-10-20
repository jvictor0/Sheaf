# Proposal — `bank-addressed-absolute-write`

**Created 2026-08-20.** The only absolute-write entry point into a bank is
slot-addressed: it always lands on whichever bank the shared `BankSlot`
currently has selected, and — because the bank resolves through its current
page — on whichever cells that bank currently shows. There is no way to set
a parameter's absolute value on a specific, named bank without first moving
the slot's live selection onto it.

## Why

`ParameterManager::HandleSetAbsolute(slotIx, position, ...)` resolves a
physical position through `BankSlot::ResolvePosition`, then dispatches
through `BankSlot::HandleSetAbsolute` to whichever `Bank*` the slot's
`selectedBank_` currently points at (`ParameterModulation.cpp:3478-3488`,
`BankSlot::HandleSetAbsolute` at `:2963-2967`). That bank in turn resolves
the encoder through `Bank::FindVisibleCell` (`:2758-2765`), which scans
`visible_` — the bank's current page. `visible_` is reset to
modulation-depth cells by `Bank::OpenModulationView` (`:2813-2859`)
whenever that bank is drilled into a parameter's modulation view. A write
through this path while the bank happens to be showing modulation therefore
lands on a depth cell instead of the parameter — spm-77's own
"Modulation-depth view receives absolute edit" scenario documents this as
the intended behavior for slot-originated writes, because a slot's encoder
gesture is by definition aimed at whatever that operator currently has
drilled into.

A write that is not an operator's own encoder gesture — one addressed
programmatically to a specific bank and a specific top-level position — has
no such intended ambiguity, and today has no path that avoids it: reaching
a bank at all means going through a `BankSlot`, and the only slot-level
write, `BankSlot::HandleSetAbsolute`, always targets `selectedBank_`.
Pointing it at a different bank means calling `BankSlot::SelectBank` first,
which changes what every other consumer of that slot (UI state, the next
operator gesture) sees as selected — a visible, shared side effect that has
nothing to do with the write itself.

## What Changes

- `synth-parameter-modulation` (delta, ADDED requirement): a bank SHALL
  accept an absolute write addressed to one of its own top-level positions,
  independent of what that bank currently has visible and independent of
  which bank any `BankSlot` currently has selected.
- Code (all additive, `ParameterModulation.hpp`/`.cpp`): `Bank` gains a
  top-level-addressed absolute-write method alongside its existing
  visible-addressed one; `ParameterManager` gains a bank-addressed write
  that resolves the position through the addressed slot's encoder layout
  but applies the write to an explicitly-addressed bank, touching neither
  `BankSlot::SelectBank` nor `selectedBank_`; `MessageIn` gains a matching
  message type and factory carrying a bank index alongside the existing
  slot/position/value/epoch fields; `MessageInBus::Apply` gains the
  corresponding dispatch case.

## Impact

- Affected specs: `synth-parameter-modulation` (ADDED requirement; spm-77's
  existing slot-addressed requirement is unchanged).
- Affected code: `projects/synth/include/synth/ParameterModulation.hpp`,
  `projects/synth/src/ParameterModulation.cpp`, and the surfaces that
  enumerate message types: `src/MidiController.cpp`,
  `include/synth/MidiConfigBlocks.hpp`, `src/MidiConfigBlocks.cpp`. Purely additive — no
  existing signature or behavior changes; the existing slot-addressed path
  (`BankSlot::HandleSetAbsolute`, `ParameterManager::HandleSetAbsolute(slotIx,
  ...)`) is untouched.
- Tests: the new path gets its own coverage in the synth suite — a
  bank-addressed write reaching a non-selected bank, leaving the slot's
  selection untouched, and landing on the top-level parameter rather than a
  depth cell when the target bank is itself showing modulation. The full
  suite is 917 tests, 915 passing; the two failing are pre-existing
  96 kHz braid-4 deadline tests unrelated to this change.
