# Delta — `synth-runtime-ui`

**Added 2026-08-19** (frogg3rs live-site smoke finding: encoder cells
blank until audio starts or an action is dispatched; the predecessor
site's spec explicitly required labels before Play).

## ADDED Requirements

### Requirement: sru-48 — UI frames reflect parameter state without the audio pump
WHEN a UI frame is built before audio has ever started, or while the
audio pump is quiescent, THE frame SHALL reflect current parameter
state — names, values, and draw commands — identical in content to a
frame built after the audio pump has published, and the population
path SHALL preserve a single writer for the UI-state buffers at every
instant (message-thread population only while the audio pump is
provably quiescent, audio-thread population otherwise, with a proven
handoff at the transition).

#### Scenario: First frame is fully drawn
- **WHEN** a browser host installs an app and builds frames with no
  audio activation and no user action
- **THEN** parameter controls render with their names and values from
  the first frames

#### Scenario: Handoff to the audio pump is race-free
- **WHEN** audio starts while frames are being built
- **THEN** UI-state population transfers to the audio pump without a
  frame of interleaved or torn state, and message-thread population
  provably stops
