#pragma once

// synth_runtime::EngineMidiConnections — the one construct a JUCE-facing host
// wires to give synth::Engine<App> its per-controller MIDI connections
// (MidiConnectionManager<App>). Runtime<App> (the standalone/browser launcher
// shell, Runtime.hpp) and a plugin host both need this exact sequence, and
// both need it in this exact order (binding):
//   - constructor: wires engine.SetMidiProcessorsWillRebuildCallback to
//     manager_->OnMidiProcessorsWillRebuild() (detaching every controller's
//     forwarding processor before the engine destroys the current MIDI
//     processor chain) and the rebuilt callback to
//     manager_->OnInstrumentRebuilt() then, if installed, the host's own
//     rebuilt hook (SetMidiProcessorsRebuiltHook, below) -- both wired here,
//     before the host ever calls engine.Initialize(), which is required by
//     MidiConnectionManager's own forwarding-processor-swap contract
//     (MidiConnectionManager.hpp's class doc comment).
//   - Start(): call once, after engine.Initialize(): starts the engine's
//     MidiSender, THEN runs manager_->StartupReconcile() (resizes to the
//     current controller count, reconciles every configured ref against
//     actually-present devices, and starts the background poller) -- so
//     every producer StartupReconcile() is about to create already has a
//     live consumer.
//   - OnTimerTick(): wire to the host's own message-thread timer, AFTER the
//     engine's own message-thread tick has run (that tick drains any
//     pending instrument-rebuild and runs OnInstrumentRebuilt()'s own
//     reconcile pass synchronously, so a device-list change is what
//     OnTimerTick() alone can still catch this pump) -- the self-healing
//     poll-driven reconcile path.
//   - destructor: stops the MidiSender before destroying the manager, so no
//     in-flight enqueued MIDI is delivered to a sink the manager's own
//     destructor is about to close, THEN destroys the manager (which itself
//     stops/joins its poller before closing any device handler).
// engine.Context().midiSender is always set, by Engine's own constructor
// (Engine.hpp), for every App -- there is no caller-reachable path to a null
// sender here, so this class reaches it directly rather than guarding a case
// that cannot occur.
#include "synth/Engine.hpp"

#include "MidiConnectionManager.hpp"

#include <functional>
#include <memory>
#include <utility>

namespace synth_runtime {

template <synth::SynthApplication App>
class EngineMidiConnections {
public:
    explicit EngineMidiConnections(synth::Engine<App>& engine, synth_juce::RuntimeMidiEpoch midiEpoch = {},
                                   MidiDeviceAccess access = {})
        : engine_(engine)
        , manager_(std::make_unique<MidiConnectionManager<App>>(engine, midiEpoch, std::move(access))) {
        engine_.SetMidiProcessorsWillRebuildCallback([this] { manager_->OnMidiProcessorsWillRebuild(); });
        engine_.SetMidiProcessorsRebuiltCallback([this] {
            manager_->OnInstrumentRebuilt();
            if (rebuiltHook_) {
                rebuiltHook_();
            }
        });
    }

    ~EngineMidiConnections() {
        engine_.Context().midiSender->Stop();
        manager_.reset();
    }

    EngineMidiConnections(const EngineMidiConnections&) = delete;
    EngineMidiConnections& operator=(const EngineMidiConnections&) = delete;

    // Startup order (binding): call once, after engine.Initialize(). See
    // this file's header comment for the full ordering rationale.
    void Start() {
        engine_.Context().midiSender->Start();
        manager_->StartupReconcile();
    }

    // Wire to the host's own message-thread timer, after the engine's own
    // message-thread tick. See this file's header comment.
    void OnTimerTick() { manager_->OnTimerTick(); }

    MidiConnectionManager<App>& MidiConnections() { return *manager_; }

    // Installs the host's rebuild-notification hook -- invoked at the end
    // of every MIDI-processor rebuild, right after manager_->OnInstrumentRebuilt()
    // has already reopened/reconciled every slot's connections. See
    // Runtime<App>::SetMidiProcessorsRebuiltHook's own doc comment for the
    // Controllers-page dirty-tracking rationale this hook exists for.
    void SetMidiProcessorsRebuiltHook(std::function<void()> hook) { rebuiltHook_ = std::move(hook); }

private:
    synth::Engine<App>& engine_;
    std::unique_ptr<MidiConnectionManager<App>> manager_;
    std::function<void()> rebuiltHook_;
};

}  // namespace synth_runtime
