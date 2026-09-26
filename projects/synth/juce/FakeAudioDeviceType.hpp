#pragma once

// synth_juce::FakeAudioDeviceType — a test-only juce::AudioIODeviceType whose
// devices are entirely synthetic, so JUCE runtime tests can exercise audio
// device negotiation (input/output channel counts, device switching, callback
// shape, open failure) without touching developer hardware and without a
// running message loop.
//
// Hermetic-by-construction: juce::AudioDeviceManager only creates the real
// platform device types when its type list is still empty
// (createDeviceTypesIfNeeded), so a test that calls
// deviceManager.addAudioDeviceType(std::make_unique<FakeAudioDeviceType>(...))
// BEFORE Runtime::Start() leaves CoreAudio out of the list entirely and the
// manager picks this type as current (pickCurrentDeviceTypeWithDevices).
//
// Each device remembers the selected input and output device names
// independently, so a duplex pair reports the right index in each direction
// (getIndexOfDevice) and a lifecycle entry names both halves of the pair --
// which is what lets a test tell an input-only switch apart from an
// output-only one.
//
// The test drives audio by hand: FakeAudioDevice::start() records the callback
// the device manager installs, and RunBlock() invokes it with whatever channel
// pointers and frame count the scenario needs, at the channel counts the
// device itself negotiated at open() — including a null pointer inside the
// counted prefix, which is the one shape no real driver callback lets a test
// produce on demand. A channel count that differs from what open() negotiated
// is not something RunBlock can express: real hardware reports its shape at
// open, never through a callback that quietly grows or shrinks it.
//
// FailOpenForInputDevice() makes any device selected with that input name fail
// to open, the way a device that is enumerated but unavailable (already in use,
// microphone permission denied) does. JUCE deletes the current device when an
// open fails, so this is how a test reaches the host's recovery path.
//
// Lifecycle() returns the ordered open/open-failed/start/stop/close/destroy
// trace across every device this type has created, so tests can assert the
// stop/reprepare/restart ordering a device switch is required to follow.

#include <juce_audio_devices/juce_audio_devices.h>

#include <string>
#include <utility>
#include <vector>

namespace synth_juce {

class FakeAudioDeviceType;

class FakeAudioDevice final : public juce::AudioIODevice {
public:
    FakeAudioDevice(FakeAudioDeviceType& owner,
                    juce::String outputDeviceName,
                    juce::String inputDeviceName,
                    int maxInputChannels,
                    int maxOutputChannels);

    ~FakeAudioDevice() override;

    juce::StringArray getOutputChannelNames() override { return ChannelNames("Out", maxOutputChannels_); }
    juce::StringArray getInputChannelNames() override { return ChannelNames("In", maxInputChannels_); }
    juce::Array<double> getAvailableSampleRates() override { return {44100.0, 48000.0}; }
    juce::Array<int> getAvailableBufferSizes() override { return {128, 256, 512}; }
    int getDefaultBufferSize() override { return 256; }

    juce::String open(const juce::BigInteger& inputChannels,
                      const juce::BigInteger& outputChannels,
                      double sampleRate,
                      int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override { return open_; }

    void start(juce::AudioIODeviceCallback* callback) override;
    void stop() override;
    bool isPlaying() override { return callback_ != nullptr; }

    juce::String getLastError() override { return lastError_; }
    int getCurrentBufferSizeSamples() override { return bufferSize_; }
    double getCurrentSampleRate() override { return sampleRate_; }
    int getCurrentBitDepth() override { return 32; }
    juce::BigInteger getActiveOutputChannels() const override { return activeOutputChannels_; }
    juce::BigInteger getActiveInputChannels() const override { return activeInputChannels_; }
    int getOutputLatencyInSamples() override { return 0; }
    int getInputLatencyInSamples() override { return 0; }

    // The device names this device was created for, each empty when that
    // direction was not selected. Kept separately from getName() (which JUCE
    // treats as one name for the whole device) so both directions resolve.
    const juce::String& SelectedOutputName() const { return outputDeviceName_; }
    const juce::String& SelectedInputName() const { return inputDeviceName_; }

    // "<output>+<input>", with "(none)" for an unselected direction. The tag
    // every lifecycle entry carries.
    juce::String Label() const {
        return (outputDeviceName_.isEmpty() ? juce::String("(none)") : outputDeviceName_) + "+" +
               (inputDeviceName_.isEmpty() ? juce::String("(none)") : inputDeviceName_);
    }

    // Delivers one device block to whatever callback the device manager
    // installed, exactly as a driver thread would: the channel counts it
    // reports are always this device's own negotiated active counts (from
    // getActiveInputChannels()/getActiveOutputChannels()), never a count the
    // caller supplies. `inputs` may still contain null entries inside that
    // counted range.
    void RunBlock(const float* const* inputs, float* const* outputs, int numSamples);

private:
    static juce::StringArray ChannelNames(const juce::String& prefix, int count) {
        juce::StringArray names;
        for (int index = 0; index < count; ++index) {
            names.add(prefix + " " + juce::String(index + 1));
        }
        return names;
    }

    // Drops every bit at or above `limit`, so a request for more channels than
    // this device has resolves to the channels it actually provides.
    static juce::BigInteger ClampChannels(const juce::BigInteger& requested, int limit);

    FakeAudioDeviceType& owner_;
    juce::String outputDeviceName_;
    juce::String inputDeviceName_;
    int maxInputChannels_ = 0;
    int maxOutputChannels_ = 0;
    juce::BigInteger activeInputChannels_;
    juce::BigInteger activeOutputChannels_;
    double sampleRate_ = 48000.0;
    int bufferSize_ = 256;
    bool open_ = false;
    juce::String lastError_;
    juce::AudioIODeviceCallback* callback_ = nullptr;
};

class FakeAudioDeviceType final : public juce::AudioIODeviceType {
public:
    static constexpr const char* kTypeName = "Fake";

    FakeAudioDeviceType(juce::StringArray inputDeviceNames,
                        juce::StringArray outputDeviceNames,
                        int maxInputChannels,
                        int maxOutputChannels)
        : juce::AudioIODeviceType(kTypeName)
        , inputDeviceNames_(std::move(inputDeviceNames))
        , outputDeviceNames_(std::move(outputDeviceNames))
        , maxInputChannels_(maxInputChannels)
        , maxOutputChannels_(maxOutputChannels) {}

    void scanForDevices() override {}

    juce::StringArray getDeviceNames(bool wantInputNames = false) const override {
        return wantInputNames ? inputDeviceNames_ : outputDeviceNames_;
    }

    int getDefaultDeviceIndex(bool) const override { return 0; }

    int getIndexOfDevice(juce::AudioIODevice* device, bool asInput) const override {
        auto* fake = dynamic_cast<FakeAudioDevice*>(device);
        if (fake == nullptr) {
            return -1;
        }
        return getDeviceNames(asInput).indexOf(asInput ? fake->SelectedInputName()
                                                       : fake->SelectedOutputName());
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    juce::AudioIODevice* createDevice(const juce::String& outputDeviceName,
                                      const juce::String& inputDeviceName) override {
        const int inputs = inputDeviceName.isNotEmpty() ? maxInputChannels_ : 0;
        const int outputs = outputDeviceName.isNotEmpty() ? maxOutputChannels_ : 0;
        auto* device = new FakeAudioDevice(*this, outputDeviceName, inputDeviceName, inputs, outputs);
        current_ = device;
        return device;
    }

    // The device the manager currently holds open, or null before the first
    // open. Devices the manager creates only to probe sample rates are
    // destroyed immediately and clear themselves from here as they go.
    FakeAudioDevice* CurrentDevice() const { return current_; }

    // Ordered lifecycle trace across every device this type has created, each
    // entry tagged with FakeAudioDevice::Label().
    const std::vector<std::string>& Lifecycle() const { return lifecycle_; }
    void ClearLifecycle() { lifecycle_.clear(); }

    void RecordLifecycle(const juce::String& label, const char* event) {
        lifecycle_.push_back(label.toStdString() + ":" + event);
    }

    // Any device selected with this input device name fails to open, with
    // OpenFailureMessage(name) as the returned error.
    void FailOpenForInputDevice(const juce::String& inputDeviceName) {
        failingInputDeviceNames_.add(inputDeviceName);
    }

    bool OpenFailsForInputDevice(const juce::String& inputDeviceName) const {
        return inputDeviceName.isNotEmpty() && failingInputDeviceNames_.contains(inputDeviceName);
    }

    static juce::String OpenFailureMessage(const juce::String& inputDeviceName) {
        return "Fake input device open failed: " + inputDeviceName;
    }

    void OnDeviceDestroyed(FakeAudioDevice* device) {
        if (current_ == device) {
            current_ = nullptr;
        }
    }

private:
    juce::StringArray inputDeviceNames_;
    juce::StringArray outputDeviceNames_;
    int maxInputChannels_ = 0;
    int maxOutputChannels_ = 0;
    juce::StringArray failingInputDeviceNames_;
    FakeAudioDevice* current_ = nullptr;
    std::vector<std::string> lifecycle_;
};

inline FakeAudioDevice::FakeAudioDevice(FakeAudioDeviceType& owner,
                                        juce::String outputDeviceName,
                                        juce::String inputDeviceName,
                                        int maxInputChannels,
                                        int maxOutputChannels)
    : juce::AudioIODevice(outputDeviceName.isNotEmpty() ? outputDeviceName : inputDeviceName,
                          FakeAudioDeviceType::kTypeName)
    , owner_(owner)
    , outputDeviceName_(std::move(outputDeviceName))
    , inputDeviceName_(std::move(inputDeviceName))
    , maxInputChannels_(maxInputChannels)
    , maxOutputChannels_(maxOutputChannels) {}

inline FakeAudioDevice::~FakeAudioDevice() {
    owner_.RecordLifecycle(Label(), "destroy");
    owner_.OnDeviceDestroyed(this);
}

inline juce::BigInteger FakeAudioDevice::ClampChannels(const juce::BigInteger& requested, int limit) {
    juce::BigInteger clamped = requested;
    const int highest = clamped.getHighestBit();
    if (highest >= limit) {
        clamped.setRange(limit, highest + 1 - limit, false);
    }
    return clamped;
}

inline juce::String FakeAudioDevice::open(const juce::BigInteger& inputChannels,
                                          const juce::BigInteger& outputChannels,
                                          double sampleRate,
                                          int bufferSizeSamples) {
    if (owner_.OpenFailsForInputDevice(inputDeviceName_)) {
        lastError_ = FakeAudioDeviceType::OpenFailureMessage(inputDeviceName_);
        owner_.RecordLifecycle(Label(), "open-failed");
        return lastError_;
    }
    lastError_.clear();
    activeInputChannels_ = ClampChannels(inputChannels, maxInputChannels_);
    activeOutputChannels_ = ClampChannels(outputChannels, maxOutputChannels_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    bufferSize_ = bufferSizeSamples > 0 ? bufferSizeSamples : getDefaultBufferSize();
    open_ = true;
    owner_.RecordLifecycle(Label(), "open");
    return {};
}

inline void FakeAudioDevice::close() {
    stop();
    if (!open_) {
        return;
    }
    open_ = false;
    owner_.RecordLifecycle(Label(), "close");
}

inline void FakeAudioDevice::start(juce::AudioIODeviceCallback* callback) {
    if (callback == nullptr) {
        return;
    }
    callback_ = callback;
    owner_.RecordLifecycle(Label(), "start");
    callback_->audioDeviceAboutToStart(this);
}

inline void FakeAudioDevice::stop() {
    juce::AudioIODeviceCallback* callback = callback_;
    if (callback == nullptr) {
        return;
    }
    callback_ = nullptr;
    owner_.RecordLifecycle(Label(), "stop");
    callback->audioDeviceStopped();
}

inline void FakeAudioDevice::RunBlock(const float* const* inputs, float* const* outputs, int numSamples) {
    if (callback_ == nullptr) {
        return;
    }
    callback_->audioDeviceIOCallbackWithContext(inputs, activeInputChannels_.countNumberOfSetBits(), outputs,
                                                activeOutputChannels_.countNumberOfSetBits(), numSamples, {});
}

}  // namespace synth_juce
