# Synth

`projects/synth` contains the synth-side C++ utilities for the parameter and
modulation system.

The parameter/modulation library owns normalized parameter state, scene and
gesture interpolation, per-voice modulation values, per-voice modulation depths,
and physical-control routing helpers. It does not own oscillator/filter DSP,
MIDI or hardware drivers, UI rendering, patch serialization, or audio-device
I/O.

The audio-rate read path is designed to stay compact:

```text
Get(voice) =
  clamp(currentCenter * currentCenterScale[voice]
        + currentNormalizationOffset[voice]
        + dot(modulatorValues[voice], currentDepths[voice]))
```

Modulation follows the Smart Grid-style range-preserving weight rule. A
modulation depth is a signed normalized value. For range accounting, the weight
is `abs(depth)`; for audio-rate modulation, the signed depth remains in the dot
product.

```text
sum = sum(abs(rawDepth[m]))

centerScale[voice] = max(0, 1 - sum)

if sum > 1:
  depth[voice][m] = rawDepth[m] / sum
else:
  depth[voice][m] = rawDepth[m]

normalizationOffset[voice] = -sum(min(0, depth[voice][m]))

if sum > 1:
  minValue[voice] = rangeMin
  maxValue[voice] = 1
else:
  base = center * centerScale[voice] + normalizationOffset[voice]
  minValue[voice] = clamp(base + sum(min(0, depth[voice][m])))
  maxValue[voice] = clamp(base + sum(max(0, depth[voice][m])))
```

`ProcessLite` slews center scale, normalization offset, depths, and min/max
values. Parameter UI state publishes the slewed current min/max values for the
encoder range arcs.

The minimal initial API includes range clamping:

```text
ClampToRange(value, Unipolar) -> clamp(value, 0, 1)
ClampToRange(value, Bipolar)   -> clamp(value, -1, 1)
```

Scene and gesture compute happens at control rate. First compute the blended
scene center:

```text
blend = clamp(scene.blend, 0, 1)
base = sceneCenter[left] * (1 - blend) + sceneCenter[right] * blend
```

Each active gesture contributes an effective weight based on whether it is
active in the blended scenes:

```text
effectiveWeight[g] =
  (active[left][g]  ? gestureWeight[g] * (1 - blend) : 0)
  + (active[right][g] ? gestureWeight[g] * blend       : 0)

gestureValue[g] =
  value[left][g] * (1 - blend) + value[right][g] * blend

mix[g] = base * (1 - effectiveWeight[g])
         + gestureValue[g] * effectiveWeight[g]

rawCenter =
  sum(effectiveWeight[g] * mix[g]) / sum(effectiveWeight[g])
```

If no gestures are active, `rawCenter = base`.

Encoder deltas use the Smart Grid scene distribution rule without tracks. At an
endpoint, the active scene is clamped after adding the full delta. In a blended
scene, the proposed edits are:

```text
targetBlended = clamp(base + delta)
left' = left + delta * (1 - blend)
right' = right + delta * blend
```

If neither proposal exceeds the parameter range, both proposals are used. If one
side saturates, that side is clamped and the opposite side is solved so the
audible blended value reaches `targetBlended`.

Banks map physical encoders to parameters. Pressing a mapped parameter opens a
modulation-depth view; missing depth parameters are materialized as bipolar
parameters with default `0` when group capacity allows. The final visible bank
cell is the selected target parameter, so an undersized bank shows as many depth
cells as fit before the target. While the modulation view is open, the owning
bank treats pressing that target cell as close-view navigation; tick and
reset-press continue to route to the visible target parameter normally.

The randomized simulation test runs bounded default seeds during
`make synth-test`. Larger runs can be requested with:

```text
SYNTH_RANDOM_SEEDS=0x51A7,0xC0FFEE SYNTH_RANDOM_STEPS=5000 make synth-test
```

## External UI State And Messages

The external control layer mirrors the parameter tree with caller-owned UI
state populated once per frame. UI-state structs contain atomics and are sized
up front through `ParameterManager::CreateUIState()`; `PopulateUIState` only
stores into existing memory.

`Parameter::UIState` reports:

- connected state, bipolar flag, parameter base color, and stable short name pointer;
- per-voice value, min/max range, and parameter-owned indicator color;
- bounded modulation-source and gesture color arrays with published counts.

Disconnected slot positions are represented by `connected=false` with neutral
values and off colors; parameter UI state does not encode page/navigation roles.

`BankSlot::UIState` exposes visible cells in `AddPhysicalEncoder` order and a
`showingModulationView` flag. `ParameterManager::UIState` includes slot state,
manager-owned gesture values/selection, scene endpoints/blend, and reset,
random, and random-modifier held state. Parameter groups own processing/storage
topology and no appearance palette. Core and portable UI code use the same
canonical `synth::Color` RGBA type; hue constructors name degrees or turns;
JUCE color conversion is only for app/UI code (e.g.
`projects/synth/apps/miniapp/MiniApp.hpp`).

External actions are represented as timestamped `MessageIn` values. The
bounded `MessageInBus` is single-producer/single-consumer: one producer may
`Push`, one consumer may `Process(timestamp)`, and callers with multiple
producers must serialize before pushing. The bus applies parameter turns and
pushes through slot position, bank selection through manager bank index,
gesture messages through manager-owned gestures, and scene messages through the
manager scene API. A `SceneSelect(sceneIx)` message selects one scene ordinal;
the manager writes it to the less-selected endpoint of the current blend
(`blend <= 0.5` updates right, `blend > 0.5` updates left) and leaves blend
unchanged. `Clock`, `Start`, `Continue`, and `Stop` are terminal realtime
messages: `Engine` merges them from the UI and MIDI buses into one bounded
timestamp-ordered batch and routes them to its runtime-owned `MasterClock`.
External messages retain their normalized timestamp and controller slot;
ordinary parameter and grid messages keep their existing routing.

Gestures are manager-owned. `ParameterManager::SetGestureCount(count)` must be
called before groups are created when gestures are needed; the default gesture
count is zero. Groups receive the fixed manager gesture count for arena sizing,
and direct gesture calls should use manager APIs.

Scene endpoint changes should use `SetSceneEndpoints(left, right)`, which
rejects endpoints that are invalid for any existing group and leaves prior
scene state unchanged. `SetSceneBlend(blend)` clamps and updates blend
independently.

## Master Clock And MIDI Sync

Every `Engine` owns one address-stable, JUCE-free `MasterClock`. Once per audio
callback the engine drains due control input, commits one immutable half-open
affine `ClockBlockPlan`, analytically queues enabled MIDI clock crossings, and
then calls the application once. Applications read musical time directly at
integer or fractional output-sample positions; they do not advance the clock.
A 4x application maps internal subframe `k` to
`block.startSample + k / 4.0`.

The runtime sidebar's Sync page stages send/receive clock and transport gates,
PPQN, and read-only lock/source/BPM/latency/late/drop diagnostics. Runtime
configuration schema v2 persists this global sync policy; schema-v1 files load
with all four gates off and PPQN 24. Patch files never contain sync policy.
PPQN values `1..960` are accepted, but values other than 24 require a matching
nonstandard peer configuration.

See [Master Clock and MIDI Sync](docs/master-clock-and-midi-sync.md) for the
timeline, transport, timestamp-epoch, scheduling, threading, diagnostics, and
fallback contracts. Its microsecond tolerances describe calculated deadlines
before host delivery, not physical MIDI-device latency.

## Runtime Button Grids

`ButtonGrid.hpp` defines a JUCE-free grid subsystem beside parameter
modulation. A `GridManager` owns runtime-sized grids and grid slots in their
own index namespaces: grid slot `0` and parameter bank slot `0` never alias.
Every `GridRange` is a signed half-open rectangle
`[xmin, xmax) x [ymin, ymax)`, and grid selection requires exact range
equality. Dense cells receive explicit press, release, and pressure-change
callbacks; `StateCell` supplies toggle, momentary, set-only, and show-only
state behavior with optional flash palettes. These contracts are exercised by
`build/button_grid_tests` and the mixed-namespace cases in
`build/parameter_modulation_tests`.

`GridManager::CreateUIState()` is the topology-finalization boundary. Grid and
cell storage is allocated before that call; creating grids, slots, or cells
after it is a coding error. Selecting existing grids, routing events, querying
cells, and publishing snapshots then reuse fixed capacities and storage
addresses. Cell callbacks and state queries run on the control/audio path and
must themselves remain nonblocking and allocation-free. Each published cell
is one atomic packed color: RGB is preserved and the final byte is overwritten
with exactly `1` for on or `0` for off, including explicit zero for empty and
disconnected cells. `build/button_grid_tests` covers finalization, stable
capacities/pointers, signed lookup, and packed publication.

The existing bounded `MessageInBus` remains the single ordered SPSC input
queue. Its four grid variants are `GridPress`, `GridRelease`,
`GridPressureChange`, and `SelectGrid`; the bus dispatches parameter and grid
messages only to their respective managers while retaining timestamp/FIFO
ordering. `build/instrument_tests` covers the message/JSON shapes and
`build/parameter_modulation_tests` covers interleaved routing, invalid-target
no-ops, and namespace isolation.

`Engine` owns one `GridManager` beside `ParameterManager`, attaches both UI and
MIDI buses to both managers, and publishes both snapshots through an internal
stable `RuntimeUIState` facade. MIDI encoder output reads its parameter member;
grid feedback reads only its immutable grid member. `AppContext::uiState`
remains a `ParameterManager::UIState*`: applications do not create, expose, or
render grids in this capability. This application-level grid surface is
intentionally out of scope, as covered by `build/engine_tests`,
`build/rig_tests`, and `build/miniapp_system_tests`.

Controller-profile JSON readers accept schema 1 without pressure data and
writers emit schema 2 with separate polyphonic-pressure mappings. Status
`0xA0` mappings derive `GridPressureChange` input while press, release, and
feedback remain ordinary system-message associations. Controllers reconstructs
only exact system/pressure pairs into momentary `Grid Button` or `Grid Block`
rows, expands edits back into all paired entries atomically, and preserves
unmatched pressure mappings invisibly and losslessly. Existing WRLD.Bldr,
Launchpad, and monochrome output wire protocols, caches, resets, and budgets
are unchanged; only feedback state lookup gained grid support. Coverage lives
in `build/instrument_tests`, `build/blocks_tests`, `build/viewmodel_tests`,
`build/controllers_page_ui_tests`, and the miniapp Controllers JUCE harness.

## Controller Configuration Wizard

`ControllerWizard.hpp` adds a JUCE-free layer between raw MIDI device discovery
and a complete controller profile. It ships one wizard, for the MIDI Fighter
Twister. The Controllers page's add row and Restore are the only callers: Add
installs a chosen preset's default profile in one activation, and Restore
regenerates a diverged row's profile from the same descriptor. Neither runs
through a wizard-specific page; both go through the ordinary instrument
commit and save path.

### Registry and discovery

`ControllerWizardRegistry()` returns a baked, ordered
`std::vector<ControllerWizardDescriptor>`. Each descriptor owns a stable id, a
display name, the resulting `MidiProfileKind`, descriptor-local input/output
alias lists, and a `factory` that builds its `ControllerWizard`. Adding a
controller means appending one descriptor plus its
`TypedControllerWizard<Form>` implementation; nothing else in the runtime,
browser, or JUCE code has to change. The shipped descriptor is
`com.sheaf.midi-fighter-twister`, display name `MIDI Fighter Twister`, kind
`twister`, with the single input and output alias `Midi Fighter Twister`.

`DiscoverControllerWizards(devices, instrument, registry)` is a pure function
returning a `WizardDiscovery`. Name matching is **case-insensitive exact
comparison against the descriptor's own aliases** — prefix, substring, fuzzy,
and implicit numeric-suffix names deliberately do not match, so a
platform-specific numbered name must be added as an explicit alias before it is
recognized. Endpoints left unpaired — whether no descriptor recognized them or a
descriptor recognized them but found no matching endpoint on the other side —
are retained in `unmatchedInputs`/`unmatchedOutputs`, and the Controllers page
renders them as "Other inputs" and "Other outputs."

A pair is *available* only when both endpoints are present, one descriptor
recognizes it, neither endpoint is claimed by any stored record (Active or
Blacklisted, identifier-first then stored-name fallback, matching
reconciliation identity semantics), and the pair has not already been emitted in
this pass. Discovery is deterministic: duplicate same-name devices pair by
stable enumeration order, and no endpoint appears in two candidates.

The Controllers page lists every available pair as read-only status under
"Available controllers," by its port names and the display name of every
preset that matches it, with "No connected controller is waiting to be set
up" when none is waiting. The add row's Preset combo starts on the first
listed device's first matching preset, or the registry's first descriptor
when none is waiting. Add binds the first unused connected pair whose port
names match the chosen preset's own aliases, so a device with more than one
matching preset — an APC40 mkII, for its Generic and Ableton presets — binds
whichever preset the player chooses.

The runtime caches the device list and the derived classification. It
reclassifies from the cached snapshot after a device-list change and after every
successful instrument commit, and never forces enumeration or reconciliation for
an unchanged list. `runtime.sidebar.controllers.warning` is a view of that
cached state, so the Controllers sidebar entry carries a warning marker exactly
while an available candidate exists, even while the page is closed.

### Opaque wizard ids and the Active/Blacklisted schema

`MidiControllerSlot` gained a `MidiControllerDisposition` (`Active` or
`Blacklisted`), an optional `wizardId`, and an optional `dormantConfig`. The
wizard id is an **opaque non-empty persisted string**: the instrument model
never consults the registry, so validity and loading do not depend on which
wizards this build happens to contain. Registry resolution gates only the
Restore action.

An Active record requires its existing kind-valid profile. A Blacklisted
record requires a non-empty wizard id and both endpoint references, has no
runtime-active profile, and carries either no dormant profile or the complete
prior Active profile retained as dormant data — shapes a configuration saved
by an earlier build can hold, since nothing on the page creates a Blacklisted
record now. Unique names and ordered iteration span both dispositions.

Instrument JSON is schema version 2. Entries write `disposition`
(`"active"`/`"blacklisted"`), an optional `wizardId`, and `profile` (the Active
profile, or the dormant one for a Blacklisted record). Loading rejects — into
scratch state, without mutating the live configuration — unknown kinds, unknown
dispositions, malformed or empty wizard ids, duplicate names, Active entries
without a valid kind-compatible profile, Blacklisted entries missing a wizard id
or either endpoint reference, and kind-invalid active or dormant profiles. An
unknown but well-formed wizard id round-trips unchanged. Schema-1 documents load
with every controller treated as Active with no wizard id and its existing
profile required exactly as before; the older single-`midiProfile` format remains
unsupported. Rolling back to an older binary means removing Blacklisted records
or converting them to Active before writing an older schema.

### Blacklisted records are inert

Blacklist disposition is checked **before every Active rule**, at both
boundaries:

- `Engine::RebuildMidiProcessors()` installs
  `CreateBlacklistedMidiControllerProfile()` for a Blacklisted slot before it
  reads `config` or registers any sink. That slot holds exactly one
  `DropMidiInProcessor` and no thru processors, no output processors, no sender
  sink, and no terminal realtime processor, so ordinary, SysEx, Clock, Start,
  Continue, and Stop bytes all emit nothing.
- Reconciliation never claims, opens, updates a stored reference, resyncs, or
  marks Offline either endpoint of a Blacklisted slot. Endpoints still online
  from an earlier Active disposition are closed and returned to `Unconfigured`,
  which for a Blacklisted slot means *deliberately inert* even though both
  stored references remain populated. Its deliberately stale references are
  never rewritten, so an Active slot may claim a device a Blacklisted slot still
  names.

Slot ordinals stay stable across these transitions, so deleting a middle
record resizes through the normal whole-instrument rebuild path. A
Blacklisted row shows its Released badge and stored port labels and offers
Delete, the same control an Active row's ports line offers; it exposes no
live endpoint selectors, mapping editor, or path back to Active.

### The MF Twister default

`MfTwisterConfigForm` holds only the fixed default the library ships: one
controller-wide Encoder Slot of `0` and exactly six side buttons, defaulting
respectively to Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start,
and Previous Bank on channel 3 CCs 8 through 13. It is in-memory generation
input only: it never enumerates MIDI, edits the engine, saves configuration,
renders a node tree, or dispatches an action. Add installs this default
exactly like any other preset, and the player edits the installed row
afterward, on the same mapping editor every other kind uses.

Generation reuses `MfTwisterDefaultProfileConfig`, passing the form's fixed
Encoder Slot as `slotIx` and exactly six side-button associations. All sixteen encoder
positions' turn, push, and output mappings target the one Encoder Slot, as do
every Bank Select, Next Bank, and Previous Bank message. Hold choices emit a
`true` press message and a matching `false` release. Side buttons are
input-only, with no output feedback.

### Lifecycle: add, restore, delete

The add row's Preset combo lists the registry's descriptors, in registry
order, followed by one Custom entry; it starts on the first
matching preset of the first device listed under "Available controllers," or
the registry's first descriptor when none is waiting, until the player picks
a different option. Add builds a named active controller from the chosen
preset through this same wizard machinery, or an empty Generic-kind record
with no generated mapping and no bound endpoints for Custom. The new row
opens expanded, with every section it lists open, through the same disclosure
and section toggles a player's own click on them uses, so those same clicks
still collapse it. A row that appears any other way, such as loading a
configuration, still starts collapsed.

A row whose persisted wizard id resolves in the current registry and whose
stored config no longer matches that preset's freshly generated profile
offers Restore. Restore regenerates the descriptor's profile from the row's
own current name and endpoint references, replaces the slot's kind and
config, and leaves its name, endpoint references, wizard id, and disposition
unchanged.

Active rows offer inline Rename (a `runtime.controllers.row.{N}.rename_draft`
text field plus a `runtime.controllers.row.{N}.rename` commit button) and an
immediate `runtime.controllers.row.{N}.delete` with no confirmation step.
Delete removes a record of either disposition. Manual and legacy records —
those with no wizard id — keep Rename, Delete, endpoint selection, and the
low-level mapping editor, and are never offered Restore.

Every action on this page — adding, choosing a port, editing a mapping,
renaming, deleting, and Restore — commits through the existing
`engine.EditInstrument` + `MidiConnectionManager` reconcile path and saves the
runtime configuration as part of that same action, so the edit survives a
reload however the player leaves the page; a failed save leaves the committed
edit in place and shows the status. The page never opens or closes device
handlers itself, and leaving by Back adds no separate save.

### Acceptance coverage

JUCE parity is pinned by
`juce/ControllersPageSimulationTests.cpp` (which drives the same node ids and
actions) and `juce/RuntimePagesJuceTests.cpp` (the sidebar warning marker).
JUCE-free contracts live in `build/controller_wizard_tests`,
`build/controllers_page_ui_tests`, `build/runtime_main_component_tests`,
`build/instrument_tests`, `build/reconcile_tests`,
`build/reconcile_executor_tests`, `build/engine_tests`, and
`build/viewmodel_tests`. See [Spec Coverage](docs/coverage.md) for the
requirement-to-test mapping.

Neither the browser TypeScript backend nor the JUCE renderer contains any
Twister, wizard, generation, matching, or validation policy.

## Portable UI Contract

Headers under `projects/synth/include/synth/PortableUI.hpp` and
`PortableUIBuilders.hpp` are JUCE-free. Applications satisfying
`synth::SynthApplication` expose a `PortableSurface()` that returns a
`synth::ui::Surface&`.

`synth::ui::Surface::BuildTree()` runs on the host UI/message thread. Action
handlers registered through `SetActionHandler()` also run on that thread.
Application actions that affect DSP should enqueue `synth::MessageIn` values
through `AppContext::uiBus` with timestamps from `AppContext::now`.

Bespoke widgets (encoders, waveform scopes, segment displays) describe paint
as bounded `synth::ui::DrawCommand` sequences on `NodeKind::Draw` nodes.
Form-style runtime pages use semantic control nodes (`Label`, `Button`,
`Toggle`, `Slider`, `ComboBox`, `TextField`, and related kinds) so a browser
or other backend can map them to DOM controls without pulling in JUCE.

The desktop implementation of that contract lives under `projects/synth/juce`.
`PortableJuceBackend.hpp` renders application and runtime-owned semantic
surfaces through the same generic `PortableComponent`. Desktop backend files
are allowed to include JUCE and translate portable geometry, draw commands,
focus, and actions into JUCE components.

Portable producers must stay under `projects/synth/include/synth` or app code
and must not include JUCE headers or expose `juce::` types. The root Makefile's
`check-ui-boundary` target enforces that rule with an allowlist for
`projects/synth/juce`, the desktop runtime host files, and app entry points:

```text
make -C projects/synth check-ui-boundary
```

`make -C projects/synth test` runs the same boundary check before the
JUCE-free test binaries.

## Browser/Wasm Follow-Up Boundary

A browser build should add another backend for the same portable surfaces
instead of changing app or runtime page producers. The bespoke miniapp widgets
already describe their visuals as draw commands; a Wasm backend can batch those
commands into canvas/WebGL drawing. Runtime pages and the Controllers page use
semantic nodes, so a browser backend can map them to DOM controls and route DOM
events back as `synth::ui::Action` values.

Audio, MIDI, and patch-storage integration remain host/runtime
responsibilities. The browser runtime should reimplement those I/O adapters
behind the same runtime object boundary used by the JUCE desktop host, while
mapping the portable File page explorer to browser storage and keeping synth
core, app UI producers, and page producers JUCE-free.

## Layout: runtime vs apps

`projects/synth/runtime/` is the shared, app-agnostic JUCE application
runtime (`synth_runtime::Runtime<App>`, `synth_runtime::ShellComponent<App>`,
`synth_runtime::MainPane<App>` and its Audio/Controllers/Sync/File pages, the
`SYNTH_RUNTIME_MAIN` macro). It owns the audio device, the message-thread
tick, and per-controller MIDI connection management that every app built on
it gets for free.

`MainPane` lays out a fixed-width right sidebar (Audio/Controllers/Sync/File
buttons plus a rolling-max deadline readout) next to a content host that
shows exactly one page at a time: the Audio page (audio device
selection/status), the Controllers page (per-controller MIDI device pickers
and mapping edits), the Sync page (`SyncPageSurface`, clock/transport policy
and status), and the File page (patch commands and patch identity).
`RuntimeMainComponent` builds each one from the portable page trees in
`include/synth/RuntimePages.hpp` and `include/synth/ControllersPageUI.hpp`.
Each page owns its own state; there is no separate shell chrome row.

`projects/synth/apps/<name>/` holds one runtime application each. An app
provides a JUCE-free core satisfying `synth::SynthApplicationCore` (so it can
also run headless under `synth_rig::SynthRig` in tests) plus a JUCE-facing UI
wrapper satisfying `synth::SynthApplication` (adds `PortableSurface()`), and a
`Main.cpp` that is just `SYNTH_RUNTIME_MAIN(AppType)`. The app's own UI
component should contain only its bespoke widgets; patch commands and MIDI
device/controller configuration are runtime-owned pages, not app code (see
`projects/synth/apps/miniapp/README.md` for a concrete example).

`projects/synth/apps/braid-4/` is an exception to the standalone app target
pattern: Braid 4 is Sheaf Patch-only and is launched from the Sheaf Patch app
registry rather than from its own `Main.cpp`/Makefile target. Its internal audio
graph runs oscillator, modulation, and matrix processing at 4× the host sample
rate, then applies the final 4:1 FIR decimation stage at the output edge before
returning audio to the host-rate runtime.

The runtime owns long-lived app data through `synth::RuntimeDataPaths`. In
production `Runtime<App>` resolves an OS application-data root under
`Sheaf/<appName>` and creates `patches/`, `logs/`, and `config.json`. Tests can
inject scratch paths. `patches/` contains synthesizer patch directories and
version files; `logs/` contains async session logs; `config.json` stores MIDI
controller/device configuration, audio input/output selection, and global sync
configuration. Writers emit runtime-config schema v2. Readers migrate schema
v1 by supplying safe sync defaults (all flags off, PPQN 24), and reject invalid
schema-v2 sync values atomically. Patch files do not carry MIDI/audio/sync
configuration.

The File page hosts the patch-command
row (New/Save/Save As/Load), an in-app patch browser rooted at
`Runtime::DataPaths().patchesRoot`, and the patch identity label. It does not
use the operating system file explorer. Patch command results are logged by the
runtime itself, at INFO level through `synth::AsyncLogQueue`
(`synth_runtime::Runtime::LogPatchCommand`) — apps do not write their own patch
log files. The page shows the current patch's name (the patch directory's
filename, or "(no patch)" when none is current), read fresh every repaint tick
from `Runtime::GetEngine().Patches().CurrentPatchDirectory()` — the
message-side `PatchManager`'s own state, never cached elsewhere and never
touched from the audio thread. The Save button checks the same state before
dispatching: with no current patch it falls through to the in-app Save As
browser instead of sending a `SavePatch()` doomed to return `NeedsSaveAsPath`
(that status is still logged as a backstop if it ever occurs).

The shared `ControllersPageSurface` hosts MIDI device selection and
controller/mapping configuration per controller slot, and the desktop renders
it through the generic `synth_juce::PortableComponent`. Page edits commit
through `engine.EditInstrument` plus a
`MidiConnectionManager` reconcile pass — the page never mutates config or
device handlers directly.

Audio and Sync page Back buttons save `config.json` before returning to the
app view. The Controllers page saves on every committed edit instead, so its
Back button adds no separate save. File page Back only dismisses the File
page; patch save/load commands are explicit.

### Audio input on the JUCE desktop host

`RuntimeConfig::numAudioInputs` is a request, not a guarantee. `Runtime::Start()`
runs the shared JUCE-free `synth::ValidateRuntimeConfig` before it touches the
log directory, the MIDI sender, or any audio device, so a negative request
throws `std::invalid_argument` ahead of every startup step rather than after a
device is already open. Any nonnegative count is accepted — the framework
imposes no channel ceiling of its own — and JUCE negotiates what the selected
device can actually provide.

Each device block reaches the application clamped to that request:
`AudioBlock::numRequestedInputChannels` is the app's count and
`AudioBlock::numInputChannels` is `clamp(actual, 0, requested)`. A device that
exposes more channels than requested is truncated to the requested prefix, so
changing hardware never silently widens an app's input shape. A device that
exposes fewer reports its actual count, and the requested-but-absent channels
read as silence through `AudioBlock::InputView()` rather than through
host-allocated scratch buffers. A null channel pointer inside the counted
prefix keeps its own logical position — nothing is compacted — and
`AudioInputView::HasActiveChannel` reports it unavailable while
`SampleOrSilence` returns zero without dereferencing it. The runtime creates no
input-to-output monitoring path: output is whatever the application writes.
Browser hosts apply their own platform limit and reject requests above 32
before prompting for capture or starting audio.

Application DSP reads input through the callback-lifetime view returned by
`block.InputView()`. Strict accessors (`Channel`, `Frame`, and `Sample`) are for
channels the host reports as active and non-null; `SampleOrSilence` is the
defined path for optional channels, shortfalls, counted null pointers, and
out-of-range frames. Do not retain an `AudioInputView`, `AudioInputFrameView`,
or `std::span` from `Channel()` after `ProcessBlock` returns.

```cpp
void ProcessBlock(synth::AudioBlock& block) {
    const auto input = block.InputView();

    if (input.HasActiveChannel(0)) {
        const std::span<const float> channel0 = input.Channel(0);
        const float firstSample = channel0.empty() ? 0.0f : channel0[0];
        (void) firstSample;
    }

    for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
        const auto frameView = input.Frame(frame);
        const float left = input.SampleOrSilence(0, frame)
            + 0.5f * input.SampleOrSilence(2, frame);
        const float right = frameView.SampleOrSilence(1)
            - frameView.SampleOrSilence(3);
        if (block.numOutputChannels > 0) block.outputs[0][frame] = left;
        if (block.numOutputChannels > 1) block.outputs[1][frame] = right;
    }
}
```

An application declaring zero inputs opens no input path at all. The runtime
passes `0` as the input channel count to `AudioDeviceManager`, so JUCE selects
no input device, the Audio page hides the input selector, and macOS never
raises the microphone permission prompt. Such an application's blocks carry
`inputs == nullptr`, zero active input channels, and an empty input view.
Mini App and Braid 4 are both zero-input today.

For input-capable applications the Audio page status line always leads with the
stable text `Input requested N / active M`, with the current device or
permission diagnostic appended after it (for example
`Input requested 2 / active 0 - audio input device not found: Studio In`). A
missing input device, a denied microphone permission, or a device that stops
providing input therefore shows up as a falling active count and safe silence,
while output, UI frames, patch persistence, and MIDI keep running — none of
those failures stops the output path. The audio callback only stores the
observed count; the message-thread UI tick renders the text, so no callback
logs, allocates, or formats strings. Input and output device selections persist
independently in `config.json`, and an absent persisted device leaves the
currently open device alone rather than failing startup.

The Audio page's `System Default` input option is an empty host-neutral device
name, and that empty name is what persists. JUCE reads an empty
`AudioDeviceSetup::inputDeviceName` as "no input device wanted", so the runtime
resolves it to the concrete native default input device before applying the
setup — exactly as it already does for the output side — and leaves the
persisted name empty. A zero-input application is deliberately excluded from
that resolution: its empty name genuinely means no input device.

Opening a device can fail even when it is enumerated (already in use,
microphone permission denied), and JUCE deletes the current device — output
included — when it does. Every returned input or initialization error is
published through the status line rather than only logged, and the runtime then
recovers a live output-only device from the last known-good setup, so output,
UI frames, persistence, and MIDI keep running while the diagnostic reports
`Input requested N / active 0` alongside the error. Recovery deliberately does
not restore the previous input device: re-selecting an input is a user action,
and nothing retries capture from the audio callback.

macOS bundles built from `runtime/juce_build.mk` carry
`NSMicrophoneUsageDescription`, which macOS requires before it will show the
microphone prompt for an app that does open an input device. Sheaf's desktop
apps are unsigned local builds with no App Sandbox entitlement file, so no
`com.apple.security.device.audio-input` entitlement is declared; a future
sandboxed signing profile has to add it to that profile's entitlements
alongside the usage description.

Build an app from `projects/synth`:

```text
make -C apps/<name>
```

or, for the miniapp specifically, via the root Makefile's convenience
targets:

```text
make miniapp   # delegates to `make -C apps/miniapp`
make sheaf-patch
make apps      # same thing, explicit about the apps/ layout
```

Building any app requires a local JUCE checkout (`JUCE_DIR`, defaulting to
`~/JUCE`); `make all`/`make test` at this directory's root do not require
JUCE, since the JUCE-free library and test binaries build independently of
any app.
