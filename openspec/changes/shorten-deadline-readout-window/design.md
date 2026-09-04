# Design — shorten-deadline-readout-window

## sru-62 — deadline readout: window and precision

- `RollingMax256` (`MidiConfigViewModel.hpp`) becomes `RollingMax`: a
  ring buffer over `std::vector<float>`, capacity fixed at
  construction instead of a compile-time `kCapacity`. `Write`/`Max`
  are otherwise unchanged.
- `DeadlineWindowCapacity(int uiFrameHz)` derives the capacity: about
  one second of UI frames (`capacity == uiFrameHz`, falling back to 30
  for an unset or non-positive rate, matching the fallback
  `Engine.hpp`/`Runtime.hpp` already use for the same field). One
  second is chosen against what the window has to catch: a spike has
  to still be on screen long enough that a person glancing at the
  sidebar actually sees it, not just long enough to survive a single
  33ms redraw. A second of hold comfortably clears that bar without
  reintroducing the original defect (an 8.5-second hold reading as
  current load).
- `RuntimeMainComponent` constructs its `RollingMax` member from
  `DeadlineWindowCapacity(App::Config().uiFrameHz)` — the window is
  sized from the UI-timer rate rather than a literal frame count,
  because `uiFrameHz` is per-application configuration
  (`RuntimeConfig::uiFrameHz`, `AppContext.hpp:40`) and a fixed frame
  count would be a different hold on a different host.
- `FormatDeadlineText` (`RuntimePages.hpp`) drops from `%.1f` to
  `%.0f`. A held maximum has no tenth-of-a-percent accuracy, and
  dropping the decimal is what lets three digits fit the 96px sidebar
  column (`Layout::kSidebarWidth`). The label stays `CPU ` (`sru-59`);
  only the number's precision changes.

## Testing

- `viewmodel_tests.cpp`: the three existing `RollingMax256*` tests
  updated to construct `RollingMax(256)` (unchanged capacity, unchanged
  assertions — they test the ring buffer's own mechanism, not the
  production window). New: `DeadlineWindowCapacityTracksTheUiFrameRate`
  pins the derivation, including the non-positive fallback;
  `ShortenedDeadlineWindowDropsAStalePeakWithinItsOwnSpan` is the
  positive control — a spike written one frame ago is still the
  reported max (proof the window isn't degenerately short), and the
  same spike is gone once its own capacity has elapsed.
- `portable_ui_tests.cpp`: new
  `TestSidebarDeadlineNodeTextIsWholePercent` asserts the whole-percent
  text through `BuildSidebarTree`, not `FormatDeadlineText` alone.
  Existing sidebar/deadline assertions across this file,
  `runtime_main_component_tests.cpp`, and
  `juce/RuntimePagesJuceTests.cpp` updated to whole-percent text; any
  existing test value that landed on a `.5` boundary (`%.0f` rounds
  half to even) was moved off it rather than asserting the rounded
  result of a boundary value.
- `browser/tests/audio-flow.spec.ts`: the `"CPU 0.0%"` sentinel becomes
  `"CPU 0%"` at all three occurrences.
