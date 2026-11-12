#pragma once

#include "synth/MidiController.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace synth_juce {

// One runtime epoch shared by the engine clock and JUCE MIDI adapters. JUCE's
// MIDI scheduler/callback timestamps use the high-resolution millisecond
// clock, while the engine uses microseconds since Runtime construction.
struct RuntimeMidiEpoch {
    double juceMillisecondsAtEngineEpochZero = 0.0;

    static RuntimeMidiEpoch Capture(std::chrono::steady_clock::time_point engineEpoch) noexcept {
        const auto steadyNow = std::chrono::steady_clock::now();
        const double elapsedMilliseconds =
            std::chrono::duration<double, std::milli>(steadyNow - engineEpoch).count();
        return {.juceMillisecondsAtEngineEpochZero =
                    juce::Time::getMillisecondCounterHiRes() - elapsedMilliseconds};
    }

    double ToJuceMilliseconds(std::uint64_t engineMicros) const noexcept {
        return juceMillisecondsAtEngineEpochZero + static_cast<double>(engineMicros) / 1000.0;
    }

    std::uint64_t ToEngineMicros(double juceTimestampSeconds) const noexcept {
        const double elapsedMilliseconds =
            juceTimestampSeconds * 1000.0 - juceMillisecondsAtEngineEpochZero;
        return elapsedMilliseconds <= 0.0
            ? 0
            : static_cast<std::uint64_t>(std::llround(elapsedMilliseconds * 1000.0));
    }
};

struct JuceScheduledMidiSubmission {
    juce::MidiBuffer buffer;
    double hostDueTimeMilliseconds = 0.0;
};

inline JuceScheduledMidiSubmission PrepareScheduledMidiSubmission(
    const synth::BasicMidi& midi,
    std::uint64_t dueTimeMicros,
    const RuntimeMidiEpoch& epoch) {
    JuceScheduledMidiSubmission submission;
    submission.hostDueTimeMilliseconds = epoch.ToJuceMilliseconds(dueTimeMicros);
    if (!midi.raw.empty()) {
        submission.buffer.addEvent(
            juce::MidiMessage(midi.raw.data(), static_cast<int>(midi.raw.size())), 0);
    }
    return submission;
}

}  // namespace synth_juce

namespace synth_runtime {

// Injectable seam over the concrete JUCE device handlers below.
// MidiConnectionManager<App> holds every controller slot's devices through
// these two interfaces (see runtime/MidiConnectionManager.hpp's
// MidiDeviceAccess), so a test can substitute fakes for Open/Close/Send
// without touching real hardware; synth_juce::MidiInHandler and
// synth_juce::MidiOutputHandler below are what the manager builds by default.
class MidiInputEndpoint {
public:
    virtual ~MidiInputEndpoint() = default;
    virtual bool Open(const juce::String& identifier) = 0;
    virtual void Close() = 0;
    virtual void SetProcessor(std::unique_ptr<synth::MidiInProcessor> processor) = 0;
};

class MidiOutputEndpoint : public synth::IMidiOutputSink {
public:
    virtual bool Open(const juce::String& identifier) = 0;
    virtual void Close() = 0;
};

}  // namespace synth_runtime

namespace synth_juce {

class MidiInHandler final : private juce::MidiInputCallback, public synth_runtime::MidiInputEndpoint {
public:
    explicit MidiInHandler(RuntimeMidiEpoch epoch = {}) : epoch_(epoch) {}
    explicit MidiInHandler(std::unique_ptr<synth::MidiInProcessor> processor,
                           RuntimeMidiEpoch epoch = {})
        : epoch_(epoch), processor_(std::move(processor)) {}
    ~MidiInHandler() override { Close(); }

    MidiInHandler(const MidiInHandler&) = delete;
    MidiInHandler& operator=(const MidiInHandler&) = delete;

    static juce::Array<juce::MidiDeviceInfo> AvailableDevices() {
        return juce::MidiInput::getAvailableDevices();
    }

    void SetProcessor(std::unique_ptr<synth::MidiInProcessor> processor) override {
        std::lock_guard lock(processorMutex_);
        processor_ = std::move(processor);
    }

    synth::MidiInProcessor* Processor() const {
        std::lock_guard lock(processorMutex_);
        return processor_.get();
    }

    bool Open(const juce::String& identifier) override {
        Close();
        lastError_.clear();
        const auto devices = juce::MidiInput::getAvailableDevices();
        juce::MidiDeviceInfo selected;
        bool found = false;
        for (const auto& device : devices) {
            if (device.identifier == identifier) {
                selected = device;
                found = true;
                break;
            }
        }
        if (!found) {
            lastError_ = "Input device not found";
            return false;
        }

        auto input = juce::MidiInput::openDevice(identifier, this);
        if (input == nullptr) {
            lastError_ = "Input open failed";
            return false;
        }

        deviceIdentifier_ = selected.identifier;
        deviceName_ = selected.name;
        input_ = std::move(input);
        input_->start();
        return true;
    }

    void Close() override {
        if (input_ != nullptr) {
            input_->stop();
            input_.reset();
        }
        deviceIdentifier_.clear();
        deviceName_.clear();
    }

    bool IsOpen() const { return input_ != nullptr; }
    const juce::String& DeviceIdentifier() const { return deviceIdentifier_; }
    const juce::String& DeviceName() const { return deviceName_; }
    const juce::String& LastError() const { return lastError_; }

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message) override {
        const auto midi = Convert(message);
        if (!midi.has_value()) {
            return;
        }
        std::lock_guard lock(processorMutex_);
        if (processor_ != nullptr) {
            processor_->Process(*midi);
        }
    }

    std::optional<synth::BasicMidi> Convert(const juce::MidiMessage& message) const {
        const auto* data = message.getRawData();
        const int size = message.getRawDataSize();
        const double timestampSeconds = std::max(0.0, message.getTimeStamp());
        const std::uint64_t timestamp = epoch_.ToEngineMicros(timestampSeconds);
        if (size == 3) {
            return synth::BasicMidi(timestamp, data[0], data[1], data[2]);
        }
        if (size == 1 && synth::BasicMidi::IsSupportedRealtimeStatus(data[0])) {
            return synth::BasicMidi::Realtime(timestamp, data[0]);
        }
        if (size > 1 && data[0] == 0xF0) {
            return synth::BasicMidi::SysEx(timestamp, std::vector<std::uint8_t>(data, data + size));
        }
        return std::nullopt;
    }

    RuntimeMidiEpoch epoch_;
    mutable std::mutex processorMutex_;
    std::unique_ptr<synth::MidiInProcessor> processor_;
    std::unique_ptr<juce::MidiInput> input_;
    juce::String deviceIdentifier_;
    juce::String deviceName_;
    juce::String lastError_;
};

class MidiOutputHandler final : public synth_runtime::MidiOutputEndpoint {
public:
    explicit MidiOutputHandler(RuntimeMidiEpoch epoch = {}) : epoch_(epoch) {}
    ~MidiOutputHandler() override { Close(); }

    MidiOutputHandler(const MidiOutputHandler&) = delete;
    MidiOutputHandler& operator=(const MidiOutputHandler&) = delete;

    static juce::Array<juce::MidiDeviceInfo> AvailableDevices() {
        return juce::MidiOutput::getAvailableDevices();
    }

    bool Open(const juce::String& identifier) override {
        Close();
        lastError_.clear();
        const auto devices = juce::MidiOutput::getAvailableDevices();
        juce::MidiDeviceInfo selected;
        bool found = false;
        for (const auto& device : devices) {
            if (device.identifier == identifier) {
                selected = device;
                found = true;
                break;
            }
        }
        if (!found) {
            lastError_ = "Output device not found";
            return false;
        }

        auto output = juce::MidiOutput::openDevice(identifier);
        if (output == nullptr) {
            lastError_ = "Output open failed";
            return false;
        }

        output->startBackgroundThread();
        std::lock_guard lock(mutex_);
        output_ = std::move(output);
        deviceIdentifier_ = selected.identifier;
        deviceName_ = selected.name;
        return true;
    }

    void Close() override {
        std::lock_guard lock(mutex_);
        if (output_ != nullptr) {
            output_->clearAllPendingMessages();
            output_->stopBackgroundThread();
        }
        output_.reset();
        deviceIdentifier_.clear();
        deviceName_.clear();
    }

    bool IsOpen() const {
        std::lock_guard lock(mutex_);
        return output_ != nullptr;
    }

    const juce::String& DeviceIdentifier() const { return deviceIdentifier_; }
    const juce::String& DeviceName() const { return deviceName_; }
    const juce::String& LastError() const { return lastError_; }

    synth::MidiSchedulingCapability SchedulingCapability() const noexcept override {
        return synth::MidiSchedulingCapability::HostTimestamped;
    }

    void Send(const synth::BasicMidi& midi) override {
        if (midi.raw.empty()) {
            return;
        }
        std::lock_guard lock(mutex_);
        if (output_ == nullptr) {
            return;
        }
        output_->sendMessageNow(juce::MidiMessage(midi.raw.data(), static_cast<int>(midi.raw.size())));
    }

    void SendScheduled(const synth::BasicMidi& midi, std::uint64_t dueTimeMicros) override {
        if (midi.raw.empty()) {
            return;
        }
        JuceScheduledMidiSubmission submission =
            PrepareScheduledMidiSubmission(midi, dueTimeMicros, epoch_);
        std::lock_guard lock(mutex_);
        if (output_ == nullptr) {
            return;
        }
        output_->sendBlockOfMessages(submission.buffer,
                                     submission.hostDueTimeMilliseconds,
                                     1000.0);
    }

private:
    RuntimeMidiEpoch epoch_;
    mutable std::mutex mutex_;
    std::unique_ptr<juce::MidiOutput> output_;
    juce::String deviceIdentifier_;
    juce::String deviceName_;
    juce::String lastError_;
};

} // namespace synth_juce
