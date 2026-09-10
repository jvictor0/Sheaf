#pragma once

// synth_runtime::Runtime — the JUCE-side host shell over synth::Engine<App>
// (sar-7 and later). Owns the audio device, drives the engine's audio-thread
// block pump from AudioIODeviceCallback, drives the engine's message-thread
// tick from a juce::Timer, and forwards patch commands from chrome (menu
// items, buttons) to the engine's PatchManager, INFO-logging each result.
//
// Startup/shutdown ordering here is binding; see the Task 2 brief
// (.superpowers/sdd/p3-task-2-brief.md) for the full rationale. MIDI
// connection lifecycle (device open/close/offline/resync, per controller
// slot including slot 0) is owned by midiConnections_ (a
// MidiConnectionManager<App>, Task 2 of Plan 3):
// engine.SetMidiProcessorsWillRebuildCallback forwards to
// midiConnections_->OnMidiProcessorsWillRebuild() (detaching every
// controller's forwarding processor before the engine destroys the current
// MIDI processor chain), and the rebuilt callback forwards to
// midiConnections_->OnInstrumentRebuilt() (resizing to the current
// controller count, reinstalling forwarding, and running one reconcile pass)
// -- see MidiConnectionManager.hpp's class doc comment for why the manager
// owns every controller's handlers/sink registration. Runtime's
// message-thread timer also drives midiConnections_->OnTimerTick() (the
// self-healing poll-driven reconcile path) every tick, and Start() calls
// midiConnections_->StartupReconcile() once after Initialize() (which starts
// the background device-list poller). As of Task 4 of Plan 4, Runtime owns
// no MIDI UI component at all -- MidiConnections() exposes midiConnections_
// directly so the shared ControllersPageSurface can read
// EnumerateNow()/State() to populate its combos/status dots, the same way
// AudioConfigPage reads DeviceManager() directly. ControllersPageSurface never
// calls into midiConnections_ to open/close a device itself, though (Task 4
// review, Important finding 3): every device combo change writes through
// synth::MidiConfigViewModel::SetEndpointRef and commits via
// engine.EditInstrument, exactly like every other edit on that page; the
// rebuilt callback below (SetMidiProcessorsRebuiltHook) is what lets the
// page notice ANY runtime-config/instrument change -- including a
// reconcile-driven ref rewrite -- and re-derive its view model.
//
// Audio device selection (Task 3 of Plan 4): the actual switch
// (AudioDeviceSetup mutation, setAudioDeviceSetup, logging) is implemented
// once, in ApplyAudioDeviceSelection()/ApplyAudioDeviceInputSelection(), and
// reached from the user changing AudioConfigPage's combo
// (AudioConfigPage.hpp calls these methods straight from its combo's
// onChange). Runtime configuration loading will seed the engine's audio
// device state separately from patch data.
//
// Runtime no longer owns a UI component for this (Task 3 of Plan 4 deleted
// AudioPanel): it exposes DeviceManager() so AudioConfigPage can read
// device names directly, and two host hooks --
// SetAudioStatusHook()/SetAudioSyncHook() -- that AudioConfigPage installs
// on construction so Runtime can push status text and "re-sync your combo
// selection" notifications to whichever page instance is currently alive,
// without Runtime holding a reference to it (MainPane/pages are constructed
// after Runtime, and can be torn down/rebuilt independently of it).
//
// Runtime owns persistent data paths. Production apps resolve an
// OS-appropriate user application data root under Sheaf/<appName>, then use
// RuntimeDataPaths children for patches/, logs/, and config.json. Patch
// documents stay parameter-only; runtime configuration stores MIDI and audio
// selection separately in config.json and is saved from the configuration-page
// Back flows.
//
// MidiPanel retirement (Task 4 of Plan 4): the single-slot MidiPanel
// component is deleted -- ControllersPage.hpp (a thin JUCE renderer over the
// JUCE-free synth::MidiConfigViewModel) replaces its device-selection role
// with a genuinely per-controller UI, and its old preset combo is gone
// entirely (kind defaults are now seeded via ControllersPage's "+" row, per
// spm-37). Runtime no longer owns a MidiPanel instance; it exposes
// MidiConnections() so ControllersPage can read EnumerateNow()/State() for
// display, the same way AudioConfigPage reads DeviceManager() directly
// instead of Runtime owning AudioPanel -- all WRITES (device selection,
// mapping edits, adding a controller) go through
// synth::MidiConfigViewModel + engine.EditInstrument instead (see the
// paragraph above and SetMidiProcessorsRebuiltHook's doc comment below).

#include "synth/AppConcepts.hpp"
#include "synth/AsyncLogger.hpp"
#include "synth/Engine.hpp"
#include "synth/PatchPersistence.hpp"
#include "synth/PortableUI.hpp"
#include "synth/ThreadId.hpp"

#include "HostDataPaths.hpp"
#include "MidiConnectionManager.hpp"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace synth_runtime {

template <synth::SynthApplication App>
class Runtime : private juce::AudioIODeviceCallback, private juce::Timer {
public:
    Runtime()
        : startTime_(std::chrono::steady_clock::now())
        , midiEpoch_(synth_juce::RuntimeMidiEpoch::Capture(startTime_))
        , engine_([this]() -> std::uint64_t { return NowMicros(); })
        , midiConnections_(std::make_unique<MidiConnectionManager<App>>(engine_, midiEpoch_)) {
        // sar-33: wires the external-input-routed signal's storage into the
        // AppContext apps see, before Start()/engine_.Initialize() can ever
        // run App::Init(). inputRoutingSignal_ is a member of this Runtime
        // (constructed above, in the member-init list, before this
        // constructor body runs), so its address is already stable here.
        engine_.Context().inputRoutingSignal = &inputRoutingSignal_;

        // The engine invokes this synchronously, on whichever thread is
        // performing a rebuild, immediately BEFORE midiProcessors_ is
        // destroyed/replaced (Initialize()'s rebuilds and
        // MessageThreadTick()'s rebuild all funnel through
        // Engine::RebuildMidiProcessors()). Forwarding straight to
        // midiConnections_ (rather than through a std::function indirection
        // like onMidiProcessorsRebuilt_) is safe here because midiConnections_
        // is constructed above, in this same initializer list, before this
        // lambda can ever run.
        engine_.SetMidiProcessorsWillRebuildCallback([this] { midiConnections_->OnMidiProcessorsWillRebuild(); });

        // The engine invokes this (on the message thread, from
        // EditInstrument whenever midiProcessors_ has just been rebuilt.
        // midiConnections_->OnInstrumentRebuilt() resizes to the current
        // controller count, reinstalls forwarding, and runs one reconcile
        // pass (sar-8) -- the same executor path the timer-driven poll uses.
        // onMidiProcessorsRebuilt_ (wired by MainPane via
        // SetMidiProcessorsRebuiltHook(), see that method's doc comment) is
        // ControllersPage's subscription to EVERY rebuild -- not just its
        // own edits -- so runtime-config or other engine-driven instrument
        // changes also mark the page dirty (Task 4 review, Critical finding 1:
        // a page that only dirtied on its own commits or a connection-status
        // fingerprint missed exactly this class of change, letting a later edit
        // commit from a stale snapshot).
        engine_.SetMidiProcessorsRebuiltCallback([this] {
            midiConnections_->OnInstrumentRebuilt();
            if (onMidiProcessorsRebuilt_) {
                onMidiProcessorsRebuilt_();
            }
        });
    }

    ~Runtime() override {
        deviceManager_.removeAudioCallback(this);
        stopTimer();
        // Shutdown ordering (binding, per Task 2/3 briefs): stop the MIDI
        // sender before closing devices, so no in-flight enqueued MIDI is
        // delivered to a sink that's about to be torn down, THEN
        // midiConnections_'s own destructor stops/joins its poller BEFORE
        // closing any device handler (binding, per p3-globals.md: "Shutdown
        // stops/joins the poller before closing devices").
        if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
            sender->Stop();
        }
        midiConnections_.reset();
        INFO("Runtime shutting down: %s", engine_.Config().appName.c_str());
        synth::AsyncLogQueue::s_instance.DoLog();
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    static synth::RuntimeDataPaths DefaultDataPathsForApp(const synth::RuntimeConfig& config) {
        return synth_runtime::DefaultDataPathsForApp(config);
    }

    void SetRuntimeDataPathsOverride(synth::RuntimeDataPaths paths) { dataPathsOverride_ = std::move(paths); }
    const synth::RuntimeDataPaths& DataPaths() const { return dataPaths_; }

    // Startup ordering (binding, per Task 2/3/4 briefs):
    //   0. synth::ValidateRuntimeConfig(App::Config()) before anything else
    //   1. resolve/create runtime data paths and configure log directory
    //   2a. wire engine_.SetAudioDeviceChangedCallback to
    //       OnEngineAudioDeviceChanged for future runtime-config-initiated
    //       audio changes
    //   3. engine_.Initialize()
    //   4. start the MidiSender worker
    //   5. synchronously reconcile MIDI devices, then start the MIDI poller
    //   6. open the audio device, applying preferred rate/block where
    //      allowed, PREFERRING engine.AudioDeviceSnapshot().outputDeviceName
    //      over the platform default when it names a currently-enumerated device
    //      (runtime configuration seeds this value before device open)
    //   7. Prepare the engine via audioDeviceAboutToStart and register the
    //      audio callback
    //   8. publish the initial requested/active input diagnostic
    //   9. start the UI timer
    void Start() {
        const synth::RuntimeConfig appConfig = App::Config();

        // Shared JUCE-free validation (sar-31), ahead of the log directory,
        // the MIDI sender, and any audio device: a negative input request must
        // never reach device negotiation. engine_.Initialize() validates the
        // same config again; this call is what makes the rejection precede
        // every host-side startup step rather than only the engine's.
        synth::ValidateRuntimeConfig(appConfig);
        requestedInputChannels_ = appConfig.numAudioInputs;

        dataPaths_ = dataPathsOverride_.has_value() ? *dataPathsOverride_ : DefaultDataPathsForApp(appConfig);
        synth::AsyncLogQueue::s_instance.ConfigureLogDirectory(dataPaths_.logsRoot.string().c_str());
        std::error_code ec;
        std::filesystem::create_directories(dataPaths_.dataRoot, ec);
        if (ec) {
            INFO("Runtime data root create FAILED: path=%s error=%s", dataPaths_.dataRoot.string().c_str(),
                 ec.message().c_str());
        }
        ec.clear();
        std::filesystem::create_directories(dataPaths_.patchesRoot, ec);
        if (ec) {
            INFO("Runtime patches root create FAILED: path=%s error=%s", dataPaths_.patchesRoot.string().c_str(),
                 ec.message().c_str());
        }
        ec.clear();
        std::filesystem::create_directories(dataPaths_.logsRoot, ec);
        if (ec) {
            INFO("Runtime logs root create FAILED: path=%s error=%s", dataPaths_.logsRoot.string().c_str(),
                 ec.message().c_str());
        }
        engine_.SetRuntimeDataPaths(dataPaths_);

        // Wired before Initialize() so future runtime-config-initiated audio
        // changes can be applied through the same host path. Patch load/revert
        // is parameter-only and never fires this callback.
        engine_.SetAudioDeviceChangedCallback([this] { OnEngineAudioDeviceChanged(); });

        engine_.Initialize();
        INFO("Runtime started: %s", appConfig.appName.c_str());
        const synth::RuntimeConfig& config = engine_.Config();

        // Start the sender before opening MIDI inputs/outputs or starting the
        // connection poller. Once ingress or the audio callback exists, every
        // producer therefore has a live consumer.
        if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
            sender->Start();
        }

        // Startup order (binding): engine init -> sender -> startup/runtime
        // config applied -> ONE synchronous reconcile -> start poller -> audio
        // device -> ... StartupReconcile() is that
        // synchronous reconcile: it resizes midiConnections_ to the current
        // controller count, reconciles every configured ref against
        // currently-present devices (absent -> offline, never a startup
        // failure), and starts the background poller. Called unconditionally
        // here (idempotent, same as the old always-reopen-at-startup behavior)
        // because Initialize()'s first RebuildMidiProcessors() call is silent
        // by design — it never invokes midiProcessorsRebuiltCallback_ (see
        // Engine::Initialize's doc comment) — so midiConnections_ is never
        // notified during startup and its handler vectors would otherwise stay
        // unsized.
        midiConnections_->StartupReconcile();

        // No input device opens merely because the application requested
        // input channels -- the operator selects one explicitly (below, from
        // the persisted selection, or later via the combo), and only then do
        // the requested channels get opened. Startup therefore always
        // initialises output-only; a failure here is output failing to open
        // at all, and the returned error is the only notice.
        const juce::String initialiseError = deviceManager_.initialiseWithDefaultDevices(0, config.numAudioOutputs);
        if (initialiseError.isNotEmpty()) {
            INFO("Audio device initialise FAILED: %s", initialiseError.toRawUTF8());
            SetAudioStatus(initialiseError);
        }

        // Prefer the persisted output device over the platform default, but
        // only when it's actually present among
        // the currently enumerated output devices — an absent device leaves
        // the just-initialised default device alone, no failure, matching
        // OnEngineAudioDeviceChanged's "absent -> keep current" contract.
        const juce::String wantedOutputName = juce::String(engine_.AudioDeviceSnapshot().outputDeviceName);
        if (wantedOutputName.isNotEmpty() && IsEnumeratedOutputDevice(wantedOutputName)) {
            SwitchOutputDevice(wantedOutputName, "startup");
        } else {
            ApplyPreferredRateAndBlockSize();
            if (wantedOutputName.isNotEmpty()) {
                const juce::String message = "audio device not found: " + wantedOutputName;
                INFO("%s", message.toRawUTF8());
                SetAudioStatus(message);
            }
        }

        // Apply persisted input device (same contract as output, above).
        // config.numAudioInputs > 0 gates input selection availability.
        if (config.numAudioInputs > 0) {
            const juce::String wantedInputName = juce::String(engine_.AudioDeviceSnapshot().inputDeviceName);
            if (wantedInputName.isNotEmpty() && IsEnumeratedInputDevice(wantedInputName)) {
                if (ApplyInputDeviceSetup(wantedInputName, "startup")) {
                    ApplyPreferredRateAndBlockSize();
                }
            } else {
                if (wantedInputName.isNotEmpty()) {
                    const juce::String message = "audio input device not found: " + wantedInputName;
                    INFO("%s", message.toRawUTF8());
                    SetAudioStatus(message);
                }
            }
        }

        // SyncAudioSelection() notifies AudioConfigPage (if one is alive) to
        // re-enumerate devices and re-sync its combo's selection to
        // engine.AudioDeviceSnapshot() as it now stands (post
        // startup-preference handling above). INFO-logged explicitly (not
        // just implied by the "Audio device switch"/"Audio device state"
        // lines above) so a session log has one unambiguous line confirming
        // the selector reflects startup state even on the "no startup
        // device, nothing to switch" path.
        SyncAudioSelection();
        const synth::AudioDeviceState startupAudioDeviceState = engine_.AudioDeviceSnapshot();
        INFO("Audio device selector startup sync: selected=%s",
             startupAudioDeviceState.outputDeviceName.empty() ? "System Default"
                                                                : startupAudioDeviceState.outputDeviceName.c_str());

        // addAudioCallback() invokes audioDeviceAboutToStart(currentDevice)
        // synchronously here (the device is already open), which is what
        // actually calls engine_.Prepare() with the negotiated rate/block and
        // records the device's negotiated active input count.
        deviceManager_.addAudioCallback(this);

        // Only now is the negotiated active input count known, so this is the
        // first point at which the requested/active diagnostic can be honest.
        // For an input-capable app it is published unconditionally (the page
        // must show it even on a clean startup); for a zero-input app it is a
        // no-op, so nothing claims an input path that was never opened.
        PublishPendingInputStatus();

        // sar-33: derives the startup value of the external-input-routed
        // signal -- false for a platform-default device, true for a
        // successfully reapplied persisted selection. See
        // RefreshInputRoutedState's doc comment.
        RefreshInputRoutedState();

        startTimerHz(config.uiFrameHz > 0 ? config.uiFrameHz : 30);
    }

    synth::Engine<App>& GetEngine() { return engine_; }

    synth::ui::Surface& AppSurface() { return engine_.Application().PortableSurface(); }

    // The per-controller MIDI connection owner (Task 4 of Plan 4):
    // ControllersPage's device combo onChange → view-model SetEndpointRef()
    // → engine.EditInstrument() commit → MIDI processors rebuilt callback →
    // MidiConnectionManager reconciles opens/closes the device as needed,
    // mirroring how engine.SetAudioDeviceFromHost works for the audio path
    // (see ApplyAudioDeviceSelection's doc comment).
    MidiConnectionManager<App>& MidiConnections() { return *midiConnections_; }

    // The JUCE audio device manager this Runtime drives as
    // AudioIODeviceCallback target (Task 3 of Plan 4): AudioConfigPage reads
    // it directly (getCurrentDeviceTypeObject()->getDeviceNames(...)) to
    // populate its combo boxes, the same enumeration AudioPanel used to do
    // internally before this task deleted it.
    juce::AudioDeviceManager& DeviceManager() { return deviceManager_; }

    // Installs AudioConfigPage's status sink (Task 3 of Plan 4): invoked by
    // Runtime's own device-switch paths (Start()'s startup preference
    // handling, ApplyAudioDeviceSelection, ApplyAudioDeviceInputSelection,
    // OnEngineAudioDeviceChanged) with human-readable status text whenever
    // one of those paths has something to report. AudioConfigPage installs
    // this on construction and clears it (via an empty std::function) from
    // its destructor, so Runtime never calls into a destroyed page -- see
    // AudioConfigPage.hpp.
    //
    // Installing a sink immediately republishes the current status line. A
    // page installed mid-session has missed every line published before it
    // existed -- MainPane and its pages are constructed after Start() returns,
    // so without this the input-capable app's startup `Input requested N /
    // active M` diagnostic would never reach a page at all, and a page rebuilt
    // later would open blank until something happened to change.
    void SetAudioStatusHook(std::function<void(const juce::String&)> hook) {
        audioStatusHook_ = std::move(hook);
        PublishAudioStatus();
    }

    // Installs AudioConfigPage's re-sync hook (Task 3 of Plan 4): invoked
    // whenever Runtime has changed the audio device state out from under a
    // live page (startup preference handling or OnEngineAudioDeviceChanged)
    // so the page can re-enumerate devices
    // and re-select the combo entry matching engine.AudioDeviceSnapshot()
    // without applying anything itself (no device switch, no notification --
    // mirrors AudioPanel::SyncSelection's old contract). Cleared the same
    // way as the status hook.
    void SetAudioSyncHook(std::function<void()> hook) { audioSyncHook_ = std::move(hook); }

    // Installs ControllersPage's rebuild-notification hook (Task 4 review,
    // Critical finding 1): invoked at the end of
    // engine_.SetMidiProcessorsRebuiltCallback's lambda (constructor, above),
    // i.e. on EVERY MIDI-processor rebuild -- not just ones ControllersPage's
    // own edits triggered. This is what closes the "missed instrument
    // changes" gap: runtime-config or any other engine-driven edit path that
    // changes controller mappings/names/kinds also rebuilds MIDI processors and
    // fires this hook, so ControllersPage can mark itself dirty regardless of
    // who caused the rebuild. Re-entrancy: when
    // ControllersPage's OWN Commit() is what triggered this rebuild, the hook
    // still fires and sets dirty_ again -- harmless, since dirty_ is a plain
    // idempotent bool the page already sets itself in Commit() (see
    // ControllersPage.hpp's Commit() doc comment). Cleared the same way as
    // the audio hooks (an empty std::function on page teardown), via
    // ControllersPage's destructor.
    void SetMidiProcessorsRebuiltHook(std::function<void()> hook) { onMidiProcessorsRebuilt_ = std::move(hook); }

    // AudioConfigPage's output combo onChange target (sar-15 semantics
    // unchanged from the deleted AudioPanel::onOutputSelected path): the
    // user picked an output device in the combo, on the message thread
    // (JUCE combo box callbacks run there). Records the selection via
    // engine_.SetAudioDeviceFromHost (so it persists into the next saved
    // runtime configuration, mirroring how ControllersPage records endpoint selection into
    // the engine's instrument via synth::MidiConfigViewModel::SetEndpointRef
    // + EditInstrument), AND advances the engine's
    // audio-device-state shadow so a later engine-side sync to this exact
    // selection is correctly treated as "already known" -- see
    // SetAudioDeviceFromHost's doc comment; this replaces the old direct
    // `engine_.AudioDevice().outputDeviceName = ...` write, which left the
    // shadow stale -- Task 3 review findings 1/2) THEN applies the switch.
    // "System Default" (empty
    // name) clears deviceManager_'s outputDeviceName preference the same
    // way, via the same AudioDeviceSetup path (an empty outputDeviceName +
    // useDefaultOutputDevice==false is what JUCE treats as "we picked no
    // explicit device" -- see AudioDeviceSetup's own doc comment); we
    // deliberately still route it through SwitchOutputDevice() rather than a
    // separate branch, so both cases get identical logging/rate/block
    // handling.
    void ApplyAudioDeviceSelection(const juce::String& outputName) {
        synth::AudioDeviceState newState = engine_.AudioDeviceSnapshot();
        newState.outputDeviceName = outputName.toStdString();
        engine_.SetAudioDeviceFromHost(newState);
        SwitchOutputDevice(outputName, "selection");
        SetAudioStatus(outputName.isEmpty() ? "Audio: System Default" : "Audio: " + outputName);
        SyncAudioSelection();
        RefreshInputRoutedState();
    }

    // AudioConfigPage's input combo onChange target (Task 3 review, Minor,
    // preserved from the deleted AudioPanel::onInputSelected path): the user
    // picked an input device in the combo, on the message thread. Wired
    // identically to ApplyAudioDeviceSelection above, just for the input
    // device name field: records the selection via
    // engine_.SetAudioDeviceFromHost (same shadow-advancing rationale) then
    // applies it via ApplyInputDeviceSetup, with the same absent-device
    // handling (an inputName not currently enumerated is not applied; the
    // status label reports it, matching SwitchOutputDevice/
    // OnEngineAudioDeviceChanged's "absent -> keep current, no failure"
    // contract). "System Default" (the empty host-neutral name) is passed
    // through as-is: an empty inputDeviceName is what opens no input device
    // at all, which is the correct outcome for declining input, not a hazard
    // to route around.
    void ApplyAudioDeviceInputSelection(const juce::String& inputName) {
        synth::AudioDeviceState newState = engine_.AudioDeviceSnapshot();
        newState.inputDeviceName = inputName.toStdString();
        engine_.SetAudioDeviceFromHost(newState);

        if (inputName.isNotEmpty() && !IsEnumeratedInputDevice(inputName)) {
            const juce::String message = "audio input device not found: " + inputName;
            INFO("%s", message.toRawUTF8());
            SetAudioStatus(message);
            SyncAudioSelection();
            RefreshInputRoutedState();
            return;
        }

        if (!ApplyInputDeviceSetup(inputName, "selection")) {
            SyncAudioSelection();
            RefreshInputRoutedState();
            return;
        }
        ApplyPreferredRateAndBlockSize();
        SetAudioStatus(inputName.isEmpty() ? "Audio In: System Default" : "Audio In: " + inputName);
        SyncAudioSelection();
        RefreshInputRoutedState();
    }

    // The current audio callback load, as a percentage (sru-2). The shell's
    // MainPane<App> writes this into its rolling deadline-max window from
    // the timer-driven repaint hook (see ShellComponent::RepaintAll in
    // Shell.hpp) -- Runtime itself has no MainPane reference (MainPane owns
    // a Runtime reference, not the other way around), so it exposes the raw
    // sample here rather than writing into the pane directly.
    float DeadlineSamplePct() const { return static_cast<float>(deviceManager_.getCpuUsage() * 100.0); }

    // Installs the shell's repaint hook (Task 4): invoked at the end of
    // every timer tick, after the message-thread tick and before DoLog(),
    // so the shell can repaint itself and the app component in lockstep
    // with the engine's UI-state refresh.
    void SetRepaintHook(std::function<void()> hook) { repaintHook_ = std::move(hook); }

    // Message thread, once per UI timer tick (see timerCallback below).
    // Re-renders the audio status line when the active input count has moved
    // since the last publication -- a device shortfall, a capture loss, or a
    // permission denial all surface as a changed count. The audio callback
    // itself only stores an int (no logging, no allocation, no string work);
    // every count change is coalesced into whatever this next tick observes.
    // A zero-input application publishes nothing at all.
    void PublishPendingInputStatus() {
        if (requestedInputChannels_ <= 0) {
            return;
        }
        if (ActiveInputChannels() == publishedActiveInputChannels_) {
            return;
        }
        PublishAudioStatus();
    }

    synth::RuntimeConfigFileStatus SaveRuntimeConfiguration() {
        return engine_.SaveRuntimeConfiguration();
    }

    void NewPatch() {
        const synth::PatchCommandResult result = engine_.Patches().NewPatch();
        LogPatchCommand("NewPatch", result);
    }

    void SavePatch() {
        const synth::PatchCommandResult result = engine_.Patches().SavePatch();
        LogPatchCommand("SavePatch", result);
    }

    void SavePatchAs(const std::filesystem::path& path) {
        const synth::PatchCommandResult result = engine_.Patches().SavePatchAs(path);
        LogPatchCommand("SavePatchAs", result);
    }

    void SavePatchAsOverwrite(const std::filesystem::path& path) {
        const synth::PatchCommandResult result = engine_.Patches().SavePatchAsOverwrite(path);
        LogPatchCommand("SavePatchAsOverwrite", result);
    }

    void SavePatchAs(const juce::File& file) {
        SavePatchAs(std::filesystem::path(file.getFullPathName().toStdString()));
    }

    void LoadPatch(const std::filesystem::path& path) {
        const synth::PatchCommandResult result = engine_.Patches().LoadPatch(path);
        LogPatchCommand("LoadPatch", result);
    }

    void LoadPatch(const juce::File& file) { LoadPatch(std::filesystem::path(file.getFullPathName().toStdString())); }

    void RevertPatch() {
        const synth::PatchCommandResult result = engine_.Patches().RevertPatch();
        LogPatchCommand("RevertPatch", result);
    }

private:
    // The input channel count the host most recently negotiated or delivered,
    // already clamped into [0, requestedInputChannels_]. Written by the audio
    // callback and the device start/stop callbacks; readable from any thread.
    int ActiveInputChannels() const { return activeInputChannels_.load(std::memory_order_relaxed); }

    // The application's requested input count is the ceiling on the logical
    // input shape it ever sees (sar-6, sar-31). A device that hands us more
    // channels than the app asked for is truncated to the requested prefix, so
    // changing hardware never silently widens the app's input shape; a device
    // that hands us fewer reports its actual count, and the requested-but-
    // absent channels read as silence through AudioInputView rather than
    // through host-allocated scratch buffers. Null pointers inside the counted
    // prefix are passed through at their own logical positions -- compacting
    // them would renumber every channel after the gap. Nothing here allocates,
    // logs, or renders text: the audio thread only stores the active count for
    // the message thread's PublishPendingInputStatus() to render.
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels, int numSamples,
                                          const juce::AudioIODeviceCallbackContext&) override {
        synth::ScopedThreadId tag(synth::ThreadId::Audio);
        const int requestedInputChannels = requestedInputChannels_;
        const int activeInputChannels = std::clamp(numInputChannels, 0, requestedInputChannels);
        activeInputChannels_.store(activeInputChannels, std::memory_order_relaxed);
        synth::AudioBlock block{
            .inputs = activeInputChannels > 0 ? inputChannelData : nullptr,
            .outputs = outputChannelData,
            .numInputChannels = activeInputChannels,
            .numOutputChannels = numOutputChannels,
            .numFrames = static_cast<std::size_t>(numSamples),
            .numRequestedInputChannels = requestedInputChannels,
        };
        engine_.ProcessBlock(block, NowMicros());
    }

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override {
        if (device != nullptr) {
            double sampleRate = device->getCurrentSampleRate();
            int blockSize = device->getCurrentBufferSizeSamples();
            engine_.Prepare(sampleRate, blockSize);
            int numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
            int numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
            // The negotiated count, clamped the same way the callback clamps
            // the delivered count, so the diagnostic is accurate from the
            // moment a device opens rather than only after its first block.
            activeInputChannels_.store(std::clamp(numInputChannels, 0, requestedInputChannels_),
                                       std::memory_order_relaxed);
            INFO("Audio prepared: %.0f Hz, %d frames, %d in / %d out", sampleRate, blockSize, numInputChannels, numOutputChannels);
        }
    }

    // A stopped device has no active input. The count is only stored here; a
    // stop that is immediately followed by a restart (every device switch)
    // therefore never publishes a transient "active 0" -- the next tick sees
    // the restarted device's count instead.
    void audioDeviceStopped() override { activeInputChannels_.store(0, std::memory_order_relaxed); }

    // True when `name` is one of deviceManager_.getCurrentDeviceTypeObject()'s
    // enumerated output device names. Guards both the startup-preference path
    // in Start() and the runtime paths (ApplyAudioDeviceSelection,
    // OnEngineAudioDeviceChanged) against naming a device that isn't
    // currently present.
    bool IsEnumeratedOutputDevice(const juce::String& name) const {
        juce::AudioIODeviceType* deviceType = deviceManager_.getCurrentDeviceTypeObject();
        return deviceType != nullptr && deviceType->getDeviceNames(false).contains(name);
    }

    // Input-side counterpart of IsEnumeratedOutputDevice, used by
    // ApplyAudioDeviceInputSelection's absent-device handling.
    bool IsEnumeratedInputDevice(const juce::String& name) const {
        juce::AudioIODeviceType* deviceType = deviceManager_.getCurrentDeviceTypeObject();
        return deviceType != nullptr && deviceType->getDeviceNames(true).contains(name);
    }

    // sar-33: recomputes and publishes the external-input-routed signal from
    // current device state. Routed iff the user-selected input device name
    // -- engine_.AudioDeviceSnapshot().inputDeviceName, the same persisted
    // selection Start() applies at startup (:287-290) and
    // ApplyAudioDeviceInputSelection/OnEngineAudioDeviceChanged apply live --
    // is non-empty AND matches deviceManager_'s CURRENTLY OPEN input device
    // (getAudioDeviceSetup().inputDeviceName, with a device actually current).
    //
    // With no persisted selection, Start() opens no input device at all: the
    // output device may still be current, but getAudioDeviceSetup() names no
    // input, so selectedInputName.isNotEmpty() alone already derives false. A
    // selection that fails to open (see ApplyInputDeviceSetup) leaves the
    // setup's actual inputDeviceName pointing at whatever
    // RecoverOutputOnlyDevice restored (never the failed name), so it also
    // derives false, matching "open and delivering".
    //
    // Message-thread only: see InputRoutingSignal::Publish's doc comment
    // (AppContext.hpp) for why -- the registered app callback runs
    // synchronously from Publish(), on whichever thread calls this method,
    // and every caller of this method is a message-thread path (Start(),
    // ApplyAudioDeviceSelection, ApplyAudioDeviceInputSelection,
    // OnEngineAudioDeviceChanged), never the audio callback.
    void RefreshInputRoutedState() {
        const juce::String selectedInputName = juce::String(engine_.AudioDeviceSnapshot().inputDeviceName);
        const bool routed = selectedInputName.isNotEmpty() &&
                            deviceManager_.getCurrentAudioDevice() != nullptr &&
                            deviceManager_.getAudioDeviceSetup().inputDeviceName == selectedInputName;
        inputRoutingSignal_.Publish(routed);
    }

    // Builds and applies the AudioDeviceSetup for `inputDeviceName`, and owns
    // the failure path. This is the one place that decides both the input
    // device name and its channel fields: a non-empty name gets
    // useDefaultInputChannels cleared and the first requestedInputChannels_
    // bits of inputChannels set, opening exactly the channel shape the
    // application asked for; an empty name gets useDefaultInputChannels
    // cleared and inputChannels cleared, opening no input device at all.
    // Every caller that wants to change the input device -- startup applying
    // a persisted selection, the operator picking a device from the combo, or
    // a runtime-config-initiated change -- routes through here rather than
    // building its own AudioDeviceSetup.
    //
    // JUCE stops and deletes the current device before it opens the requested
    // one and deletes it again if the open fails, so a failed input switch
    // leaves the host with NO device at all -- output included -- and the
    // returned error string is the only notice anyone gets. Publish that
    // error through the status line (rather than only INFO-logging it and
    // then reporting a success that did not happen), then recover a live
    // device so independent output keeps running.
    //
    // Recovery re-applies the last setup JUCE reported as current with its
    // input device dropped: that keeps the known-good output device and leaves
    // the input diagnostic honestly reporting `active 0` instead of silently
    // reverting to a capture the user did not ask for. Re-selecting an input
    // device is a user action; nothing here retries, and nothing on this path
    // is reachable from the audio callback.
    //
    // Returns true when the requested setup applied, false when it failed (in
    // which case recovery has already run and the caller must not report
    // success).
    bool ApplyInputDeviceSetup(const juce::String& inputDeviceName, const char* reason) {
        const juce::AudioDeviceManager::AudioDeviceSetup lastKnownGood = deviceManager_.getAudioDeviceSetup();
        juce::AudioDeviceManager::AudioDeviceSetup setup = lastKnownGood;
        setup.inputDeviceName = inputDeviceName;
        setup.useDefaultInputChannels = false;
        if (inputDeviceName.isNotEmpty()) {
            juce::BigInteger channels;
            channels.setRange(0, requestedInputChannels_, true);
            setup.inputChannels = channels;
        } else {
            setup.inputChannels.clear();
        }
        const juce::String setupError = deviceManager_.setAudioDeviceSetup(setup, true);
        if (setupError.isEmpty()) {
            return true;
        }
        INFO("Audio input device switch (%s) FAILED: %s", reason, setupError.toRawUTF8());
        SetAudioStatus(setupError);
        RecoverOutputOnlyDevice(lastKnownGood, reason);
        return false;
    }

    // Restores output after a failed input setup, from the setup that was
    // current before it. See ApplyInputDeviceSetup for why the input device is
    // dropped rather than restored.
    void RecoverOutputOnlyDevice(juce::AudioDeviceManager::AudioDeviceSetup lastKnownGood,
                                 const char* reason) {
        lastKnownGood.inputDeviceName = juce::String();
        const juce::String recoveryError = deviceManager_.setAudioDeviceSetup(lastKnownGood, true);
        if (recoveryError.isNotEmpty()) {
            INFO("Audio device output-only recovery (%s) FAILED: %s", reason,
                 recoveryError.toRawUTF8());
        }
    }

    // Mutates the current AudioDeviceSetup's sampleRate/bufferSize to
    // config.preferredSampleRate/preferredBlockSize when the CURRENT device
    // supports them, and applies it via setAudioDeviceSetup — the same
    // preference logic Start() has always applied to the platform-default
    // device, factored out so SwitchOutputDevice() can re-apply it against
    // whatever device is current after a switch. Logs the setup error
    // string (if any) and the resulting open/playing state, matching the
    // existing instrumentation pattern (precedent commit adf0181).
    void ApplyPreferredRateAndBlockSize() {
        const synth::RuntimeConfig& config = engine_.Config();
        juce::AudioIODevice* device = deviceManager_.getCurrentAudioDevice();
        if (device == nullptr) {
            INFO("Audio device state: no current device");
            return;
        }
        juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager_.getAudioDeviceSetup();
        const juce::Array<double> availableRates = device->getAvailableSampleRates();
        if (availableRates.contains(config.preferredSampleRate)) {
            setup.sampleRate = config.preferredSampleRate;
        }
        const juce::Array<int> availableBufferSizes = device->getAvailableBufferSizes();
        if (availableBufferSizes.contains(config.preferredBlockSize)) {
            setup.bufferSize = config.preferredBlockSize;
        }
        const juce::String setupError = deviceManager_.setAudioDeviceSetup(setup, true);
        if (setupError.isNotEmpty()) {
            INFO("Audio device setup FAILED: %s", setupError.toRawUTF8());
        }
        device = deviceManager_.getCurrentAudioDevice();
        if (device != nullptr) {
            INFO("Audio device state: open=%d playing=%d name=%s", device->isOpen() ? 1 : 0,
                 device->isPlaying() ? 1 : 0, device->getName().toRawUTF8());
        } else {
            INFO("Audio device state: no current device after setup");
        }
    }

    // Switches deviceManager_'s output device to `outputName` ("" for
    // System Default; a non-empty name must already be confirmed present
    // via IsEnumeratedOutputDevice) via AudioDeviceSetup.outputDeviceName +
    // setAudioDeviceSetup(..., true), logging the setup error string if
    // non-empty and the resulting open/playing state (precedent commit
    // adf0181). `reason` is a short tag ("startup"/"selection"/"patch")
    // folded into the log line so a session log distinguishes why a switch
    // happened. A successful switch re-fires audioDeviceAboutToStart (JUCE
    // calls it synchronously from setAudioDeviceSetup when the device
    // changes), which re-Prepares the engine automatically — no separate
    // Prepare() call needed here. Also re-applies the preferred rate/block
    // size against the newly current device, the same way Start() does for
    // the platform-default device.
    //
    // "System Default" resolution note: outputName=="" is NOT passed
    // through to AudioDeviceSetup.outputDeviceName verbatim.
    // AudioDeviceManager::setAudioDeviceSetup treats an AudioDeviceSetup
    // whose inputDeviceName AND outputDeviceName are BOTH empty as "no
    // device wanted" and deletes the current device outright (see its
    // implementation) rather than falling back to the platform default —
    // and inputDeviceName is legitimately empty whenever the app requests 0
    // input channels (the miniapp's case), so passing through an empty
    // outputDeviceName here would silently kill audio instead of selecting
    // the default output. Resolve "" to the concrete default output device
    // name (getCurrentDeviceTypeObject()->getDeviceNames(false)[
    // getDefaultDeviceIndex(false)]) first, so the setup we actually apply
    // always names a real device.
    void SwitchOutputDevice(const juce::String& outputName, const char* reason) {
        juce::String resolvedName = outputName;
        if (resolvedName.isEmpty()) {
            if (juce::AudioIODeviceType* deviceType = deviceManager_.getCurrentDeviceTypeObject();
                deviceType != nullptr) {
                const juce::StringArray names = deviceType->getDeviceNames(false);
                const int defaultIx = deviceType->getDefaultDeviceIndex(false);
                if (defaultIx >= 0 && defaultIx < names.size()) {
                    resolvedName = names[defaultIx];
                }
            }
        }
        juce::AudioDeviceManager::AudioDeviceSetup setup = deviceManager_.getAudioDeviceSetup();
        setup.outputDeviceName = resolvedName;
        const juce::String setupError = deviceManager_.setAudioDeviceSetup(setup, true);
        if (setupError.isNotEmpty()) {
            INFO("Audio device switch (%s) FAILED: %s", reason, setupError.toRawUTF8());
        }
        juce::AudioIODevice* device = deviceManager_.getCurrentAudioDevice();
        if (device != nullptr) {
            INFO("Audio device switch (%s): open=%d playing=%d name=%s", reason, device->isOpen() ? 1 : 0,
                 device->isPlaying() ? 1 : 0, device->getName().toRawUTF8());
        } else {
            INFO("Audio device switch (%s): no current device after setup", reason);
        }
        ApplyPreferredRateAndBlockSize();
    }

    // Forwards to audioStatusHook_ (AudioConfigPage's status sink) when a
    // page is currently alive and has installed one; a no-op otherwise (e.g.
    // before any page has been shown, or while none is constructed). See
    // SetAudioStatusHook's doc comment.
    //
    // `text` is remembered as the current diagnostic detail so a later
    // count-driven republication (PublishPendingInputStatus) can re-render the
    // same line with the new counts instead of dropping what it said.
    void SetAudioStatus(const juce::String& text) {
        audioStatusDetail_ = text;
        PublishAudioStatus();
    }

    // Renders and pushes the current status line, and records the active input
    // count it rendered so PublishPendingInputStatus only fires on a change.
    // Message thread.
    //
    // With no sink installed, or with nothing to say, this records nothing:
    // publishedActiveInputChannels_ must not advance past a count no page ever
    // saw, or the next sink to appear would sit blank until the count moved
    // again.
    void PublishAudioStatus() {
        if (!audioStatusHook_) {
            return;
        }
        const int activeInputChannels = ActiveInputChannels();
        const juce::String text = ComposeAudioStatus(activeInputChannels);
        if (text.isEmpty()) {
            return;
        }
        publishedActiveInputChannels_ = activeInputChannels;
        audioStatusHook_(text);
    }

    // For an input-capable application the status line always leads with the
    // stable `Input requested N / active M` text (sru-3), with whatever device
    // or permission diagnostic is current appended after it -- a missing input
    // device, a failed setup, or a plain selection acknowledgement never
    // displaces the requested/active counts, and none of them stops output.
    // A zero-input application is unchanged: its status line is exactly the
    // diagnostic text, with no input claim attached.
    juce::String ComposeAudioStatus(int activeInputChannels) const {
        if (requestedInputChannels_ <= 0) {
            return audioStatusDetail_;
        }
        juce::String text = "Input requested " + juce::String(requestedInputChannels_) + " / active " +
                            juce::String(activeInputChannels);
        if (audioStatusDetail_.isNotEmpty()) {
            text += " - " + audioStatusDetail_;
        }
        return text;
    }

    // Forwards to audioSyncHook_ (AudioConfigPage's re-sync hook), same
    // no-op-when-absent contract as SetAudioStatus above. See
    // SetAudioSyncHook's doc comment.
    void SyncAudioSelection() {
        if (audioSyncHook_) {
            audioSyncHook_();
        }
    }

    // engine_.SetAudioDeviceChangedCallback's target. Patch load/revert is
    // parameter-only and never reaches this path; it is reserved for
    // runtime-config-initiated audio changes that are sourced from
    // engine.AudioDeviceSnapshot() instead of a combo pick. The callback
    // tolerates running before a current device exists, which keeps startup
    // ordering flexible for persisted configuration loading.
    void OnEngineAudioDeviceChanged() {
        const synth::AudioDeviceState state = engine_.AudioDeviceSnapshot();
        const juce::String outputName = juce::String(state.outputDeviceName);
        if (outputName.isEmpty()) {
            if (deviceManager_.getCurrentAudioDevice() != nullptr) {
                SwitchOutputDevice(outputName, "engine");
            }
            SetAudioStatus("Audio: System Default");
        } else if (!IsEnumeratedOutputDevice(outputName)) {
            // Pre-device-open case (see this method's doc comment) also
            // lands here (no device type yet -> "not enumerated"), which is
            // correct: nothing to log as missing yet, Start()'s device-open
            // step will find it. Distinguish the two by whether a device
            // manager is already up.
            if (deviceManager_.getCurrentAudioDevice() != nullptr) {
                const juce::String message = "audio device not found: " + outputName;
                INFO("%s", message.toRawUTF8());
                SetAudioStatus(message);
            }
        } else {
            SwitchOutputDevice(outputName, "engine");
            SetAudioStatus("Audio: " + outputName);
        }

        // Input-side counterpart (Task 3 review round 2, Minor): the old
        // implementation only ever applied outputDeviceName here, so a patch
        // that changed just the input device would sync the combo's display
        // (SyncAudioSelection() below reads engine_.AudioDeviceSnapshot()
        // directly) without ever actually switching the input device on
        // deviceManager_. Apply it the same way ApplyAudioDeviceInputSelection
        // does, via ApplyInputDeviceSetup, still tolerant of the
        // pre-device-open case (see this method's doc comment): an empty
        // deviceManager_ device type makes IsEnumeratedInputDevice return
        // false unconditionally, so this is a no-op until Start()'s own
        // device-open step runs. The persisted name is passed straight
        // through, empty included -- an empty inputName is a deliberate "no
        // input device" choice, not a value to resolve away.
        const juce::String inputName = juce::String(state.inputDeviceName);
        if (inputName.isEmpty() || IsEnumeratedInputDevice(inputName)) {
            if (deviceManager_.getCurrentAudioDevice() != nullptr) {
                const juce::AudioDeviceManager::AudioDeviceSetup currentSetup = deviceManager_.getAudioDeviceSetup();
                if (currentSetup.inputDeviceName != inputName) {
                    if (ApplyInputDeviceSetup(inputName, "patch")) {
                        ApplyPreferredRateAndBlockSize();
                    }
                }
            }
        } else if (deviceManager_.getCurrentAudioDevice() != nullptr) {
            const juce::String message = "audio input device not found: " + inputName;
            INFO("%s", message.toRawUTF8());
            SetAudioStatus(message);
        }

        SyncAudioSelection();
        RefreshInputRoutedState();
    }

    // Timer tick order (binding): engine message-thread tick -> MIDI
    // connection poll-driven reconcile -> repaint hook -> DoLog() last.
    // midiConnections_->OnTimerTick() consumes the background poller's dirty
    // flag (if any) and, on a change, re-enumerates authoritatively on this
    // (the message) thread and runs one reconcile pass (the self-healing
    // path, binding per p3-globals.md's message-thread-executor paragraph).
    // Panel repaint after a poll-driven reconcile is intentionally NOT
    // wired here beyond the repaint hook -- Refresh() re-reads
    // engine.InstrumentSnapshot()/midiConnections_ state cheaply enough that
    // the repaint hook's own periodic UI refresh (if the host wires one) is
    // sufficient; explicit reconcile-triggered repaint can be added if a
    // host needs tighter status-label latency.
    void timerCallback() override {
        engine_.MessageThreadTick();
        midiConnections_->OnTimerTick();
        // Before the repaint hook, so a page refreshed by this same tick shows
        // the input diagnostic this tick published.
        PublishPendingInputStatus();
        if (repaintHook_) {
            repaintHook_();
        }
        synth::AsyncLogQueue::s_instance.DoLog();
    }

    std::uint64_t NowMicros() const {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startTime_)
                .count());
    }

    void LogPatchCommand(const char* action, const synth::PatchCommandResult& result) {
        INFO("%s status=%s requestId=%llu", action, synth::PatchCommandStatusName(result.status),
             static_cast<unsigned long long>(result.requestId));
    }

    // Declared before engine_ so it is initialized first: engine_'s
    // TimestampProvider lambda captures `this` and reads startTime_ on
    // every call, so startTime_ must already hold a valid value by the time
    // the engine can possibly invoke it (audio never starts before
    // Start() completes, well after construction).
    std::chrono::steady_clock::time_point startTime_;
    synth_juce::RuntimeMidiEpoch midiEpoch_;
    juce::AudioDeviceManager deviceManager_;

    // Shutdown ordering (binding, review fix on sar-33): declared before
    // engine_ so it is destroyed AFTER engine_ -- member destruction runs in
    // the reverse of declaration order, so a member declared earlier outlives
    // one declared later. engine_.Context().inputRoutingSignal (wired in the
    // constructor, above) is a raw pointer into this object, and the engine
    // -- including its own teardown and the App inside it -- may still read
    // Context().inputRoutingSignal while engine_ itself is being destroyed.
    // Declaring inputRoutingSignal_ after engine_ would destroy it first,
    // leaving that pointer dangling for the whole remainder of engine_'s
    // teardown. Mirrors BrowserRuntime.hpp, which declares
    // inputRoutingSignal_ before engine_ for the same reason. See
    // RefreshInputRoutedState for the JUCE derivation and every call site
    // that keeps it current.
    synth::InputRoutingSignal inputRoutingSignal_;

    synth::Engine<App> engine_;
    synth::RuntimeDataPaths dataPaths_;
    std::optional<synth::RuntimeDataPaths> dataPathsOverride_;

    // Owns every controller slot's MIDI device handlers, connection state,
    // and background poller (Task 2 of Plan 3) -- see MidiConnectionManager
    // .hpp's class doc comment. A unique_ptr because it must be constructed
    // after engine_ (it holds a reference to it) and destroyed before
    // engine_ is torn down.
    std::unique_ptr<MidiConnectionManager<App>> midiConnections_;

    // ControllersPage's rebuild-notification hook (Task 4 review, Critical
    // finding 1), installed via SetMidiProcessorsRebuiltHook() -- see that
    // method's doc comment. Invoked at the end of every MIDI-processor
    // rebuild, AFTER midiConnections_->OnInstrumentRebuilt() has already
    // reopened/reconciled every slot's connections; empty (a no-op) until
    // MainPane wires it, and cleared the same way the audio hooks are when
    // the owning page is torn down.
    std::function<void()> onMidiProcessorsRebuilt_;

    // Shell hook: set later by whatever owns the UI, invoked at the end of
    // every timer tick so the app's component(s) can repaint.
    std::function<void()> repaintHook_;

    // AudioConfigPage's status/re-sync hooks (Task 3 of Plan 4) -- see
    // SetAudioStatusHook()/SetAudioSyncHook()'s doc comments. Empty
    // (default-constructed std::function) whenever no page has installed
    // one, which SetAudioStatus()/SyncAudioSelection() tolerate as a no-op.
    std::function<void(const juce::String&)> audioStatusHook_;
    std::function<void()> audioSyncHook_;

    // The application's validated input request (sar-31). Written once, by
    // Start(), before any audio callback can be registered; read from the
    // audio thread thereafter, which is why it needs no synchronization of
    // its own.
    int requestedInputChannels_ = 0;

    // The host-observed active input count, already clamped into
    // [0, requestedInputChannels_]. Written by the audio callback and by the
    // device start/stop callbacks, read by the message thread's status
    // publication -- the whole audio-thread side of the input diagnostic.
    std::atomic<int> activeInputChannels_{0};

    // Message thread only. The active count the status line currently shows
    // (-1 before anything has been published), and the diagnostic detail
    // ComposeAudioStatus appends to it. See PublishPendingInputStatus.
    int publishedActiveInputChannels_ = -1;
    juce::String audioStatusDetail_;
};

}  // namespace synth_runtime
