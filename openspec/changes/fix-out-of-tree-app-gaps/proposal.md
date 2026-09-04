# Fix out-of-tree app gaps (GitHub issues #1–#6, #8)

## Why

Seven open GitHub issues, all filed from building an out-of-tree app
(`daguilarc/frogg3rs`) against the `EXTRA_APP_*` hook, describe gaps where the
library either withholds a signal apps need, hard-codes a choice apps cannot
decline, or forces apps to copy code that then drifts. Every item is general to
Sheaf — none encodes app-specific behavior:

- **#1** — shell composition pins the sidebar to `App::Config().uiWidth`
  (`RuntimeMainComponent.hpp:122`), so an embedded app surface can never
  resolve against the live window extent; resize leaves dead space.
- **#2** — the sidebar deadline meter renders an unlabeled percentage
  (`RuntimePages.hpp:294-299`, emitted at `:669`); and out-of-tree apps must
  copy `Main.cpp` to skip the picker, a copy that has already drifted — the
  window-size bug (sized from `config.uiWidth/uiHeight` at `Main.cpp:84-99`,
  missing `Layout::kSidebarWidth`) shipped in both the copy and the original.
- **#3** — `juce_build.mk` copies `APP_INFO_PLIST` verbatim (`:158-160`) while
  deriving the binary path from `APP_NAME` (`:25-27`); a mismatch builds green
  and breaks only Finder launch. And `ControlStyle::caption` can only lead its
  control, never follow it.
- **#4** — an app cannot distinguish "the device presented an input channel"
  from "the user routed an input"; the default-opened laptop mic makes every
  existence-shaped test permanently true. The needed signal exists
  runtime-side (`AudioDeviceSnapshot().inputDeviceName`) with no route to apps.
- **#5** — `GangedRandomLfoVisualizer` draws an unconditional opaque
  background; embedding apps cannot opt out.
- **#6** — `check_ui_boundary.sh` aborts on macOS system bash 3.2 when a
  dynamically-built array is empty, pre-empting the discovery-floor diagnostic
  that exists precisely for the empty case.
- **#8** — apps can contribute nothing to the runtime sidebar: the audio page
  is built from a closed snapshot (`AudioPageSnapshot`, `RuntimePages.hpp:168`)
  and the page set is a closed enum (`MainPane.hpp:20`).

## What Changes

One requirement per fix, defaults preserved everywhere — no existing app or
backend changes behavior unless it opts in:

- **synth-portable-runtime-shell** (`sprs-13`…`sprs-17`):
  - `sprs-13` (#1): the shell hands the app surface the live content extent
    before `BuildTree()` (generalizing the existing `SetContentBounds`
    convention) and places the sidebar at the resolved app tree's width, not
    `App::Config().uiWidth`.
  - `sprs-14` (#2): the window/launch plumbing (`MainWindow`,
    `LaunchRegisteredApp<App>`) is hoisted into a reusable runtime header;
    `Main.cpp` is rewritten on top of it; `initialise()` honors a command-line
    appId to skip the picker.
  - `sprs-15` (#2): windows are sized from the shell component's
    `IntrinsicBounds()`, never raw `config.uiWidth/uiHeight`.
  - `sprs-16` (#8): `AudioPageSnapshot` carries an optional app-supplied
    section builder appended beneath the device rows.
  - `sprs-17` (#8): an app may register one additional sidebar page (id,
    title, tree builder) alongside Audio/Controllers/Sync/File.
- **synth-runtime-ui** (`sru-59`…`sru-61`):
  - `sru-59` (#2): the deadline meter is labeled (`CPU ` prefix).
  - `sru-60` (#3): `ControlStyle` gains caption placement Before (default) /
    After.
  - `sru-61` (#6): the UI-boundary check completes under macOS bash 3.2.57
    with zero-length discovery arrays, and the discovery floor evaluates
    before any expansion of the discovered set.
- **synth-app-runtime** (`sar-33`, `sar-34`):
  - `sar-33` (#4): an explicit external-input-routed signal reachable from
    `AppContext`, with a change notification; a default-opened device the user
    never chose reports not-routed. Browser semantic: user-gesture-granted
    capture active.
  - `sar-34` (#3): the bundle rule fails the build when the plist's
    `CFBundleExecutable` does not equal `APP_NAME`.
- **synth-portable-visualizers** (`spv-9`):
  - `spv-9` (#5): `GangedRandomLfoVisualizer` accepts a construction-time
    background opt-out; the default draws today's background.

## Impact

- Affected specs: `synth-portable-runtime-shell`, `synth-runtime-ui`,
  `synth-app-runtime`, `synth-portable-visualizers`.
- Affected code (paths relative to `projects/synth/`, per repo convention):
  `include/synth/RuntimeMainComponent.hpp`,
  `include/synth/RuntimePages.hpp`, `include/synth/PortableUIBuilders.hpp`,
  `include/synth/GangedRandomLfoVisualizer.hpp`, `runtime/MainPane.hpp`,
  `runtime/Shell.hpp`, `runtime/Runtime.hpp`, `runtime/juce_build.mk`,
  `apps/sheaf-patch/Main.cpp`, `scripts/check_ui_boundary.sh`, plus the
  browser host for the `sar-33` routed semantic and both backends' tests.
- Backward compatibility: every new lever defaults to current behavior —
  legacy apps compose, size, and render bit-identically (`sprs-13/15/16/17`,
  `sru-60`, `spv-9` each carry an explicit unchanged-default scenario).
- Interaction: the `add-browser-wasm-runtime` change (archived 2026-08-02,
  already merged at this branch's base) reworked the same browser host;
  `sar-33`'s browser-side delta was kept minimal (one derived flag from the
  existing capture lifecycle) regardless. (Corrected 2026-08-18: the
  original text called that change "active" — stale at write time.)
- Base: upstream `main` = `508d9d68`. The consuming app verified this base
  before any fix lands (273/273 app tests green at the bumped pin).
- Delivery: an initial commit per task group on `fix-out-of-tree-app-gaps`
  (sprs-14 and sprs-15 share one commit — the sizing fix lands inside the
  hoist rewrite), plus review-driven fix commits landed against 5 of the 10
  groups from task-scoped review; single PR closing #1–#6 and #8, its body
  mapping every commit to the issue(s) it addresses.
