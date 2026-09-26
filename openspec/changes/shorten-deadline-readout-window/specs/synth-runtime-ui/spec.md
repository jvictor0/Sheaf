# Delta — `synth-runtime-ui`

**Added 2026-08-27** (an 8.5-second hold on the deadline readout reads
as current load long after the instrument is idle; `sru-2` fixes no
window length, so the hold is shortened without a spec change there).

## ADDED Requirements

### Requirement: sru-62 — Deadline readout: window sized from the UI-timer rate, whole-percent precision

THE runtime library SHALL size the deadline readout's rolling window
from the application's configured UI-timer rate rather than a fixed
frame count, targeting approximately one second of UI frames, and
SHALL render the readout's held maximum as a whole percentage.

#### Scenario: A spike is still visible a frame later

- **WHEN** a single audio callback spikes the load percentage and the
  very next UI frame is cheap
- **THEN** the readout still displays the spike value

#### Scenario: A spike is gone once its window has elapsed

- **WHEN** a spike is followed by cheap callbacks for the rest of the
  window's own span
- **THEN** the readout no longer displays the spike

#### Scenario: A different UI-timer rate gets a different frame count, the same duration

- **WHEN** an application configures a UI-timer rate other than the
  runtime's 30 Hz default
- **THEN** the readout's window still covers approximately one second
  of wall time, not a fixed number of frames

#### Scenario: Whole-percent rendering

- **WHEN** the sidebar renders the deadline readout at any value
- **THEN** the rendered percentage carries no decimal digits
