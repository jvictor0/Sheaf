# Proposal — `app-plugin-controllers`

For the upstream reviewer. Frogg3rs ships as a VST3 and AU plugin that embeds
`synth::Engine<App>` directly, with no `Runtime<App>`: the DAW owns audio,
tempo and transport. The frogg3rs change
`frogg3rs-plugin-controllers-and-patches` (in the frogg3rs repository,
`openspec/changes/`) gives that plugin the Controllers and File pages the
standalone and browser builds have. It holds every task; the Sheaf tasks carry
the prefix S. This change is the library half: what a host shaped like a
plugin needs from Sheaf, each piece usable by any such host.

## What the plugin host is

- It owns a `synth_runtime::MidiConnectionManager<App>` itself, constructed
  the way `Runtime<App>` constructs one (the engine and a
  `synth_juce::RuntimeMidiEpoch`), and wires it to the engine's rebuild
  callbacks and its own message-thread timer as `Runtime<App>` does. Each
  controller row opens its own MIDI input and output ports, exactly as in the
  standalone. The DAW track's MIDI buffer is not read for controllers: a
  VST3 `Steinberg::Vst::Event` carries a bus index, a sample offset, a
  project position and flags, and no source port, so a track cannot tell one
  controller from another.
- It keeps no runtime configuration file. The DAW project holds the
  instance's state, and its patches are the standalone application's
  patches.
- It serializes its own state for the DAW on the same patch buses the File
  page's saves use.
- It shows only the runtime pages it can serve: Controllers and File.

## What changes

1. **A host with no runtime configuration** (`sar-42`, task S1). When a
   host's data paths name no configuration file, `Engine::Initialize` opens
   no startup patch. Today such a host opens the newest patch under its
   patches root, because `LoadRuntimeConfiguration` leaves no reopen record
   and the no-record branch then opens `LatestPatchDirectory`. Writing is
   already refused for that host: `Engine::SaveRuntimeConfiguration` returns
   without writing when `RuntimeDataPaths::configFile` is empty.
2. **One requester on the patch output bus** (`spp-14`, task S2).
   `PatchSerializationContext` promises that its caller-owned arena is safe
   "as long as all serialize requests flow through PatchManager", and
   `PatchManager::ProcessResponses` discards any `SerializedJSON` response
   whose request id is not its pending save. A host that also wants a
   serialized snapshot for itself therefore asks `PatchManager` for it:
   `PatchManager` gains a host-snapshot request that writes no file and
   hands the document to a consumer the host installs, inside the
   `ProcessResponses` call that pops it. One request is outstanding at a
   time. A Save, Save As or Save As (overwrite) asked for while a snapshot is
   outstanding is held in one slot and dispatched by the call that consumes
   the snapshot, so a save never answers `Busy` because of a snapshot; no
   snapshot is requested while a save is outstanding or held.
3. **Naming the current patch without loading it** (`spp-15`, task S3). A
   host restoring its own saved state (the sound comes from that state)
   names which patch is current, so the next Save adds a version to it. The
   name is a path relative to the engine's own patches root, never an
   absolute path from the restored state, and it is accepted only when it
   resolves to an existing directory strictly inside that root, by the same
   containment `PatchBrowser::ResolveLoadPath` applies to a Load.
4. **An injectable device access for `MidiConnectionManager`** (task S4).
   The manager enumerates through `detail::EnumerateDevices` and constructs
   `synth_juce::MidiInHandler` and `synth_juce::MidiOutputHandler` directly,
   so a test of a host that owns a manager sees and drives whatever devices
   are plugged into the machine. The manager takes an optional device access
   (enumeration, input and output endpoint factories, and the poll interval),
   defaulting to exactly what it uses today, so `Runtime<App>` passes
   nothing and behaves as before.
5. **A host-declared sidebar** (`sru-69`, task S5). A host declares which of
   Audio, Controllers, Sync, File and the load readout its sidebar offers.
   The default is all of them, so the JUCE and browser runtimes are
   unchanged. This adds to `sru-2` without restating it.
6. **One construct per duplicated host binding** (task S6).
   `JuceRuntimeMainServices` and `BrowserRuntimeMainServices` each build the
   Controllers page's engine-derived callbacks (`MakeControllersCallbacks`),
   each feed the page its discovery cache (`RefreshControllers`), and each
   bind the File page to the engine (`MakeFileCallbacks`). The plugin would be
   the third copy of each, so each becomes one construct the three hosts
   share. No requirement changes.

## Impact

`include/synth/Engine.hpp`, `include/synth/PatchPersistence.hpp`,
`src/PatchPersistence.cpp`, `runtime/MidiConnectionManager.hpp`,
`juce/MidiHandlers.hpp`, `runtime/Runtime.hpp`, a new
`runtime/EngineMidiConnections.hpp` (with `runtime/juce_build.mk`'s
`SYNTH_JUCE_HEADERS` taking `runtime/*.hpp` by wildcard, so editing the new
header rebuilds every JUCE target),
`include/synth/RuntimePages.hpp`, `include/synth/RuntimeMainComponent.hpp`,
`include/synth/RuntimeFileService.hpp`, `runtime/JuceRuntimeMainServices.hpp`,
`include/synth/browser/BrowserRuntimeMainServices.hpp`, a new
`include/synth/ControllersPageBinding.hpp`, and tests in
`tests/engine_tests.cpp`, `tests/runtime_file_service_tests.cpp`,
`tests/runtime_main_component_tests.cpp`, and a new JUCE-linked test file
under `juce/` built by `apps/miniapp`'s `test` target. The standalone and
browser runtimes keep their behaviour; their existing tests, including the
JUCE-linked ones that `make -C apps/miniapp test` runs, are the check.

Overlap with other open Sheaf changes: `shorten-deadline-readout-window`
edits the load readout that task S5 makes optional (both touch
`BuildSidebarTree` and `RuntimeMainComponent`); `app-operator-runs` task 1.2,
if it runs, changes how `MidiConnectionManager` constructs its input handlers,
which task S4 routes through the device access; `ui-state-before-audio`
edits `Engine::MessageThreadTick`, which calls the `ProcessResponses` that
task S2 changes.

## Delivery

On the branch `app-plugin-controllers`, made from the tip of
`unbounded-patch-serialization-arena` (#22), pushed to the fork
`daguilarc/Sheaf` and opened as the next sequential pull request against
`jvictor0/Sheaf` `main`, stacked on #22. Task S1 wraps the startup block
whose reopen-record branches `fold-controller-wizard-into-add-row` (#19)
wrote, and reads the empty-configuration predicate #19 added to
`Engine::SaveRuntimeConfiguration` for a bare test engine. It adds a kind of
host #19 does not have and leaves the reopen record's rules as they are, so it
is not part of #19's concept and goes in this pull request with the rest.
