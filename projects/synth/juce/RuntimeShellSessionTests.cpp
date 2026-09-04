#include "FakeAudioDeviceType.hpp"
#include "JuceRuntimeMainServices.hpp"
#include "LauncherWindow.hpp"
#include "MiniApp.hpp"
#include "MiniAppUiModel.hpp"
#include "HostDataPaths.hpp"
#include "Shell.hpp"

#include "synth/AppContext.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/MidiConfigViewModel.hpp"
#include "synth/PortableUIBuilders.hpp"
#include "synth/RuntimePages.hpp"
#include "synth/ThreadId.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
}

void CollectPortableComponents(juce::Component& parent,
                               std::vector<synth_juce::PortableComponent*>& components) {
    for (int index = 0; index < parent.getNumChildComponents(); ++index) {
        juce::Component* child = parent.getChildComponent(index);
        if (auto* portable = dynamic_cast<synth_juce::PortableComponent*>(child); portable != nullptr) {
            components.push_back(portable);
        }
        CollectPortableComponents(*child, components);
    }
}

juce::Rectangle<int> SurfaceBoundsOf(const juce::Component& surface,
                                     const juce::Component& child) {
    return surface.getLocalArea(&child, child.getLocalBounds());
}

juce::Viewport* FindViewport(juce::Component& parent) {
    for (int index = 0; index < parent.getNumChildComponents(); ++index) {
        juce::Component* child = parent.getChildComponent(index);
        if (auto* viewport = dynamic_cast<juce::Viewport*>(child); viewport != nullptr) {
            return viewport;
        }
        if (juce::Viewport* viewport = FindViewport(*child); viewport != nullptr) {
            return viewport;
        }
    }
    return nullptr;
}

bool IsEnabledEditableControl(juce::Component& component) {
    if (!component.isEnabled()) {
        return false;
    }
    if (auto* editor = dynamic_cast<juce::TextEditor*>(&component); editor != nullptr) {
        return !editor->isReadOnly();
    }
    return dynamic_cast<juce::ComboBox*>(&component) != nullptr
           || dynamic_cast<juce::ToggleButton*>(&component) != nullptr;
}

struct WideDrawSurface final : synth::ui::Surface {
    synth::ui::NodeTree BuildTree() override {
        synth::ui::Builder builder;
        builder.Root("wide.root", {0.0f, 0.0f, 900.0f, 240.0f})
            .DrawInteractive("wide.draw",
                             {0.0f, 0.0f, 900.0f, 120.0f},
                             {},
                             synth::ui::Action::Named("wide.drag"),
                             std::nullopt,
                             {});
        return builder.Build({0.0f, 0.0f, 900.0f, 240.0f});
    }

    void SetActionHandler(ActionHandler handler) override { actionHandler = std::move(handler); }
    void DispatchAction(const synth::ui::Action&) override {}

    ActionHandler actionHandler;
};

struct WideDrawApp final {
    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "WideDrawTest";
        config.numAudioInputs = 0;
        config.numAudioOutputs = 0;
        config.uiWidth = 900;
        config.uiHeight = 240;
        config.uiFrameHz = 30;
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    WideDrawSurface surface;
};

// Fix round 1, finding 1 (sprs-13, Task 8 review): a production-wiring
// fixture. Unlike WideDrawSurface above, this surface also implements
// ui::ExtentAwareSurface, so it observes whether MainPane::resized() ever
// reaches RuntimeMainComponent::SetContentExtent() when a real JUCE window
// is resized -- the exact gap the finding identified (SetContentExtent()
// existed and was unit-tested, but nothing in the shipped app called it).
struct WiringExtentAwareSurface final : synth::ui::Surface, synth::ui::ExtentAwareSurface {
    synth::ui::NodeTree BuildTree() override {
        synth::ui::Node root;
        root.id = "wiring.extent.root";
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = extent;
        return synth::ui::NodeTree{{std::move(root)}};
    }

    void SetActionHandler(ActionHandler handler) override { actionHandler = std::move(handler); }
    void DispatchAction(const synth::ui::Action&) override {}

    void SetContentExtent(synth::ui::Bounds newExtent) override {
        ++extentOffers;
        extent = newExtent;
    }

    int extentOffers = 0;
    synth::ui::Bounds extent{0.0f, 0.0f, 640.0f, 480.0f};
    ActionHandler actionHandler;
};

struct WiringExtentAwareApp final {
    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "WiringExtentAwareTest";
        config.numAudioInputs = 0;
        config.numAudioOutputs = 0;
        config.uiWidth = 640;
        config.uiHeight = 480;
        config.uiFrameHz = 30;
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    WiringExtentAwareSurface surface;
};

// --- JUCE audio input negotiation (sar-31, sar-6, sru-3) -------------------
//
// Every scenario below drives a synth_juce::FakeAudioDeviceType instead of the
// developer's real hardware (see FakeAudioDeviceType.hpp for why that is
// hermetic). A device channel count narrower than the application requested
// is staged the way real hardware reports it: by giving the fake device
// fewer channels than the app asks for, so open() negotiates the shortfall.
// The one shape no real driver callback lets a test produce on demand -- a
// null pointer inside the counted prefix -- is still staged directly through
// RunBlock().

struct EmptySurface final : synth::ui::Surface {
    synth::ui::NodeTree BuildTree() override {
        synth::ui::Builder builder;
        builder.Root("input.probe.root", {0.0f, 0.0f, 320.0f, 160.0f});
        return builder.Build({0.0f, 0.0f, 320.0f, 160.0f});
    }

    void SetActionHandler(ActionHandler) override {}
    void DispatchAction(const synth::ui::Action&) override {}
};

// The largest requested input count any scenario below uses. Fixed-size
// observation storage keeps the probe app's ProcessBlock allocation-free.
inline constexpr std::size_t kMaxProbeChannels = 32;

// The constant the probe app writes into every output sample. Output is
// determined solely by the application's own writes, so a scenario can prove
// the host neither monitors input into output nor stops output when input is
// missing.
inline constexpr float kOutputMarker = 0.125f;

// What the runtime actually delegated, recorded from inside the application's
// own ProcessBlock so assertions read the delegated contract rather than a
// JUCE-side proxy for it.
struct ObservedInput {
    int blockCount = 0;
    int prepareCount = 0;
    int numRequestedInputChannels = -1;
    int numInputChannels = -1;
    int numOutputChannels = -1;
    bool inputsNull = true;
    std::size_t numFrames = 0;
    std::size_t requestedChannelCount = 0;
    std::size_t activeChannelCount = 0;
    bool viewEmpty = true;
    std::array<bool, kMaxProbeChannels> hasActiveChannel{};
    std::array<float, kMaxProbeChannels> firstFrameOrSilence{};
    std::array<float, kMaxProbeChannels> secondFrameOrSilence{};
    std::array<float, kMaxProbeChannels> frameViewOrSilence{};
    float outputChannel0Frame0 = 0.0f;
    float outputChannel1Frame0 = 0.0f;
};

template <int RequestedInputs>
struct InputProbeApp final {
    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "RuntimeInputProbe";
        config.numAudioInputs = RequestedInputs;
        config.numAudioOutputs = 2;
        config.preferredSampleRate = 48000.0;
        config.preferredBlockSize = 256;
        config.uiWidth = 320;
        config.uiHeight = 160;
        config.uiFrameHz = 30;
        return config;
    }

    void Init(synth::AppContext*) {}

    void PrepareToPlay(double, int) { ++observed.prepareCount; }

    void ProcessBlock(synth::AudioBlock& block) {
        ++observed.blockCount;
        observed.numRequestedInputChannels = block.numRequestedInputChannels;
        observed.numInputChannels = block.numInputChannels;
        observed.numOutputChannels = block.numOutputChannels;
        observed.inputsNull = block.inputs == nullptr;
        observed.numFrames = block.numFrames;

        const synth::AudioInputView view = block.InputView();
        observed.requestedChannelCount = view.RequestedChannelCount();
        observed.activeChannelCount = view.ActiveChannelCount();
        observed.viewEmpty = view.Empty();
        observed.hasActiveChannel.fill(false);
        observed.firstFrameOrSilence.fill(0.0f);
        observed.secondFrameOrSilence.fill(0.0f);
        observed.frameViewOrSilence.fill(0.0f);
        const synth::AudioInputFrameView frame = block.numFrames > 0 ? view.Frame(0)
                                                                     : synth::AudioInputFrameView();
        for (std::size_t channel = 0;
             channel < view.RequestedChannelCount() && channel < kMaxProbeChannels; ++channel) {
            observed.hasActiveChannel[channel] = view.HasActiveChannel(channel);
            observed.firstFrameOrSilence[channel] = view.SampleOrSilence(channel, 0);
            observed.secondFrameOrSilence[channel] = view.SampleOrSilence(channel, 1);
            observed.frameViewOrSilence[channel] = frame.SampleOrSilence(channel);
        }

        // Output is written by the application alone. Nothing here reads the
        // input view into the output, so nonzero output after a block with
        // nonzero input proves the host added no monitoring path of its own.
        for (int channel = 0; channel < block.numOutputChannels; ++channel) {
            for (std::size_t sample = 0; sample < block.numFrames; ++sample) {
                block.outputs[channel][sample] = kOutputMarker + static_cast<float>(channel);
            }
        }
        if (block.numOutputChannels > 0 && block.numFrames > 0) {
            observed.outputChannel0Frame0 = block.outputs[0][0];
        }
        if (block.numOutputChannels > 1 && block.numFrames > 0) {
            observed.outputChannel1Frame0 = block.outputs[1][0];
        }
    }

    synth::ui::Surface& PortableSurface() { return surface; }

    EmptySurface surface;
    ObservedInput observed;
};

// One device block's planar storage. `inputChannels` carries one entry per
// device input channel: a value for a channel the driver supplies (sample
// (channel, frame) reads value + frame, so channels and frames are
// distinguishable in one assertion) or nullopt for a null pointer the runtime
// must pass through without compacting.
struct DeviceBlockSpec {
    std::vector<std::optional<float>> inputChannels;
    int numOutputChannels = 2;
    std::size_t numFrames = 8;
};

class DeviceBlockBuffers {
public:
    explicit DeviceBlockBuffers(const DeviceBlockSpec& spec) : numFrames_(spec.numFrames) {
        inputStorage_.resize(spec.inputChannels.size());
        for (std::size_t channel = 0; channel < spec.inputChannels.size(); ++channel) {
            if (!spec.inputChannels[channel].has_value()) {
                continue;
            }
            inputStorage_[channel].resize(numFrames_);
            for (std::size_t sample = 0; sample < numFrames_; ++sample) {
                inputStorage_[channel][sample] =
                    *spec.inputChannels[channel] + static_cast<float>(sample);
            }
        }
        outputStorage_.assign(static_cast<std::size_t>(spec.numOutputChannels),
                              std::vector<float>(numFrames_, 0.0f));

        inputPointers_.reserve(spec.inputChannels.size());
        for (std::size_t channel = 0; channel < spec.inputChannels.size(); ++channel) {
            inputPointers_.push_back(spec.inputChannels[channel].has_value()
                                         ? inputStorage_[channel].data()
                                         : nullptr);
        }
        outputPointers_.reserve(outputStorage_.size());
        for (std::vector<float>& channel : outputStorage_) {
            outputPointers_.push_back(channel.data());
        }
    }

    const float* const* Inputs() const {
        return inputPointers_.empty() ? nullptr : inputPointers_.data();
    }
    int NumInputChannels() const { return static_cast<int>(inputPointers_.size()); }
    float* const* Outputs() { return outputPointers_.empty() ? nullptr : outputPointers_.data(); }
    int NumOutputChannels() const { return static_cast<int>(outputPointers_.size()); }
    int NumFrames() const { return static_cast<int>(numFrames_); }

private:
    std::size_t numFrames_ = 0;
    std::vector<std::vector<float>> inputStorage_;
    std::vector<std::vector<float>> outputStorage_;
    std::vector<const float*> inputPointers_;
    std::vector<float*> outputPointers_;
};

// A started Runtime<App> whose only audio device type is synthetic.
template <typename App>
class FakeDeviceRuntime {
public:
    FakeDeviceRuntime(const std::filesystem::path& root, int maxInputChannels, int maxOutputChannels) {
        runtime_.SetRuntimeDataPathsOverride(synth::RuntimeDataPaths::FromRoots(
            root, root / "patches", root / "logs", root / "config"));
        auto deviceType = std::make_unique<synth_juce::FakeAudioDeviceType>(
            juce::StringArray{"Fake In A", "Fake In B"},
            juce::StringArray{"Fake Out A", "Fake Out B"},
            maxInputChannels,
            maxOutputChannels);
        deviceType_ = deviceType.get();
        runtime_.DeviceManager().addAudioDeviceType(std::move(deviceType));
        InstallStatusHook();
    }

    ~FakeDeviceRuntime() { runtime_.SetAudioStatusHook({}); }

    FakeDeviceRuntime(const FakeDeviceRuntime&) = delete;
    FakeDeviceRuntime& operator=(const FakeDeviceRuntime&) = delete;

    void Start() { runtime_.Start(); }

    synth_runtime::Runtime<App>& Get() { return runtime_; }
    synth_juce::FakeAudioDeviceType& DeviceType() { return *deviceType_; }
    ObservedInput& Observed() { return runtime_.GetEngine().Application().observed; }
    const std::string& Status() const { return status_; }

    // Delivers one device block and then runs the runtime's message-thread
    // diagnostic publication step, which the UI timer owns in production.
    void RunBlock(DeviceBlockBuffers& buffers) {
        synth_juce::FakeAudioDevice* device = deviceType_->CurrentDevice();
        Require(device != nullptr, "fake device is open before a block is delivered");
        device->RunBlock(buffers.Inputs(), buffers.Outputs(), buffers.NumFrames());
        runtime_.PublishPendingInputStatus();
    }

    // Reads the Audio page snapshot through the same services object MainPane
    // uses, then reinstalls this harness's status hook: the services object
    // installs its own on construction and clears it on destruction, which
    // would otherwise leave the runtime with no status sink at all.
    bool ShowsInputCombo() {
        bool showsInputCombo = false;
        {
            synth_runtime::JuceRuntimeMainServices<App> services(runtime_);
            synth::runtime_ui::AudioPageSnapshot snapshot;
            services.RefreshAudio(snapshot);
            showsInputCombo = snapshot.showInputCombo;
        }
        InstallStatusHook();
        return showsInputCombo;
    }

    // Tears the status sink down and installs a fresh one, the way a rebuilt
    // Audio page does, and reports what that new sink was handed.
    std::string StatusAfterFreshHook() {
        runtime_.SetAudioStatusHook({});
        status_.clear();
        InstallStatusHook();
        return status_;
    }

private:
    void InstallStatusHook() {
        runtime_.SetAudioStatusHook([this](const juce::String& text) { status_ = text.toStdString(); });
    }

    synth_runtime::Runtime<App> runtime_;
    synth_juce::FakeAudioDeviceType* deviceType_ = nullptr;
    std::string status_;
};

std::filesystem::path FreshRoot(const std::filesystem::path& parent, const char* name) {
    const std::filesystem::path root = parent / name;
    std::filesystem::remove_all(root);
    return root;
}

// A zero-input application opens no input path, receives a null input block,
// and shows no input selector -- whatever the device offers (sar-31).
void CheckZeroInputApplication(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<0>> host(FreshRoot(parent, "zero-input"), 2, 2);
    host.Start();

    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "zero-input application opens no input device");
    Require(!host.ShowsInputCombo(), "zero-input application hides the input selector");
    Require(host.Status().find("Input requested") == std::string::npos,
            "zero-input application publishes no requested/active input diagnostic");

    // The device has two input channels available -- opening it anyway (a
    // direct selection, bypassing the hidden combo) must not leak any of them
    // into the zero-input application's block: the application's own request
    // of zero is the ceiling regardless of what the device offers.
    host.Get().ApplyAudioDeviceInputSelection("Fake In A");
    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName == "Fake In A",
            "the device with input channels available is actually open");

    DeviceBlockSpec spec;
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);

    const ObservedInput& observed = host.Observed();
    Require(observed.blockCount == 1, "zero-input application still receives its output block");
    Require(observed.numRequestedInputChannels == 0, "zero-input block requests zero input channels");
    Require(observed.numInputChannels == 0, "zero-input block exposes zero active input channels");
    Require(observed.inputsNull, "zero-input block exposes null input storage");
    Require(observed.viewEmpty, "zero-input block exposes an empty input view");
    Require(observed.outputChannel0Frame0 == kOutputMarker &&
                observed.outputChannel1Frame0 == kOutputMarker + 1.0f,
            "zero-input application output is unaffected by the device's input channels");
}

// A 17-channel request is accepted without a framework ceiling; the host
// reports the shortfall and keeps output running (sar-31, sru-3).
void CheckShortfallRequest(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<17>> host(FreshRoot(parent, "seventeen-input"), 4, 2);
    host.Start();

    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "input-capable application opens no input device at startup");
    Require(host.ShowsInputCombo(), "input-capable application shows the input selector");
    Require(host.Status() == "Input requested 17 / active 0",
            "startup publishes the requested/active input diagnostic with no device open");
    // MainPane and its Audio page are constructed after Start() returns, so a
    // status sink installed later must be handed the current diagnostic rather
    // than waiting for the next active-count change.
    Require(host.StatusAfterFreshHook() == "Input requested 17 / active 0",
            "a status sink installed after startup receives the current input diagnostic");

    // Opening the 4-channel device negotiates the shortfall at open, not at
    // callback time: the device has fewer channels than the 17 requested.
    host.Get().ApplyAudioDeviceInputSelection("Fake In A");
    Require(host.Status() == "Input requested 17 / active 4 - Audio In: Fake In A",
            "a device narrower than the request negotiates its actual channel count on selection");

    DeviceBlockSpec spec;
    spec.inputChannels = {10.0f, 20.0f, 30.0f, 40.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);

    const ObservedInput& observed = host.Observed();
    Require(observed.numRequestedInputChannels == 17, "17-channel request reaches the block verbatim");
    Require(observed.numInputChannels == 4, "block reports the device's actual input channel count");
    Require(observed.requestedChannelCount == 17 && observed.activeChannelCount == 4,
            "input view separates the request from the active count");
    Require(observed.hasActiveChannel[3] && !observed.hasActiveChannel[4],
            "channels beyond the active count are inactive");
    Require(observed.firstFrameOrSilence[3] == 40.0f && observed.firstFrameOrSilence[4] == 0.0f,
            "safe access returns device samples below the active count and silence above it");
    Require(observed.firstFrameOrSilence[16] == 0.0f,
            "the last requested channel reads as silence when the device does not supply it");
    Require(host.Status() == "Input requested 17 / active 4 - Audio In: Fake In A",
            "a matching callback shape leaves the diagnostic unchanged");
    Require(observed.outputChannel0Frame0 == kOutputMarker,
            "an input shortfall does not stop output");
}

// A device with fewer input channels available than the application
// requested -- a mono microphone answering a stereo request -- negotiates the
// shortfall at open, and the application observes only the channels the
// device actually delivers, with output unaffected (sar-6, sar-31).
void CheckCallbackShapeNegotiation(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "narrower-input"), 1, 2);
    host.Start();
    Require(host.Status() == "Input requested 2 / active 0",
            "no input device is open yet, so startup reports a zero active count");

    host.Get().ApplyAudioDeviceInputSelection("Fake In A");
    Require(host.Status() == "Input requested 2 / active 1 - Audio In: Fake In A",
            "a device narrower than the request negotiates its actual channel count on selection");

    DeviceBlockSpec spec;
    spec.inputChannels = {7.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);

    const ObservedInput& observed = host.Observed();
    Require(observed.numInputChannels == 1, "a narrower device reports its actual negotiated count");
    Require(observed.hasActiveChannel[0] && !observed.hasActiveChannel[1],
            "the missing channel is inactive rather than fabricated");
    Require(observed.firstFrameOrSilence[0] == 7.0f && observed.firstFrameOrSilence[1] == 0.0f,
            "safe access returns silence for the missing channel");
    Require(host.Status() == "Input requested 2 / active 1 - Audio In: Fake In A",
            "the negotiated shape leaves the diagnostic unchanged across a delivered block");
    Require(observed.outputChannel0Frame0 == kOutputMarker &&
                observed.outputChannel1Frame0 == kOutputMarker + 1.0f,
            "a narrower input device does not stop output");
}

// A null pointer inside the counted range keeps its logical channel position
// (sar-31).
void CheckCountedNullChannel(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<3>> host(FreshRoot(parent, "null-channel"), 3, 2);
    host.Start();
    host.Get().ApplyAudioDeviceInputSelection("Fake In A");

    DeviceBlockSpec spec;
    spec.inputChannels = {5.0f, std::nullopt, 9.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);

    const ObservedInput& observed = host.Observed();
    Require(observed.numInputChannels == 3,
            "a null counted channel still counts toward the active channel count");
    Require(observed.hasActiveChannel[0] && !observed.hasActiveChannel[1] && observed.hasActiveChannel[2],
            "only the null channel is reported unavailable");
    Require(observed.firstFrameOrSilence[0] == 5.0f && observed.firstFrameOrSilence[1] == 0.0f &&
                observed.firstFrameOrSilence[2] == 9.0f,
            "the channel after the null pointer keeps its logical position");
    Require(observed.secondFrameOrSilence[2] == 10.0f,
            "frame indexing is unaffected by the null channel");
    Require(observed.frameViewOrSilence[1] == 0.0f && observed.frameViewOrSilence[2] == 9.0f,
            "the cross-channel frame view agrees with the channel view");
}

// A persisted input device that is no longer present leaves output initialized
// and reports itself alongside the stable input diagnostic (sar-31, sru-3).
void CheckMissingPersistedInputDevice(const std::filesystem::path& parent) {
    const std::filesystem::path root = FreshRoot(parent, "missing-input-device");
    std::filesystem::create_directories(root);
    synth::AudioDeviceState persisted;
    persisted.outputDeviceName = "Fake Out B";
    persisted.inputDeviceName = "Ghost In";
    Require(synth::SaveRuntimeConfigFile(root / "config", synth::MidiInstrumentConfig{}, persisted,
                                         synth::SyncConfig{}) == synth::RuntimeConfigFileStatus::Ok,
            "persisted runtime configuration is written for the missing-input scenario");

    FakeDeviceRuntime<InputProbeApp<2>> host(root, 2, 2);
    host.Start();

    const juce::AudioDeviceManager::AudioDeviceSetup setup =
        host.Get().DeviceManager().getAudioDeviceSetup();
    Require(setup.outputDeviceName == "Fake Out B",
            "a missing input device leaves the persisted output device selected");
    Require(setup.inputDeviceName.isEmpty(),
            "a missing persisted input device opens no input device at all");
    Require(host.Get().DeviceManager().getCurrentAudioDevice() != nullptr,
            "a missing input device leaves the audio device open");
    Require(host.Status() == "Input requested 2 / active 0 - audio input device not found: Ghost In",
            "the missing-device diagnostic is appended to the stable input status");

    DeviceBlockSpec spec;
    spec.inputChannels = {3.0f, 4.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);
    Require(host.Observed().outputChannel0Frame0 == kOutputMarker,
            "output keeps running after a missing input device");
}

// External-input-routed signal: with no input device open at startup, the
// signal is not routed; a live user selection routes it with a
// message-thread callback, and deselecting back to no input un-routes it
// with another callback -- no app restart either way.
void CheckInputRoutedSignal(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "input-routed-signal"), 2, 2);
    host.Start();

    synth::AppContext& context = host.Get().GetEngine().Context();
    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "no input device is open at startup");
    Require(!context.InputRouted(),
            "with no input device open, the routed signal is not routed");

    std::vector<bool> notifications;
    std::vector<synth::ThreadId> notificationThreads;
    context.SetInputRoutedChangedCallback([&](bool routed) {
        notifications.push_back(routed);
        notificationThreads.push_back(synth::GetCurrentThreadId());
    });

    host.Get().ApplyAudioDeviceInputSelection("Fake In B");
    Require(context.InputRouted(), "selecting a named input device routes the signal");
    Require(notifications.size() == 1 && notifications[0] == true,
            "the change callback fires exactly once on the selecting edge");
    Require(notificationThreads[0] == synth::ThreadId::Message,
            "the change callback runs on the message thread");

    host.Get().ApplyAudioDeviceInputSelection("");
    Require(!context.InputRouted(), "deselecting back to System Default un-routes the signal");
    Require(notifications.size() == 2 && notifications[1] == false,
            "the change callback fires exactly once on the deselecting edge");
    Require(notificationThreads[1] == synth::ThreadId::Message,
            "the deselecting callback also runs on the message thread");

    // A block delivered while routed is unaffected -- the signal augments,
    // never replaces, the existing input negotiation path, and does not
    // itself change on every block.
    host.Get().ApplyAudioDeviceInputSelection("Fake In B");
    notifications.clear();
    DeviceBlockSpec spec;
    spec.inputChannels = {1.0f, 2.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);
    Require(context.InputRouted(), "the signal stays routed across a delivered block");
    Require(notifications.empty(), "delivering a block does not itself change the signal");
}

// A device switch stops, re-prepares, and restarts before any further callback,
// and input and output selections round-trip independently (sar-15, sar-31).
void CheckDeviceSwitchLifecycle(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "device-switch"), 2, 2);
    host.Start();

    const int blocksBeforeSwitch = host.Observed().blockCount;
    const int preparesBeforeOutputSwitch = host.Observed().prepareCount;
    host.DeviceType().ClearLifecycle();
    host.Get().ApplyAudioDeviceSelection("Fake Out B");

    const std::vector<std::string> expectedOutputSwitch{
        "Fake Out A+(none):stop", "Fake Out A+(none):destroy",
        "Fake Out B+(none):open", "Fake Out B+(none):start"};
    Require(host.DeviceType().Lifecycle() == expectedOutputSwitch,
            "an output device switch stops and destroys the old device before starting the new one");
    Require(host.Observed().prepareCount == preparesBeforeOutputSwitch + 1,
            "the engine is re-prepared exactly once across the output switch");
    Require(host.Observed().blockCount == blocksBeforeSwitch,
            "no block is delegated while the output device is being switched");

    // The input-switch path is a distinct call site and must follow the same
    // stop/re-prepare/restart ordering, with no block delegated in between.
    const int preparesBeforeInputSwitch = host.Observed().prepareCount;
    host.DeviceType().ClearLifecycle();
    host.Get().ApplyAudioDeviceInputSelection("Fake In B");

    const std::vector<std::string> expectedInputSwitch{
        "Fake Out B+(none):stop", "Fake Out B+(none):destroy",
        "Fake Out B+Fake In B:open", "Fake Out B+Fake In B:start"};
    Require(host.DeviceType().Lifecycle() == expectedInputSwitch,
            "an input device switch stops and destroys the old device before starting the new one");
    Require(host.Observed().prepareCount == preparesBeforeInputSwitch + 1,
            "the engine is re-prepared exactly once across the input switch");
    Require(host.Observed().blockCount == blocksBeforeSwitch,
            "no block is delegated while the input device is being switched");

    const synth::AudioDeviceState state = host.Get().GetEngine().AudioDeviceSnapshot();
    Require(state.outputDeviceName == "Fake Out B" && state.inputDeviceName == "Fake In B",
            "input and output device names are recorded independently");
    const juce::AudioDeviceManager::AudioDeviceSetup setup =
        host.Get().DeviceManager().getAudioDeviceSetup();
    Require(setup.outputDeviceName == "Fake Out B" && setup.inputDeviceName == "Fake In B",
            "input selection does not disturb the selected output device");

    host.Get().ApplyAudioDeviceSelection("Fake Out A");
    const synth::AudioDeviceState afterOutputSwitch = host.Get().GetEngine().AudioDeviceSnapshot();
    Require(afterOutputSwitch.outputDeviceName == "Fake Out A" &&
                afterOutputSwitch.inputDeviceName == "Fake In B",
            "output selection does not disturb the selected input device");

    DeviceBlockSpec spec;
    spec.inputChannels = {6.0f, 8.0f};
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);
    Require(host.Observed().blockCount == blocksBeforeSwitch + 1 &&
                host.Observed().numInputChannels == 2,
            "the switched device delegates blocks with the negotiated input shape");
    Require(host.Observed().numFrames == 8 && host.Observed().numOutputChannels == 2,
            "a device switch leaves the delegated frame count and output shape unchanged");
}

// Portable "System Default" input is an empty host-neutral name, and an empty
// AudioDeviceSetup::inputDeviceName is how JUCE says "no input device at
// all" -- which is exactly what System Default input now means: no device
// opens, no capture happens, and the routed signal follows.
void CheckSystemDefaultInputSelection(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "system-default-input"), 2, 2);
    host.Start();

    host.Get().ApplyAudioDeviceInputSelection("Fake In B");
    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName == "Fake In B",
            "a named input selection reaches the device setup verbatim");

    host.Get().ApplyAudioDeviceInputSelection("");
    Require(host.Get().GetEngine().AudioDeviceSnapshot().inputDeviceName.empty(),
            "System Default input persists the empty host-neutral name");
    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "System Default input opens no input device at all");
    Require(host.Status() == "Input requested 2 / active 0 - Audio In: System Default",
            "System Default input reports a zero active count");
    Require(!host.Get().GetEngine().Context().InputRouted(),
            "System Default input is not routed");
}

// A device that is enumerated but fails to open (already in use, permission
// denied) leaves JUCE with no device at all, output included. The host must
// publish the returned error and recover a live output-only device while the
// diagnostic honestly reports active 0 (sar-31, sru-3).
void CheckInputDeviceOpenFailure(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "input-open-failure"), 2, 2);
    host.DeviceType().FailOpenForInputDevice("Fake In B");
    host.Start();
    Require(host.Status() == "Input requested 2 / active 0",
            "startup opens no input device, so the failing device is never attempted");

    const std::string expectedStatus =
        "Input requested 2 / active 0 - Fake input device open failed: Fake In B";
    const int blocksBeforeFailure = host.Observed().blockCount;
    host.Get().ApplyAudioDeviceInputSelection("Fake In B");

    Require(host.Status() == expectedStatus,
            "a failed input open publishes its exact returned diagnostic with a zero active count");
    Require(host.Get().DeviceManager().getCurrentAudioDevice() != nullptr,
            "a failed input open recovers a live audio device rather than leaving none");
    const juce::AudioDeviceManager::AudioDeviceSetup setup =
        host.Get().DeviceManager().getAudioDeviceSetup();
    Require(setup.inputDeviceName.isEmpty(), "recovery falls back to an output-only setup");
    Require(setup.outputDeviceName == "Fake Out A",
            "recovery keeps the prior known-good output device");
    Require(host.Observed().blockCount == blocksBeforeFailure,
            "no block is delegated while the failure is being recovered");

    // The recovered device has no input channels, so its driver delivers none.
    DeviceBlockSpec spec;
    spec.numOutputChannels = 2;
    DeviceBlockBuffers buffers(spec);
    host.RunBlock(buffers);

    const ObservedInput& observed = host.Observed();
    Require(observed.numRequestedInputChannels == 2, "the request survives a failed input open");
    Require(observed.numInputChannels == 0 && observed.inputsNull && observed.viewEmpty,
            "a failed input open leaves zero active input channels and null input storage");
    Require(observed.firstFrameOrSilence[0] == 0.0f && observed.firstFrameOrSilence[1] == 0.0f,
            "safe access returns silence for every requested channel after a failed open");
    Require(observed.numFrames == 8 && observed.numOutputChannels == 2,
            "a failed input open leaves the delegated frame count and output shape unchanged");
    Require(observed.outputChannel0Frame0 == kOutputMarker &&
                observed.outputChannel1Frame0 == kOutputMarker + 1.0f,
            "output continues after a failed input open");
    Require(host.Status() == expectedStatus,
            "the failure diagnostic survives subsequent blocks");
}

// A negative input request is rejected before any device is opened (sar-31).
void CheckNegativeInputRequestRejected(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<-1>> host(FreshRoot(parent, "negative-input"), 2, 2);
    bool threw = false;
    try {
        host.Start();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    Require(threw, "a negative input request throws std::invalid_argument from Start()");
    Require(host.DeviceType().Lifecycle().empty(),
            "a negative input request opens no audio device");
}

// The host does not open an input device merely because the application
// requested input channels -- only an explicit device selection does. With
// no selection made, startup opens no input device and reports a zero
// active count.
void CheckNoInputDeviceOpensWithoutSelection(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "no-selection-no-input"), 2, 2);
    host.Start();

    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "an input-capable application opens no input device at startup");
    Require(host.Get().DeviceManager().getCurrentAudioDevice() != nullptr,
            "output still opens with no input device selected");
    Require(host.Status() == "Input requested 2 / active 0",
            "startup reports a zero active input count with no device open");
}

// The positive control for the scenario above: an operator selection
// actually opens the named input device and negotiates the requested input
// channels, proving that a zero active count elsewhere in this suite means
// no device was open rather than a harness that can never open one.
void CheckSelectedInputDeviceDeliversChannels(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "selection-opens-input"), 2, 2);
    host.Start();

    host.Get().ApplyAudioDeviceInputSelection("Fake In B");

    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName == "Fake In B",
            "selecting a named input device opens it");
    Require(host.Status() == "Input requested 2 / active 2 - Audio In: Fake In B",
            "the selected input device negotiates the requested channel count");
}

// Deselecting back to no input closes the input device and un-routes the
// external-input-routed signal.
void CheckDeselectingInputClosesDeviceAndUnroutes(const std::filesystem::path& parent) {
    FakeDeviceRuntime<InputProbeApp<2>> host(FreshRoot(parent, "deselect-closes-input"), 2, 2);
    host.Start();
    host.Get().ApplyAudioDeviceInputSelection("Fake In B");
    Require(host.Get().GetEngine().Context().InputRouted(),
            "a named input selection routes the signal before it is deselected");

    host.Get().ApplyAudioDeviceInputSelection("");

    Require(host.Get().DeviceManager().getAudioDeviceSetup().inputDeviceName.isEmpty(),
            "deselecting input closes the input device");
    Require(host.Status() == "Input requested 2 / active 0 - Audio In: System Default",
            "deselecting input returns the active count to zero");
    Require(!host.Get().GetEngine().Context().InputRouted(),
            "deselecting input returns the routed signal to not-routed");
}

void CheckJuceAudioInputNegotiation() {
    const std::filesystem::path parent =
        std::filesystem::temp_directory_path() / "sheaf-runtime-audio-input-test";
    std::filesystem::remove_all(parent);

    CheckZeroInputApplication(parent);
    CheckShortfallRequest(parent);
    CheckCallbackShapeNegotiation(parent);
    CheckCountedNullChannel(parent);
    CheckMissingPersistedInputDevice(parent);
    CheckInputRoutedSignal(parent);
    CheckDeviceSwitchLifecycle(parent);
    CheckSystemDefaultInputSelection(parent);
    CheckInputDeviceOpenFailure(parent);
    CheckNegativeInputRequestRejected(parent);
    CheckNoInputDeviceOpensWithoutSelection(parent);
    CheckSelectedInputDeviceDeliversChannels(parent);
    CheckDeselectingInputClosesDeviceAndUnroutes(parent);

    std::filesystem::remove_all(parent);
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;
    synth::SetCurrentThreadId(synth::ThreadId::Message);

    const std::filesystem::path sheafRoot = synth_runtime::SheafUserApplicationDataRoot();
    Require(sheafRoot.filename() == "Sheaf", "host data root is the shared Sheaf app-support root");

    const synth::RuntimeDataPaths standalonePaths =
        synth_runtime::DefaultDataPathsForApp(synth_miniapp::MiniApp::Config());
    Require(standalonePaths.dataRoot == sheafRoot / "SynthMiniapp",
            "standalone miniapp data root stays under the shared Sheaf app-support root");

    const synth::RuntimeDataPaths sheafPatchPaths = synth_runtime::SheafPatchDataPathsForApp("miniapp");
    Require(sheafPatchPaths.dataRoot == sheafRoot / "synth" / "sheaf-patch",
            "SheafPatch data root uses stable app-support storage");
    Require(sheafPatchPaths.configFile == sheafRoot / "synth" / "sheaf-patch" / "config",
            "SheafPatch config path is stable under app support");
    Require(sheafPatchPaths.patchesRoot == sheafRoot / "synth" / "sheaf-patch" / "patches" / "miniapp",
            "SheafPatch patches path is stable under app support");

    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "sheaf-runtime-shell-session-test";
    std::filesystem::remove_all(root);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromRoots(
        root,
        root / "patches" / "miniapp",
        root / "logs",
        root / "config");

    synth_runtime::RuntimeShellSession<synth_miniapp::MiniApp> session(paths);
    const synth::RuntimeDataPaths& runtimePaths = session.GetRuntime().DataPaths();

    Require(runtimePaths.dataRoot == paths.dataRoot, "runtime session receives supplied data root");
    Require(runtimePaths.patchesRoot == paths.patchesRoot, "runtime session receives supplied patches root");
    Require(runtimePaths.logsRoot == paths.logsRoot, "runtime session receives supplied logs root");
    Require(runtimePaths.configFile == paths.configFile, "runtime session receives supplied config path");
    Require(std::filesystem::exists(paths.patchesRoot), "runtime session creates supplied patches root");
    Require(std::filesystem::exists(paths.logsRoot), "runtime session creates supplied logs root");

    const synth::RuntimeConfig config = synth_miniapp::MiniApp::Config();
    Require(session.Component().getWidth() == config.uiWidth + 96,
            "runtime shell content width adds the shared sidebar");
    Require(session.Component().getHeight() == config.uiHeight,
            "runtime shell content height preserves the app height");

    {
        // sprs-15: MainWindow::ShowContent(component) derives the window's
        // size from the shell component's own (already-intrinsic) bounds
        // rather than the app's raw config.uiWidth/uiHeight, so the window
        // is wide enough to show the sidebar without resizing.
        synth_runtime::MainWindow window("runtime shell session sizing test");
        window.ShowContent(session.Component());
        Require(window.getWidth() == config.uiWidth + 96,
                "launch window width derives from the shell's intrinsic bounds, not raw config width");
        Require(window.getHeight() == config.uiHeight,
                "launch window height derives from the shell's intrinsic bounds, not raw config height");
    }

    std::vector<synth_juce::PortableComponent*> renderers;
    CollectPortableComponents(session.Component(), renderers);
    Require(renderers.size() == 1, "runtime shell owns exactly one portable renderer");
    synth_juce::PortableComponent& renderer = *renderers.front();
    Require(renderer.FindByNodeId("runtime.main.root") == &renderer,
            "composite root is discoverable as the shared renderer");
    Require(renderer.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0)) != nullptr,
            "app encoder is discoverable through the shared renderer");
    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarAudio) != nullptr,
            "audio sidebar control is discoverable through the shared renderer");
    Require(renderer.SurfaceBoundsForNode(synth::runtime_ui::NodeIds::kSidebarAudio).getX() ==
                config.uiWidth,
            "sidebar descendants resolve at the sidebar root's offset, not twice it");

    synth::ui::Surface* appSurface = &session.GetRuntime().AppSurface();
    auto* audioButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarAudio));
    Require(audioButton != nullptr, "audio sidebar control is a button");
    audioButton->onClick();
    Require(renderer.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0)) == nullptr,
            "audio click synchronously replaces app controls in the shared renderer");
    auto* backButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioBack));
    Require(backButton != nullptr, "audio page back control is rendered");
    backButton->onClick();
    Require(renderer.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0)) != nullptr,
            "back synchronously restores app controls in the shared renderer");
    Require(&session.GetRuntime().AppSurface() == appSurface,
            "the app surface and its state survive runtime-page navigation");

    auto* syncButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarSync));
    Require(syncButton != nullptr, "Sync sidebar control is a button");
    syncButton->onClick();
    auto* syncShell = dynamic_cast<synth_runtime::ShellComponent<synth_miniapp::MiniApp>*>(
        &session.Component());
    Require(syncShell != nullptr &&
                syncShell->GetMainPane().CurrentPage() ==
                    synth_runtime::MainPane<synth_miniapp::MiniApp>::Page::Sync,
            "JUCE host reports the portable Sync page as current");
    auto* sendClockToggle = dynamic_cast<juce::ToggleButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncSendClock));
    auto* syncPpqnEditor = dynamic_cast<juce::TextEditor*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncPpqn));
    Require(sendClockToggle != nullptr && !sendClockToggle->getToggleState(),
            "JUCE Sync session opens with the Engine snapshot");
    Require(syncPpqnEditor != nullptr && syncPpqnEditor->getText() == "24",
            "JUCE Sync session opens with default PPQN");
    sendClockToggle->setToggleState(true, juce::dontSendNotification);
    sendClockToggle->onClick();
    syncPpqnEditor = dynamic_cast<juce::TextEditor*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncPpqn));
    Require(syncPpqnEditor != nullptr, "JUCE Sync PPQN remains rendered after toggle edit");
    syncPpqnEditor->setText("96");
    syncPpqnEditor->onReturnKey();
    Require(session.GetRuntime().GetEngine().SyncConfigurationSnapshot() == synth::SyncConfig{},
            "JUCE Sync edits stay staged before Back");

    auto* syncBackButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncBack));
    Require(syncBackButton != nullptr, "JUCE Sync Back is rendered");
    syncBackButton->onClick();
    const synth::SyncConfig committedSync{
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = false,
        .receiveTransport = false,
        .ppqn = 96,
    };
    Require(session.GetRuntime().GetEngine().SyncConfigurationSnapshot() == committedSync,
            "JUCE Sync Back commits one complete configuration");
    synth::MidiInstrumentConfig savedInstrument;
    synth::AudioDeviceState savedAudio;
    synth::SyncConfig savedSync;
    Require(synth::LoadRuntimeConfigFile(
                paths.configFile, savedInstrument, savedAudio, savedSync) ==
                synth::RuntimeConfigFileStatus::Ok &&
                savedSync == committedSync,
            "JUCE Sync Back persists the requested configuration");

    syncButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarSync));
    Require(syncButton != nullptr, "Sync sidebar returns after Back");
    syncButton->onClick();
    sendClockToggle = dynamic_cast<juce::ToggleButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncSendClock));
    syncPpqnEditor = dynamic_cast<juce::TextEditor*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncPpqn));
    Require(sendClockToggle != nullptr && sendClockToggle->getToggleState(),
            "JUCE Sync reopen uses committed toggle state");
    Require(syncPpqnEditor != nullptr && syncPpqnEditor->getText() == "96",
            "JUCE Sync reopen uses committed PPQN");
    syncBackButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncBack));
    Require(syncBackButton != nullptr, "JUCE Sync Back remains available after reopen");
    syncBackButton->onClick();

    session.GetRuntime().GetEngine().EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        instrument.AddController(synth::WrldBldrDefaultControllerSlot("second"));
    });
    auto* shell = dynamic_cast<synth_runtime::ShellComponent<synth_miniapp::MiniApp>*>(
        &session.Component());
    Require(shell != nullptr, "runtime session exposes its typed shell for refresh");
    shell->RepaintAll();
    auto* controllersButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarControllers));
    Require(controllersButton != nullptr, "controllers sidebar control is a button");
    controllersButton->onClick();

    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kBack) != nullptr,
            "controllers back renders through the shared renderer");
    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerRow(0)) != nullptr
                && renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerRow(1)) != nullptr,
            "controller rows render through the shared renderer");
    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kAddRow) != nullptr,
            "controllers add row renders through the shared renderer");
    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kStatus) != nullptr,
            "controllers status renders through the shared renderer");
    juce::Component* controllersScroll =
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kScroll);
    Require(controllersScroll != nullptr,
            "controllers scroll renders through the shared renderer");

    const juce::Rectangle<int> controllersBackBounds =
        renderer.SurfaceBoundsForNode(synth::runtime_ui::NodeIds::kBack);
    const juce::Rectangle<int> controllersScrollBounds =
        renderer.SurfaceBoundsForNode(synth::runtime_ui::NodeIds::kScroll);
    const juce::Rectangle<int> firstControllerBounds =
        renderer.SurfaceBoundsForNode(synth::runtime_ui::NodeIds::ControllerRow(0));
    const juce::Rectangle<int> secondControllerBounds =
        renderer.SurfaceBoundsForNode(synth::runtime_ui::NodeIds::ControllerRow(1));
    Require(controllersBackBounds.getBottom() <= controllersScrollBounds.getY(),
            "controllers header controls end above the scroll viewport");
    Require(firstControllerBounds.getY() != secondControllerBounds.getY(),
            "controller rows resolve to distinct surface positions");

    auto* disclosure = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerDisclosure(0)));
    Require(disclosure != nullptr, "first controller disclosure renders");
    disclosure->onClick();
    Require(shell->GetMainPane().HasDeferredRendererRefresh(),
            "disclosure click queues its structural renderer refresh");
    shell->GetMainPane().FlushDeferredRendererRefresh();
    auto* systemSection = dynamic_cast<juce::TextButton*>(renderer.FindByNodeId(
        synth::runtime_ui::NodeIds::SectionToggle(0, synth::MidiConfigSection::SystemMessages)));
    Require(systemSection != nullptr, "system messages section renders after disclosure");
    systemSection->onClick();
    Require(shell->GetMainPane().HasDeferredRendererRefresh(),
            "section click queues its structural renderer refresh");
    shell->GetMainPane().FlushDeferredRendererRefresh();

    controllersScroll = renderer.FindByNodeId(synth::runtime_ui::NodeIds::kScroll);
    Require(controllersScroll != nullptr, "controllers scroll survives section expansion");
    juce::Viewport* viewport = FindViewport(*controllersScroll);
    Require(viewport != nullptr && viewport->getViewedComponent() != nullptr,
            "generic controllers scroll owns a JUCE viewport and content component");
    Require(viewport->getViewedComponent()->getHeight() > viewport->getHeight(),
            "expanded controllers content is taller than its viewport");

    synth::MidiConfigViewModel controllersModel;
    controllersModel.Rebuild(session.GetRuntime().GetEngine().InstrumentSnapshot(),
                             session.GetRuntime().MidiConnections().State());
    const std::vector<synth::MidiMappingRowVM> systemRows =
        controllersModel.SectionRows(0, synth::MidiConfigSection::SystemMessages);
    Require(!systemRows.empty() && !systemRows.back().editableFields.empty(),
            "runtime controller has editable system-message mappings");
    const std::size_t finalRowIndex = systemRows.size() - 1;
    const synth::MidiMappingRowVM::Field finalField = systemRows.back().editableFields.back();
    const std::string finalMappingId = synth::runtime_ui::NodeIds::MappingField(
        0, synth::MidiConfigSection::SystemMessages, finalRowIndex, finalField);
    juce::Component* finalMappingControl = renderer.FindByNodeId(finalMappingId);
    Require(finalMappingControl != nullptr, "final system-message mapping control renders");

    viewport->setViewPosition(viewport->getViewPositionX(),
                              viewport->getViewedComponent()->getHeight() - viewport->getHeight());
    Require(SurfaceBoundsOf(renderer, *viewport).intersects(
                SurfaceBoundsOf(renderer, *finalMappingControl)),
            "final mapping control is reachable at the maximum vertical scroll position");
    Require(IsEnabledEditableControl(*finalMappingControl),
            "final mapping control remains enabled and editable after scrolling");

    auto* controllersBackButton = dynamic_cast<juce::TextButton*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::kBack));
    Require(controllersBackButton != nullptr, "controllers back control remains available");
    for (const char* actionName : {synth::runtime_ui::Actions::kBack,
                                   synth::runtime_ui::Actions::kWizardOpen,
                                   synth::runtime_ui::Actions::kWizardChoose,
                                   synth::runtime_ui::Actions::kWizardBack,
                                   synth::runtime_ui::Actions::kWizardCancel}) {
        Require(shell->GetMainPane().NeedsDeferredRendererRefresh(
                    synth::ui::Action::Named(actionName)),
                "structural Controllers action uses the deferred renderer path");
    }
    controllersBackButton->onClick();
    Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kBack) != nullptr,
            "deferred Controllers navigation does not destroy the clicked button during onClick");
    Require(shell->GetMainPane().HasDeferredRendererRefresh(),
            "structural Controllers click queues one deferred renderer refresh");
    shell->GetMainPane().FlushDeferredRendererRefresh();
    Require(renderer.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0)) != nullptr,
            "deferred Controllers navigation refreshes after the click callback returns");

    const std::filesystem::path wideRoot = root / "wide";
    synth_runtime::RuntimeShellSession<WideDrawApp> wideSession(
        synth::RuntimeDataPaths::FromRoots(
            wideRoot, wideRoot / "patches", wideRoot / "logs", wideRoot / "config"));
    std::vector<synth_juce::PortableComponent*> wideRenderers;
    CollectPortableComponents(wideSession.Component(), wideRenderers);
    Require(wideRenderers.size() == 1, "wide-draw shell owns one portable renderer");
    juce::Component* wideDraw = wideRenderers.front()->FindByNodeId("wide.draw");
    Require(wideDraw != nullptr && wideDraw->getBounds().getRight() == 900,
            "full-width app draw reaches x 900 without clipping");

    // Fix round 1, finding 1 (sprs-13, Task 8 review): production wiring.
    // RuntimeMainComponent::SetContentExtent() existed and was
    // unit/parity-tested (runtime_main_component_tests,
    // browser_runtime_contract_tests) before this fix, but nothing in the
    // shipped JUCE app ever called it, so a real window resize never reached
    // an ExtentAwareSurface app. This constructs a real RuntimeShellSession
    // (the same construction path SynthMiniapp uses) around
    // WiringExtentAwareApp and drives an actual JUCE resize through the
    // top-level shell component, then asserts -- through the app's own
    // surface object, reached via Runtime::AppSurface() -- that the offered
    // extent tracks it.
    const std::filesystem::path wiringExtentRoot = root / "wiring-extent";
    synth_runtime::RuntimeShellSession<WiringExtentAwareApp> wiringSession(
        synth::RuntimeDataPaths::FromRoots(wiringExtentRoot,
                                           wiringExtentRoot / "patches",
                                           wiringExtentRoot / "logs",
                                           wiringExtentRoot / "config"));
    auto* wiringSurface =
        dynamic_cast<WiringExtentAwareSurface*>(&wiringSession.GetRuntime().AppSurface());
    Require(wiringSurface != nullptr, "wiring test app's extent-aware surface is reachable "
                                      "through the runtime");
    Require(wiringSurface->extentOffers >= 1,
            "constructing the shell already offers the extent-aware surface its initial "
            "content extent (via MainPane's constructor-time RefreshOnTick), matching "
            "runtime_main_component_tests' unit coverage of the same hook");

    // Fix round 2 (sprs-13 Task 8 re-review): RuntimeShellSession's
    // constructor sizes the shell to MainPane::IntrinsicBounds() -- the
    // composite footprint, config.uiWidth(640) + kSidebarWidth(96) = 736
    // wide, 480 tall (Shell.hpp:86-87). That construction-time resize
    // already ran through the fixed MainPane::resized(), so the extent
    // offered to the app by now must already be the CONTENT area (736 - 96
    // = 640, 480) -- exactly WiringExtentAwareApp's own declared
    // config.uiWidth/uiHeight -- not the full 736-wide composite pane.
    Require(wiringSurface->extent.width == 640.0f && wiringSurface->extent.height == 480.0f,
            "the shell's construction-time resize already offers the app its sidebar-free "
            "content extent (pane width minus Layout::kSidebarWidth), matching its own "
            "declared config.uiWidth/uiHeight");

    // RuntimeMainComponent::BuildTree() unconditionally re-offers whatever
    // liveContentExtent_ it currently holds to an ExtentAwareSurface app on
    // *every* call (RuntimeMainComponent.hpp:134-137), including the
    // RefreshFromSurface() call resized() already made before this fix -- so
    // extentOffers alone climbs regardless of whether MainPane wires the new
    // JUCE bounds through. The value offered is the only thing that can
    // distinguish "wired" from "not wired"; capture it before resizing and
    // require it to actually change to the new size, not merely that another
    // offer happened.
    const synth::ui::Bounds extentBeforeResize = wiringSurface->extent;
    wiringSession.Component().setSize(800, 600);
    Require(wiringSurface->extent.width != extentBeforeResize.width ||
                wiringSurface->extent.height != extentBeforeResize.height,
            "resizing the shell's real JUCE top-level component changes the extent offered "
            "to the app surface -- the production wiring gap finding 1 identified (before "
            "the fix, MainPane::resized() never called RuntimeMainComponent::"
            "SetContentExtent(), so the app kept resolving at its original extent regardless "
            "of window size)");
    // Fix round 2: the offered width is the pane's new width (800) MINUS
    // the sidebar (96) = 704, not the raw 800 -- otherwise an extent-aware
    // app would resolve at the full pane width and BuildTree() would place
    // the sidebar at x 800, past the 800-wide pane's own right edge
    // (RuntimeMainComponent.hpp:156, sidebar x == resolved app root width).
    // Height is unaffected: the sidebar sits beside the content, not above
    // or below it.
    Require(wiringSurface->extent.width == 704.0f && wiringSurface->extent.height == 600.0f,
            "the offered extent is MainPane's live JUCE bounds with the sidebar's width "
            "subtracted (pane 800 wide - Layout::kSidebarWidth 96 = 704), matching the app "
            "CONTENT area the composition actually places beside the sidebar, not the pane's "
            "full composite footprint");

    {
        auto owner = synth_runtime::MakeRuntimeSessionOwner<synth_miniapp::MiniApp>(paths);
        Require(owner != nullptr, "type-erased runtime session owner is constructed");

        juce::Component parent;
        parent.addAndMakeVisible(owner->Component());

        Require(owner->Component().getParentComponent() == &parent,
                "type-erased runtime session owner exposes a displayable component");
        Require(dynamic_cast<synth_runtime::ShellComponent<synth_miniapp::MiniApp>*>(&owner->Component()) != nullptr,
                "type-erased runtime session owner exposes the contained typed shell component");

        parent.removeChildComponent(&owner->Component());
        owner.reset();
    }

    CheckJuceAudioInputNegotiation();

    return 0;
}
