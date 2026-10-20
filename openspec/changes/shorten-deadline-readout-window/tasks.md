# Tasks — shorten-deadline-readout-window

Gate: `make -C projects/synth test`. Not run as part of this change's
authoring pass (serialized by the consuming session to avoid overloading
the build machine); implementation and tests are written and traced by
hand against the gate below, verification pending.

## 1. sru-62 — shorter window, whole percent

- [ ] 1.1 `RollingMax256` → `RollingMax` (capacity parameter) plus
      `DeadlineWindowCapacity(uiFrameHz)` in `MidiConfigViewModel.hpp`;
      `RuntimeMainComponent` constructs from
      `DeadlineWindowCapacity(App::Config().uiFrameHz)`.
- [ ] 1.2 `FormatDeadlineText` → `%.0f`; stale comment in
      `runtime/Runtime.hpp` updated.
- [ ] 1.3 Every consumer of the rendered string or the old type name
      updated: `tests/viewmodel_tests.cpp`,
      `juce/RuntimePagesJuceTests.cpp`, `tests/portable_ui_tests.cpp`,
      `tests/runtime_main_component_tests.cpp`,
      `browser/tests/audio-flow.spec.ts`.
- [ ] 1.4 New tests: window-capacity derivation, positive-control spike
      test, sidebar-tree whole-percent test (see design.md Testing).
- [ ] 1.5 Full synth gate green.

## 2. Operator-gated PR

- [ ] 2.1 OPERATOR GATE: own commit and, per this repo's per-fix
      delivery convention, own PR.
