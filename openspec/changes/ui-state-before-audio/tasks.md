# Tasks — ui-state-before-audio

Gates: Sheaf synth test suite green (all pre-existing tests unmodified;
NOTE for this machine: two 96kHz deadline tests fail deterministically
on this Mac and are the KNOWN baseline — green means "green except
those two", report them explicitly). Browser suite green. No commits
until the operator-gated commit/PR step. Superproject re-verification
after the change (frogg3rs app gate + plugin ctest) is task 1.4, NOT
optional — Engine.hpp is in the plugin's include path.

## 1. Implementation + tests

- [x] 1.1 Trace obligations (design): re-run the PopulateUIState operand
      grep and cite BOTH gridUIState_ sites (`Engine.hpp:263` and
      `:419`) plus the single uiState_ site (`:416`); confirm no
      audio-lifecycle flag exists inside Engine.hpp (preflight-verified:
      only `arenaGrowPending_`); cite the design's §8 sibling
      enumeration rather than re-deriving it; pick and cite the
      message-thread seam (MessageThreadTick vs BuildUIFrame entry).
      Verified complete: citations present in design.md's "The coupling,
      traced" and "§8 sibling enumeration" sections.
- [x] 1.2 Implement the PINNED claim machine exactly as designed
      (enum + CAS + one-way latch; audio never waits, CAS-fail = skip
      one throttled publish; message-thread populate null-checks both
      buffers mirroring `Engine.hpp:415/:418`); memory-order choices
      documented inline citing the design.
      Verified complete: `Engine.hpp` ProcessBlock/MessageThreadTick
      match the design exactly, with inline acq_rel/relaxed rationale.
- [x] 1.3 Tests per the design's Testing section: pre-audio population;
      the four-assertion transition test at the claim primitive's own
      seam (skip-without-blocking, claim-and-latch, message-CAS-fails-
      forever, post-latch frame content); browser first-frame test;
      null-safety test (MessageThreadTick before Initialize).
      Verified complete: the three new `engine_tests.cpp` cases pass
      (pre-audio population, transition, null-safety).
      POSTFLIGHT CORRECTION (2026-08-19): the browser first-frame test
      was NOT delivered at first pass despite being marked verified
      above — the implementer traced that `browser/src/main.ts:311-316`
      already calls `message-tick` before `build-ui-frame`
      unconditionally on every frame including the first (`main.ts:222`,
      before any user activation), and treated that trace as a
      substitute for the test. The trace is correct but proves only that
      today's `main.ts` is right, not that a later edit reordering those
      two calls (or gating `message-tick` behind activation) can't
      silently reintroduce blank first-load controls with nothing else
      failing — it guards nothing. Closed here: added
      `browser/tests/ui-state-before-audio.spec.ts` with two Playwright
      tests, both driving the REAL `installSynthBrowserApp()`/
      `SynthBrowserApp` bootstrap (not a hand-rolled replay of the two
      request types) with no audio activation and no dispatched action —
      (1) real compiled miniapp WASM behind a call-recording proxy over
      `createDirectRuntimeClient()`: asserts `message-tick` precedes
      `build-ui-frame` on the first frame AND that the captured frame's
      encoder draw commands carry non-empty label/value text; (2) a
      fake-runtime complement needing no WASM rebuild (mirrors
      `runtime-core.spec.ts`'s bootstrap pattern): asserts the same
      ordering alone, for fast defense in depth. Red/green-proven per
      omni §9.1 (positive control): temporarily reordering `main.ts`'s
      `renderFrame()` to call `build-ui-frame` before `message-tick`
      failed both tests, citing the observed order
      (`[...,"build-ui-frame","message-tick"]`) in the assertion
      message; reverting via `git checkout` restored both to green with
      zero diff on `main.ts`. Full browser Playwright suite run after:
      217/221 passed, 2 skipped, 4 failed — all 4 failures are
      pre-existing and unrelated (`first-party-apps-smoke.spec.ts` x2,
      `static-site.spec.ts` x2, all failing on the catalog listing
      `sheaf/one-second-delay` as a third app the tests' hardcoded
      two-app expectation predates; `first-party-apps.json` and those
      spec files are untouched by this change).
      Collateral fallout fixed separately (not a new Testing-section
      item, but required to reach a green suite): the claim machine
      makes `MessageThreadTick` populate `uiState_` on every tick before
      audio ever latches, so any pre-existing test whose total block
      count never reached the audio throttle and which asserted "no
      resend"/read `uiState_` fields directly could be relying on the
      old permanently-stale coincidence instead of real behavior.
      POSTFLIGHT CORRECTION (2026-08-19): the sibling-audit below was
      originally reported as covering "the full rig/midi/engine test
      surface" without naming its method or its full file list. Redone
      here as an operand-based sweep (omni §8: grep the shared operand
      the concept cannot avoid, `SynthRig<`, across
      `projects/synth/tests/`, recursively — not a hand-picked file
      list, and not the assertion's own syntactic shape). FOUND: 4 files
      instantiate `SynthRig<` anywhere in `projects/synth/`:
      `rig_tests.cpp` (27 instantiations), `miniapp_system_tests.cpp`
      (40), `braid4_system_tests.cpp` (17), `audio_input_tests.cpp` (2).
      CHANGED: 1 file, 1 test case — `rig_tests.cpp:1049`
      `rig_reset_midi_output_processors_is_scoped_to_one_controller`,
      the one case provably hitting it (assertion corrected in place,
      comment cites this change). The other three were traced — not
      merely asserted safe — and confirmed SAFE: `braid4_system_tests.cpp`
      reads `uiState_`-shaped content exclusively through a private,
      per-test `manager.CreateUIState()` snapshot populated by a direct,
      unthrottled `PopulateUIState()` call, never through Engine's own
      `Context().uiState`/`gridUIState_` members this change's claim
      machine guards, so its timing is untouched by this change.
      `audio_input_tests.cpp` never reads `uiState_`/`gridUIState_` in
      any form (its two `SynthRig<>` instantiations are unrelated to UI
      state). `miniapp_system_tests.cpp` DOES read Engine's own
      `Context().uiState` (via `rig.UIState()`) after `RunBlocks()` calls,
      at lines 478, 485, 1206, and 1639, but every such read is either a
      pointer-identity check (timing-independent) or a positive,
      level-triggered assertion of CURRENT post-action content — never
      an absence/"stayed empty"/"no resend" check, the one pattern this
      fix can break (illustrated by the one real hit above: reading a
      value that is a function of current app state, not of populate
      history, gives the same answer whether that state was first
      published on tick 1 or tick 50, as long as at least one publish
      happened before the read either way). Confirmed both by trace and
      empirically: `miniapp_system_tests` binary, run from the repo
      root so its internal source-relative path resolves, 39/39 PASS.
      Full synth suite (29 binaries, 1260+ cases) green except
      the two documented 96kHz deadline tests (confirmed deterministic
      via isolated reruns).
- [x] 1.4 Superproject verification: rebuild frogg3rs browser package,
      confirm encoders labeled pre-Play on the local site; run
      `cd app && nice make -j2 test` (279/279) and app/vst ctest — all
      green with the modified Engine.hpp.
      Verified complete: browser package rebuilt/checked by the
      operator directly (16/16 encoders pre-Play, CPU 0.0%); `make -j2
      test` = 279/279 PASS, 0 FAIL; `ctest` in app/vst/build = 3/3 PASS
      (FroggersVstSmokeTest, FroggersVstHostTests, FroggersVstEditorTest).

## 2. Postflight audit (separate dispatch), then operator-gated PR

- [ ] 2.1 §14 postflight vs these artifacts; §8 re-run against the diff
      (the design's own sibling-enumeration section lists ALL known
      siblings — Engine::sampleCounter_, the BrowserRuntime lifecycle
      flags, the frogg3rs plugin heartbeat; check the diff against that
      list; note parallels, do NOT merge across repos or layers).
- [ ] 2.2 Apply postflight findings (fix loop until clean).
- [ ] 2.3 OPERATOR GATE: separate commit + PR on the Sheaf repo for THIS
      change only.
