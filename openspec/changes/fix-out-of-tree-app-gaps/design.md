# Design — fix-out-of-tree-app-gaps

All file:line anchors verified at `508d9d68` (branch base) unless marked
otherwise. Anchors carried from the issue texts were re-checked against the
`77a3019e..508d9d68` diff: `PortableUIBuilders.hpp`, `PortableUI.hpp`,
`GangedRandomLfoVisualizer.hpp`, `MainPane.hpp`, `Shell.hpp`,
`PortableJuceBackend.hpp`, and `check_ui_boundary.sh` did not change in that
range, so their issue-text anchors remain literal; `RuntimePages.hpp`,
`RuntimeMainComponent.hpp`, `Runtime.hpp`, `Main.cpp`, and `juce_build.mk`
moved and were re-anchored fresh.

All repository paths in this change's artifacts are relative to
`projects/synth/` unless explicitly prefixed (repo convention: archived
changes cite the full `projects/synth/...` path; the prefix is elided here
once, globally, instead).

## sru-61 (#6) — boundary check on bash 3.2

- Apply the `${arr[@]+"${arr[@]}"}` guard at every expansion of a
  dynamically-populated array under `set -u`; the issue's site table (lines
  179, 195, 213, 268, 436, 441, 499, 506, 512, 525, 533, 540) is the work
  list, re-verified against the file at implementation time.
- Move the `APP_PRODUCER_DISCOVERY_FLOOR` check above the line-179
  concatenation so empty discovery produces the intended `fail` diagnostic
  rather than a shell abort. The sibling floor check keeps its position
  relative to its own first expansion.
- Sweep `git ls-files '*.sh'` for other dynamically-populated arrays
  (`grep -n '+=('`) expanded under `set -u`; fix any found in synth-owned
  scripts, report others in the PR body. Count found vs changed.
- Verification: run the script's self-test and an empty-discovery simulation
  under literal `/bin/bash` (3.2.57) as well as the default `bash`.

## sru-59 (#2 item 1) — labeled deadline meter

- `FormatDeadlineText` (`RuntimePages.hpp:294`) prefixes `CPU ` in the
  rendered string; the `StatusText` emission site (`:669`) is unchanged.
- Prefix over caption row: the meter lives in dense sidebar chrome where a
  second node changes layout; four characters answer the actual confusion.

## sar-34 (#3 item 1) — plist/executable coherence guard

- Guard at the copy site: the `$(APP_BUNDLE)` rule (`juce_build.mk:158-160`)
  extracts `CFBundleExecutable` from `$(APP_INFO_PLIST)` (via `plutil` or
  `PlistBuddy`, whichever the repo already uses; else `plutil -extract`) and
  fails with a message naming both values when it differs from `$(APP_NAME)`.
- Guard over templating: rewriting a user-supplied plist silently is the same
  class of surprise the issue reports; a loud build error is the contract.

## sru-60 (#3 item 2) — caption placement

- `ControlStyle` gains `captionPlacement` enum { `Before` (default),
  `After` }. `Builder::FinishControl` (`PortableUIBuilders.hpp:428-465`,
  unchanged since 77a3019e) emits the caption `Label` after the control node
  inside the same implicit Row when `After`.
- Caption id derivation and sync semantics are placement-independent.

## spv-9 (#5) — visualizer background opt-out

- `GangedRandomLfoVisualizer<N>` constructor gains
  `bool drawBackground = true`, threaded to the
  `ganged_random_lfo_detail::AppendBackgroundAndAxis()` call inside
  `BuildGangedRandomLfoCommands()`. When false, neither the full-cell fill
  nor the midline is emitted; voice traces are unchanged.
- Constructor parameter over a `WantsBackground()` virtual: the background is
  private to this visualizer's command builder — it is not part of the frame
  mechanism `WantsEncoderFrame()` participates in, and no other visualizer
  draws one.

## sprs-14/15 (#2 item 2) — reusable launch, intrinsic window sizing

- Hoist `MainWindow` and `LaunchRegisteredApp<App>` from
  `apps/sheaf-patch/Main.cpp` (private members of `SheafPatchApplication`)
  into a reusable runtime header (naming matched to `runtime/` conventions at
  implementation time). Out-of-tree `main` becomes ~a dozen lines on the same
  code path Sheaf uses.
- Rewrite `Main.cpp` on the hoisted header — picker path included — so
  exactly one implementation of "show the app" exists.
- Sizing bug fixed in the one shared implementation: the window derives its
  size from the shell component (`IntrinsicBounds()` accounts for
  `Layout::kSidebarWidth`, `RuntimeMainComponent.hpp:216`,
  `RuntimePages.hpp:269`), replacing the raw `config.uiWidth/uiHeight` sizing
  at `Main.cpp:84-99`.
- `initialise()` stops discarding its command-line argument: a recognized
  appId launches that app directly; unrecognized/absent falls back to the
  picker.

## sar-33 (#4) — external-input-routed signal

- **Semantic, identical meaning both backends:** *a user-chosen input source
  is open and delivering.* JUCE: the user-selected input device (the
  persisted/Audio-page choice) is non-empty and is the open device. The
  default device auto-opened by `initialiseWithDefaultDevices` before the
  persisted choice applies (`Runtime.hpp`, re-anchored at implementation)
  reports **false**. Browser: user-gesture-granted input capture is active,
  derived from the existing capture lifecycle (`1c7aa725`).
- **Plumbing:** the host derives the flag where it already handles device
  state and publishes it through an atomic; apps read a getter on
  `AppContext`. Audio-thread reads are wait-free.
- **Notification:** apps register a change callback (idiom matched to
  existing `AppContext` conventions at implementation time); invoked on the
  message thread when the derived value changes, so modulation-source
  `connected` metadata can flip live — pick an input, sources light up, no
  restart.
- **Out of scope:** changing the default-open-first device ordering. The
  issue reports it as context; the flag makes it harmless to apps that honor
  the signal.

## sprs-13 (#1) — live extent for embedded app surfaces

- **Hook:** before calling `app_.PortableSurface().BuildTree()`
  (`RuntimeMainComponent.hpp` `BuildTree()`), the shell offers the surface
  the live content extent, generalizing the `SetContentBounds` convention
  Sheaf's own page surfaces already declare (`RuntimePages.hpp:1410` region;
  `ControllersPageUI.hpp`). Base `ui::Surface` stays extent-free; the hook is
  optional — surfaces that don't implement it resolve at compiled-in size.
- **Composition:** the sidebar x becomes the resolved app tree's root width
  (`sidebarTree.nodes.front().bounds.x`, currently pinned at `:122` to
  `App::Config().uiWidth`). For legacy apps resolved width equals
  `config.uiWidth`, so composition is bit-identical; for extent-aware apps
  the sidebar tracks.
- **Extent source:** the renderer's live bounds, already propagated by
  `MainPane::resized()` → `RefreshFromSurface()` and the `uiFrameHz` timer —
  every refresh layer exists; the composition arithmetic and the root-bounds
  validation (next bullet) are what change.
- **Validation (found in preflight audit, 2026-08-17):**
  `ValidateApplicationTree` (`RuntimeMainComponent.hpp:369`, invoked from the
  compose path at `:113`) currently throws unless the app root's bounds equal
  `config.uiWidth/uiHeight` exactly (`:472-477`). It must validate against
  the extent the surface actually resolved — the offered live extent when the
  hook was accepted, `config.uiWidth/uiHeight` otherwise — so the legacy path
  keeps today's exact check and an extent-aware tree is not rejected.
- Browser-backend composition parity is asserted by test; where the browser
  shell composes independently, the same resolved-width rule applies.

## sprs-16/17 (#8) — app-supplied audio section, app sidebar page

- `sprs-16`: `AudioPageSnapshot` (`RuntimePages.hpp:168`) gains an optional
  `std::function<ui::Subtree(ui::Bounds)>` section builder;
  `BuildAudioPageTree(snapshot, area)` (`:776`) splices its subtree beneath
  the device rows within the remaining area. Default empty → byte-identical
  page. (Amended 2026-08-18 in postflight: the original `ui::NodeTree`
  signature routed through `Splice(NodeTree)`, which drops the builder's
  `LayoutOptions` map — nested containers in an app section would re-resolve
  with default layout. `ui::Subtree` + `Splice(Subtree)` is the repo's own
  established graft idiom — `BuildPatchBrowserSubtree`/`Splice(Subtree)`,
  `RuntimePages.hpp:954/:1082` — and preserves the app's declared layout.)
- `sprs-17`: one optional app-registered page (id, title, tree builder)
  alongside the closed page set. **The page set has multiple definition
  sites, all of which the registered page must thread (enumerated in
  preflight audit, 2026-08-17):** `MainPane::Page` (`MainPane.hpp:20`) and
  its two mapping switches to/from `RuntimeMainPage` (`MainPane.hpp:128-139`,
  `:145+`); the `RuntimeMainPage` enum itself
  (`RuntimeMainComponent.hpp:21`), its sidebar action handlers (`:61-101`),
  and the `BuildRuntimePageTree` switch (`:485-491`); the sidebar node
  id/action constants (`RuntimePages.hpp:33-37` NodeIds and `:116-119`
  Actions — parallel namespaces, both get the new entry) and the
  `BuildSidebarTree` button emission (`:642-671`, new button after File).
  Registration rides the app registration surface (exact idiom — `Config()`
  field vs surface interface — matched to existing conventions at
  implementation time). No registration → exactly today's four pages, no nav
  change.
- Both mechanisms are proven by Sheaf-side tests only (the consuming app
  adopts neither yet, by its owner's decision).

## Testing

- Each requirement lands with tests in the suite owning its layer:
  `portable_ui_tests` (sru-60, sprs-16/17 trees), backend parity suites
  (sprs-13 composition, sar-33 both-backend semantics, spv-9 command
  streams), make-level negative test (sar-34), `/bin/bash` 3.2.57 matrix +
  self-test (sru-61), launcher/window test or scripted verification
  (sprs-14/15).
- Gate per commit: `make -C projects/synth test` green (includes
  `check-ui-boundary`); browser-side suites where touched.
- Consumer verification after all fixes: rebuild `daguilarc/frogg3rs` against
  the branch tip and run its 273-test suite; operator acceptance test drives
  the PR decision.

## Risks

- `sprs-13` touches composition every app renders through — mitigated by the
  resolved-width-equals-config identity for legacy apps and parity tests.
- `sar-33`'s browser half lands beside the active `add-browser-wasm-runtime`
  change — kept to one derived flag over the existing capture lifecycle.
- `sprs-14` moves `main`-adjacent code; the rewrite must keep the picker
  behavior identical except for window size (which is the point).
