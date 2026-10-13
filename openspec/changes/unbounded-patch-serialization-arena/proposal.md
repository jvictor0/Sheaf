# Proposal — `unbounded-patch-serialization-arena`

Stacks on `app-commands-on-the-bus` (PR #21, branched at `e18eee39`).

## Why

Past about 4,600 live depths, a patch's serialized JSON no longer fits in
`PatchSerializationContext`'s 8 MB `maxArenaCapacity`. `SerializeToJSON`
keeps reporting `ArenaExhausted`; `GrowSerializationArenaForTick` doubles
the engine's arena on each `MessageThreadTick` until it hits that cap, then
drops the stashed message instead of growing further. `PatchManager`'s
`pendingSave_` was set when the save was dispatched and is only cleared by
`ProcessResponses` popping a matching `SerializedJSON` response — a response
that dropped stash never produces — so `pendingSave_` never clears: Save As
never completes, and every later Save or Save As reports `Busy` forever.
The plugin's DAW-state snapshot (`FroggersPluginProcessor::PumpStatePersistence`)
serializes through this same engine arena and the same cap, with no cap or
failure mode of its own — a save past the cap leaves
`pendingStateSnapshotRequestId_` set and `cachedStateJsonText_` (what
`getStateInformation` returns to the host) stuck at its last value before
the cap, so the DAW silently saves a stale project.

The cap was never a capacity limit a patch needs enforced elsewhere; it
just stopped growth partway through the one case that needs it most. A
save's arena should keep growing, one tick at a time, until the patch fits.

## What changes

- `synth-patch-persistence` (delta, ADDED requirement `spp-13`): a save's
  serialization arena has no ceiling — it doubles until the patch fits, and
  every dispatched save eventually completes.
- Code:
  - `include/synth/PatchPersistence.hpp`: `PatchSerializationContext` drops
    `maxArenaCapacity`.
  - `src/PatchPersistence.cpp`: `ApplyPatchMessage`'s non-caller-owned
    `SerializeToJSON` branch grows until it fits instead of stopping at a
    cap and reporting `ArenaExhausted`.
  - `include/synth/Engine.hpp`: `GrowSerializationArenaForTick` always
    doubles the arena; the drop-at-cap branch and its `INFO` log are
    removed. Comments describing the cap or the drop, on
    `GrowSerializationArenaForTick`, `MessageThreadTick`,
    `StashPendingPatchMessage`, and `pendingPatchMessage_`, are updated to
    describe unbounded growth.
  - `tests/parameter_modulation_tests.cpp`: the sub-test asserting
    `ArenaExhausted` from a `maxArenaCapacity` of 1 is removed (the field it
    exercises no longer exists).
- Out of scope: `kRuntimeConfigMaxArenaCapacity` in `PatchPersistence.cpp`
  bounds the runtime-configuration document (MIDI instrument, audio device,
  sync config), a small, fixed-shape file unrelated to a patch's depths;
  left as-is.

## Impact

- Affected specs: `synth-patch-persistence` (ADDED requirement `spp-13`).
- Affected code: as above, in `External/Sheaf/projects/synth`; no frogg3rs
  change (`FroggersPluginProcessor`'s snapshot path is fixed by the same
  engine-arena change, with nothing plugin-side to touch).
- Backward compatibility: a save now takes one more message-thread tick per
  doubling past 8 MB instead of failing; every patch that fit before still
  saves in the same number of ticks. No stored patch format changes.
- Delivery: the frogg3rs pin moves to this branch's tip in its own commit,
  per this repo's per-fix delivery convention.
