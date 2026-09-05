# Tasks — fix-out-of-tree-app-gaps

Order is risk-ascending; every task ends with the synth gate green
(`make -C projects/synth test`, which includes `check-ui-boundary`). One
commit per numbered task group. Subagent dispatch per the consuming repo's
omni-rule: implementation/review on the lightest capable model.

## 0. Base verification (complete)

- [x] 0.1 Bump consumer pin `77a3019e` → `508d9d68` (upstream `main`),
      rebuild app, run its full suite. Evidence: 273/273 tests green,
      frogg3rs commit `211f1df`. Branch `fix-out-of-tree-app-gaps` created at
      `508d9d68`.

## 1. sru-61 — boundary check on bash 3.2 (#6)

- [x] 1.1 Guard every dynamic-array expansion in `check_ui_boundary.sh` with
      `${arr[@]+"${arr[@]}"}`; re-verify the issue's site list against the
      file; move the `APP_PRODUCER_DISCOVERY_FLOOR` check above the line-179
      concatenation.
- [x] 1.2 Sweep `git ls-files '*.sh'` for `+=(` arrays expanded under
      `set -u`; fix synth-owned hits; record found vs changed counts.
- [x] 1.3 Verify under literal `/bin/bash` 3.2.57: self-test path, plus an
      empty-discovery simulation that must reach the floor diagnostic
      (positive control: show the simulation actually empties the array).

## 2. sru-59 — label the deadline meter (#2 item 1)

- [x] 2.1 `CPU ` prefix in `FormatDeadlineText` (`RuntimePages.hpp:294`);
      update any test pinning the rendered string.

## 3. sar-34 — plist/executable guard (#3 item 1)

- [x] 3.1 Guard in the `$(APP_BUNDLE)` rule (`juce_build.mk:158-160`):
      extract `CFBundleExecutable`, fail naming both values on mismatch.
- [x] 3.2 Negative test: mismatched plist must fail the bundle rule;
      matching plist must build. (Scripted make invocation, both paths.)

## 4. sru-60 — caption placement (#3 item 2)

- [x] 4.1 `ControlStyle::captionPlacement` { Before (default), After };
      `FinishControl` emits trailing caption when After.
- [x] 4.2 `portable_ui_tests`: node-order assertions for both placements;
      default-unchanged assertion.

## 5. spv-9 — visualizer background opt-out (#5)

- [x] 5.1 `GangedRandomLfoVisualizer<N>(uiState, bool drawBackground = true)`;
      thread to `AppendBackgroundAndAxis` call site.
- [x] 5.2 Command-stream test: opt-out emits no background fill/midline,
      traces identical; default emits today's exact commands.

## 6. sprs-14 + sprs-15 — reusable launch, intrinsic sizing (#2 item 2)

- [x] 6.1 Hoist `MainWindow` + `LaunchRegisteredApp<App>` into a reusable
      runtime header (name per `runtime/` conventions).
- [x] 6.2 Rewrite `apps/sheaf-patch/Main.cpp` on the hoisted header; window
      size derives from `IntrinsicBounds()`; delete the raw
      `config.uiWidth/uiHeight` sizing at `Main.cpp:84-99`.
- [x] 6.3 `initialise()` honors a command-line appId (fallback: picker).
- [x] 6.4 Tests: intrinsic-size assertion (config-sized window +
      `kSidebarWidth`); direct-launch selection logic unit test.

## 7. sar-33 — external-input-routed signal (#4)

- [x] 7.1 Host-side derivation (JUCE): user-selected input device is
      non-empty and open ⇒ routed; default-auto-opened device ⇒ not routed.
      Atomic publication; `AppContext` getter.
- [x] 7.2 Change notification: app-registered callback, message-thread
      invocation on value change (idiom matched to existing `AppContext`
      conventions).
- [x] 7.3 Browser host: routed = user-gesture-granted capture active, derived
      from the existing capture lifecycle; minimal surface beside the active
      `add-browser-wasm-runtime` change.
- [x] 7.4 Parity tests both backends: default-open ⇒ false; select ⇒ true +
      callback fires; deselect ⇒ false + callback fires.

## 8. sprs-13 — live extent composition (#1)

- [x] 8.1 Optional extent hook on the app-surface seam, generalizing
      `SetContentBounds`; shell offers live content extent before
      `BuildTree()`.
- [x] 8.2 Composition: sidebar x = resolved app tree root width
      (`RuntimeMainComponent.hpp:122` today); `ValidateApplicationTree`
      (`:369`, root-bounds equality at `:472-477`) validates against the
      extent the surface resolved — offered extent when the hook was
      accepted, `config.uiWidth/uiHeight` otherwise.
- [x] 8.3 Tests: legacy app ⇒ bit-identical composition; extent-aware test
      surface ⇒ sidebar tracks resized extents; browser parity where the
      browser shell composes.

## 9. sprs-16 — app-supplied audio-page section (#8)

- [x] 9.1 Optional section builder on `AudioPageSnapshot`
      (`RuntimePages.hpp:168`), typed `std::function<ui::Subtree(ui::Bounds)>`
      (amended in postflight — see design §sprs-16); `BuildAudioPageTree`
      (`:776`) splices beneath device rows via `Splice(Subtree)`.
- [x] 9.2 Tests: default ⇒ byte-identical page tree; supplied builder ⇒
      section present within remaining area.

## 10. sprs-17 — app-registered sidebar page (#8)

- [x] 10.1 Registration surface (id, title, tree builder) on the app
      registration path; nav button after File. Thread every page-set
      definition site (design §sprs-17): `MainPane::Page` + both mapping
      switches (`MainPane.hpp:20`, `:128-139`, `:145+`); `RuntimeMainPage`,
      sidebar action handlers, `BuildRuntimePageTree`
      (`RuntimeMainComponent.hpp:21`, `:61-101`, `:485-491`); NodeIds and
      Actions constants plus `BuildSidebarTree` emission
      (`RuntimePages.hpp:33-37`, `:116-119`, `:642-671`).
- [x] 10.2 Tests: no registration ⇒ four pages, nav unchanged; registered ⇒
      button renders, selection shows app tree, other pages unaffected.

## 11. Consumer verification and PR (user-gated)

- [x] 11.1 Push branch; rebuild `daguilarc/frogg3rs` against branch tip; full
      app suite green.
- [x] 11.2 Operator acceptance test of the running app (owner decides which
      opt-in toggles to exercise: `spv-9` one-liner, `sar-33` re-enable).
- [x] 11.3 On approval: PR `daguilarc/Sheaf:fix-out-of-tree-app-gaps` →
      `jvictor0/Sheaf:main`, an initial commit per requirement plus the
      review-driven fix commits task-scoped review produced along the way,
      body mapping every commit → the issue(s) it addresses, closing #1–#6
      and #8.
