## Why

A player who opens the frogg3rs site is asked for Web MIDI sysex permission
about a second after the page loads, before they have touched anything but
the initial view. Measured on the live site, in a fresh browser with a wrapper installed
before any page script: `navigator.requestMIDIAccess({ sysex: true
})` fires 966ms after navigation, before any click, from
`SynthBrowserApp.startUserActivation()` (`main.ts:293-305`). It fires that
early because frogg3rs's mobile layout code dispatches a synthetic viewport
action during the app's first rendered frame, and `main.ts`'s dispatch
wiring (`main.ts:179-181`) calls `startUserActivation()` after *every*
dispatched action, with no check on which action fired. The same call fires
again on every later click, whether or not it has anything to do with
Controllers.

The fix the operator wants: MIDI permission is requested only once the
player opens the Controllers page, where MIDI is actually used. A player
whose browser already holds the permission is never asked again: their MIDI
starts at load, silently, so a controller set up on an earlier visit works
as soon as the page opens.

## What Changes

- The browser's per-action activation handler requests MIDI only when the
  dispatched action is the sidebar Controllers action
  (`Actions::kSidebarControllers`, `"runtime.sidebar.controllers"`), not on
  every dispatched action. The handler's existing one-shot latch is
  restructured so it can no longer swallow a later Controllers dispatch: it
  still gates re-running the audio half, but the MIDI half stays reachable
  from the Controllers action regardless of the latch's state.
- At load, the browser asks the Permissions API whether Web MIDI with sysex
  is already granted. Only when it reports `granted` does it start MIDI
  then; a request in that state shows no prompt. Any other answer, or a
  browser without the query, leaves MIDI to the Controllers action.
- The browser's mirror of that action string is checked against the C++
  `Actions::kSidebarControllers` constant by an automated test that fails on
  drift, in the style `version-drift.test.mjs` already establishes for other
  cross-language mirrors — proven to fail by breaking the mirror once, not
  asserted by comment.
- `BrowserMidiManager.startFromUserActivation` shares one in-flight
  `navigator.requestMIDIAccess` call and its outcome across overlapping
  callers, instead of each issuing its own request: a second Controllers
  click while an earlier one's prompt is still open, and the load-time
  saved-grant start racing a Controllers click, now report the one real
  outcome instead of a stale `offline` on a manager that is online.
- The load-time saved-grant start renders the app's status once MIDI comes
  up, the same way the Controllers path already does, instead of leaving the
  displayed status wherever it stood before that background start finished.

## Not in this change, and why

The activation-lease launcher path (`ActivationLease`, and
`SynthBrowserApp.start()`'s `options.midiAccess` branch) is untouched.
frogg3rs's own site boot supplies no activation lease, so the player story
this change serves never reaches that path; that branch is also the only
place a lease-launched app starts audio today, so changing it would leave a
Sheaf-catalog launcher silent until a second click. That is a separate
change.

A Controllers-page MIDI retry or permission-request button is also not
added here. The gate above already is the way back: reopening the
Controllers page re-requests MIDI whenever it is not yet held, because the
request fires on the Controllers action itself every time it dispatches,
not once per session. A dedicated button would only ever be reached the same
way a player already reaches it, so it would add a second control for the
same action without adding capability.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `synth-browser-wasm-runtime`: `sbw-5` (MIDI: Web MIDI sysex multi-device
  bridge) gains normative text for *when* the per-action activation handler
  requests MIDI access — on the Controllers action, or at load when the
  Permissions API already reports the access granted — and keeps that
  handler's existing invariant that a denied, unavailable, or
  not-yet-requested MIDI condition never blocks audio activation or startup.

## Impact

- Browser TypeScript: `projects/synth/browser/src/main.ts`, `projects/synth/browser/src/midi.ts`.
- Tests: `midi-flow.spec.ts`, `runtime-core.spec.ts`, `version-drift.test.mjs`.
- frogg3rs, delivered as this change's delivery steps: its
  `app/browser/e2e/midi-activation.spec.mjs` pins "the first in-app
  action... requests audio and MIDI together", and
  `app/browser/site/site-boot.mjs`'s header comment says the same. Both are
  corrected there, alongside the pin bump. No frogg3rs spec text changes: the
  promoted clause that a lease requests MIDI stays true.
- No other active change overlaps.
