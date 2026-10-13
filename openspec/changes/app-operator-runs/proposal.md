# Proposal — `app-operator-runs`

Holds the library fixes that wait on runs only the operator can make: they
need hardware, host applications, `sudo` or a machine this Mac is not. The
frogg3rs change `frogg3rs-operator-runs`
(`openspec/changes/frogg3rs-operator-runs/`) holds the runs and orders these
tasks. Nothing in this change waited on them (operator ruling 2026-09-23:
"Operator runs never hold delivery").

## Why

The audit of frogg3rs `main` against its ratified user story left three
library items that reading could not settle:

- BUG-14: the comment above `MidiSender::TryEnqueue` in
  `include/synth/MidiController.hpp` calls it "mutex-free, wait-free", while
  it signals the worker's condition variable on clock and transport enqueues,
  which can enter the kernel. Settled by a `dtrace` count on the standalone.
- BUG-04: every MIDI input port's `MidiInHandler` (`juce/MidiHandlers.hpp`)
  pushes into the engine's single-producer MIDI bus under its own
  `processorMutex_`, which serializes nothing across two ports. Settled by
  logging the callback thread with two controllers streaming.
- OPT-04: whether flush-to-zero is set in the standalone's audio callback,
  and, where it is not, whether a decaying tail misses the block deadline on
  x86.

## What changes

Tasks 1.1 to 1.3, each applying only if its frogg3rs run confirms it. sar-35 is
added (`specs/synth-app-runtime/spec.md`).

## Impact

`include/synth/MidiController.hpp` (comment), `juce/MidiHandlers.hpp` and the
code in `runtime/` that creates the input handlers, `runtime/Runtime.hpp`, and
a JUCE-linked test binary under `juce/`.

## Delivery

On the branch `app-operator-runs`, pushed to the fork `daguilarc/Sheaf` and
opened as the next sequential pull request against `jvictor0/Sheaf` `main`;
nothing is merged, and frogg3rs pins the branch tip (task 2.1).
