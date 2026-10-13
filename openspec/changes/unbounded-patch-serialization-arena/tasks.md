# Tasks — unbounded-patch-serialization-arena

Gate: `make -C projects/synth test` (then every binary the recipe stops
before, run by path) and `make -C app test`.

## 1. spp-13 — remove the serialization arena cap

- [x] 1.1 `PatchSerializationContext::maxArenaCapacity` removed
      (`include/synth/PatchPersistence.hpp`).
- [x] 1.2 `ApplyPatchMessage`'s non-caller-owned `SerializeToJSON` loop
      (`src/PatchPersistence.cpp`) grows until it fits; no cap check, no
      `ArenaExhausted` return from that loop.
- [x] 1.3 `GrowSerializationArenaForTick` (`include/synth/Engine.hpp`)
      always doubles; the drop-at-cap branch and its `INFO` log removed.
- [x] 1.4 Comments describing the cap or the drop-at-cap carve-out
      (`GrowSerializationArenaForTick`, `MessageThreadTick`,
      `StashPendingPatchMessage`, `pendingPatchMessage_`) updated to
      describe unbounded growth.
- [x] 1.5 `tests/parameter_modulation_tests.cpp`: the `maxArenaCapacity = 1`
      sub-test removed.
- [x] 1.6 Probe run: a save past the old ~4,600-live-depth failure point
      completes, and the next Save As is not `Busy`.

## 2. Operator-gated delivery

- [ ] 2.1 OPERATOR GATE: push to main; frogg3rs pin moves to this branch's
      tip in its own commit; upstream PR.
