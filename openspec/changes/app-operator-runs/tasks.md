The executor's deliverable is a report; code changes are a side effect of it.
A conflict between this file and the code or a document is reported and stops
that task. Every new assertion is shown to fail with its production change
reverted, by the executor, before the task is reported done. A test that is red
before the task starts is reported, never edited. Build and run one binary at a
time; C++ builds run at `-j2` under `nice`. Nothing is installed. Paths are
relative to `projects/synth`. The runs are in the frogg3rs change
`frogg3rs-operator-runs`, named there "Sheaf N.N" for these tasks.

## 1. Fixes gated on operator runs

- [ ] 1.1 BUG-14, only if frogg3rs `frogg3rs-operator-runs` task 1.4 (the
      operator's `dtrace` run) confirms it: rewrite the "mutex-free,
      wait-free" claim in `include/synth/MidiController.hpp` above
      `MidiSender::TryEnqueue` to say that it signals the worker's condition
      variable on every clock or transport enqueue and edge, which can enter
      the kernel when the worker is waiting. If the run clears it, this task
      is skipped and the report says so.
      Check: the report quotes the run's count and the new comment.
- [ ] 1.2 BUG-04, only if frogg3rs `frogg3rs-operator-runs` task 1.1 (the
      operator's thread-id run) shows two thread ids (sar-35). Each
      `MidiInHandler` in `juce/MidiHandlers.hpp` takes its own
      `processorMutex_` around `processor_->Process(*midi)`, which does not
      serialize two ports that feed one bus. Give `MidiInHandler` a mutex
      shared by every handler that feeds the same engine MIDI bus, passed in
      by the code in `runtime/` that creates the handlers
      (`grep -rn "MidiInHandler" runtime` lists it), and hold it around
      `Process`; `processorMutex_` keeps guarding `SetProcessor` and
      `Processor`. The audio thread keeps popping without a lock. If the run
      shows one thread id, this task is skipped and the report says so.
      Check: a new case in a JUCE-linked test binary under `juce/` (built by
      `make -C apps/miniapp test`) drives two handlers' callback path from
      two threads into one bus for 10^6 messages and counts each message
      exactly once; it fails with the shared mutex replaced by each
      handler's own.
- [ ] 1.3 OPT-04, only if frogg3rs `frogg3rs-operator-runs` task 1.5 is a
      finding: the standalone's audio callback in `runtime/Runtime.hpp` sets
      `juce::ScopedNoDenormals`. Equal: audio above about 1.2e-38.

## 2. Delivery

- [ ] 2.1 Delivered on the branch `app-operator-runs`, made from the commit
      frogg3rs pins, pushed to the fork `daguilarc/Sheaf` (remote `fork`,
      `git@github.com:daguilarc/Sheaf.git`) and opened as the next sequential
      pull request against `jvictor0/Sheaf` `main` with `--head
      daguilarc:app-operator-runs`, once every task above is done or skipped;
      the archive commit is on that branch before it is pushed, and frogg3rs
      pins its tip (`frogg3rs-operator-runs` 3.1 runs these steps).
