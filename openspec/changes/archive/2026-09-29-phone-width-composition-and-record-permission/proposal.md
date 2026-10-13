# Proposal — `phone-width-composition-and-record-permission`

Paired with the frogg3rs change `frogg3rs-android-app`
(`openspec/changes/frogg3rs-android-app/` in the repository that pins this
submodule). This change holds only its proposal and spec deltas. Every task
for it is in that change's `tasks.md`, prefixed S.

## Why

frogg3rs ships a native Android app built on this runtime shell, and its
phone layout must be the same arrangement its browser build already shows.
Four things in `projects/synth` stand in the way.

- **The composition pins an app to its configured size.**
  `RuntimeMainComponent::BuildTree` (`include/synth/RuntimeMainComponent.hpp`)
  validates the app root against `App::Config()`'s width and height unless the
  app is an `ExtentAwareSurface`, and then against the offered extent, exactly.
  A phone arrangement that stacks two blocks is taller than any configured or
  offered size, so it cannot be composed today. The sidebar is always placed
  to the right of the app (sprs-2), which on a 412-dp phone costs a quarter of
  the width.
- **The JUCE shell neither scales nor scrolls.** `ShellComponent`
  (`runtime/Shell.hpp`) gives `MainPane` its own bounds, and
  `PortableComponent` (`juce/PortableJuceBackend.hpp`) paints every container
  itself, so a composite wider or taller than the window is clipped. The
  browser's `fitSurface` (`browser/src/ui.ts`) already scales the root to
  `min(1, width / surfaceWidth)` and lets the page scroll.
- **Input opens without asking.** `Runtime::ApplyAudioDeviceInputSelection`
  and the persisted-input path in `Runtime::Start` (`runtime/Runtime.hpp`) call
  `setAudioDeviceSetup` directly. On Android, JUCE's Oboe device refuses an
  input without the record permission (`juce_Oboe_android.cpp`), which the
  user would see only as an opaque error. On every other platform
  `juce::RuntimePermissions::request` calls back at once with true.
- **A controller's Android name never matches its desktop alias.** On
  Android, JUCE names an unnamed MIDI port "<device name> Output Port N" or
  "<device name> Input Port N"; a preset whose alias is the bare device name
  (the MIDI Fighter Twister and every other `deviceDefaults` entry in
  frogg3rs's `FroggersMidiCatalog.hpp`) then never matches that name by the
  exact-string comparison `MatchesAnyAlias` uses, so the Controllers page
  offers no preset for a controller connected to an Android app, found while
  building frogg3rs's Android app (`frogg3rs-android-app` task 4.8).

## What changes

- NEW interface `ui::SelfSizedSurface` in `include/synth/PortableUI.hpp`,
  beside `ui::ExtentAwareSurface` and detected the same way. `RootBounds()`
  returns the bounds the surface's next `BuildTree()` root will have;
  `SidebarSlot()` optionally names a node of the app tree as the place for the
  runtime sidebar.
- `RuntimeMainComponent::BuildTree` validates the app root against
  `RootBounds()` when the surface implements the interface. With a slot, it
  places the sidebar root at the slot node's position in composite
  coordinates, makes the composite exactly the app root's size, and fails with
  a diagnostic naming the slot when the sidebar does not fit inside it. The
  declared root and the slot apply only while the application page is shown.
  While Audio, Controllers, Sync, File or a registered app page is shown,
  that page keeps the content bounds the constructor gives it (the
  configured width and height) and the sidebar sits at the page root's
  right edge, as sprs-2 states: today `BuildTree` places the sidebar at the
  app root's width, which for a narrow self-sized app would be narrower than
  the 900-wide page and make `RequireCompositionHolds` throw. A surface that
  does not implement the interface composes as today while the application
  page is shown. With a runtime page shown, placing the sidebar at the page's
  width instead of the app root's changes nothing for an app at its
  configured size (the two widths are equal). It changes the result for an
  `ExtentAwareSurface` whose live width differs from its configured width:
  today that page is composed inside the live width, which throws when the
  window is narrower than the page and leaves a gap when it is wider; now
  the composite is the page plus the sidebar. No app under `apps/` is
  extent-aware.
  `RuntimeMainComponent` exposes the bounds of the last composite it built.
- `ShellComponent` sizes `MainPane` to the larger of the shell and the
  composite in each dimension, scales it by `min(1, shell width / pane
  width)`, puts it inside a vertical viewport when the scaled height exceeds
  the shell, and re-reads the composite bounds on every refresh.
  `MainPane::resized` is the only caller of
  `RuntimeMainComponent::SetContentExtent` and offers the pane's own bounds,
  which can now be the last composite rather than the shell. So `MainPane`
  gains `OfferContentExtent`, which offers a given area less the sidebar,
  and `ShellComponent` calls it with its own bounds after every layout of the
  pane. The last extent offered is then always the shell's, and an
  `ExtentAwareSurface` (sprs-13, `fix-out-of-tree-app-gaps`) tracks the
  window as it grows and as it shrinks: the existing wiring check in
  `juce/RuntimeShellSessionTests.cpp` (shell 800 by 600, offered 704 by 600)
  stays true, and a shrink to 600 by 400 offers 504 by 400. A host with no
  shell (frogg3rs's plugin editor) keeps getting its extent from
  `MainPane::resized`. A drag that starts on a draggable node does not scroll; one that
  starts elsewhere does.
- `Runtime` requests the record permission before it opens an input device and,
  when refused, keeps output running and reports that microphone access was not
  granted in the Audio page status.
- `ControllerWizard.cpp`'s `MatchesAnyAlias` also accepts a name that, once a
  trailing " Output Port N" or " Input Port N" is stripped, ends with one of
  a descriptor's aliases, so a preset built around a bare device-name alias
  pairs on Android for any device presenting that construction, not the
  Twister only; a name carrying no such suffix still matches only by exact
  case-insensitive equality, as before. frogg3rs's Twister-only Android
  aliases become unnecessary and are removed from `FroggersMidiCatalog.hpp`.

The browser runtime needs no change: it composes through the same
`RuntimeMainComponent` and reads the composite size from every frame.

## Impact

`include/synth/PortableUI.hpp`, `include/synth/RuntimeMainComponent.hpp`,
`runtime/Shell.hpp`, `runtime/MainPane.hpp`, `runtime/Runtime.hpp`,
`tests/runtime_main_component_tests.cpp`,
`juce/RuntimeShellSessionTests.cpp`, `src/ControllerWizard.cpp`,
`tests/controller_wizard_tests.cpp`. No app under `apps/` changes behaviour:
none implements the new interface, and desktop windows open at the composite
size, where the scale is 1. One desktop behaviour changes: a resizable
desktop window shrunk narrower than its composite now scales its content to
the window width instead of clipping the right-hand side.

## Overlap

- `app-plugin-controllers` (pull request 23) carries `SYNTH_JUCE_HEADERS`'s
  `runtime/*.hpp` wildcard, because its new `EngineMidiConnections.hpp` is the
  header the hand-kept list missed.
- `attenuverter-blend-mode` is opened as the next sequential pull request
  before this one (frogg3rs operator ruling, 2026-09-28); this branch is
  rebased onto its tip before it is pushed.

- `fix-out-of-tree-app-gaps` (pull request 9, sprs-13) owns the live-extent
  path `MainPane::resized` to `RuntimeMainComponent::SetContentExtent`;
  sprs-20 adds the shell's own offer beside it (see What changes).
- `browser-slider-value-readout` edits `browser/src/ui.ts`; this change relies
  on `fitSurface` in that file staying `min(1, width / surfaceWidth)` with
  the mount height following it.
- `app-operator-runs` and `launchpad-model-on-the-row` each also plan the next
  sequential pull request; this branch is based on whatever the stack tip is
  when its first task runs.
- `shorten-deadline-readout-window`'s code is already in the pinned commit
  (80d9f4bb is an ancestor of the pin), so it no longer overlaps.
- `app-midi-catalog` also carries a MODIFIED `scw-2` requirement (its own
  registry-construction rewrite, `MakeControllerWizardRegistry(catalog)`),
  already implemented in the pinned commit but not yet archived, so the
  promoted `synth-controller-wizards` spec this change's own `scw-2` delta
  restates is `app-midi-catalog`'s pre-image, not its post-image. Whichever
  of the two changes archives second must restate `scw-2` again against the
  spec the first one leaves behind; this change does not attempt that
  reconciliation.

## Delivery

Branch `phone-width-composition-and-record-permission`, from the current stack
tip, pushed to the fork `daguilarc/Sheaf` and opened as the next sequential
pull request against `jvictor0/Sheaf` `main`. frogg3rs then pins that branch's
tip.
