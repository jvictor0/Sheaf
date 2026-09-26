#include "synth/AppConcepts.hpp"
#include "synth/AppContext.hpp"
#include "synth/Engine.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"
#include "synth/browser/BrowserAppEntry.hpp"
#include "synth/browser/BrowserCommandBuffer.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "browser runtime contract tests must not see JUCE"
#endif

#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

extern "C" synth_browser::RuntimeAbi* synth_browser_create_runtime()
{
    return nullptr;
}

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

bool NearlyEqual(float left, float right)
{
    return std::fabs(left - right) <= 0.0001f;
}

const synth_browser::DecodedNode* FindNode(
    const synth_browser::DecodedCommandBuffer& frame,
    const char* id)
{
    for (const synth_browser::DecodedNode& node : frame.nodes)
    {
        if (node.id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

const synth::ui::Node* FindPortableNode(const synth::ui::NodeTree& tree, const char* id)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id == synth::ui::NodeId(id))
        {
            return &node;
        }
    }
    return nullptr;
}

bool HasOption(const synth_browser::DecodedNode& node, const char* id, const char* label)
{
    for (const synth_browser::DecodedOption& option : node.options)
    {
        if (option.id == id && option.label == label)
        {
            return true;
        }
    }
    return false;
}

std::size_t JsonFileCount(const std::filesystem::path& directory)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec)
    {
        return 0;
    }

    std::size_t count = 0;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".json")
        {
            ++count;
        }
    }
    return count;
}

class ContractSurface final : public synth::ui::Surface
{
public:
    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Builder builder;
        builder.Root("contract.app.root", {0.0f, 0.0f, 640.0f, 480.0f})
            .Button("contract.app.button",
                    "Apply",
                    synth::ui::Action::Named("contract.app.apply"), {});
        return builder.Build({0.0f, 0.0f, 640.0f, 480.0f});
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        ++dispatchCount;
        lastAction = action.name;
        lastValue = action.value;
        if (handler_)
        {
            handler_(action);
        }
    }

    int dispatchCount = 0;
    std::string lastAction;
    std::string lastValue;

private:
    ActionHandler handler_;
};

// Browser-backend parity for the extent-aware hook. The
// browser Runtime shares synth::runtime_ui::RuntimeMainComponent verbatim
// (it constructs RuntimeMainComponent<App, BrowserRuntimeMainServices<App>>),
// so exercising the same class directly under this (non-JUCE) browser
// translation unit proves the identical resolved-width rule applies here too.
class ExtentAwareContractSurface final : public synth::ui::Surface,
                                         public synth::ui::ExtentAwareSurface
{
public:
    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Node root;
        root.id = "extent.contract.app.root";
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = extent_;
        return synth::ui::NodeTree{{std::move(root)}};
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action&) override {}

    void SetContentExtent(synth::ui::Bounds extent) override
    {
        extent_ = extent;
    }

private:
    synth::ui::Bounds extent_{0.0f, 0.0f, 640.0f, 480.0f};
    ActionHandler handler_;
};

class ExtentAwareContractApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "ExtentAwareContractApp";
        config.uiWidth = 640;
        config.uiHeight = 480;
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    ExtentAwareContractSurface surface;
};

class ValidApp
{
public:
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{
            .appName = "BrowserContractApp",
            .uiWidth = 640,
            .uiHeight = 480,
        };
    }

    void Init(synth::AppContext* context)
    {
        auto& group = context->parameterManager->CreateGroup({
            .numVoices = 1,
            .numModulators = 0,
            .numScenes = 1,
            .maxParameters = 1,
            .processLiteAlpha = 1.0f,
        });
        probeId = context->parameterManager
                      ->CreateParameter(group, {.name = "Probe", .defaultValue = 0.25f})
                      .Id();

        context->instrument->controllers = {
            synth::MidiControllerSlot{
                .name = "Controller A",
                .kind = synth::MidiProfileKind::Generic,
                .input = {.identifier = "in-a", .name = "Input A"},
                .output = {.identifier = "out-a", .name = "Output A"},
            },
            synth::MidiControllerSlot{
                .name = "Controller B",
                .kind = synth::MidiProfileKind::Generic,
                .input = {.identifier = "in-b", .name = "Input B"},
                .output = {.identifier = "out-b", .name = "Output B"},
            },
        };
    }
    void ProcessBlock(synth::AudioBlock&) {}
    void PrepareToPlay(double sampleRate, int blockSize)
    {
        preparedSampleRate = sampleRate;
        preparedBlockSize = blockSize;
    }
    synth::ui::Surface& PortableSurface() { return surface; }

    ContractSurface surface;
    synth::ParameterId probeId = 0;
    double preparedSampleRate = 0.0;
    int preparedBlockSize = 0;
};

template <int InputChannels>
class InputCountApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "InputCountApp";
        config.numAudioInputs = InputChannels;
        config.numAudioOutputs = 2;
        config.uiWidth = 640;
        config.uiHeight = 480;
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    ContractSurface surface;
};

template <int InputChannels>
class RequestedInputCountProbeApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "RequestedInputCountProbeApp";
        config.numAudioInputs = InputChannels;
        config.numAudioOutputs = 2;
        config.uiWidth = 640;
        config.uiHeight = 480;
        return config;
    }

    void Init(synth::AppContext*) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    void ProcessBlock(synth::AudioBlock& block)
    {
        observedRequestedInputs = block.numRequestedInputChannels;
        observedActiveInputs = block.numInputChannels;
    }

    ContractSurface surface;
    int observedRequestedInputs = -1;
    int observedActiveInputs = -1;
};

class NegativeInputApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "NegativeInputApp";
        config.numAudioInputs = -1;
        config.numAudioOutputs = 2;
        config.uiWidth = 640;
        config.uiHeight = 480;
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    ContractSurface surface;
};

struct BrowserCallbackObservation
{
    std::size_t frames = 0;
    std::size_t requestedInputs = 0;
    std::size_t activeInputs = 0;
    std::uint64_t startSample = 0;
    float firstInput = 0.0f;
    float missingInput = 0.0f;
};

class BrowserCallbackProbeApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "BrowserCallbackProbeApp";
        config.numAudioInputs = 4;
        config.numAudioOutputs = 2;
        config.uiWidth = 640;
        config.uiHeight = 480;
        return config;
    }

    void Init(synth::AppContext*) {}
    void PrepareToPlay(double, int) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    void ProcessBlock(synth::AudioBlock& block)
    {
        const auto input = block.InputView();
        observations.push_back({
            .frames = block.numFrames,
            .requestedInputs = input.RequestedChannelCount(),
            .activeInputs = input.ActiveChannelCount(),
            .startSample = block.startSample,
            .firstInput = input.SampleOrSilence(0, 0),
            .missingInput = input.SampleOrSilence(3, 0),
        });
        for (std::size_t frame = 0; frame < block.numFrames; ++frame)
        {
            const float left = input.SampleOrSilence(0, frame) + input.SampleOrSilence(1, frame);
            const float right = input.SampleOrSilence(3, frame);
            if (block.numOutputChannels > 0 && block.outputs != nullptr && block.outputs[0] != nullptr)
            {
                block.outputs[0][frame] = left;
            }
            if (block.numOutputChannels > 1 && block.outputs != nullptr && block.outputs[1] != nullptr)
            {
                block.outputs[1][frame] = right;
            }
        }
    }

    ContractSurface surface;
    std::vector<BrowserCallbackObservation> observations;
};

class ThrowingBrowserCallbackApp
{
public:
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config;
        config.appName = "ThrowingBrowserCallbackApp";
        config.numAudioInputs = 1;
        config.numAudioOutputs = 1;
        return config;
    }

    void Init(synth::AppContext*) {}
    synth::ui::Surface& PortableSurface() { return surface; }

    void ProcessBlock(synth::AudioBlock&)
    {
        throw std::runtime_error("intentional test failure");
    }

    ContractSurface surface;
};

class MissingSurface
{
public:
    static synth::RuntimeConfig Config() { return {}; }
    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
};

class RuntimeFixture
{
public:
    RuntimeFixture()
    {
        std::filesystem::remove_all(dataRoot_);
        std::filesystem::create_directories(paths_.patchesRoot);
        std::filesystem::create_directories(paths_.logsRoot);
        runtime.SetRuntimeDataPaths(paths_);
        runtime.Start();
        runtime.MessageTick(1);
    }

    ~RuntimeFixture()
    {
        runtime.Stop();
        std::filesystem::remove_all(dataRoot_);
    }

    synth_browser::DecodedCommandBuffer Frame()
    {
        const synth_browser::CommandBuffer buffer = runtime.BuildUiFrame();
        return synth_browser::DecodeCommandBuffer(buffer.bytes);
    }

    void Prepare(double sampleRate = 48000.0, std::size_t blockSize = 128)
    {
        runtime.Prepare(sampleRate, blockSize);
    }

    void PumpOnce()
    {
        runtime.Process(nullptr, 0, 64, nextTimestamp_++);
        runtime.MessageTick(nextTimestamp_++);
    }

    void PumpUntilJsonCount(const std::filesystem::path& patchDirectory,
                            std::size_t expectedCount)
    {
        for (int iteration = 0; iteration < 16 && JsonFileCount(patchDirectory) < expectedCount;
             ++iteration)
        {
            PumpOnce();
        }
        Require(JsonFileCount(patchDirectory) == expectedCount,
                "browser patch command reaches expected version count");
    }

    float ProbeCenter()
    {
        return runtime.Engine().Manager()
            .ParameterById(runtime.Engine().Application().probeId)
            .SceneCenter(0);
    }

    void SetProbeCenter(float value)
    {
        runtime.Engine().Manager()
            .ParameterById(runtime.Engine().Application().probeId)
            .SceneCenter(0) = value;
    }

    const synth::RuntimeDataPaths& Paths() const { return paths_; }

    synth_browser::Runtime<ValidApp> runtime;

private:
    const std::filesystem::path dataRoot_ =
        std::filesystem::temp_directory_path() / "sheaf-browser-runtime-contract";
    const synth::RuntimeDataPaths paths_ = synth::RuntimeDataPaths::FromDataRoot(dataRoot_);
    std::uint64_t nextTimestamp_ = 2;
};

void TestBrowserRuntimeUsesSharedFrameAndActionRouting()
{
    RuntimeFixture fixture;

    synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    Require(FindNode(frame, "runtime.main.root") != nullptr,
            "browser frame contains shared runtime root");
    Require(FindNode(frame, "contract.app.root") != nullptr,
            "browser frame contains application root");
    Require(FindNode(frame, "contract.app.button") != nullptr,
            "browser frame contains application content");
    Require(FindNode(frame, "runtime.sidebar.root") != nullptr,
            "browser frame contains runtime sidebar");
    Require(FindNode(frame, "runtime.sidebar.audio") != nullptr,
            "browser frame contains audio navigation");

    fixture.runtime.DispatchAction("runtime.sidebar.audio", "");
    frame = fixture.Frame();
    Require(FindNode(frame, "runtime.audio.root") != nullptr,
            "audio action displays shared audio page");
    Require(FindNode(frame, "contract.app.root") == nullptr,
            "audio page replaces application content");
    Require(FindNode(frame, "runtime.sidebar.root") != nullptr,
            "sidebar remains visible beside audio page");

    fixture.runtime.DispatchAction("runtime.audio.back", "");
    frame = fixture.Frame();
    Require(FindNode(frame, "contract.app.root") != nullptr,
            "audio back restores application content");
    Require(FindNode(frame, "runtime.audio.root") == nullptr,
            "audio page is removed after back");

    fixture.runtime.DispatchAction("contract.app.apply", "17");
    const ContractSurface& surface = fixture.runtime.Engine().Application().surface;
    Require(surface.dispatchCount == 1, "application action dispatches exactly once");
    Require(surface.lastAction == "contract.app.apply", "application receives action name");
    Require(surface.lastValue == "17", "application receives action value");
}

void TestBrowserRuntimeMainComponentSidebarTracksResolvedAppWidth()
{
    synth::Engine<ExtentAwareContractApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    synth_browser::BrowserMidiBridge<synth::Engine<ExtentAwareContractApp>> bridge(engine);
    bridge.Start();
    std::vector<synth_browser::BrowserAudioDevice> audioDevices;
    synth_browser::BrowserRuntimeMainServices<ExtentAwareContractApp> services(engine, bridge, audioDevices);
    synth::runtime_ui::RuntimeMainComponent<ExtentAwareContractApp,
                                            synth_browser::BrowserRuntimeMainServices<ExtentAwareContractApp>>
        mainComponent(engine.Application(), services);

    // Default (never offered anything beyond compiled-in size): the browser
    // composition path places the sidebar at the resolved app width, same
    // as the pinned-width legacy rule (640 == config.uiWidth). The tree is
    // captured in a named local (not passed as a temporary) because
    // FindPortableNode returns a pointer into it that must outlive this
    // Require pair.
    const synth::ui::NodeTree initial = mainComponent.BuildTree();
    const synth::ui::Node* sidebarDefault = FindPortableNode(initial, "runtime.sidebar.root");
    Require(sidebarDefault != nullptr, "sidebar renders through the browser composition path");
    Require(NearlyEqual(sidebarDefault->bounds.x, 640.0f),
            "browser composition places the sidebar at the resolved app width");

    // The browser host offers a new live extent through the same
    // RuntimeMainComponent::SetContentExtent hook the JUCE renderer would
    // use; the resolved app width and sidebar placement track it identically
    // (backend parity -- same class, same composition arithmetic).
    mainComponent.SetContentExtent({0.0f, 0.0f, 800.0f, 480.0f});
    const synth::ui::NodeTree resized = mainComponent.BuildTree();
    const synth::ui::Node* appRoot = FindPortableNode(resized, "extent.contract.app.root");
    const synth::ui::Node* sidebarResized = FindPortableNode(resized, "runtime.sidebar.root");
    Require(appRoot != nullptr, "app root renders after the browser offers a new extent");
    Require(NearlyEqual(appRoot->bounds.width, 800.0f), "app resolves against the offered extent");
    Require(sidebarResized != nullptr, "sidebar renders after the browser offers a new extent");
    Require(NearlyEqual(sidebarResized->bounds.x, 800.0f),
            "browser composition tracks the resized extent identically to JUCE");

    // A vertical resize too -- the composite root
    // (runtime.main.root) height must track the resolved app height rather
    // than staying pinned to config.uiHeight (480). The composite root's
    // height used to stay hardcoded to config.uiHeight even on this
    // extent-aware branch, so this resize would validate (the validator only
    // checks the app root) and then throw inside RequireCompositionHolds,
    // because the taller app root no longer fit inside a 480-tall composite
    // root.
    mainComponent.SetContentExtent({0.0f, 0.0f, 800.0f, 600.0f});
    const synth::ui::NodeTree resizedTaller = mainComponent.BuildTree();
    const synth::ui::Node* appRootTaller = FindPortableNode(resizedTaller, "extent.contract.app.root");
    const synth::ui::Node* sidebarTaller = FindPortableNode(resizedTaller, "runtime.sidebar.root");
    const synth::ui::Node* compositeRootTaller = FindPortableNode(resizedTaller, "runtime.main.root");
    Require(appRootTaller != nullptr, "app root renders after the browser offers a taller extent");
    Require(NearlyEqual(appRootTaller->bounds.height, 600.0f),
            "app resolves against the taller offered extent");
    Require(sidebarTaller != nullptr, "sidebar renders after the browser offers a taller extent");
    Require(NearlyEqual(sidebarTaller->bounds.x, 800.0f),
            "sidebar x still tracks the resolved width, unaffected by the height-only change");
    Require(compositeRootTaller != nullptr,
            "composite root renders after the browser offers a taller extent");
    Require(NearlyEqual(compositeRootTaller->bounds.height, 600.0f),
            "composite root height tracks the resolved app height (600), not config.uiHeight "
            "(480) -- BuildTree() completing without throwing also proves the validator accepted "
            "the taller app root and RequireCompositionHolds held for both children");
}

void TestBrowserControllerDiscoveryCacheUsesSignalsAndSuccessfulCommits()
{
    synth::Engine<ValidApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    synth_browser::BrowserMidiBridge<synth::Engine<ValidApp>> bridge(engine);
    bridge.Start();
    std::vector<synth_browser::BrowserAudioDevice> audioDevices;
    synth_browser::BrowserRuntimeMainServices<ValidApp> services(engine, bridge, audioDevices);
    synth::runtime_ui::RuntimeMainComponent<ValidApp,
                                            synth_browser::BrowserRuntimeMainServices<ValidApp>>
        mainComponent(engine.Application(), services);
    synth::runtime_ui::ControllersPageSurface surface(
        services.MakeControllersCallbacks([] {}));

    mainComponent.Refresh();
    services.RefreshControllers(surface);
    const std::uint64_t initialRevision = surface.TreeRevision();
    bridge.SubmitEndpoints({
        {.identifier = "twister-input",
         .name = "Midi Fighter Twister",
         .kind = synth_browser::BrowserMidiBridge<synth::Engine<ValidApp>>::EndpointKind::Input},
        {.identifier = "twister-output",
         .name = "Midi Fighter Twister",
         .kind = synth_browser::BrowserMidiBridge<synth::Engine<ValidApp>>::EndpointKind::Output},
    });
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.Discovery().available.size() == 1,
            "real browser service classifies one changed device-list signal");
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") != nullptr,
            "unclaimed candidate warns through the production runtime sidebar");
    const std::uint64_t deviceRevision = surface.TreeRevision();
    Require(deviceRevision > initialRevision, "changed device signal revises the Controllers surface");

    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.TreeRevision() == deviceRevision,
            "unchanged browser source does not reclassify or revise the Controllers surface");

    synth::MfTwisterControllerWizard wizard;
    synth::MfTwisterConfigForm form;
    const synth::WizardGenerationResult generated = wizard.GenerateProfile(
        form,
        {.name = "claimed", .input = {"twister-input", "Midi Fighter Twister"},
         .output = {"twister-output", "Midi Fighter Twister"}});
    Require(static_cast<bool>(generated), "create a committed Twister record");
    synth::MidiInstrumentConfig committed;
    committed.controllers.push_back(*generated.controller);
    auto callbacks = services.MakeControllersCallbacks([] {});
    callbacks.commitInstrument(std::move(committed));
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.Discovery().available.empty(),
            "successful real-browser instrument commit reclassifies cached devices");
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") == nullptr,
            "successful claimed Active commit clears the production sidebar warning");
    Require(surface.TreeRevision() > deviceRevision,
            "successful instrument commit revises the Controllers surface");

    synth::MidiControllerSlot ignored = *generated.controller;
    ignored.disposition = synth::MidiControllerDisposition::Blacklisted;
    ignored.dormantConfig = ignored.config;
    ignored.config = {};
    synth::MidiInstrumentConfig blacklisted;
    blacklisted.controllers.push_back(std::move(ignored));
    callbacks.commitInstrument(std::move(blacklisted));
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.Discovery().available.empty(),
            "Blacklisted endpoint claim suppresses cached discovery");
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") == nullptr,
            "Blacklisted endpoint claim suppresses the production sidebar warning");

    callbacks.commitInstrument({});
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.Discovery().available.size() == 1,
            "removing Blacklisted claim restores cached availability without a device signal");
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") != nullptr,
            "remove from blacklist restores the production sidebar warning immediately");

    synth::MidiInstrumentConfig activeAgain;
    activeAgain.controllers.push_back(*generated.controller);
    callbacks.commitInstrument(std::move(activeAgain));
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") == nullptr,
            "reclaimed active pair clears the warning before delete");
    callbacks.commitInstrument({});
    mainComponent.Refresh();
    services.RefreshControllers(surface);
    Require(surface.Discovery().available.size() == 1,
            "deleting active claim restores cached availability without a device signal");
    Require(FindPortableNode(mainComponent.BuildTree(), "runtime.sidebar.controllers.warning") != nullptr,
            "delete restores the production sidebar warning immediately");
    bridge.Stop();
}

void TestWizardSubmitRefusesACandidateRemovedSinceTheLastFrame()
{
    RuntimeFixture fixture;
    using Bridge = synth_browser::BrowserMidiBridge<synth::Engine<ValidApp>>;
    const std::vector<Bridge::Endpoint> twisterPair = {
        {.identifier = "twister-in", .name = "Midi Fighter Twister", .kind = Bridge::EndpointKind::Input},
        {.identifier = "twister-out", .name = "Midi Fighter Twister", .kind = Bridge::EndpointKind::Output},
    };
    fixture.runtime.SubmitMidiEndpoints(twisterPair);
    fixture.runtime.MessageTick(2);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarControllers, "");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardOpen, "");
    const synth_browser::DecodedCommandBuffer openFrame = fixture.Frame();
    Require(FindNode(openFrame, "controller-wizard.twister.encoder-slot") != nullptr,
            "the unique candidate opens its wizard form");

    // The controller is unplugged and Submit is activated before the host
    // builds another frame. Submit must recheck that the candidate is
    // still present, so the cached classification follows the device-list
    // change itself rather than the next frame build.
    fixture.runtime.SubmitMidiEndpoints({});
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardSubmit, "");

    const synth_browser::DecodedCommandBuffer refusedFrame = fixture.Frame();
    const synth_browser::DecodedNode* status =
        FindNode(refusedFrame, synth::runtime_ui::NodeIds::kWizardStatus);
    Require(status != nullptr && status->text.find("reconnect") != std::string::npos,
            "Submit refuses a candidate removed since the last frame with a reconnect message");
    Require(FindNode(refusedFrame, "controller-wizard.twister.encoder-slot") != nullptr,
            "the refused wizard session stays open");
    Require(fixture.runtime.Engine().InstrumentSnapshot().FindController("MIDI Fighter Twister") == nullptr,
            "a refused stale Submit commits no controller");
    Require(!fixture.runtime.ConsumePersistenceDirty(),
            "a refused stale Submit saves no runtime configuration");
}

void TestBrowserPrepareFeedsNegotiatedAudioPageAndRejectsOversizedBlocks()
{
    RuntimeFixture fixture;

    bool rejected = false;
    try
    {
        fixture.runtime.Prepare(
            48000.0,
            static_cast<std::size_t>(std::numeric_limits<int>::max()) + std::size_t{1});
    }
    catch (const std::out_of_range&)
    {
        rejected = true;
    }
    Require(rejected, "runtime rejects a browser block size outside the engine range");

    fixture.Prepare(48000.0, 128);
    fixture.runtime.MessageTick(2);
    const ValidApp& app = fixture.runtime.Engine().Application();
    Require(app.preparedSampleRate == 48000.0, "prepare reaches the generic application");
    Require(app.preparedBlockSize == 128, "prepare preserves the negotiated block size");
    Require(fixture.runtime.Engine().Clock().OutputSchedulingHorizonMicros() == 25'000,
            "browser runtime configures the Web MIDI scheduling horizon before prepare");
    Require(fixture.runtime.Engine().Clock().OutputLatencyMicros() == 30'334,
            "browser runtime preserves the full base latency before its scheduling horizon");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarAudio, "");
    const synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    const synth_browser::DecodedNode* deviceLine =
        FindNode(frame, synth::runtime_ui::NodeIds::kAudioDeviceLine);
    Require(deviceLine != nullptr, "audio page exposes negotiated device line");
    Require(deviceLine->text == "System Default: 48000 Hz, 128 frames",
            "audio page reports the negotiated default output");
}

void TestNativeBuildRejectsBrowserAudioWorkletStart()
{
    RuntimeFixture fixture;
    Require(!fixture.runtime.StartAudioWorklet(),
            "native browser runtime cannot start a WebAudio worklet");
}

void TestSharedBrowserNavigationReplacesAndRestoresEveryRuntimePage()
{
    RuntimeFixture fixture;

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarControllers, "");
    synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kRoot) != nullptr,
            "controllers navigation displays shared controllers page");
    Require(FindNode(frame, "contract.app.root") == nullptr,
            "controllers page replaces application content");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kBack, "");
    frame = fixture.Frame();
    Require(FindNode(frame, "contract.app.root") != nullptr,
            "controllers Back restores application content");
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kRoot) == nullptr,
            "controllers page is removed after Back");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarFile, "");
    frame = fixture.Frame();
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kFileRoot) != nullptr,
            "file navigation displays shared file page");
    Require(FindNode(frame, "contract.app.root") == nullptr,
            "file page replaces application content");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kFileBack, "");
    frame = fixture.Frame();
    Require(FindNode(frame, "contract.app.root") != nullptr,
            "file Back restores application content");
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kFileRoot) == nullptr,
            "file page is removed after Back");
}

void TestBrowserSyncUsesSharedStagingPersistsAndResolvesSourceNames()
{
    RuntimeFixture fixture;
    fixture.Prepare(48'000.0, 128);
    fixture.runtime.MessageTick(2);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarSync, "");

    synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    const synth_browser::DecodedNode* ppqn =
        FindNode(frame, synth::runtime_ui::NodeIds::kSyncPpqn);
    const synth_browser::DecodedNode* source =
        FindNode(frame, synth::runtime_ui::NodeIds::kSyncSource);
    Require(ppqn != nullptr && ppqn->text == "24", "browser Sync opens at requested PPQN");
    Require(source != nullptr && source->text.find("Internal") != std::string::npos,
            "browser Sync has deterministic no-external-source status");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncSendClock, "1");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncReceiveClock, "1");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncSendTransport, "1");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncReceiveTransport, "1");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncPpqn, "96x");
    frame = fixture.Frame();
    ppqn = FindNode(frame, synth::runtime_ui::NodeIds::kSyncPpqn);
    Require(ppqn != nullptr && ppqn->text == "24",
            "browser invalid PPQN retains prior staged value");
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kSyncValidation)->text.find("1 to 960") !=
                std::string::npos,
            "browser invalid PPQN renders inline validation");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncPpqn, "96");
    frame = fixture.Frame();
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kSyncWarning)->text.find("nonstandard") !=
                std::string::npos,
            "browser valid non-24 PPQN renders compatibility warning before Back");
    Require(fixture.runtime.Engine().SyncConfigurationSnapshot() == synth::SyncConfig{},
            "browser staged edits do not commit before Back");

    const synth::SyncConfig committed{true, true, true, true, 96};
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSyncBack, "");
    Require(fixture.runtime.Engine().SyncConfigurationSnapshot() == committed,
            "browser Sync Back commits one complete requested config");
    Require(fixture.runtime.Engine().Clock().SyncConfiguration() == synth::SyncConfig{},
            "browser Sync Back does not mutate MasterClock before audio handoff");
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Sync Back marks persistence dirty");
    synth::MidiInstrumentConfig savedInstrument;
    synth::AudioDeviceState savedAudio;
    synth::SyncConfig savedSync;
    Require(synth::LoadRuntimeConfigFile(fixture.Paths().configFile,
                                         savedInstrument,
                                         savedAudio,
                                         savedSync) ==
                synth::RuntimeConfigFileStatus::Ok &&
                savedSync == committed,
            "browser Sync Back saves requested config before the next audio block");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarSync, "");
    frame = fixture.Frame();
    Require(FindNode(frame, synth::runtime_ui::NodeIds::kSyncPpqn)->text == "96",
            "browser Sync reopen starts from committed requested state");

    fixture.runtime.Process(nullptr, 0, 128, 10'000);
    Require(fixture.runtime.Engine().Clock().SyncConfiguration() == committed,
            "browser requested config applies at next audio block");
    Require(fixture.runtime.Engine().Clock().HandleExternalClock(10'100, 1),
            "test external source is accepted");
    fixture.runtime.Process(nullptr, 0, 128, 12'667);
    fixture.runtime.MessageTick(15'334);
    frame = fixture.Frame();
    source = FindNode(frame, synth::runtime_ui::NodeIds::kSyncSource);
    Require(source != nullptr && source->text == "Source: Controller B",
            "browser resolves active source slot to controller name off audio path");

    const synth::SyncConfig receiveOff{true, false, true, true, 96};
    Require(fixture.runtime.Engine().RequestSyncConfiguration(receiveOff),
            "test disables receive clock through Engine request");
    fixture.runtime.Process(nullptr, 0, 128, 18'001);
    Require(fixture.runtime.Engine().RequestSyncConfiguration(committed),
            "test re-enables receive clock through Engine request");
    fixture.runtime.Process(nullptr, 0, 128, 20'668);
    Require(fixture.runtime.Engine().Clock().HandleExternalClock(20'700, 99),
            "out-of-range source slot can be diagnosed");
    fixture.runtime.Process(nullptr, 0, 128, 23'335);
    fixture.runtime.MessageTick(26'002);
    frame = fixture.Frame();
    source = FindNode(frame, synth::runtime_ui::NodeIds::kSyncSource);
    Require(source != nullptr && source->text == "Source: External source slot 99",
            "browser uses deterministic out-of-range source fallback");
}

void TestControllersUseLatestBridgeSnapshotCommitEditsAndSaveOnBack()
{
    RuntimeFixture fixture;
    using Bridge = synth_browser::BrowserMidiBridge<synth::Engine<ValidApp>>;
    fixture.runtime.SubmitMidiEndpoints({
        {.identifier = "in-a", .name = "Input A", .kind = Bridge::EndpointKind::Input},
        {.identifier = "out-a", .name = "Output A", .kind = Bridge::EndpointKind::Output},
        {.identifier = "in-b", .name = "Input B", .kind = Bridge::EndpointKind::Input},
        {.identifier = "out-b", .name = "Output B", .kind = Bridge::EndpointKind::Output},
    });
    fixture.runtime.MessageTick(2);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarControllers, "");

    synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    const synth_browser::DecodedNode* inputA = FindNode(
        frame, synth::runtime_ui::NodeIds::ControllerInput(0).c_str());
    const synth_browser::DecodedNode* outputB = FindNode(
        frame, synth::runtime_ui::NodeIds::ControllerOutput(1).c_str());
    Require(inputA != nullptr && outputB != nullptr,
            "controllers snapshot contains both configured controller rows");
    Require(HasOption(*inputA, "in-a", "Input A") &&
                HasOption(*inputA, "in-b", "Input B"),
            "controller input uses latest multi-device enumeration");
    Require(HasOption(*outputB, "out-a", "Output A") &&
                HasOption(*outputB, "out-b", "Output B"),
            "controller output uses latest multi-device enumeration");
    Require(inputA->selectedOption == "in-a" && outputB->selectedOption == "out-b",
            "online connection snapshot selects each configured endpoint");

    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kEndpointSelect, "0:input:in-b");
    fixture.runtime.MessageTick(3);
    frame = fixture.Frame();
    inputA = FindNode(frame, synth::runtime_ui::NodeIds::ControllerInput(0).c_str());
    Require(inputA != nullptr && inputA->selectedOption == "in-b",
            "controller commit is visible after services dirty refresh");
    Require(fixture.runtime.Engine().InstrumentSnapshot().controllers[0].input.identifier ==
                "in-b",
            "controller edit commits through the browser services callback");

    fixture.runtime.SubmitMidiEndpoints({
        {.identifier = "in-a", .name = "Input A", .kind = Bridge::EndpointKind::Input},
        {.identifier = "out-a", .name = "Output A", .kind = Bridge::EndpointKind::Output},
        {.identifier = "in-b", .name = "Input B", .kind = Bridge::EndpointKind::Input},
        {.identifier = "out-b", .name = "Output B", .kind = Bridge::EndpointKind::Output},
        {.identifier = "twister-in", .name = "Midi Fighter Twister", .kind = Bridge::EndpointKind::Input},
        {.identifier = "twister-out", .name = "Midi Fighter Twister", .kind = Bridge::EndpointKind::Output},
    });
    fixture.runtime.MessageTick(4);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardOpen, "");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardSubmit, "");
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "successful browser wizard Submit immediately marks persisted runtime configuration dirty");
    synth::MidiInstrumentConfig wizardSavedInstrument;
    synth::AudioDeviceState wizardSavedAudio;
    synth::SyncConfig wizardSavedSync;
    Require(synth::LoadRuntimeConfigFile(
                fixture.Paths().configFile, wizardSavedInstrument, wizardSavedAudio, wizardSavedSync) ==
                synth::RuntimeConfigFileStatus::Ok &&
                wizardSavedInstrument.FindController("MIDI Fighter Twister") != nullptr,
            "browser wizard Submit saves the committed Twister profile through the real runtime path");

    std::string lastLifecycleAction;
    const auto dispatchNode = [&](const std::string& id, std::string suffix = {}) {
        const synth_browser::DecodedCommandBuffer frame = fixture.Frame();
        const synth_browser::DecodedNode* node = FindNode(frame, id.c_str());
        Require(node != nullptr && node->action.has_value(), "browser lifecycle node has a portable action");
        if (!node->enabled)
        {
            throw std::runtime_error("browser lifecycle node is disabled: " + id);
        }
        lastLifecycleAction = node->action->name + "=" + node->action->value + suffix;
        fixture.runtime.DispatchAction(node->action->name, node->action->value + suffix);
    };
    const auto requirePersisted = [&](std::size_t expectedCount, const char* label) {
        synth::MidiInstrumentConfig persisted;
        synth::AudioDeviceState audio;
        synth::SyncConfig sync;
        Require(synth::LoadRuntimeConfigFile(fixture.Paths().configFile, persisted, audio, sync) ==
                    synth::RuntimeConfigFileStatus::Ok &&
                    persisted.controllers.size() == expectedCount,
                label);
    };

    Require(FindNode(fixture.Frame(), synth::runtime_ui::NodeIds::ControllerRenameDraft(2).c_str()) == nullptr,
            "browser Rename draft field is absent while controller 2 is collapsed");
    Require(FindNode(fixture.Frame(), synth::runtime_ui::NodeIds::ControllerRename(2).c_str()) == nullptr,
            "browser Rename button is absent while controller 2 is collapsed");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kToggleConfig, "2");

    dispatchNode(synth::runtime_ui::NodeIds::ControllerRenameDraft(2), ":Browser Twister");
    const synth_browser::DecodedCommandBuffer renamedDraftFrame = fixture.Frame();
    if (FindNode(renamedDraftFrame, synth::runtime_ui::NodeIds::ControllerRenameDraft(2).c_str())->text !=
        "Browser Twister")
    {
        throw std::runtime_error("browser Rename draft does not retain text: " + lastLifecycleAction);
    }
    dispatchNode(synth::runtime_ui::NodeIds::ControllerRename(2));
    if (fixture.runtime.Engine().InstrumentSnapshot().controllers[2].name != "Browser Twister")
    {
        throw std::runtime_error("browser Rename reaches the production controller commit callback: " +
                                 lastLifecycleAction);
    }
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Rename immediately reports a real runtime-configuration save");
    requirePersisted(3, "browser Rename persists the renamed controller record");

    dispatchNode(synth::runtime_ui::NodeIds::ControllerBlacklist(2));
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Blacklist immediately reports a real runtime-configuration save");
    requirePersisted(3, "browser Blacklist persists the inert record");

    dispatchNode(synth::runtime_ui::NodeIds::ControllerConfigure(2));
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardSubmit, "");
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser blacklisted Configure immediately reports a real runtime-configuration save");
    requirePersisted(3, "browser blacklisted Configure persists its active replacement");

    dispatchNode(synth::runtime_ui::NodeIds::ControllerBlacklist(2));
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "second browser Blacklist saves before removal");
    dispatchNode(synth::runtime_ui::NodeIds::ControllerRemoveBlacklist(2));
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Remove from blacklist immediately reports a real runtime-configuration save");
    requirePersisted(2, "browser Remove from blacklist persists record deletion");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardOpen, "");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardSubmit, "");
    Require(fixture.runtime.ConsumePersistenceDirty(), "second browser wizard Submit saves before Delete");
    dispatchNode(synth::runtime_ui::NodeIds::ControllerDelete(2));
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Delete immediately reports a real runtime-configuration save");
    requirePersisted(2, "browser Delete persists record deletion");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardOpen, "");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kWizardIgnore, "");
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "browser Ignore immediately reports a real runtime-configuration save");
    requirePersisted(3, "browser Ignore persists the blacklisted record");

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kBack, "");
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "controllers Back marks browser persistence dirty after saving runtime configuration");
    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    Require(synth::LoadRuntimeConfigFile(
                fixture.Paths().configFile, loadedInstrument, loadedAudio, loadedSync) ==
                synth::RuntimeConfigFileStatus::Ok,
            "controllers Back persists runtime configuration");
    Require(loadedInstrument.controllers.size() == 3,
            "saved browser configuration retains every controller");
    Require(loadedInstrument.controllers[0].input.identifier == "in-b",
            "saved browser configuration contains the committed endpoint edit");
}

void TestFilePageDispatchesPatchLifecycleThroughBrowserRuntime()
{
    RuntimeFixture fixture;
    fixture.Prepare();
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarFile, "");

    const std::filesystem::path patchA = fixture.Paths().patchesRoot / "Patch A";
    const std::filesystem::path patchB = fixture.Paths().patchesRoot / "Patch B";

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kFileSave, "");
    fixture.PumpOnce();
    Require(!fixture.runtime.Engine().Patches().CurrentPatchDirectory().has_value(),
            "Save without a current patch preserves Save As requirement");
    Require(JsonFileCount(patchA) == 0, "Save without a current patch writes nothing");

    fixture.SetProbeCenter(0.4f);
    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kFileConfirmedSaveAs, patchA.string());
    fixture.PumpUntilJsonCount(patchA, 1);
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "completed browser Save As marks persistence dirty for host sync");
    Require(fixture.runtime.Engine().Patches().CurrentPatchDirectory() == patchA,
            "Save As selects the new patch directory");

    fixture.SetProbeCenter(0.5f);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kFileSave, "");
    fixture.PumpUntilJsonCount(patchA, 2);
    Require(fixture.runtime.ConsumePersistenceDirty(),
            "completed browser Save marks persistence dirty for host sync");

    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kFileConfirmedSaveAs, patchA.string());
    fixture.PumpOnce();
    Require(JsonFileCount(patchA) == 2,
            "non-overwrite Save As does not replace an existing patch");

    std::filesystem::create_directories(patchB);
    fixture.SetProbeCenter(0.6f);
    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kFileConfirmedOverwriteSaveAs, patchB.string());
    fixture.PumpUntilJsonCount(patchB, 1);
    Require(fixture.runtime.Engine().Patches().CurrentPatchDirectory() == patchB,
            "overwrite Save As selects the existing patch directory");

    fixture.SetProbeCenter(0.9f);
    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kFileConfirmedLoad, patchA.string());
    fixture.PumpOnce();
    Require(fixture.runtime.Engine().Patches().CurrentPatchDirectory() == patchA,
            "Load selects the requested patch directory");
    Require(fixture.ProbeCenter() == 0.5f, "Load applies the latest saved patch state");

    fixture.SetProbeCenter(0.8f);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kFileRevert, "");
    fixture.PumpOnce();
    Require(fixture.ProbeCenter() == 0.5f, "Revert restores the current patch state");

    fixture.SetProbeCenter(0.7f);
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kFileNew, "");
    fixture.PumpOnce();
    Require(!fixture.runtime.Engine().Patches().CurrentPatchDirectory().has_value(),
            "New clears the current patch directory");
    Require(fixture.ProbeCenter() == 0.25f, "New restores the generic app defaults");

    fixture.runtime.MessageTick(100);
    const synth_browser::DecodedCommandBuffer frame = fixture.Frame();
    const synth_browser::DecodedNode* patchName =
        FindNode(frame, synth::runtime_ui::NodeIds::kFilePatchName);
    Require(patchName != nullptr && patchName->text == "(no patch)",
            "file refresh reports the New patch state through the portable tree");
}

void TestPersistenceDirtyConsumesRuntimeAndServicesSourcesTogether()
{
    RuntimeFixture fixture;
    fixture.Prepare();

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarFile, "");
    const std::filesystem::path patchA = fixture.Paths().patchesRoot / "Patch A";
    fixture.SetProbeCenter(0.4f);
    fixture.runtime.DispatchAction(
        synth::runtime_ui::Actions::kFileConfirmedSaveAs, patchA.string());
    fixture.PumpUntilJsonCount(patchA, 1);

    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kSidebarControllers, "");
    fixture.runtime.DispatchAction(synth::runtime_ui::Actions::kBack, "");

    Require(fixture.runtime.ConsumePersistenceDirty(),
            "combined patch and config persistence dirties are reported once");
    Require(!fixture.runtime.ConsumePersistenceDirty(),
            "combined patch and config persistence dirties are consumed together");
}

void TestAudioWorkletDeadlineMeterAveragesQuantizedTimerSamples()
{
    synth_browser::AudioWorkletDeadlineMeter meter;
    for (int block = 0; block < 38; ++block)
    {
        meter.RecordCallbackMicros(block == 0 ? 1'000 : 0, 2'667);
    }

    const float percent = meter.SamplePercent();
    Require(percent > 0.8f && percent < 1.2f,
            "quantized callback timing is averaged over a window");
}

void TestBrowserContractVersionsAreReadableBeforeRuntimeCreation()
{
    Require(synth_browser_abi_version() == 6,
            "browser ABI v6 version is available before runtime creation");
    Require(synth_browser_ui_protocol_version() == 2,
            "browser UI protocol version is available before runtime creation");
    Require(synth_browser_ui_protocol_version() == synth_browser::kCommandBufferVersion,
            "the package's exported UI protocol version equals the command buffer version");
    Require(synth_browser_runtime_config_version() == 1,
            "browser runtime-config version is available before runtime creation");
}

void TestBrowserRuntimeDiscoversRequestedAudioInputChannels()
{
    synth_browser::RuntimeAbiAdapter<InputCountApp<0>> zero;
    synth_browser::RuntimeAbiAdapter<InputCountApp<4>> four;
    synth_browser::RuntimeAbiAdapter<InputCountApp<32>> thirtyTwo;

    Require(zero.AudioInputChannels() == 0,
            "browser ABI reports zero requested audio inputs");
    Require(four.AudioInputChannels() == 4,
            "browser ABI reports four requested audio inputs");
    Require(thirtyTwo.AudioInputChannels() == 32,
            "browser ABI reports the maximum requested audio inputs");
}

void TestBrowserRuntimeRejectsUnsupportedAudioInputCountsBeforeCapture()
{
    synth_browser::Runtime<InputCountApp<32>> supported;
    synth_browser::Runtime<InputCountApp<33>> unsupported;
    Require(supported.AudioWorkletConfigurationSupported(),
            "browser AudioWorklet host limit accepts exactly thirty-two requested inputs");
    Require(!unsupported.AudioWorkletConfigurationSupported(),
            "browser AudioWorklet host limit rejects thirty-three requested inputs");

    synth_browser::RuntimeAbiAdapter<InputCountApp<33>> tooMany;
    Require(tooMany.AudioInputChannels() == 33,
            "browser ABI exposes an unsupported requested input count for diagnosis");
    Require(tooMany.Initialize("sheaf", "too-many-inputs", 1) == 0,
            "oversized browser input test app initializes before audio capture");
    Require(tooMany.SetAudioInputSource(
                19,
                33,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)) == -1,
            "browser ABI rejects a source with more physical inputs than the native input ceiling");
    Require(tooMany.StartAudioWorklet(0) == -1,
            "browser ABI rejects oversized requested inputs before worklet startup");
}

void TestBrowserRuntimePreCreationNegativeInputIsZeroUntilInitializeRejects()
{
    synth_browser::RuntimeAbiAdapter<NegativeInputApp> negative;
    Require(negative.AudioInputChannels() == 0,
            "pre-creation browser ABI clamps a negative app constant to zero");
    Require(negative.Initialize("sheaf", "negative-inputs", 1) == -1,
            "shared RuntimeConfig validation rejects the negative input count at initialization");
}

void TestBrowserRuntimeDirectProcessPublishesImmutableRequestedInputCount()
{
    synth_browser::Runtime<RequestedInputCountProbeApp<33>> runtime;
    runtime.Start();
    runtime.Process(nullptr, 0, 64, 1);

    Require(runtime.Engine().Application().observedRequestedInputs == 33,
            "direct browser Process publishes the immutable app input request");
    Require(runtime.Engine().Application().observedActiveInputs == 0,
            "direct browser Process separately reports zero active input pointers");
}

class AudioContextHandleCapture final : public synth_browser::RuntimeAbi
{
public:
    std::size_t AudioOutputChannels() const override { return 0; }
    std::size_t AudioInputChannels() const override { return 4; }
    int Initialize(const char*, const char*, std::uint32_t) override { return 0; }
    int Prepare(double, std::size_t) override { return 0; }
    int Process(float**, std::size_t, std::size_t, std::uint64_t) override { return 0; }
    int StartAudioWorklet(std::uint32_t audioContextHandle) override
    {
        observedHandles.push_back(audioContextHandle);
        return 0;
    }
    std::uint32_t AudioWorkletBlockCount() const override { return 0; }
    std::uint32_t AudioWorkletPeakMicrounits() const override { return 0; }
    std::uint32_t AudioWorkletDeadlineMicrounits() const override { return 0; }
    int SetAudioInputSource(std::uint32_t sourceHandle,
                            std::uint32_t physicalChannels,
                            std::uint32_t statusCode) override
    {
        observedSources.push_back({sourceHandle, physicalChannels, statusCode});
        return 0;
    }
    int ClearAudioInputSource(std::uint32_t statusCode) override
    {
        observedClears.push_back(statusCode);
        return 0;
    }
    int ConsumePendingAudioRequest(std::uint32_t* outControl) override
    {
        ++retryConsumeCount;
        if (outControl != nullptr) {
            *outControl = static_cast<std::uint32_t>(synth_browser::BrowserAudioDeviceKind::Input);
        }
        return 1;
    }
    int SetTimestampEpochOffsetMicros(std::int64_t) override { return 0; }
    int MessageTick(std::uint64_t) override { return 0; }
    const std::uint8_t* BuildUiFrame(std::size_t*) override { return nullptr; }
    int DispatchAction(const char*, const char*) override { return 0; }
    bool ConsumePersistenceDirty() override { return false; }
    int SubmitMidiEndpoints(const synth_browser::MidiEndpointDescriptor*, std::uint32_t) override { return 0; }
    int SubmitAudioDevices(const synth_browser::AudioDeviceDescriptor*, std::uint32_t) override { return 0; }
    int DequeueMidiAction(synth_browser::MidiActionDescriptor*) override { return 0; }
    int DeliverMidi(std::uint32_t, const std::uint8_t*, std::uint32_t, std::uint64_t) override { return 0; }
    const std::uint8_t* DequeueMidiOutput(synth_browser::MidiOutputDescriptor*) override { return nullptr; }
    int MidiDiagnostics(synth_browser::MidiDiagnosticsDescriptor*) override { return 0; }
    void Destroy() override {}

    std::vector<std::uint32_t> observedHandles;
    struct Source
    {
        std::uint32_t handle = 0;
        std::uint32_t physicalChannels = 0;
        std::uint32_t statusCode = 0;
    };
    std::vector<Source> observedSources;
    std::vector<std::uint32_t> observedClears;
    int retryConsumeCount = 0;
};

// The input claim the realtime callback reads is published before the Web Audio
// graph is rewired, so the first callback after a successful connect already
// sees the right count. That ordering is only safe if a failed graph operation
// rolls the claim back: otherwise the callback and the Audio page would keep
// claiming active input for a source that is not connected to anything.
void TestBrowserAudioInputPublicationRollsBackFailedGraphReplacement()
{
    using synth_browser::BrowserAudioInputPublication;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto ended = static_cast<std::uint32_t>(BrowserAudioInputStatus::StreamEnded);
    const auto unavailable = static_cast<std::uint32_t>(BrowserAudioInputStatus::ApiUnavailable);
    const auto succeed = [](std::uint32_t) { return true; };

    BrowserAudioInputPublication publication;
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() ==
                    static_cast<std::uint32_t>(BrowserAudioInputStatus::NotRequested),
            "a fresh publication claims no input");
    Require(publication.ConnectedSourceHandle() == 0,
            "a fresh publication has nothing attached to the graph");

    // The graph operation observes the already-published claim: that is the
    // ordering guarantee, and it must hold on the success path.
    std::vector<std::uint32_t> disconnects;
    std::vector<std::uint32_t> connects;
    std::uint32_t observedChannelsDuringConnect = 0;
    const auto recordDisconnect = [&](std::uint32_t handle) {
        disconnects.push_back(handle);
        return true;
    };
    const auto recordConnect = [&](std::uint32_t handle) {
        connects.push_back(handle);
        observedChannelsDuringConnect = publication.PhysicalChannels();
        return synth_browser::BrowserAudioInputConnectResult::Connected;
    };

    Require(publication.Publish(91, 4, online, recordDisconnect, recordConnect),
            "a successful graph replacement keeps the published claim");
    Require(observedChannelsDuringConnect == 4,
            "the physical count is published before the graph is connected");
    Require(disconnects.empty() && connects == std::vector<std::uint32_t>{91},
            "the first publication connects the new source with nothing to disconnect");
    Require(publication.SourceHandle() == 91 && publication.PhysicalChannels() == 4 &&
                publication.StatusCode() == online &&
                publication.ConnectedSourceHandle() == 91,
            "a successful publication claims and attaches the registered source");

    // Re-publishing the same handle is not a graph change and must not disconnect
    // a live source and reconnect it.
    connects.clear();
    Require(publication.Publish(91, 2, online, recordDisconnect, recordConnect),
            "re-publishing an unchanged handle succeeds");
    Require(disconnects.empty() && connects.empty(),
            "re-publishing an unchanged handle performs no graph work");
    Require(publication.PhysicalChannels() == 2,
            "re-publishing an unchanged handle still republishes its count");

    // Clearing is the safe direction: the claim is dropped before the disconnect
    // is attempted, and a failed disconnect is still reported.
    std::uint32_t observedChannelsDuringDisconnect = 1;
    Require(publication.Clear(ended, [&](std::uint32_t source) {
                disconnects.push_back(source);
                observedChannelsDuringDisconnect = publication.PhysicalChannels();
                return true;
            }),
            "clearing a live source succeeds");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "clearing disconnects exactly the attached source");
    Require(observedChannelsDuringDisconnect == 0,
            "the active count is cleared before the source is disconnected");
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() == ended && publication.ConnectedSourceHandle() == 0,
            "clearing publishes the offline status with nothing claimed or attached");

    Require(publication.Clear(ended, [](std::uint32_t) { return false; }),
            "clearing an already-cleared publication is a no-op success");
    Require(publication.Publish(94, 1, online, recordDisconnect, recordConnect),
            "a source publishes after a clean teardown");
    Require(!publication.Clear(ended, [](std::uint32_t) { return false; }),
            "a failed disconnect is reported through the clear result");
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() == ended,
            "a failed disconnect still leaves nothing claimed");
    Require(publication.ConnectedSourceHandle() == 94,
            "a failed disconnect keeps the identity of the source still attached");
    (void)unavailable;
    (void)succeed;
}

// A disconnect that fails leaves the previous source attached to the worklet
// input bus. Forgetting it would let the next publication connect a second
// source alongside it, and the bus would sum both. So the publication stays
// blocked on that identity: nothing new is connected until the stuck source is
// verifiably gone.
void TestBrowserAudioInputPublicationBlocksNewConnectionsUntilTheOldSourceIsGone()
{
    using synth_browser::BrowserAudioInputPublication;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto unavailable = static_cast<std::uint32_t>(BrowserAudioInputStatus::ApiUnavailable);

    BrowserAudioInputPublication publication;
    std::vector<std::uint32_t> disconnects;
    std::vector<std::uint32_t> connects;
    const auto connect = [&](std::uint32_t handle) {
        connects.push_back(handle);
        return synth_browser::BrowserAudioInputConnectResult::Connected;
    };
    const auto disconnectFails = [&](std::uint32_t handle) {
        disconnects.push_back(handle);
        return false;
    };
    const auto disconnectSucceeds = [&](std::uint32_t handle) {
        disconnects.push_back(handle);
        return true;
    };

    Require(publication.Publish(91, 4, online, disconnectSucceeds, connect),
            "the first source attaches");
    connects.clear();
    disconnects.clear();

    Require(!publication.Publish(92, 2, online, disconnectFails, connect),
            "a failed disconnect of the attached source fails the publication");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "the failed publication tried to disconnect the attached source");
    Require(connects.empty(),
            "a source is never connected while the previous one is still attached");
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() == unavailable,
            "a blocked publication claims no active input");
    Require(publication.ConnectedSourceHandle() == 91,
            "a blocked publication remembers which source is still attached");

    disconnects.clear();
    Require(!publication.Publish(93, 1, online, disconnectFails, connect),
            "a second attempt while blocked also fails");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "the blocked publication retries the stuck source, not the abandoned one");
    Require(connects.empty(), "still nothing is connected while the bus is occupied");

    disconnects.clear();
    Require(publication.Publish(94, 3, online, disconnectSucceeds, connect),
            "publication resumes once the stuck source is verifiably disconnected");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "the stuck source is disconnected exactly once it can be");
    Require(connects == std::vector<std::uint32_t>{94},
            "exactly one source is connected, so nothing double-sums");
    Require(publication.SourceHandle() == 94 && publication.PhysicalChannels() == 3 &&
                publication.ConnectedSourceHandle() == 94,
            "the recovered publication claims and attaches the new source");
}

// A connect that fails is a different stage: the previous source is already
// gone, so nothing is attached and the next publication has nothing to clean up.
void TestBrowserAudioInputPublicationRollsBackAFailedConnectWithoutBlocking()
{
    using synth_browser::BrowserAudioInputPublication;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto unavailable = static_cast<std::uint32_t>(BrowserAudioInputStatus::ApiUnavailable);

    BrowserAudioInputPublication publication;
    std::vector<std::uint32_t> disconnects;
    std::vector<std::uint32_t> connects;
    const auto disconnect = [&](std::uint32_t handle) {
        disconnects.push_back(handle);
        return true;
    };
    const auto connectSucceeds = [&](std::uint32_t handle) {
        connects.push_back(handle);
        return synth_browser::BrowserAudioInputConnectResult::Connected;
    };
    const auto connectFails = [&](std::uint32_t handle) {
        connects.push_back(handle);
        return synth_browser::BrowserAudioInputConnectResult::Failed;
    };

    Require(publication.Publish(91, 4, online, disconnect, connectSucceeds),
            "the first source attaches");
    disconnects.clear();
    connects.clear();

    Require(!publication.Publish(92, 2, online, disconnect, connectFails),
            "a failed connect fails the publication");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "the previous source was disconnected before the failed connect");
    Require(connects == std::vector<std::uint32_t>{92}, "the failed connect was attempted once");
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() == unavailable,
            "a failed connect claims no active input");
    Require(publication.ConnectedSourceHandle() == 0,
            "a failed connect leaves nothing attached to clean up");

    disconnects.clear();
    connects.clear();
    Require(publication.Publish(93, 1, online, disconnect, connectSucceeds),
            "the next publication succeeds after a failed connect");
    Require(disconnects.empty(),
            "no stale disconnect is attempted for a source that never attached");
    Require(connects == std::vector<std::uint32_t>{93},
            "exactly one source is connected, so nothing double-sums");
}

// A source published before the worklet node exists is claimed but NOT attached
// to anything: `AudioWorkletProcessorCreated` attaches it later. Recording it as
// connected would make the eventual cleanup try to disconnect a source that was
// never in the graph, and a lookup failure there would retain its identity and
// block every later publication. So the pending intent and the confirmed
// attachment are separate, and a deferred connect that fails drops the intent
// outright rather than attempting a disconnect.
//
// This drives the real runtime: `SetAudioInputSource` before any worklet node,
// then the same `ResolveDeferredAudioInputConnection` that
// `AudioWorkletProcessorCreated` calls, then a replacement publication. A native
// build has no Web Audio graph, so the deferred connect genuinely cannot
// succeed -- which is exactly the failure stage under test.
void TestBrowserRuntimeDefersPreWorkletInputConnectionAndRecoversFromItsFailure()
{
    using synth_browser::BrowserAudioInputConnectResult;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);

    // Stands in for the Web Audio graph a native build does not have. Before
    // `AudioWorkletProcessorCreated` runs there is no node, so every connection
    // defers; afterwards source 91's attachment fails the way a lost
    // `emscriptenGetAudioObject` lookup does, and anything else attaches.
    bool workletNodeExists = false;
    std::vector<std::uint32_t> connects;
    std::vector<std::uint32_t> disconnects;
    int successfulConnections = 0;

    synth_browser::Runtime<InputCountApp<4>> runtime;
    runtime.SetAudioInputGraphForTesting(
        [&](std::uint32_t sourceHandle) {
            connects.push_back(sourceHandle);
            if (!workletNodeExists) {
                return BrowserAudioInputConnectResult::Deferred;
            }
            if (sourceHandle == 91) {
                return BrowserAudioInputConnectResult::Failed;
            }
            ++successfulConnections;
            return BrowserAudioInputConnectResult::Connected;
        },
        [&](std::uint32_t sourceHandle) {
            disconnects.push_back(sourceHandle);
            return true;
        });
    runtime.Start();

    Require(runtime.SetAudioInputSource(91, 4, online),
            "a source publishes before the worklet node exists");
    Require(runtime.AudioInputStateSnapshot().activeChannels == 4 &&
                runtime.AudioInputStateSnapshot().status == BrowserAudioInputStatus::Online,
            "the pre-worklet publication claims its channels for the callback");
    Require(runtime.PendingAudioInputSourceHandle() == 91,
            "the pre-worklet source is pending attachment");
    Require(runtime.ConnectedAudioInputSourceHandle() == 0,
            "a pending source is not recorded as attached to the graph");

    // The worklet node is created, and the deferred attachment of 91 fails.
    workletNodeExists = true;
    Require(!runtime.ResolveDeferredAudioInputConnection(),
            "a deferred attachment that cannot be made reports failure");
    Require(runtime.AudioInputStateSnapshot().activeChannels == 0 &&
                runtime.AudioInputStateSnapshot().status ==
                    BrowserAudioInputStatus::ApiUnavailable,
            "a failed deferred attachment publishes zero active channels and an offline status");
    Require(runtime.PendingAudioInputSourceHandle() == 0 &&
                runtime.ConnectedAudioInputSourceHandle() == 0,
            "a source that never attached leaves no identity behind to clean up");
    Require(disconnects.empty(),
            "the runtime never asks the graph to detach a source it never attached");

    // The replacement attaches for real, through the same production path.
    Require(runtime.SetAudioInputSource(92, 2, online),
            "a replacement publishes after a failed deferred attachment");
    Require(runtime.ConnectedAudioInputSourceHandle() == 92 &&
                runtime.PendingAudioInputSourceHandle() == 0,
            "the replacement is attached, not left pending behind the failed source");
    Require(runtime.AudioInputStateSnapshot().activeChannels == 2 &&
                runtime.AudioInputStateSnapshot().status == BrowserAudioInputStatus::Online,
            "the attached replacement claims its own channels");
    Require(connects == std::vector<std::uint32_t>{91, 91, 92},
            "91 was offered before and after node creation, then 92 exactly once");
    Require(successfulConnections == 1,
            "exactly one source is ever connected, so nothing double-sums into the bus");
    Require(disconnects.empty(),
            "recovery attempts no disconnect of the never-attached source");

    Require(runtime.ClearAudioInputSource(
                static_cast<std::uint32_t>(BrowserAudioInputStatus::StreamEnded)),
            "clearing the attached replacement succeeds");
    Require(disconnects == std::vector<std::uint32_t>{92},
            "clearing detaches exactly the source that was attached");
    Require(runtime.PendingAudioInputSourceHandle() == 0 &&
                runtime.ConnectedAudioInputSourceHandle() == 0,
            "clearing leaves nothing claimed, pending, or attached");
}

void TestBrowserRuntimeDeferredAttachFailureKeepsOutputUiAndMidiLive()
{
    using synth_browser::BrowserAudioInputConnectResult;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);

    bool workletNodeExists = false;
    std::vector<std::uint32_t> connects;
    std::vector<std::uint32_t> disconnects;

    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.SetAudioInputGraphForTesting(
        [&](std::uint32_t sourceHandle) {
            connects.push_back(sourceHandle);
            return workletNodeExists ? BrowserAudioInputConnectResult::Failed
                                     : BrowserAudioInputConnectResult::Deferred;
        },
        [&](std::uint32_t sourceHandle) {
            disconnects.push_back(sourceHandle);
            return true;
        });
    runtime.Start();

    Require(runtime.SetAudioInputSource(91, 4, online),
            "pre-worklet capture publishes before native node creation");
    workletNodeExists = true;
    Require(!runtime.ResolveDeferredAudioInputConnection(),
            "persistent deferred attachment failure is reported to the control path");

    const auto state = runtime.AudioInputStateSnapshot();
    Require(state.activeChannels == 0 && state.status == BrowserAudioInputStatus::ApiUnavailable,
            "persistent deferred attachment failure rolls active input back to offline");
    Require(runtime.PendingAudioInputSourceHandle() == 0 &&
                runtime.ConnectedAudioInputSourceHandle() == 0,
            "failed deferred attachment leaves no source identity attached or pending");
    Require(disconnects.empty(),
            "failed deferred attachment never disconnects a source that was not attached");

    std::array<float, 8> outputs{};
    synth_browser::BrowserAudioSampleFrameDescriptor outputDescriptor{
        .numberOfChannels = 2,
        .samplesPerChannel = 4,
        .data = outputs.data(),
    };
    Require(runtime.ProcessAudioWorkletPlanarBlock(0, nullptr, 1, &outputDescriptor, 10),
            "worklet callback remains live after deferred input attachment failure");
    Require(runtime.AudioWorkletBlockCount() == 1,
            "worklet block count advances after deferred input attachment failure");
    for (float sample : outputs)
    {
        Require(NearlyEqual(sample, 0.0f),
                "output remains safe silence when failed deferred input is rolled back");
    }

    runtime.MessageTick(11);
    const synth_browser::DecodedCommandBuffer frame =
        synth_browser::DecodeCommandBuffer(runtime.BuildUiFrame().bytes);
    Require(FindNode(frame, "runtime.main.root") != nullptr,
            "UI frame building remains live after deferred input attachment failure");
    runtime.SubmitMidiEndpoints({});
    (void)runtime.MidiDiagnosticsSnapshot();
}

void TestBrowserRuntimeSuccessfulDeferredAttachStaysConnectedUntilClear()
{
    using synth_browser::BrowserAudioInputConnectResult;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto ended = static_cast<std::uint32_t>(BrowserAudioInputStatus::StreamEnded);

    bool workletNodeExists = false;
    std::vector<std::uint32_t> connects;
    std::vector<std::uint32_t> disconnects;

    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.SetAudioInputGraphForTesting(
        [&](std::uint32_t sourceHandle) {
            connects.push_back(sourceHandle);
            return workletNodeExists ? BrowserAudioInputConnectResult::Connected
                                     : BrowserAudioInputConnectResult::Deferred;
        },
        [&](std::uint32_t sourceHandle) {
            disconnects.push_back(sourceHandle);
            return true;
        });
    runtime.Start();

    Require(runtime.SetAudioInputSource(91, 4, online),
            "pre-worklet capture publishes before native node creation");
    workletNodeExists = true;
    Require(runtime.ResolveDeferredAudioInputConnection(),
            "deferred attachment succeeds once the native node exists");
    Require(runtime.ConnectedAudioInputSourceHandle() == 91 &&
                runtime.PendingAudioInputSourceHandle() == 0,
            "successful deferred attachment is recorded as connected");
    Require(runtime.AudioInputStateSnapshot().activeChannels == 4 &&
                runtime.AudioInputStateSnapshot().status == BrowserAudioInputStatus::Online,
            "successful deferred attachment keeps the active input claim online");
    Require(disconnects.empty(),
            "successful deferred attachment is not spuriously released");

    std::array<float, 16> inputs{};
    for (std::size_t frame = 0; frame < 4; ++frame)
    {
        inputs[frame] = 0.25f;
        inputs[4 + frame] = 0.5f;
        inputs[12 + frame] = -0.75f;
    }
    std::array<float, 8> outputs{};
    synth_browser::BrowserAudioSampleFrameDescriptor inputDescriptor{
        .numberOfChannels = 4,
        .samplesPerChannel = 4,
        .data = inputs.data(),
    };
    synth_browser::BrowserAudioSampleFrameDescriptor outputDescriptor{
        .numberOfChannels = 2,
        .samplesPerChannel = 4,
        .data = outputs.data(),
    };
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, &inputDescriptor, 1, &outputDescriptor, 20),
            "worklet callback processes a successfully attached deferred source");
    Require(runtime.AudioWorkletBlockCount() == 1,
            "worklet block count advances with a successfully attached deferred source");
    Require(NearlyEqual(outputs[0], 0.75f) && NearlyEqual(outputs[4], -0.75f),
            "successfully attached deferred source reaches application DSP");
    Require(disconnects.empty(),
            "processing a connected deferred source does not release it");

    Require(runtime.ClearAudioInputSource(ended),
            "clearing the successful deferred source succeeds");
    Require(disconnects == std::vector<std::uint32_t>{91},
            "clearing disconnects the successful deferred source exactly once");
}

// External-input-routed signal (sar-33): the browser derivation. No granted
// capture is not routed (including a fresh, just-started runtime); a granted
// source (SetAudioInputSource, the grant point BrowserRuntimeAbi.cpp's
// synth_browser_set_audio_input_source calls through to,
// BrowserRuntime.hpp's SetAudioInputSource) routes it with a callback;
// clearing that source (the revoke point, ClearAudioInputSource) un-routes it
// with another callback. Identical semantic to the JUCE side
// (RuntimeShellSessionTests.cpp's CheckInputRoutedSignal): "a user-chosen
// input source is open and delivering." The browser host has no ThreadId
// tagging to assert against (unlike the JUCE side), so this instead pins the
// weaker but still load-bearing contract directly: the callback runs
// synchronously on the calling thread (this test's thread, standing in for
// the browser message/main thread that SetAudioInputSource/
// ClearAudioInputSource are always invoked from), by comparing
// std::this_thread::get_id() captured at test start against the id observed
// inside the callback.
void TestBrowserRuntimeInputRoutedSignalTracksCaptureGrantAndRevoke()
{
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto streamEnded = static_cast<std::uint32_t>(BrowserAudioInputStatus::StreamEnded);
    const std::thread::id callingThreadId = std::this_thread::get_id();

    synth_browser::Runtime<InputCountApp<4>> runtime;
    runtime.Start();

    synth::AppContext& context = runtime.Engine().Context();
    Require(!context.InputRouted(), "a freshly started runtime with no granted capture is not routed");

    std::vector<bool> notifications;
    std::vector<std::thread::id> notificationThreadIds;
    context.SetInputRoutedChangedCallback([&](bool routed) {
        notifications.push_back(routed);
        notificationThreadIds.push_back(std::this_thread::get_id());
    });

    Require(runtime.SetAudioInputSource(91, 4, online), "a granted capture source publishes");
    Require(context.InputRouted(), "a granted capture routes the signal");
    Require(notifications.size() == 1 && notifications[0] == true,
            "the change callback fires exactly once on the granting edge");
    Require(notificationThreadIds[0] == callingThreadId,
            "the granting-edge callback runs synchronously on the calling thread");

    Require(runtime.ClearAudioInputSource(streamEnded), "revoking the granted capture clears it");
    Require(!context.InputRouted(), "revoking capture un-routes the signal");
    Require(notifications.size() == 2 && notifications[1] == false,
            "the change callback fires exactly once on the revoking edge");
    Require(notificationThreadIds[1] == callingThreadId,
            "the revoking-edge callback runs synchronously on the calling thread");
}

// The rule the runtime path depends on, asserted directly: a source that was
// only ever pending is never handed to `disconnect`.
void TestBrowserAudioInputPublicationNeverDisconnectsANeverAttachedSource()
{
    using synth_browser::BrowserAudioInputConnectResult;
    using synth_browser::BrowserAudioInputPublication;
    using synth_browser::BrowserAudioInputStatus;
    const auto online = static_cast<std::uint32_t>(BrowserAudioInputStatus::Online);
    const auto ended = static_cast<std::uint32_t>(BrowserAudioInputStatus::StreamEnded);
    const auto unavailable = static_cast<std::uint32_t>(BrowserAudioInputStatus::ApiUnavailable);

    BrowserAudioInputPublication publication;
    std::vector<std::uint32_t> disconnects;
    // Fails loudly if a never-attached source is ever offered for disconnect.
    const auto disconnect = [&](std::uint32_t handle) {
        disconnects.push_back(handle);
        return false;
    };
    const auto deferConnect = [](std::uint32_t) { return BrowserAudioInputConnectResult::Deferred; };

    Require(publication.Publish(91, 4, online, disconnect, deferConnect),
            "a deferred connection publishes its claim");
    Require(publication.PendingSourceHandle() == 91 && publication.ConnectedSourceHandle() == 0,
            "a deferred connection is pending, not attached");

    // A replacement arriving before the deferred one attaches supersedes it.
    Require(publication.Publish(92, 2, online, disconnect, deferConnect),
            "a replacement supersedes a pending source");
    Require(disconnects.empty(),
            "superseding a never-attached source attempts no disconnect");
    Require(publication.PendingSourceHandle() == 92, "the replacement is now the pending source");

    // The deferred attachment fails: nothing was ever attached, so nothing is
    // disconnected and nothing is left blocking the next publication.
    Require(!publication.ResolvePendingConnection([](std::uint32_t) { return false; }),
            "a failed deferred attachment reports failure");
    Require(disconnects.empty(), "a failed deferred attachment attempts no disconnect");
    Require(publication.SourceHandle() == 0 && publication.PhysicalChannels() == 0 &&
                publication.StatusCode() == unavailable,
            "a failed deferred attachment publishes an offline claim");
    Require(publication.PendingSourceHandle() == 0 && publication.ConnectedSourceHandle() == 0,
            "a failed deferred attachment leaves no identity behind");

    std::vector<std::uint32_t> connects;
    Require(publication.Publish(93, 1, online, disconnect, [&](std::uint32_t handle) {
                connects.push_back(handle);
                return BrowserAudioInputConnectResult::Connected;
            }),
            "the next source connects after a failed deferred attachment");
    Require(disconnects.empty(), "no stale disconnect is attempted for a never-attached source");
    Require(connects == std::vector<std::uint32_t>{93},
            "exactly one source is connected, so nothing double-sums");
    Require(publication.ConnectedSourceHandle() == 93 && publication.PendingSourceHandle() == 0,
            "a confirmed attachment is recorded as connected, not pending");

    // Promotion: a deferred source that does attach becomes the connected one.
    Require(publication.Clear(ended, [](std::uint32_t) { return true; }),
            "the connected source clears cleanly");
    Require(publication.Publish(94, 1, online, disconnect, deferConnect),
            "a further source publishes as pending");
    Require(publication.ResolvePendingConnection([](std::uint32_t) { return true; }),
            "a deferred attachment that succeeds is promoted");
    Require(publication.ConnectedSourceHandle() == 94 && publication.PendingSourceHandle() == 0,
            "promotion moves the identity from pending to connected");
    Require(publication.SourceHandle() == 94 && publication.PhysicalChannels() == 1,
            "promotion leaves the published claim intact");
    Require(publication.ResolvePendingConnection([](std::uint32_t) { return false; }),
            "resolving with nothing pending is a no-op success");

    // Clearing a source that is only pending drops it without a disconnect, so
    // the fail-loudly disconnect above must still never be reached.
    BrowserAudioInputPublication pendingOnly;
    Require(pendingOnly.Publish(95, 1, online, disconnect, deferConnect),
            "a source publishes as pending");
    Require(pendingOnly.Clear(ended, disconnect),
            "clearing a pending source needs no disconnect and succeeds");
    Require(disconnects.empty(), "clearing a pending source attempts no disconnect");
    Require(pendingOnly.PendingSourceHandle() == 0 && pendingOnly.ConnectedSourceHandle() == 0,
            "clearing drops the pending intent with the claim");
}

void TestBrowserAbiPreservesSuppliedAudioContextHandleAndDirectZero()
{
    AudioContextHandleCapture capture;
    auto* runtime = reinterpret_cast<synth_browser_runtime*>(&capture);
    Require(synth_browser_start_audio_worklet(runtime, 0) == 0,
            "direct audio startup reaches the ABI");
    Require(synth_browser_start_audio_worklet(runtime, 73) == 0,
            "supplied-context audio startup reaches the ABI");
    Require(capture.observedHandles == std::vector<std::uint32_t>{0, 73},
            "browser ABI preserves direct zero and the supplied context handle");
}

void TestBrowserAbiCarriesAudioInputSourceLifecycle()
{
    AudioContextHandleCapture capture;
    auto* runtime = reinterpret_cast<synth_browser_runtime*>(&capture);
    Require(synth_browser_audio_input_channels(runtime) == 4,
            "browser ABI reports requested audio inputs through the C facade");
    Require(synth_browser_set_audio_input_source(
                runtime,
                91,
                4,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)) == 0,
            "browser ABI forwards an online native input source");
    Require(synth_browser_clear_audio_input_source(
                runtime,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::StreamEnded)) == 0,
            "browser ABI forwards input source clear status");
    std::uint32_t consumedControl = 7;  // poisoned: only a real write proves the ABI touched it
    Require(synth_browser_consume_pending_audio_request(runtime, &consumedControl) == 1 &&
                capture.retryConsumeCount == 1 &&
                consumedControl == static_cast<std::uint32_t>(synth_browser::BrowserAudioDeviceKind::Input),
            "browser ABI forwards pending audio request consumption and its control");
    Require(capture.observedSources.size() == 1 &&
                capture.observedSources[0].handle == 91 &&
                capture.observedSources[0].physicalChannels == 4 &&
                capture.observedSources[0].statusCode ==
                    static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online),
            "browser ABI preserves source handle, physical channel count, and status");
    Require(capture.observedClears ==
                std::vector<std::uint32_t>{
                    static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::StreamEnded)},
            "browser ABI preserves clear status");
}

void TestBrowserAudioWorkletAdaptsPlanarInputAndOutput()
{
    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.Start();
    runtime.Prepare(48000.0, 3);
    Require(runtime.SetAudioInputSource(
                71,
                4,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)),
            "browser runtime accepts a four-channel native source");

    float input[12] = {
        0.10f, 0.20f, 0.30f,
        1.00f, 2.00f, 3.00f,
        9.00f, 9.00f, 9.00f,
        0.01f, 0.02f, 0.03f,
    };
    float output[6] = {};
    const synth_browser::BrowserAudioSampleFrameDescriptor inputs[] = {
        {.numberOfChannels = 4, .samplesPerChannel = 3, .data = input},
    };
    synth_browser::BrowserAudioSampleFrameDescriptor outputs[] = {
        {.numberOfChannels = 2, .samplesPerChannel = 3, .data = output},
    };

    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 1000),
            "browser runtime processes one planar AudioWorklet block");
    const BrowserCallbackObservation& observation =
        runtime.Engine().Application().observations.back();
    Require(observation.frames == 3,
            "browser callback uses the actual output frame count");
    Require(observation.requestedInputs == 4 && observation.activeInputs == 4,
            "browser callback reports requested and active input channels together");
    Require(NearlyEqual(output[0], 1.10f) &&
                NearlyEqual(output[1], 2.20f) &&
                NearlyEqual(output[2], 3.30f),
            "browser callback maps planar input channels to output channel zero");
    Require(NearlyEqual(output[3], 0.01f) &&
                NearlyEqual(output[4], 0.02f) &&
                NearlyEqual(output[5], 0.03f),
            "browser callback maps planar input channel three to output channel one");
    runtime.Stop();
}

void TestBrowserAudioWorkletUsesDescriptorFramesIndependentOfPreparedBlockSize()
{
    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.Start();
    runtime.Prepare(48000.0, 128);
    Require(runtime.SetAudioInputSource(
                75,
                4,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)),
            "browser runtime accepts a source before short descriptor callbacks");

    float input[256] = {};
    float output[128] = {};
    const synth_browser::BrowserAudioSampleFrameDescriptor inputs[] = {
        {.numberOfChannels = 4, .samplesPerChannel = 64, .data = input},
    };
    synth_browser::BrowserAudioSampleFrameDescriptor outputs[] = {
        {.numberOfChannels = 2, .samplesPerChannel = 64, .data = output},
    };

    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 2100),
            "browser runtime processes the first short descriptor block");
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 2200),
            "browser runtime processes the second short descriptor block");
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 2300),
            "browser runtime processes the third short descriptor block");

    const auto& observations = runtime.Engine().Application().observations;
    Require(observations.size() == 3,
            "browser callback records every short descriptor block");
    Require(observations[0].frames == 64 && observations[1].frames == 64 &&
                observations[2].frames == 64,
            "browser callback uses descriptor frame counts instead of prepared block size");
    Require(observations[0].startSample == 0 && observations[1].startSample == 64 &&
                observations[2].startSample == 128,
            "browser callback advances startSample by each descriptor frame count");
    runtime.Stop();
}

void TestBrowserAudioWorkletInputClampingClearingAndStartSamples()
{
    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.Start();
    runtime.Prepare(48000.0, 5);

    float input[30] = {
        0.10f, 0.20f, 0.30f, 0.40f, 0.50f,
        1.00f, 2.00f, 3.00f, 4.00f, 5.00f,
        7.00f, 7.00f, 7.00f, 7.00f, 7.00f,
        0.01f, 0.02f, 0.03f, 0.04f, 0.05f,
        8.00f, 8.00f, 8.00f, 8.00f, 8.00f,
        9.00f, 9.00f, 9.00f, 9.00f, 9.00f,
    };
    float output[10] = {};
    const synth_browser::BrowserAudioSampleFrameDescriptor inputs[] = {
        {.numberOfChannels = 6, .samplesPerChannel = 5, .data = input},
    };
    synth_browser::BrowserAudioSampleFrameDescriptor outputs[] = {
        {.numberOfChannels = 2, .samplesPerChannel = 5, .data = output},
    };

    Require(runtime.SetAudioInputSource(
                72,
                2,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)),
            "browser runtime accepts a short physical input source");
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 2000),
            "browser runtime processes a physically clamped input block");
    Require(runtime.Engine().Application().observations.back().activeInputs == 2,
            "browser callback clamps active inputs to the published physical count");
    Require(NearlyEqual(output[5], 0.0f),
            "browser callback supplies silence for requested channels beyond the physical source");

    Require(runtime.SetAudioInputSource(
                73,
                32,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)),
            "browser runtime accepts the maximum physical input source");
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 3000),
            "browser runtime processes a requested-count clamped input block");
    Require(runtime.Engine().Application().observations.back().activeInputs == 4,
            "browser callback clamps active inputs to the app request");
    Require(runtime.Engine().Application().observations.back().startSample == 5,
            "browser callback advances startSample by the first output frame count");

    Require(runtime.ClearAudioInputSource(
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::StreamEnded)),
            "browser runtime clears a stale native input source");
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, inputs, 1, outputs, 4000),
            "browser runtime processes safely after input source clear");
    const BrowserCallbackObservation& cleared =
        runtime.Engine().Application().observations.back();
    Require(cleared.requestedInputs == 4 && cleared.activeInputs == 0,
            "browser callback preserves requested count but clears active input count");
    Require(cleared.startSample == 10,
            "browser callback keeps monotonic startSample after clear");
    Require(NearlyEqual(output[0], 0.0f) && NearlyEqual(output[5], 0.0f),
            "browser callback produces silence after clearing stale input");
    runtime.Stop();
}

void TestBrowserAudioWorkletTreatsNullInputAsSafeSilence()
{
    synth_browser::Runtime<BrowserCallbackProbeApp> runtime;
    runtime.Start();
    runtime.Prepare(48000.0, 2);
    Require(runtime.SetAudioInputSource(
                74,
                4,
                static_cast<std::uint32_t>(synth_browser::BrowserAudioInputStatus::Online)),
            "browser runtime accepts a source before null-input callback");

    float output[4] = {5.0f, 5.0f, 5.0f, 5.0f};
    synth_browser::BrowserAudioSampleFrameDescriptor outputs[] = {
        {.numberOfChannels = 2, .samplesPerChannel = 2, .data = output},
    };
    Require(runtime.ProcessAudioWorkletPlanarBlock(1, nullptr, 1, outputs, 5000),
            "browser runtime processes a null input descriptor as silence");
    const BrowserCallbackObservation& observation =
        runtime.Engine().Application().observations.back();
    Require(observation.requestedInputs == 4 && observation.activeInputs == 0,
            "browser callback reports no active channels for null input");
    Require(output[0] == 0.0f && output[1] == 0.0f &&
                output[2] == 0.0f && output[3] == 0.0f,
            "browser callback overwrites prior output with safe silence for null input");
    runtime.Stop();
}

void TestBrowserAudioWorkletSilencesOutputWhenProcessingFails()
{
    synth_browser::Runtime<ThrowingBrowserCallbackApp> runtime;
    runtime.Start();
    runtime.Prepare(48000.0, 3);
    float output[3] = {9.0f, 8.0f, 7.0f};
    synth_browser::BrowserAudioSampleFrameDescriptor outputs[] = {
        {.numberOfChannels = 1, .samplesPerChannel = 3, .data = output},
    };

    Require(runtime.ProcessAudioWorkletPlanarBlock(0, nullptr, 1, outputs, 6000),
            "browser runtime keeps the AudioWorklet alive after process failure");
    Require(output[0] == 0.0f && output[1] == 0.0f && output[2] == 0.0f,
            "browser runtime silences output after process failure");
    runtime.Stop();
}

void TestNativeAudioCallbackRemainsTheSoleProcessImplementation()
{
    const std::filesystem::path candidates[] = {
        "projects/synth/include/synth/browser/BrowserRuntime.hpp",
        "include/synth/browser/BrowserRuntime.hpp",
    };
    std::string source;
    for (const auto& candidate : candidates)
    {
        std::ifstream input(candidate);
        if (input)
        {
            source.assign(std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>());
            break;
        }
    }
    Require(!source.empty(), "browser runtime source is available to the contract test");
    const std::string callback = "static bool ProcessAudioWorklet";
    const auto first = source.find(callback);
    Require(first != std::string::npos && source.find(callback, first + callback.size()) == std::string::npos,
            "ProcessAudioWorklet remains the sole native callback implementation");
    Require(source.find("runtime->ProcessAudioWorkletPlanarBlock(", first) != std::string::npos,
            "ProcessAudioWorklet delegates DSP through the native planar input adapter");
    Require(source.find("AdaptBrowserAudioWorkletPlanarBlock(") != std::string::npos,
            "browser native input adapter remains available to JUCE-free tests");
}

void TestBrowserRuntimeAdapterRejectsIncompatibleRuntimeConfigVersion()
{
    synth_browser::RuntimeAbiAdapter<ValidApp> runtime;
    Require(runtime.Initialize("sheaf", "miniapp", 2) == -1,
            "browser runtime adapter rejects incompatible runtime-config versions");
}

void TestBrowserPersistenceIdentityDerivesSharedAndIsolatedRoots()
{
    const synth::RuntimeDataPaths first =
        synth_browser::BrowserPersistentDataPaths("sheaf", "miniapp", 1);
    const synth::RuntimeDataPaths updated =
        synth_browser::BrowserPersistentDataPaths("sheaf", "miniapp", 1);
    const synth::RuntimeDataPaths otherPublisher =
        synth_browser::BrowserPersistentDataPaths("friend", "miniapp", 1);

    Require(first.dataRoot == "/data", "browser persistence mounts the shared /data root");
    Require(first.configFile == "/data/config.json", "browser runtime config is shared");
    Require(first.logsRoot == "/data/logs", "browser logs root is shared");
    Require(first.patchesRoot == "/data/patches/sheaf/miniapp",
            "browser patches use publisher and app identity");
    Require(updated.patchesRoot == first.patchesRoot,
            "browser patch root is stable across builds because build identity is absent");
    Require(otherPublisher.patchesRoot == "/data/patches/friend/miniapp",
            "publishers with the same app id have distinct patch roots");
    Require(otherPublisher.configFile == first.configFile,
            "compatible applications share browser runtime config");

    bool rejectedTraversal = false;
    try
    {
        (void)synth_browser::BrowserPersistentDataPaths("../sheaf", "miniapp", 1);
    }
    catch (const std::invalid_argument&)
    {
        rejectedTraversal = true;
    }
    Require(rejectedTraversal, "browser persistence rejects invalid publisher path components");

    bool rejectedVersion = false;
    try
    {
        (void)synth_browser::BrowserPersistentDataPaths("sheaf", "miniapp", 2);
    }
    catch (const std::invalid_argument&)
    {
        rejectedVersion = true;
    }
    Require(rejectedVersion, "browser persistence rejects incompatible runtime-config versions");
}

void TestMidiOutputDescriptorHasStableWasmLayout()
{
    static_assert(std::is_standard_layout_v<synth_browser::MidiOutputDescriptor>);
    static_assert(sizeof(synth_browser::MidiOutputDescriptor) == 24);
    static_assert(offsetof(synth_browser::MidiOutputDescriptor, controllerIx) == 0);
    static_assert(offsetof(synth_browser::MidiOutputDescriptor, size) == 4);
    static_assert(offsetof(synth_browser::MidiOutputDescriptor, delivery) == 8);
    static_assert(offsetof(synth_browser::MidiOutputDescriptor, dueTimeMicros) == 16);

    const synth_browser::MidiOutputDescriptor scheduled{
        .controllerIx = 7,
        .size = 1,
        .delivery = 1,
        .dueTimeMicros = 9'876'543,
    };
    Require(scheduled.controllerIx == 7 && scheduled.size == 1,
            "MIDI output ABI retains controller and byte count");
    Require(scheduled.delivery == 1 && scheduled.dueTimeMicros == 9'876'543,
            "MIDI output ABI retains scheduled delivery and absolute deadline");
}

void TestMidiDiagnosticsDescriptorAndTimestampEpochOffsetContract()
{
    static_assert(std::is_standard_layout_v<synth_browser::MidiDiagnosticsDescriptor>);
    static_assert(sizeof(synth_browser::MidiDiagnosticsDescriptor) == 24);
    static_assert(offsetof(synth_browser::MidiDiagnosticsDescriptor,
                           droppedImmediateOutputCount) == 0);
    static_assert(offsetof(synth_browser::MidiDiagnosticsDescriptor,
                           droppedScheduledOutputCount) == 8);
    static_assert(offsetof(synth_browser::MidiDiagnosticsDescriptor,
                           lateScheduledOutputCount) == 16);

    synth_browser::Runtime<ValidApp> runtime;
    runtime.SetTimestampEpochOffsetMicros(250'000);
    Require(runtime.TimestampEpochOffsetMicros() == 250'000,
            "runtime retains the signed document-to-worker epoch offset before startup");
}

}  // namespace

int main()
{
    static_assert(synth::SynthApplication<ValidApp>);
    static_assert(synth::SynthApplication<InputCountApp<4>>);
    static_assert(synth::SynthApplication<BrowserCallbackProbeApp>);
    static_assert(synth::SynthApplication<ExtentAwareContractApp>);
    static_assert(synth_browser::BrowserApplication<ValidApp>);
    static_assert(!synth_browser::BrowserApplication<MissingSurface>);
    static_assert(!synth::SynthApplication<MissingSurface>);
    TestBrowserContractVersionsAreReadableBeforeRuntimeCreation();
    TestBrowserRuntimeDiscoversRequestedAudioInputChannels();
    TestBrowserRuntimeRejectsUnsupportedAudioInputCountsBeforeCapture();
    TestBrowserRuntimePreCreationNegativeInputIsZeroUntilInitializeRejects();
    TestBrowserRuntimeDirectProcessPublishesImmutableRequestedInputCount();
    TestBrowserAudioInputPublicationRollsBackFailedGraphReplacement();
    TestBrowserAudioInputPublicationBlocksNewConnectionsUntilTheOldSourceIsGone();
    TestBrowserAudioInputPublicationRollsBackAFailedConnectWithoutBlocking();
    TestBrowserAudioInputPublicationNeverDisconnectsANeverAttachedSource();
    TestBrowserRuntimeInputRoutedSignalTracksCaptureGrantAndRevoke();
    TestBrowserRuntimeDefersPreWorkletInputConnectionAndRecoversFromItsFailure();
    TestBrowserRuntimeDeferredAttachFailureKeepsOutputUiAndMidiLive();
    TestBrowserRuntimeSuccessfulDeferredAttachStaysConnectedUntilClear();
    TestBrowserAbiPreservesSuppliedAudioContextHandleAndDirectZero();
    TestBrowserAbiCarriesAudioInputSourceLifecycle();
    TestBrowserAudioWorkletAdaptsPlanarInputAndOutput();
    TestBrowserAudioWorkletUsesDescriptorFramesIndependentOfPreparedBlockSize();
    TestBrowserAudioWorkletInputClampingClearingAndStartSamples();
    TestBrowserAudioWorkletTreatsNullInputAsSafeSilence();
    TestBrowserAudioWorkletSilencesOutputWhenProcessingFails();
    TestNativeAudioCallbackRemainsTheSoleProcessImplementation();
    TestBrowserRuntimeAdapterRejectsIncompatibleRuntimeConfigVersion();
    TestBrowserPersistenceIdentityDerivesSharedAndIsolatedRoots();
    TestBrowserRuntimeUsesSharedFrameAndActionRouting();
    TestBrowserRuntimeMainComponentSidebarTracksResolvedAppWidth();
    TestBrowserPrepareFeedsNegotiatedAudioPageAndRejectsOversizedBlocks();
    TestNativeBuildRejectsBrowserAudioWorkletStart();
    TestSharedBrowserNavigationReplacesAndRestoresEveryRuntimePage();
    TestBrowserSyncUsesSharedStagingPersistsAndResolvesSourceNames();
    TestControllersUseLatestBridgeSnapshotCommitEditsAndSaveOnBack();
    TestBrowserControllerDiscoveryCacheUsesSignalsAndSuccessfulCommits();
    TestWizardSubmitRefusesACandidateRemovedSinceTheLastFrame();
    TestFilePageDispatchesPatchLifecycleThroughBrowserRuntime();
    TestPersistenceDirtyConsumesRuntimeAndServicesSourcesTogether();
    TestAudioWorkletDeadlineMeterAveragesQuantizedTimerSamples();
    TestMidiOutputDescriptorHasStableWasmLayout();
    TestMidiDiagnosticsDescriptorAndTimestampEpochOffsetContract();
    return 0;
}
