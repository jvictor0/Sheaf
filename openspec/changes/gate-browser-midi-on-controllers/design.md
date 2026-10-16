## Context

Two boot paths exist for a browser-hosted synth application, and both end at
the same `SynthBrowserApp`:

- The direct path (`app/browser/site/site-boot.mjs`, the frogg3rs deployed
  site): supplies an `AudioContext` through `audioOptions` and no
  `activationLease`. MIDI reaches the runtime only through `main.ts`'s
  generic dispatch wiring.
- The lease path (`installSheafPatchLauncher` → `launchCatalogApplication` →
  `installSynthBrowserApp`, `main.ts:449-486`): a "Launch" click synchronously
  calls `ActivationLease.acquire()` (`main.ts:449`), which starts
  `AudioContext.resume()` and `navigator.requestMIDIAccess({ sysex: true })`
  together (`activation.ts:45-63`). `installSynthBrowserApp` awaits the lease
  (`main.ts:400`) and threads the resolved `midiAccess` into
  `SynthBrowserApp`'s constructor options (`main.ts:411`); `start()` sets its
  one-shot latch (`this.activationStarted = true`, `main.ts:238`) and starts
  MIDI from that pre-fetched access before the latch is ever consulted by a
  dispatched action (`main.ts:237-246`).

This change is scoped to the direct path only. The lease path's own eager
request, and `start()`'s latch-before-dispatch behavior, are unaffected: see
the proposal's "Not in this change, and why". Because `start()` sets the
latch before any action dispatches on a leased app, `startUserActivation`'s
gating (below) never runs meaningfully for a leased app in the first place —
its dispatch-time check only has an effect on the direct path, where the
latch starts false and the first dispatched action is what reaches it.

`main.ts`'s dispatch callback (`:179-181`) forwards every dispatched action to
both `dispatchAction` (the generic WASM channel) and `startUserActivation`
(`:293-305`), which races audio and MIDI activation on every action with no
name check:

```ts
private async startUserActivation(): Promise<void> {
  if (this.activationStarted || !this.audio) return;
  const [audio, midi] = await Promise.all([
    this.audio.startFromUserActivation(),
    this.midi.startFromUserActivation(),
  ]);
  this.activationStarted = audio.started && midi.status === "online";
  ...
}
```

The early return governs the whole function today: once
`this.activationStarted` is true, neither half runs again. Gating only the
MIDI half's call site without touching this early return would leave a gap —
see "Risks" below.

`Actions::kSidebarControllers` is declared once, at `RuntimePages.hpp:142`
(`"runtime.sidebar.controllers"`; the same literal also names the sidebar's
`NodeIds::kSidebarControllers` at line 39 — two different namespaces, same
value, an established pattern in this file). The browser side has no mirror
of that literal today, so there is nothing for it to check the dispatched
action's name against.

The behavioral premise — that the live site requests MIDI on load, from a
synthetic action dispatched during the first render frame rather than from a
Controllers click — was measured directly against the deployed site: a wrapped
`requestMIDIAccess` recorded a call 966ms after navigation, with no input yet
performed, whose stack trace runs through `dispatchViewportNarrow`
(`mobile-stack.mjs`) into `SynthBrowserApp.startUserActivation`. The same
measurement confirmed the Permissions API answers `navigator.permissions.query
({ name: "midi", sysex: true })` without prompting, updates a held
`PermissionStatus` reactively when granted later in the same page lifetime,
and lets `requestMIDIAccess` resolve without a prompt once granted — the
mechanism task 1.8 depends on.

No other active change overlaps. This change modifies `sbw-5`'s existing text
in place and claims no new requirement ID.

## Goals / Non-Goals

**Goals:**

- MIDI permission is requested only when the user opens the Controllers page,
  on the boot path that has no activation lease (the frogg3rs site's own
  boot).
- A player whose browser already holds the permission gets MIDI at load, with
  no prompt.
- The C++ action-name constant and its browser mirror cannot drift silently.
- Every existing test that exercises the per-action handler and pins its old
  unconditional MIDI request is updated to pin the Controllers-only contract.

**Non-Goals:**

- Changing `ActivationLease` or `SynthBrowserApp.start()`'s lease-driven MIDI
  branch. frogg3rs's site boot supplies no lease, so this change's player
  story does not need it touched; it is a separate, larger change (see the
  proposal's "Not in this change, and why").
- Adding a Controllers-page MIDI retry or permission-request button. The
  Controllers action is already the route back: opening Controllers
  re-requests MIDI whenever it is not yet held, since the request fires on
  that action every time it dispatches, not once per session.
- Changing how MIDI messages are routed, mapped, or reconciled once access is
  granted (`sbw-5`'s existing scenarios below the permission-timing text).
- Any change to non-browser hosts' MIDI behavior (JUCE/VST embed): they never
  call `navigator.requestMIDIAccess` and nothing in this change touches C++
  MIDI dispatch.

## Decisions

**Controllers-gating lives in `main.ts`'s existing dispatch callback, keyed
on a browser-owned mirror of `Actions::kSidebarControllers`.** The callback
already sees `action.name` for every dispatched action (`:179-181`); no new
channel is needed to learn which action fired. The mirror is a `SidebarAction`
(or similarly named) `as const` string constant colocated with its one
consumer in `main.ts`, following the same colocation `audio.ts`'s
`AudioOutputRouteAction` already demonstrates for a mirrored C++ action name.
Considered: generating the TypeScript constant from the C++ header at build
time. Rejected as disproportionate — this codebase's established answer to
one cross-language literal is a checked mirror (`version-drift.test.mjs`),
not a code generator, and introducing the first generator for one string
is a bigger, riskier change than the feature it would serve.

**The gate changes what the early return governs, not just what the MIDI
call site checks.** `startUserActivation`'s latch (`this.activationStarted`)
is set only after both halves are attempted together today; once the MIDI
half runs conditionally, the latch can no longer mean "both halves are done"
without risking that a later Controllers dispatch finds the function already
short-circuited by an earlier, non-Controllers action's audio success. The
early return is restructured so it governs re-running the audio half only;
the MIDI half's own call, gated on the action name, stays reachable
independent of the latch's value.

**The drift check is a new test in `version-drift.test.mjs`, not a new
file.** It reads `RuntimePages.hpp` for `Actions::kSidebarControllers`'s
literal value the same way the existing tests in that file read C++ headers
for version integers and sentinel integers, and asserts it equals the new
TypeScript literal. It must be proven to fail once, by deliberately
mismatching one side, capturing the failure, then reverting — a task in this
change's tasks.md carries that step explicitly rather than trusting the
check by construction.

## Risks / Trade-offs

- [The existing one-shot latch can silently swallow the Controllers request]
  `startUserActivation`'s early return (`if (this.activationStarted ||
  !this.audio) return;`) gates the whole function today. A version of the
  gate that leaves this check unchanged and only adds a name check around
  the MIDI call risks exactly this: a non-Controllers action runs first,
  audio starts, and if the latch is set from that alone, a Controllers
  action dispatched afterward finds the function already returning early and
  never reaches `this.midi.startFromUserActivation()` at all → task 1.2
  restructures the early return so the MIDI half stays reachable regardless
  of the latch, and task 1.3's test (dispatch a non-Controllers action, then
  the Controllers action, assert exactly one MIDI request) is the one that
  goes red against a version that gets this wrong.
- [Frogg3rs superproject test asserts the old contract]
  `app/browser/e2e/midi-activation.spec.mjs`'s "reports a MIDI status after
  the first in-app action" pins MIDI firing on Play, not Controllers → this
  change's delivery rewrites it in frogg3rs alongside the pin bump, and its
  reverted-gating check proves the rewritten test can fail.
- [New cross-language mirror is another thing to keep in sync] Every mirror
  this codebase has added has needed its own check (`version-drift.test.mjs`'s
  own history, per its file comment) → this change adds exactly one, checked,
  proven to fail on drift before this change ships.
- [Overlapping MIDI activation calls shared one outcome] Two callers reaching
  `BrowserMidiManager.startFromUserActivation()` before either settles —
  a second Controllers click while an earlier one's permission prompt is
  still open, or the load-time saved-grant start racing a Controllers click —
  used to both call `navigator.requestMIDIAccess`, with the second call
  throwing into `startFromUserActivation`'s own catch and reporting
  `offline` on a manager that was actually online. `startFromUserActivation`
  now holds the in-flight activation as a field: a second caller that
  arrives before the first settles is handed that same promise instead of
  issuing its own request, so both callers see the one outcome. Gating the
  MIDI request on the Controllers action also narrows how often two callers
  can still land inside the same tick in the first place, but the sharing is
  what makes the outcome correct when they do.

## Migration Plan

No data migration. This is a behavior change in a browser build with no
persisted state affected: `SynthBrowserApp` and `BrowserMidiManager` are
constructed fresh per page load. Rollback is reverting the commit; no flag or
staged rollout is needed because the change is contained to Sheaf's
browser package and the frogg3rs test and comment its delivery updates.

## Open Questions

- The exact rewrite of `app/browser/e2e/midi-activation.spec.mjs` cannot be
  finalized from this submodule: it depends on whether frogg3rs's site
  exposes a reachable Controllers sidebar button in the same page the Play
  button lives on (its own helpers already list `runtime.sidebar.controllers`
  as a known selector, per `app/browser/e2e/helpers.mjs:40`, which suggests
  yes, but that is the frogg3rs superproject's call to confirm).
