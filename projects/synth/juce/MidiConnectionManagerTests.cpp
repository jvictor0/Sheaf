#include "MidiConnectionManager.hpp"

#include "../apps/miniapp/MiniApp.hpp"

#include "synth/MidiController.hpp"
#include "synth/MidiReconcile.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
}

// Fakes for synth_runtime::MidiInputEndpoint/MidiOutputEndpoint: they record
// every Open() call and, for the output, every Send(), instead of touching a
// real MIDI device.
class FakeInputEndpoint final : public synth_runtime::MidiInputEndpoint {
public:
    bool Open(const juce::String& identifier) override {
        opens.push_back(identifier.toStdString());
        open_ = true;
        return true;
    }
    void Close() override { open_ = false; }
    void SetProcessor(std::unique_ptr<synth::MidiInProcessor>) override {}

    std::vector<std::string> opens;

private:
    bool open_ = false;
};

class FakeOutputEndpoint final : public synth_runtime::MidiOutputEndpoint {
public:
    bool Open(const juce::String& identifier) override {
        opens.push_back(identifier.toStdString());
        open_ = true;
        return true;
    }
    void Close() override { open_ = false; }
    void Send(const synth::BasicMidi& midi) override { sent.push_back(midi); }

    std::vector<std::string> opens;
    std::vector<synth::BasicMidi> sent;

private:
    bool open_ = false;
};

// Names a single fake input/output pair and lets a test flip whether it is
// present in the enumerated device list, without touching anything real.
class FakeDeviceAccess {
public:
    synth::MidiDeviceList Enumerate() const {
        synth::MidiDeviceList list;
        if (present_) {
            list.inputs.push_back({inputIdentifier, inputName});
            list.outputs.push_back({outputIdentifier, outputName});
        }
        return list;
    }

    void SetPresent(bool present) { present_ = present; }

    synth_runtime::MidiDeviceAccess Access() {
        synth_runtime::MidiDeviceAccess access;
        access.enumerate = [this] { return Enumerate(); };
        access.makeInput = [this](synth_juce::RuntimeMidiEpoch) -> std::unique_ptr<synth_runtime::MidiInputEndpoint> {
            auto endpoint = std::make_unique<FakeInputEndpoint>();
            lastInput = endpoint.get();
            return endpoint;
        };
        access.makeOutput =
            [this](synth_juce::RuntimeMidiEpoch) -> std::unique_ptr<synth_runtime::MidiOutputEndpoint> {
            auto endpoint = std::make_unique<FakeOutputEndpoint>();
            lastOutput = endpoint.get();
            return endpoint;
        };
        access.pollInterval = std::chrono::milliseconds(5);
        return access;
    }

    const std::string inputIdentifier = "fake-controller-in";
    const std::string inputName = "Fake Controller In";
    const std::string outputIdentifier = "fake-controller-out";
    const std::string outputName = "Fake Controller Out";

    FakeInputEndpoint* lastInput = nullptr;
    FakeOutputEndpoint* lastOutput = nullptr;

private:
    bool present_ = true;
};

// A manager given a fake device access builds its endpoints through that
// access alone: it never reaches a real MIDI device, so this test sees and
// drives only the fakes regardless of what is plugged into the machine.
void CheckConnectionManagerOpensTheInjectedEndpoints() {
    FakeDeviceAccess access;

    synth::Engine<synth_miniapp::MiniApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    Require(engine.MidiControllerCount() == 1, "MiniApp's default instrument has one controller");

    engine.EditInstrument([&access](synth::MidiInstrumentConfig& instrument) {
        instrument.controllers[0].input = synth::MidiEndpointRef{access.inputIdentifier, access.inputName};
        instrument.controllers[0].output = synth::MidiEndpointRef{access.outputIdentifier, access.outputName};
    });

    synth_runtime::MidiConnectionManager<synth_miniapp::MiniApp> manager(
        engine, synth_juce::RuntimeMidiEpoch{}, access.Access());
    engine.SetMidiProcessorsWillRebuildCallback([&manager] { manager.OnMidiProcessorsWillRebuild(); });
    engine.SetMidiProcessorsRebuiltCallback([&manager] { manager.OnInstrumentRebuilt(); });

    manager.StartupReconcile();

    Require(access.lastInput != nullptr && access.lastOutput != nullptr,
            "the manager built its endpoints through the injected access");
    Require(access.lastInput->opens == std::vector<std::string>{access.inputIdentifier},
            "the fake input reports one Open with the pair's identifier");
    Require(access.lastOutput->opens == std::vector<std::string>{access.outputIdentifier},
            "the fake output reports one Open with the pair's identifier");

    synth::MidiSender* const sender = engine.Context().midiSender;
    Require(sender != nullptr, "the engine exposes its MidiSender");
    sender->Start();
    Require(sender->Enqueue(0, synth::BasicMidi::CC(0, 1, 0, 64)), "a probe message enqueues on sink 0");
    Require(sender->FlushForTests(std::chrono::milliseconds(500)), "the sender drains the probe message");
    sender->Stop();
    Require(access.lastOutput->sent.size() == 1, "the engine's MidiSender sink 0 is the fake output");

    access.SetPresent(false);
    bool bothOffline = false;
    for (int attempt = 0; attempt < 100 && !bothOffline; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        manager.OnTimerTick();
        const synth::MidiConnectionState& state = manager.State();
        bothOffline = !state.controllers.empty() &&
                     state.controllers[0].input.status == synth::MidiEndpointStatus::Offline &&
                     state.controllers[0].output.status == synth::MidiEndpointStatus::Offline;
    }
    Require(bothOffline,
            "removing the pair from the list and consuming the poller's change marks both offline");
}

}  // namespace

int main() {
    CheckConnectionManagerOpensTheInjectedEndpoints();
    std::cout << "MidiConnectionManagerTests passed\n";
    return 0;
}
