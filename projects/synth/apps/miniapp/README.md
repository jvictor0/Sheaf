# Synth Miniapp

This directory contains the real SynthMiniapp application: a small probe app
for the synth parameter/modulation external UI/message layer, built on top of
the shared application runtime (`projects/synth/runtime/`) instead of its own
bespoke JUCE shell.

## Layout

- `MiniAppCore.hpp` — JUCE-free application core (`synth_miniapp::MiniAppCore`).
  Builds the parameter/bank/modulation/scope topology and runs the per-sample
  DSP. Satisfies `synth::SynthApplicationCore`, so it also runs headless
  under `synth_rig::SynthRig` (see `tests/miniapp_system_tests.cpp`).
- `DemoModulation.hpp` — small pure-math helpers (LFO phase step, unipolar
  sine modulator, etc.) used by `MiniAppCore`. Covered by test cases in the
  JUCE-free `module_tests` binary (`projects/synth/tests/module_tests.cpp`,
  built with `-Iapps/miniapp`).
- `MiniAppUI.hpp` — JUCE-free portable UI surface, stable node IDs, layout
  helpers, and action routing from portable actions to `MessageIn` values.
- `MiniApp.hpp` — application wrapper (`synth_miniapp::MiniApp`, extends
  `MiniAppCore`) that exposes `PortableSurface()`. Satisfies
  `synth::SynthApplication` without owning JUCE widgets.
- `Main.cpp` — `SYNTH_RUNTIME_MAIN(synth_miniapp::MiniApp)`.

## Runtime-owned chrome

This app does **not** provide patch load/save buttons or MIDI device
selection UI. Those are owned by the runtime shell:

- Patch commands (New/Save/Save As/Load/Revert), the current-patch-name
  label, and the patch status label live on the File page, built in
  `include/synth/RuntimePages.hpp` and hosted by the shell's
  `synth_runtime::MainPane`
  (`projects/synth/runtime/MainPane.hpp`).
- Per-controller MIDI device combo boxes, status dots, and mapping editors are
  produced by the shared JUCE-free `ControllersPageSurface` and rendered by
  the generic desktop `synth_juce::PortableComponent`, over
  `synth::MidiConfigViewModel`
  (`projects/synth/include/synth/MidiConfigViewModel.hpp`).

The File page's patch-name label shows the current patch directory's name
(or "(no patch)" when none is current), read fresh from
`Runtime::GetEngine().Patches().CurrentPatchDirectory()` on every repaint
tick — never cached. The Save button checks the same state before
dispatching: with no current patch it opens the in-app Save As browser
directly instead of sending a `SavePatch()` that would only come back
`NeedsSaveAsPath`.

`MiniApp::PortableSurface()` returns this app's portable UI tree; `MainPane`
hosts it through the JUCE backend adapter alongside the Audio/Controllers/File
pages and the sidebar (see `projects/synth/runtime/MainPane.hpp`), showing
exactly one of the app or a library page at a time.

## Runtime data

The shared runtime owns long-lived app data. In production it resolves an
OS-appropriate user application data root under `Sheaf/SynthMiniapp`; tests can
override this with `Runtime::SetRuntimeDataPathsOverride(...)` and scratch
`synth::RuntimeDataPaths`.

The data root contains:

- `patches/` — patch directories and their version files. The File page's
  Save As and Load flows use an in-app browser rooted here, not the operating
  system file explorer.
- `logs/` — async session logs written by `synth::AsyncLogQueue`.
- `config.json` — runtime configuration, currently MIDI controller/device
  configuration plus audio input/output selection. Audio and Controllers page
  Back buttons save this file before returning to the app view; File page Back
  does not save it just because the File page was dismissed.

Patch files contain synthesizer patch data only. MIDI and audio configuration
are stored separately in `config.json`.

## Portable UI boundary

The app-facing UI files in this directory are intentionally JUCE-free. Desktop
rendering is supplied by `projects/synth/juce`, and the boundary is checked by:

```text
make -C projects/synth check-ui-boundary
```

That same split is the intended browser/Wasm path: keep `MiniAppUI.hpp` and
the runtime page producers as portable tree/draw-command producers, then add a
browser backend that maps those nodes to canvas/DOM and supplies browser-native
audio, MIDI, file, and storage adapters behind the runtime boundary.

## Logging

Patch command results are logged by the runtime itself
(`synth_runtime::Runtime::LogPatchCommand`, INFO-level, via
`synth::AsyncLogQueue`), not by this app. There is no app-side `std::ofstream`
patch log — the old miniapp's `appendPatchLog`/`patchLogPath`/
`logPatchCommand` functions were not ported.

Log files are written under `Runtime::DataPaths().logsRoot`, normally the
`logs/` directory described above.

## Patch root

Patches are written under `Runtime::DataPaths().patchesRoot`, normally the
`patches/` directory described above. Save As and Load use the runtime's
in-app patch browser rooted there.

## Build

Requires a local JUCE checkout. The Makefile expects it at `~/JUCE` by
default; override with `JUCE_DIR=/path/to/JUCE` if yours lives elsewhere.

From `projects/synth`:

```text
make -C apps/miniapp
```

or, equivalently, via the root Makefile's `apps` target:

```text
make -C projects/synth apps
```

The `miniapp` target at the repo's `projects/synth/Makefile` level delegates
here too:

```text
make -C projects/synth miniapp
```

## Run

```text
open projects/synth/apps/miniapp/build/SynthMiniapp.app
```

## What this app does

It creates one duophonic parameter group with 17 top-level parameters,
15 modulation sources, three scenes, one gesture, two pages (VCO and LFO),
two banks, and one 16-encoder bank slot. The VCO page retains Tune, Phase,
Shape, Volume, and the filter's Cutoff, Resonance, and Blend controls. The
expanded LFO page and bank expose, in order, LFO Frequency, Shape, Phase
Offset, Skew, Exponent, ADSR Attack, Decay, Sustain, Release, and Tempo.

The app queries the runtime's immutable committed clock plan at each absolute
output frame. While transport is running, the shared ADSR gate is high for
quarter-note phase `[0, 0.5)` and low for `[0.5, 1.0)`, so each integer quarter
retriggers an eighth-note-long envelope. Current per-voice ADSR outputs are
copied into an app-owned stable mirror registered as modulation source 7
before the same-frame modulation update. Tempo maps linearly from 30 to
300 BPM (120 BPM by default); voice 0 is the global authority, and MiniApp
requests a master-clock tempo change only when that effective value changes.

Portable UI actions send `MessageIn` values through `context->uiBus`; the JUCE
backend under `projects/synth/juce` turns portable nodes and draw commands
into desktop widgets and graphics calls. Selecting the VCO/LFO bank buttons
pushes a `SelectParamBank` message onto `context->uiBus`;
`MiniAppCore::ProcessBlock` keeps the active parameter page in sync with
whichever bank ends up selected, once per audio block.
