## 1. Gate the MIDI request on the Controllers action, with a checked mirror

- [x] 1.1 Add a browser-owned mirror of `Actions::kSidebarControllers`'s
      literal value in `main.ts` (a small `as const` constant colocated with
      its one consumer, following `audio.ts`'s `AudioOutputRouteAction`
      pattern for a mirrored C++ action name).
- [x] 1.2 Change `SynthBrowserApp`'s dispatch callback / `startUserActivation`
      (`main.ts:179-181,293-305`) so the MIDI half of activation
      (`this.midi.startFromUserActivation()`) runs only when the dispatched
      action's name equals task 1.1's constant; the audio half
      (`this.audio.startFromUserActivation()`) stays unconditional on every
      dispatched action, unchanged.

      The existing early return (`if (this.activationStarted || !this.audio)
      return;`, `main.ts:294`) gates the whole function today, and it must
      never gate the MIDI half: a Controllers dispatch has to reach
      `this.midi.startFromUserActivation()` even after an earlier,
      non-Controllers dispatch has already started audio and set the latch.
      Restructure the early return so it governs only whether the audio half
      re-runs; the MIDI half's call, gated on the action name, must stay
      reachable independent of the latch's value. Task 1.3's test is what
      catches a version that gets this wrong: it dispatches a non-Controllers
      action before the Controllers action, so a latch that also blocks the
      MIDI half will report zero MIDI requests instead of the one the test
      asserts.
- [x] 1.3 Add a test (`midi-flow.spec.ts`, name: `"requests MIDI only when the
      Controllers sidebar action dispatches"`) that dispatches a non-Controllers
      action and asserts no MIDI request occurs, then dispatches the
      Controllers action and asserts exactly one.
- [x] 1.4 Add a test to `version-drift.test.mjs` (name: `"the sidebar
      Controllers action name agrees with RuntimePages.hpp"`) that reads
      `include/synth/RuntimePages.hpp` for `Actions::kSidebarControllers`'s
      literal value the same way that file's existing tests read C++ headers
      for version and sentinel literals, and asserts it equals task 1.1's
      constant.
- [x] 1.5 Prove task 1.4's check fails on drift: temporarily change one side
      (either the TS constant or the C++ literal) to a different string, run
      the new test, and capture the failing assertion output verbatim. Revert
      the temporary change and re-run to confirm the test passes again.
      Record both outputs in the commit or PR description; this task is not
      complete until both are captured.
- [x] 1.6 Confirm `midi-flow.spec.ts:153`'s `"requests Web MIDI sysex
      permission and remains offline when it is denied"` and `:180`'s
      `"reconciles leased MIDI access without requesting permission a second
      time"` construct `BrowserMidiManager` directly rather than going through
      `main.ts`'s dispatch wiring; if so, they are unaffected by task 1.2's
      gating and need no change beyond confirming that. If either test
      exercises the gating path instead, update it there.
- [x] 1.7 Fix `startUserActivation`'s comment in `main.ts` (the function task
      1.2 already edits): it asserts that both
      `this.audio.startFromUserActivation()` and
      `this.midi.startFromUserActivation()` short-circuit once already
      running. `BrowserMidiManager.startFromUserActivation()` guards re-entry
      on a field its callee assigns only after a permission grant resolves,
      not before the request is made, so the claim is false for the MIDI
      half: two dispatches reaching it before the first settles both call
      `navigator.requestMIDIAccess`, and the second then reports `offline` on
      a manager that is actually online. Reword the comment to state what the
      guard actually does, without changing the guard itself — this task
      fixes the comment's claim, not the underlying re-entrancy behavior.
- [x] 1.8 At load, query `navigator.permissions.query({ name: "midi", sysex: true })`.
      When it reports `granted`, start MIDI immediately through the same
      `BrowserMidiManager` path the Controllers action uses; a request in that
      state shows no prompt. On any other state, on a rejected query, or where
      the Permissions API is absent, do nothing at load and leave MIDI to the
      Controllers action. Never let the query's outcome affect audio or boot.

      lib.dom's `PermissionDescriptor` type is `{ name: PermissionName }`
      (`node_modules/typescript/lib/lib.dom.d.ts:1441-1443`), which has no
      `sysex` field, and this package builds under `strict: true`
      (`tsconfig.json`). The query argument needs a cast (for example, `{
      name: "midi", sysex: true } as PermissionDescriptor`) to type-check.

      Check: a browser test in `midi-flow.spec.ts` covers three cases with a
      stubbed Permissions API — `granted` requests MIDI at load with no
      Controllers action; `prompt` requests nothing until the Controllers
      action; a query that throws requests nothing and boot completes — and is
      proven able to fail by forcing the `granted` branch off and watching the
      first case go red.

## 2. Documentation

- [x] 2.1 Check `projects/synth/docs/coverage.md` (and any other
      spec-to-test coverage doc under `projects/synth/docs/`) for references
      to `sbw-5`'s permission-timing text; update any that this change's
      revised requirement text makes stale.

## 3. Verification

- [x] 3.1 `openspec validate gate-browser-midi-on-controllers --strict`
      passes with no errors.
- [x] 3.2 `make openspec-check` passes (no duplicate live requirement IDs
      introduced once this change is archived).
- [x] 3.3 Full existing suites this change touches
      (`projects/synth/browser`'s `npm test`, and `make test` under
      `projects/synth`) pass, including every test named in group 1 above.

## 4. Delivery

- [x] 4.1 Commit on branch `gate-browser-midi-on-controllers`, push it to the
      fork, and open the next pull request in the stack against upstream
      `main`, stating that it stacks on #17. Then add the delivery-record commit
      naming the pull request, and push it to the same branch.
- [x] 4.2 In frogg3rs, correct `app/browser/site/site-boot.mjs`'s header
      comment about when MIDI is requested, against the pinned code.
      Check: every statement in it about MIDI is true of the pinned code.
- [x] 4.3 In frogg3rs, pin `External/Sheaf` at the branch tip from 4.1.
      Check: the log between the old and new pin shows source changes.
- [x] 4.4 In frogg3rs, rewrite `app/browser/e2e/midi-activation.spec.mjs`:
      - `"reports a MIDI status after the first in-app action"`: a first
        in-app action outside the Controllers page requests no MIDI; opening
        Controllers (`runtime.sidebar.controllers`) does. Correct the file's
        header comment to match.
      - `"reports the MIDI state its host can actually reach"` grants
        `midi-sysex` before the page loads, so task 1.8's saved-grant start
        brings MIDI up at load in this test. Assert on the status after the
        sequence the test drives, not on which action started it.
      Check: both pass against the new pin; with task 1.2's gating reverted,
      the first fails.
- [x] 4.5 Run frogg3rs's full suite against the new pin.
      Check: counts are reported against counts measured on the same tree
      before the pin bump.
- [x] 4.6 Push frogg3rs to `main`. After the site redeploys, verify in a fresh
      browser with no stored permission: no MIDI request on load or on a first
      click outside the Controllers page, and one on opening Controllers.

## 5. Fix reviewer-found defects in the MIDI activation gate

- [x] 5.1 Share one in-flight `navigator.requestMIDIAccess` call and its
      outcome across overlapping callers of
      `BrowserMidiManager.startFromUserActivation` (`src/midi.ts`): a second
      Controllers click while an earlier one's permission prompt is still
      open, or the load-time saved-grant start racing a Controllers click,
      both reach the guard before `this.access` is set, so each used to issue
      its own `requestMIDIAccess` call, with the second reporting `offline`
      on a manager that was actually online.
      Check: `midi-flow.spec.ts: overlapping activation calls share one MIDI
      request and report the same outcome` — two overlapping calls make
      exactly one `requestMIDIAccess` call and both report `online` after the
      grant. Proven able to fail: reverting the sharing made the same test
      report two requests.
- [x] 5.2 Render the app's status once the load-time saved-grant start
      (`main.ts`'s `startMidiIfAlreadyGranted`) brings MIDI up, the same way
      the Controllers path already renders it; this path previously left
      whatever status was last rendered before that background start
      resolved.
      Check: `midi-flow.spec.ts: starts MIDI at load only when the
      Permissions API already reports it granted` — with permission granted
      at load and no Controllers action, the status reads `midi:online`.
      Proven able to fail: reverting the render left the status at
      `running`.
- [x] 5.3 Extend that same test so its own Check is asserted directly rather
      than left implicit in the request count alone: after a `prompt`
      answer, opening Controllers makes exactly one MIDI request; and a
      throwing permissions query still leaves boot completed (the status
      reaches `running`).
      Check: same test as 5.2, its `prompt`-then-Controllers and `throws`
      assertions. Proven able to fail: breaking the Controllers action mirror
      made the `prompt`-then-Controllers assertion fail; awaiting the
      saved-grant check without its catch made the `throws` case fail boot
      outright.
- [x] 5.4 Fix the stale setup in `tests/midi-flow.spec.ts`'s real-miniapp-WASM
      reconnect test: `HandleAddController` now reads the page's own preset
      draft instead of the value `add_controller` is dispatched with, so
      dispatching it with `"peer:wrldbldr"` silently added whichever preset
      the combo defaulted to (a MIDI Fighter Twister, CC-only feedback)
      instead of WRLD.Bldr. Select the WRLD.Bldr preset first through
      `add_preset_draft` so the reconnected slot's re-sent feedback actually
      includes the SysEx frame the test asserts on.
      Check: `midi-flow.spec.ts: real miniapp WASM keeps two Web MIDI
      controller slots independent through reconnect`. Proven able to fail:
      the unmodified setup reports no SysEx frame in the re-sent feedback.

