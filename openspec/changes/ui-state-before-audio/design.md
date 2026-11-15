# Design — ui-state-before-audio

Anchors verified 2026-08-19 at Sheaf `fix-out-of-tree-app-gaps` working
tree; re-audited 2026-08-19 (§14 preflight REJECT round 1 — this
revision pins the mechanism the audit's adversarial trace demands and
corrects every cited defect). UNVERIFIED items are the executing task's
trace obligations.

## The coupling, traced (audit-corrected)

- `uiState_` (parameter/encoder state) has exactly ONE population site:
  `Engine::ProcessBlock` under the publish throttle
  (`include/synth/Engine.hpp:413-421`, populate at `:416`).
  `gridUIState_` has TWO: the same throttled site (`:419`) AND a
  one-time pre-audio populate in `Engine::Initialize()` (`:263`) —
  which is why this change's guard covers `uiState_` primarily and
  treats `gridUIState_` uniformly for writer discipline, not because
  grid state lacks a pre-audio site.
- `PopulateUIState` is allocation-free, per-FIELD relaxed atomic stores
  into the LIVE shared buffers (`ParameterModulation.cpp:1254-1298`,
  `:2984-3009`, `:3697-3710`) — individually atomic, NOT atomic as a
  whole. The buffers are aliased by raw pointers
  (`Engine.hpp:264-266`).
- READER tearing is pre-existing and tolerated by design: frame builds
  (message thread) already read these buffers while the audio thread's
  throttled populate writes them, with no synchronization — per-field
  atomics, display-grade data. The invariant this change must add is
  therefore WRITER-WRITER exclusion only; it must not pretend to add
  reader atomicity that never existed. (This is the audit's core
  finding, accepted: "discard on change" addressed the wrong invariant
  and could not un-tear an already-observed interleave.)
- Browser: real cross-thread concurrency (AudioWorklet native thread
  `BrowserRuntime.hpp:673-695` vs Worker-thread
  `MessageThreadTick`/frame builds, `worker.ts:554-559`, shared WASM
  linear memory).
- JUCE plugin (consuming repo): the message pump starts at CONSTRUCTION
  (`FroggersPluginProcessor.cpp:148-149`), `ProcessBlock` only after
  `prepareToPlay` + host playback — the same pre-audio gap exists in a
  DAW, so this change IMPROVES the plugin editor's pre-play frames;
  "JUCE unaffected" (round-1 proposal) was wrong and is corrected.

## Mechanism (PINNED — no candidates)

A single-slot lock-free CLAIM over UI-state publication, Engine-owned:

- `enum class UiStatePublisher : std::uint8_t { Quiescent, MessageThread, AudioThread };`
  one `std::atomic<UiStatePublisher> uiStatePublisher_{Quiescent}` in
  Engine (the layer that OWNS the buffers).
- Message thread (`MessageThreadTick`, after its existing duties): if
  `uiStatePublisher_` is `Quiescent`, CAS `Quiescent→MessageThread`;
  on success, null-check BOTH buffers exactly as `:415/:418` do, run
  the same populate pair ProcessBlock runs, then store `Quiescent`
  (release). CAS failure or non-Quiescent load → do nothing this tick.
- Audio thread (ProcessBlock's existing throttled publish site): on
  each publish opportunity, if a one-way `audioOwnsUiState_` latch
  (plain bool, audio-thread-private once set) is already true →
  populate with NO synchronization (today's exact behavior). Otherwise
  attempt CAS `Quiescent→AudioThread`; success → populate, then store
  `AudioThread` PERMANENTLY (the one-way latch: set
  `audioOwnsUiState_ = true`; `uiStatePublisher_` stays `AudioThread`
  forever). CAS FAILURE (message thread mid-populate) → SKIP this
  publish entirely and retry at the next throttle window — audio NEVER
  waits, never spins, never locks (the publish is throttled and
  display-grade; skipping one window is invisible).
- After the latch: the message thread's CAS can never succeed again
  (state is never Quiescent), so message-thread population ends
  permanently the moment audio first publishes. Writer sets are
  disjoint at every instant by construction.
- No new locks anywhere; the audio path's worst case is one failed CAS
  per throttle window during the (at most once per engine lifetime)
  transition. The audit's precedent note (`Engine.hpp:377-381` already
  takes a mutex in ProcessBlock for patch-retry) is acknowledged; the
  CAS shape is chosen over a lock anyway because it needs no blocking
  even in the transition window.

## §8 sibling enumeration (audit defect 4, closed here)

Existing "is audio live" state, all at HOST layers or for other
purposes — none owns the Engine's buffers, which is why a new
Engine-internal primitive is correct rather than duplicative:
- `Engine::sampleCounter_` (`Engine.hpp:1137`, incremented `:403`,
  exposed `SampleCount()` `:705`) — block progress, used by tests as
  corroboration; NOT a writer-exclusion primitive.
- `BrowserRuntime`: `started_` (`:1041`), `audioWorkletStarted_`
  (`:1047`), `audioWorkletBlockCount_` (`:1025`, inc `:688`) — host
  lifecycle, one layer above the buffers.
- FroggersTiga plugin: `processBlockCounter_` heartbeat
  (`FroggersPluginProcessor.hpp:432,444`) — host staleness detection,
  other repo.
The claim enum is the buffer-ownership primitive at the owning layer;
the executing task cites this section rather than re-deriving it.

## Testing

- Engine-level: never start audio → N MessageThreadTicks → UI frame
  carries names/values (positive control: identical content to a
  post-audio frame).
- Transition, both directions per §9.1, at the REAL primitive: force
  the CAS collision deterministically (test seam: hold the claim in
  MessageThread state via a test hook or a controlled tick while
  triggering a publish window) and assert (a) audio skips that window
  without blocking (sampleCounter_ advances, no populate), (b) audio
  claims and latches at the next window, (c) message-thread CAS fails
  forever after, (d) a frame built after the latch reflects
  audio-published state. The harness controls interleaving at the
  primitive's own seam — it does NOT claim to prove arbitrary
  OS-thread interleavings safe; the SAFETY argument is the mutual
  exclusion by CAS (structural), and the test proves the machine's
  states and transitions behave as designed (§9.1: the guard is shown
  able to fire, both directions).
- Browser-level: freshly installed app, no activation, no action →
  first frames carry encoder draw commands/labels.
- Null-safety: MessageThreadTick populate branch with an uninitialized
  engine (no Initialize) → no crash, no populate (mirrors `:415/:418`).
- JUCE window: covered superproject-side (tasks 1.4) — plugin editor
  frames pre-processBlock carry parameter state.

## Risks

- The claim machine is new cross-thread state: memory ordering must be
  acquire/release on the CAS/store pair (executor documents each order
  choice inline, citing this design).
- Liveness is lock-free, not starvation-free: audio's latch could in
  principle be deferred while message-thread claims collide with its
  throttled publish windows. Accepted explicitly: the claim window is
  sub-millisecond inside a ~33ms tick, the two clocks are independent
  and jittered, a sustained collision has no realistic mechanism, and a
  skipped window costs one display-grade publish (re-audit question 2,
  answered non-blocking).
- `uiPublishInterval_` semantics unchanged while audio runs (latched
  path is byte-identical to today's behavior).
