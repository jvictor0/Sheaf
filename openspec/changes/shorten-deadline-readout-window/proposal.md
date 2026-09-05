# Proposal — `shorten-deadline-readout-window`

**Added 2026-08-27**, from an audit of the sidebar's deadline readout
alongside `fix-out-of-tree-app-gaps` (`sru-59`, which labels the same
readout).

## Why

The readout holds the maximum audio-callback deadline percent over the
last 256 UI-timer writes (`RollingMax256`, `MidiConfigViewModel.hpp`).
At the runtime's 30 Hz UI timer (`RuntimeConfig::uiFrameHz`,
`AppContext.hpp:40`) that is an 8.5-second hold: a one-off startup
transient sits on screen for over eight seconds after the instrument
goes idle, reading as the current load. `sru-2` fixes no window length
("a rolling window of recent UI frames, updated on the UI timer"), so a
shorter window already satisfies it — 256 was an implementation choice,
not a requirement.

Separately, the readout renders to a tenth of a percent
(`FormatDeadlineText`, `"CPU %.1f%%"`) despite being a held maximum, not
a live sample. A held maximum has no tenth-of-a-percent accuracy, and
the extra digit is also why the label needs the full 96px sidebar
column (`Layout::kSidebarWidth`) it has today.

## What Changes

- `synth-runtime-ui` (delta, ADDED requirement, refining `sru-2`): the
  deadline readout's rolling window covers approximately one second of
  UI frames — sized from the UI-timer rate rather than a fixed
  frame count, because that rate is per application — and the readout
  renders the held maximum as a whole percentage.
- Code:
  - `include/synth/MidiConfigViewModel.hpp`: `RollingMax256` (fixed
    256-slot ring buffer) becomes `RollingMax` (ring buffer
    parameterized by capacity) plus `DeadlineWindowCapacity(uiFrameHz)`,
    which derives that capacity from the UI-timer rate.
  - `include/synth/RuntimeMainComponent.hpp`: constructs its
    `RollingMax` member from `DeadlineWindowCapacity(App::Config().uiFrameHz)`.
  - `include/synth/RuntimePages.hpp`: `FormatDeadlineText` drops to
    `%.0f`.
  - `runtime/Runtime.hpp`: comment referencing the old type name and
    capacity updated.
  - Every test pinning the rendered string or the old type name.

## Impact

- Affected specs: `synth-runtime-ui` (ADDED requirement).
- Affected code: as above; no backend-specific change (both JUCE and
  browser hosts read the same formatted string off the same node).
- Backward compatibility: the readout still shows the running maximum,
  still labeled `CPU `; only the hold duration and the decimal
  precision change. A single-frame spike is still on screen for many
  redraws after it happens (about a second at the runtime's default
  30 Hz), so it stays readable by eye.
- Delivery: implemented on the `fix-out-of-tree-app-gaps` working
  branch alongside other in-flight work; own commit and, per this
  repo's per-fix delivery convention, its own PR when the branch is
  otherwise ready.
