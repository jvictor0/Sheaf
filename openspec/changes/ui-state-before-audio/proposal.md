# Proposal — `ui-state-before-audio`

**Created 2026-08-19 at the operator's instruction**, from a live-site
smoke finding on the frogg3rs browser host: every encoder cell renders
BLANK until audio starts or the user clicks something — no knob rings,
no parameter names, no values on first load.

## Why

Parameter/encoder UI state (`uiState_`) is populated at exactly ONE
site: `Engine::ProcessBlock`, under a publish throttle
(`include/synth/Engine.hpp:413-421`, populate at `:416`). The grid
buffer (`gridUIState_`) additionally gets a one-time pre-audio populate
in `Engine::Initialize()` (`:263`) — an asymmetry the fix's writer
discipline covers uniformly, and the reason encoders (fed by
`uiState_`) are what render blank. In the browser host, audio starts on the first user
gesture — so the frame timer (`browser/src/main.ts:222-223,311-316`)
builds frames all day against never-populated parameter state, and the
encoders draw nothing. Clicking a bank "fixes" it only because
`dispatch-action` reaches app code that rebuilds the surface with fresh
reads. The predecessor site had an explicit spec requirement (FroggersTiga
`web-mobile-knob-labels`: labels visible on load, before Play) — this is
a regression class the platform should preclude, for every browser app.
A synthetic boot-time action in the app/site shell was considered and
rejected (omni §7: incremental patch masking the real coupling; §1
corollary: the workaround exists only to avoid the correct upstream
edit).

## What Changes

- `synth-runtime-ui` (delta, ADDED requirement): building a UI frame
  SHALL reflect current parameter state whether or not the audio pump
  has ever run.
- Code: `include/synth/Engine.hpp` (a lock-free single-slot publisher
  claim — pinned in the design — giving the message thread a populate
  path only while the audio pump has provably never published, with a
  one-way latch to the audio thread). JUCE hosts GAIN behavior: the
  consuming plugin's message pump starts at construction
  (`FroggersPluginProcessor.cpp:148-149`) while `ProcessBlock` waits on
  the host, so DAW-hosted editors currently sit in the same pre-audio
  gap and will now render parameter state pre-play; behavior once audio
  runs is byte-identical to today (latched path).

## Impact

- Affected specs: `synth-runtime-ui` (ADDED).
- Affected code: `projects/synth/include/synth/Engine.hpp`
  (+ tests under the synth test suite; + browser worker only if traced
  necessary).
- Risk center: thread-safety — PopulateUIState is audio-thread-owned
  today. The design constrains the fix to a single-writer handoff, and
  the preflight audit must reject any double-writer shape.
- Delivery: own branch + PR (operator instruction: separate PR per fix,
  after preflight audit, implementation, and postflight audit with
  fixes applied). NOTE: `Engine.hpp` is in the frogg3rs plugin's include
  path — the superproject's app gate and plugin tests must re-run after
  this lands (recorded in tasks).
