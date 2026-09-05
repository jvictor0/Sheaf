#include "synth/AppRegistry.hpp"
#include "synth/Engine.hpp"
#include "synth/RuntimePagePolicy.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth engine tests must not see JUCE headers"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace engine_allocation_probe {

std::atomic<bool> enabled{false};
std::atomic<std::size_t> count{0};

}  // namespace engine_allocation_probe

void* operator new(std::size_t size) {
    if (engine_allocation_probe::enabled.load(std::memory_order_relaxed)) {
        engine_allocation_probe::count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*fn)()) {
        Registry().push_back({name, fn});
    }
};

#define TEST_CASE(name) \
    void name(); \
    Register reg_##name(#name, &name); \
    void name()

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << __FILE__ << ":" << __LINE__ << " requirement failed: " #expr; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (false)

void RequireNear(float actual, float expected, float tolerance, const char* expr) {
    if (std::fabs(actual - expected) > tolerance) {
        std::ostringstream oss;
        oss << expr << " expected " << expected << " got " << actual;
        throw std::runtime_error(oss.str());
    }
}

#define REQUIRE_NEAR(actual, expected, tolerance) RequireNear((actual), (expected), (tolerance), #actual)

std::size_t gScheduledEventCount = 0;

struct EngineScheduledEventSink final : synth::IScheduledMidiEventSink {
    static constexpr std::size_t kCapacity = 512;
    std::array<synth::ScheduledMidiEvent, kCapacity> events{};
    std::size_t size = 0;

    bool TryEnqueue(const synth::ScheduledMidiEvent& event) noexcept override {
        if (size >= events.size()) {
            return false;
        }
        events[size++] = event;
        gScheduledEventCount = size;
        return true;
    }
};

struct EngineMidiOutputSink final : synth::IMidiOutputSink {
    synth::MidiSchedulingCapability SchedulingCapability() const noexcept override {
        return synth::MidiSchedulingCapability::HostTimestamped;
    }

    void Send(const synth::BasicMidi& midi) override {
        std::lock_guard lock(mutex);
        delivered.push_back(midi);
    }

    void SendScheduled(const synth::BasicMidi& midi, std::uint64_t dueTimeMicros) override {
        std::lock_guard lock(mutex);
        delivered.push_back(midi);
        deadlines.push_back(dueTimeMicros);
    }

    std::mutex mutex;
    std::vector<synth::BasicMidi> delivered;
    std::vector<std::uint64_t> deadlines;
};

struct EngineTestApp {
    static inline bool sawNullUiStateDuringInit = false;
    static inline int initCalls = 0;
    static inline double preparedSampleRate = 0.0;
    static inline int preparedBlockSize = 0;
    // processLiteAlpha is configurable per-test via this static, read by
    // Init() when building the parameter group (must be set before
    // constructing the Engine, since Init() runs during Engine::Initialize).
    static inline float processLiteAlpha = 1.0f;
    // When set, Init() installs a minimal encoder MIDI-input profile so
    // RebuildMidiProcessors() produces a non-null, freshly-allocated
    // MidiInProcessor each time it runs (tests that need to observe rebuild
    // identity/ordering set this before constructing the Engine; default
    // false keeps every other test's profile empty, as before).
    static inline bool wantEncoderMidiInput = false;
    synth::AppContext* context = nullptr;
    synth::ParameterId probeId = 0;
    synth::BankSlot* probeSlot = nullptr;
    synth::Bank* emptyBank = nullptr;
    int processBlockCalls = 0;
    float lastProbeDuringBlock = -1.0f;
    float lastProbeSceneCenterDuringBlock = -1.0f;
    std::uint64_t lastBlockStartSample = 0;
    const synth::ClockBlockPlan* lastClockPlan = nullptr;
    synth::ClockPlanDescriptor lastClockDescriptor{};
    bool planStableDuringBlock = false;
    bool clockPreparedDuringPrepare = false;
    std::size_t scheduledEventsDuringBlock = 0;

    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "EngineTest";
        config.numAudioOutputs = 2;
        return config;
    }
    void Init(synth::AppContext* ctx) {
        ++initCalls;
        context = ctx;
        sawNullUiStateDuringInit = (ctx->uiState == nullptr);
        if (wantEncoderMidiInput && ctx->instrument != nullptr) {
            synth::MidiControllerSlot slot;
            slot.name = "test";
            slot.kind = synth::MidiProfileKind::Generic;
            slot.config.encoderInput = synth::EncoderMidiInConfig{};
            ctx->instrument->AddController(std::move(slot));
        }
        auto& group = ctx->parameterManager->CreateGroup({.numVoices = 1,
                                                           .numModulators = 0,
                                                           .numScenes = 1,
                                                           .maxParameters = 4,
                                                           .processLiteAlpha = processLiteAlpha,
                                                           .targetCenterAlpha = 1.0f});
        auto& probe = ctx->parameterManager->CreateParameter(group, {.name = "Probe", .defaultValue = 0.25f});
        probeId = probe.Id();

        // Bank/slot routing so MessageIn::ParamIncDec(slotIx=0, position=0, delta)
        // reaches the probe parameter, matching the miniapp's CreateBank /
        // CreateBankSlot / AddPhysicalEncoder / AddMapping / SelectBank shape.
        auto& bank = ctx->parameterManager->CreateBank();
        bank.AddMapping(/*encoderId=*/0, probe);
        probeSlot = &ctx->parameterManager->CreateBankSlot();
        probeSlot->AddPhysicalEncoder(/*encoderId=*/0);
        probeSlot->SelectBank(&bank);
        emptyBank = &ctx->parameterManager->CreateBank();
    }
    void PrepareToPlay(double sampleRate, int blockSize) {
        preparedSampleRate = sampleRate;
        preparedBlockSize = blockSize;
        clockPreparedDuringPrepare = context != nullptr && context->masterClock != nullptr &&
                                   context->masterClock->IsPrepared();
    }
    void ProcessBlock(synth::AudioBlock& block) {
        ++processBlockCalls;
        lastBlockStartSample = block.startSample;
        lastClockPlan = block.clockPlan;
        const synth::ClockPlanDescriptor before = block.clockPlan != nullptr
            ? block.clockPlan->Descriptor()
            : synth::ClockPlanDescriptor{};
        scheduledEventsDuringBlock = gScheduledEventCount;
        for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
            context->parameterManager->ParameterById(probeId).ProcessLite();
        }
        // Read after the per-frame slewing above. The engine pump applies
        // patch/UI/MIDI messages before calling into the app, but it no
        // longer recomputes parameter targets at the host block boundary.
        auto& probe = context->parameterManager->ParameterById(probeId);
        lastProbeDuringBlock = probe.CurrentCenter();
        lastProbeSceneCenterDuringBlock = probe.SceneCenter(0);
        for (int channel = 0; channel < block.numOutputChannels; ++channel) {
            float* out = block.outputs[channel];
            if (out == nullptr) {
                continue;
            }
            for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
                out[frame] = 0.5f;
            }
        }
        lastClockDescriptor = before;
        planStableDuringBlock = block.clockPlan != nullptr && block.clockPlan->Descriptor().startSample == before.startSample &&
                                block.clockPlan->Descriptor().endSample == before.endSample &&
                                block.clockPlan->Descriptor().generation == before.generation;
    }
};

class InitTopologyCell final : public synth::Cell {
public:
    void OnPress(std::uint8_t) override {}
    void OnRelease() override {}
    void OnPressureChange(std::uint8_t) override {}
    synth::Color GetColor() const override { return synth::Color::Rgb(10, 20, 30); }
    bool GetOnOff() const override { return true; }
};

struct InitTopologyApp {
    static inline bool sawGridManagerDuringInit = false;
    static inline bool declaredTopologyDuringInit = false;
    static inline std::size_t declaredSlotIx = 0;

    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "InitTopology";
        return config;
    }

    void Init(synth::AppContext* ctx) {
        sawGridManagerDuringInit = ctx->gridManager != nullptr;
        if (ctx->gridManager == nullptr) {
            return;
        }

        const auto range = synth::GridRange::Create(0, 1, 0, 1);
        if (!range.has_value()) {
            return;
        }
        const auto gridIx = ctx->gridManager->CreateGrid(*range);
        const auto slotIx = ctx->gridManager->CreateSlot(*range);
        if (!gridIx.has_value() || !slotIx.has_value()) {
            return;
        }
        if (!ctx->gridManager->GridAt(*gridIx)->RegisterCell(
                0, 0, std::make_unique<InitTopologyCell>())) {
            return;
        }
        if (!ctx->gridManager->SelectGridForSlot(*slotIx, *gridIx)) {
            return;
        }
        declaredSlotIx = *slotIx;
        declaredTopologyDuringInit = true;
    }

    void ProcessBlock(synth::AudioBlock&) {}
};

// Builds a patch JSON document (matching EngineTestApp's Init topology, i.e.
// a single group with the "Probe" parameter) with Probe set to probeValue,
// and writes it as a version file in patchDir via SavePatchVersionInDirectory
// at the given time point. Some tests append legacy audio/instrument sections
// after BuildPatchJSON to prove patch application ignores old runtime config
// embedded in patch files.
void WriteProbePatchVersion(const std::filesystem::path& patchDir, float probeValue,
                            std::chrono::system_clock::time_point when,
                            const synth::AudioDeviceState& audioDevice = {},
                            const synth::MidiInstrumentConfig& instrument = {}) {
    synth::ParameterManager scratchManager;
    auto& group = scratchManager.CreateGroup(
        {.numVoices = 1, .numModulators = 0, .numScenes = 1, .maxParameters = 4, .processLiteAlpha = 1.0f});
    const synth::ParameterId probeId = scratchManager.RegisterParameter(group, {.name = "Probe", .defaultValue = 0.25f});
    scratchManager.ParameterById(probeId).SceneCenter(0) = probeValue;
    scratchManager.CaptureDefaultControlState();
    scratchManager.ComputeAllParameters();

    synth::JsonArena arena(64 * 1024);
    synth::JSON root = synth::BuildPatchJSON(arena, "Probe Patch", scratchManager, instrument, audioDevice);
    if (!instrument.controllers.empty()) {
        root.SetNew("midiInstrument", synth::ToJSON(arena, instrument));
    }
    if (!audioDevice.outputDeviceName.empty() || !audioDevice.inputDeviceName.empty()) {
        root.SetNew("audioDevice", synth::ToJSON(arena, audioDevice));
    }
    REQUIRE_TRUE(!root.IsNull());
    char* dumped = root.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);
    const std::string jsonText(dumped);
    std::free(dumped);

    synth::SavePatchVersionInDirectory(patchDir, jsonText, when);
}

synth::MidiInstrumentConfig MakeRuntimeConfigInstrument(std::string name) {
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot slot;
    slot.name = std::move(name);
    slot.kind = synth::MidiProfileKind::Generic;
    slot.config.encoderInput = synth::EncoderMidiInConfig{};
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    return instrument;
}

void WriteRuntimeConfigFile(const std::filesystem::path& configFile,
                            const synth::MidiInstrumentConfig& instrument,
                            const synth::AudioDeviceState& audioDevice,
                            const synth::SyncConfig& sync = {}) {
    const synth::RuntimeConfigFileStatus status =
        synth::SaveRuntimeConfigFile(configFile, instrument, audioDevice, sync);
    REQUIRE_TRUE(status == synth::RuntimeConfigFileStatus::Ok);
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE_TRUE(static_cast<bool>(in));
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

}  // namespace

TEST_CASE(engine_initialize_orders_init_before_ui_state) {
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    REQUIRE_TRUE(EngineTestApp::sawNullUiStateDuringInit);
    REQUIRE_TRUE(engine.Context().uiState != nullptr);
    REQUIRE_TRUE(EngineTestApp::initCalls >= 1);
}

TEST_CASE(engine_exposes_grid_topology_declaration_during_init) {
    InitTopologyApp::sawGridManagerDuringInit = false;
    InitTopologyApp::declaredTopologyDuringInit = false;

    synth::Engine<InitTopologyApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();

    REQUIRE_TRUE(InitTopologyApp::sawGridManagerDuringInit);
    REQUIRE_TRUE(InitTopologyApp::declaredTopologyDuringInit);
    REQUIRE_TRUE(engine.GridManagerForTest().Finalized());
    const synth::RuntimeUIState& state = engine.RuntimeUIStateForTest();
    REQUIRE_TRUE(state.grids != nullptr);
    REQUIRE_TRUE(state.grids->slots.size() == 1);
    REQUIRE_TRUE(state.grids->slots[InitTopologyApp::declaredSlotIx]->colors.size() == 1);
    REQUIRE_TRUE(state.grids->slots[InitTopologyApp::declaredSlotIx]->colors[0].Load() ==
                 synth::Color::Rgba(10, 20, 30, 1));
}

TEST_CASE(engine_prepare_forwards_negotiated_values) {
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(44100.0, 128);
    REQUIRE_NEAR(static_cast<float>(EngineTestApp::preparedSampleRate), 44100.0f, 1e-3f);
    REQUIRE_TRUE(EngineTestApp::preparedBlockSize == 128);
}

TEST_CASE(engine_full_concept_rejects_ui_less_core) {
    REQUIRE_TRUE(synth::SynthApplicationCore<EngineTestApp>);
    REQUIRE_TRUE(!synth::SynthApplication<EngineTestApp>);
}

TEST_CASE(engine_missing_patches_root_keeps_defaults_silently) {
    const std::filesystem::path dataRoot = std::filesystem::temp_directory_path() / "engine-no-such-data-root";
    std::filesystem::remove_all(dataRoot);
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(synth::RuntimeDataPaths::FromDataRoot(dataRoot));
    engine.Initialize();  // must not throw or report failure
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.25f, 1e-5f);
}

TEST_CASE(engine_startup_loads_lexicographically_latest_patch) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-startup-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);

    const std::filesystem::path dirAAA = paths.patchesRoot / "AAA";
    const std::filesystem::path dirZZZ = paths.patchesRoot / "ZZZ";

    // "AAA" gets the numerically later time point (greater version filename);
    // "ZZZ" gets the earlier one. The rule is greatest VERSION FILENAME wins,
    // not directory name, so "AAA" (0.75) must win over "ZZZ" (0.5) despite
    // "ZZZ" sorting later alphabetically as a directory name.
    const auto earlier = std::chrono::system_clock::from_time_t(1700000000);
    const auto later = earlier + std::chrono::seconds(1);

    WriteProbePatchVersion(dirZZZ, 0.5f, earlier);
    WriteProbePatchVersion(dirAAA, 0.75f, later);

    const auto latestDir = synth::Engine<EngineTestApp>::LatestPatchDirectory(paths.patchesRoot);
    REQUIRE_TRUE(latestDir.has_value());
    REQUIRE_TRUE(latestDir->filename() == "AAA");

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.75f, 1e-5f);

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_initialize_loads_runtime_config_before_midi_processors_and_then_startup_patch) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-runtime-config-before-midi-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);

    const synth::MidiInstrumentConfig runtimeInstrument = MakeRuntimeConfigInstrument("runtime-loaded");
    const synth::AudioDeviceState runtimeAudio{.outputDeviceName = "Runtime Output", .inputDeviceName = "Runtime Input"};
    WriteRuntimeConfigFile(paths.configFile, runtimeInstrument, runtimeAudio);

    const synth::MidiInstrumentConfig legacyPatchInstrument = MakeRuntimeConfigInstrument("legacy-patch");
    WriteProbePatchVersion(paths.patchesRoot / "PatchA", 0.75f, std::chrono::system_clock::now(),
                           synth::AudioDeviceState{.outputDeviceName = "Patch Output", .inputDeviceName = "Patch Input"},
                           legacyPatchInstrument);

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();

    REQUIRE_TRUE(engine.DefaultInstrument().controllers.empty());
    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "runtime-loaded");
    REQUIRE_TRUE(engine.MidiControllerCount() == 1);
    REQUIRE_TRUE(engine.MidiInputProcessor(0) != nullptr);
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "Runtime Output");
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().inputDeviceName == "Runtime Input");
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.75f, 1e-5f);

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_loaded_sync_precedes_first_processor_rebuild_prepare_and_first_block) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-runtime-sync-startup-order";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    const synth::SyncConfig loadedSync{
        .sendClock = true,
        .receiveClock = true,
        .sendTransport = true,
        .receiveTransport = true,
        .ppqn = 96,
    };
    WriteRuntimeConfigFile(paths.configFile, MakeRuntimeConfigInstrument("sync-loaded"), {}, loadedSync);

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{5000}; });
    engine.SetRuntimeDataPaths(paths);
    bool rebuildSawLoadedSync = false;
    engine.SetMidiProcessorsWillRebuildCallback([&] {
        rebuildSawLoadedSync = engine.Clock().SyncConfiguration() == loadedSync;
    });
    engine.Initialize();
    REQUIRE_TRUE(rebuildSawLoadedSync);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == loadedSync);
    REQUIRE_TRUE(engine.SyncConfigurationSnapshot() == loadedSync);

    EngineScheduledEventSink sink;
    gScheduledEventCount = 0;
    engine.SetScheduledMidiEventSink(&sink);
    engine.Prepare(44100.0, 17);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == loadedSync);
    REQUIRE_TRUE(engine.Clock().SampleRate() == 44100.0);
    REQUIRE_TRUE(engine.Clock().BlockSize() == 17);

    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Start(5000)));
    std::array<float, 17> left{};
    std::array<float, 17> right{};
    float* outputs[] = {left.data(), right.data()};
    synth::AudioBlock block;
    block.outputs = outputs;
    block.numOutputChannels = 2;
    block.numFrames = 17;
    engine.ProcessBlock(block, 5000);
    REQUIRE_TRUE(block.clockPlan == engine.Clock().CurrentPlan());
    REQUIRE_TRUE(block.clockPlan->StartSample() == 0);
    REQUIRE_TRUE(block.clockPlan->EndSample() == 17);
    REQUIRE_TRUE(block.clockPlan->TransportState() == synth::ClockTransportState::ArmedStart);
    REQUIRE_TRUE(sink.size > 0);

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_initialize_ignores_invalid_runtime_config_and_still_loads_startup_patch) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-invalid-runtime-config-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);

    {
        std::ofstream out(paths.configFile, std::ios::binary | std::ios::trunc);
        out << R"({"schema":"wrong","schemaVersion":1})";
    }
    WriteProbePatchVersion(paths.patchesRoot / "PatchA", 0.7f, std::chrono::system_clock::now());

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "test");
    REQUIRE_TRUE(engine.MidiControllerCount() == 1);
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.7f, 1e-5f);

    std::filesystem::remove_all(dataRoot);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(engine_initialize_treats_missing_runtime_config_as_defaults_and_still_loads_startup_patch) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-missing-runtime-config-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);
    WriteProbePatchVersion(paths.patchesRoot / "PatchA", 0.8f, std::chrono::system_clock::now());

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "test");
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.8f, 1e-5f);

    std::filesystem::remove_all(dataRoot);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(engine_save_runtime_configuration_snapshots_current_midi_and_audio_state) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-save-runtime-config-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "saved-controller"));
    });
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Saved Output",
                                                          .inputDeviceName = "Saved Input"});

    const synth::RuntimeConfigFileStatus saveStatus = engine.SaveRuntimeConfiguration();
    REQUIRE_TRUE(saveStatus == synth::RuntimeConfigFileStatus::Ok);

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    const synth::RuntimeConfigFileStatus loadStatus =
        synth::LoadRuntimeConfigFile(paths.configFile, loadedInstrument, loadedAudio, loadedSync);
    REQUIRE_TRUE(loadStatus == synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers.front().name == "saved-controller");
    REQUIRE_TRUE(loadedAudio.outputDeviceName == "Saved Output");
    REQUIRE_TRUE(loadedAudio.inputDeviceName == "Saved Input");

    std::filesystem::remove_all(dataRoot);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(engine_runtime_page_back_policy_saves_config_for_audio_and_controllers_only) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-runtime-page-back-save-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();

    auto simulateBack = [&](synth::RuntimePageKind page) {
        if (synth::RuntimePageBackSavesConfiguration(page)) {
            REQUIRE_TRUE(engine.SaveRuntimeConfiguration() == synth::RuntimeConfigFileStatus::Ok);
        }
    };

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "audio-back-controller"));
    });
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Audio Back Output",
                                                          .inputDeviceName = "Audio Back Input"});
    simulateBack(synth::RuntimePageKind::Audio);

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigFile(paths.configFile, loadedInstrument, loadedAudio, loadedSync) ==
                 synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(loadedInstrument.controllers.front().name == "audio-back-controller");
    REQUIRE_TRUE(loadedAudio.outputDeviceName == "Audio Back Output");
    REQUIRE_TRUE(loadedAudio.inputDeviceName == "Audio Back Input");

    std::filesystem::remove(paths.configFile);
    simulateBack(synth::RuntimePageKind::File);
    REQUIRE_TRUE(!std::filesystem::exists(paths.configFile));

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "controllers-back-controller"));
    });
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Controllers Back Output",
                                                          .inputDeviceName = "Controllers Back Input"});
    simulateBack(synth::RuntimePageKind::Controllers);

    REQUIRE_TRUE(synth::LoadRuntimeConfigFile(paths.configFile, loadedInstrument, loadedAudio, loadedSync) ==
                 synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(loadedInstrument.controllers.front().name == "controllers-back-controller");
    REQUIRE_TRUE(loadedAudio.outputDeviceName == "Controllers Back Output");
    REQUIRE_TRUE(loadedAudio.inputDeviceName == "Controllers Back Input");

    std::filesystem::remove_all(dataRoot);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(engine_runtime_configuration_load_and_save_status_are_logged) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-runtime-config-logging-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);

    synth::AsyncLogQueue& log = synth::AsyncLogQueue::s_instance;
    log.ResetForTesting();
    log.SetLogDirectoryForTesting(paths.logsRoot.string().c_str());

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();
    REQUIRE_TRUE(engine.SaveRuntimeConfiguration() == synth::RuntimeConfigFileStatus::Ok);

    log.DoLog();
    std::ifstream in(log.LogFilePathForTesting());
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE_TRUE(contents.find("Runtime config load status=Missing") != std::string::npos);
    REQUIRE_TRUE(contents.find(paths.configFile.string()) != std::string::npos);
    REQUIRE_TRUE(contents.find("Runtime config save status=Ok") != std::string::npos);

    log.ResetForTesting();
    std::filesystem::remove_all(dataRoot);
}

namespace {

// Builds a numFrames-frame, numOutputChannels-channel AudioBlock backed by
// caller-owned storage (the vector-of-vectors below), matching AudioBlock's
// non-owning-pointer contract.
struct TestBlockBuffers {
    std::vector<std::vector<float>> channels;
    std::vector<float*> outputPointers;

    explicit TestBlockBuffers(int numOutputChannels, std::size_t numFrames) {
        channels.resize(static_cast<std::size_t>(numOutputChannels));
        outputPointers.resize(static_cast<std::size_t>(numOutputChannels));
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            channels[static_cast<std::size_t>(ch)].assign(numFrames, 0.0f);
            outputPointers[static_cast<std::size_t>(ch)] = channels[static_cast<std::size_t>(ch)].data();
        }
    }

    synth::AudioBlock Block(std::size_t numFrames) {
        synth::AudioBlock block;
        block.inputs = nullptr;
        block.outputs = outputPointers.data();
        block.numInputChannels = 0;
        block.numOutputChannels = static_cast<int>(outputPointers.size());
        block.numFrames = numFrames;
        return block;
    }
};

struct EngineFakeMidiSink final : synth::IMidiOutputSink {
    std::vector<synth::BasicMidi> received;

    void Send(const synth::BasicMidi& midi) override { received.push_back(midi); }
};

}  // namespace

bool MatchesPublishedClockTuple(const synth::ClockDiagnostics& actual,
                                const synth::ClockDiagnostics& expected) {
    return actual.acquisition == expected.acquisition &&
           actual.source == expected.source &&
           actual.hasActiveExternalSource == expected.hasActiveExternalSource &&
           actual.activeExternalSourceSlot == expected.activeExternalSourceSlot &&
           actual.currentBpm == expected.currentBpm &&
           actual.outputLatencyMicros == expected.outputLatencyMicros &&
           actual.ignoredInputCount == expected.ignoredInputCount &&
           actual.lateEventCount == expected.lateEventCount &&
           actual.droppedOutputCount == expected.droppedOutputCount;
}

TEST_CASE(engine_clock_diagnostics_publication_never_tears_known_tuples) {
    synth::ClockDiagnosticsPublication publication;
    const synth::ClockDiagnostics first{
        .acquisition = synth::ClockAcquisitionState::Locked,
        .source = synth::ClockSource::ExternalMidi,
        .hasActiveExternalSource = true,
        .activeExternalSourceSlot = 3,
        .currentBpm = 111.25,
        .outputLatencyMicros = 5'001,
        .ignoredInputCount = 7,
        .lateEventCount = 11,
        .droppedOutputCount = 13,
    };
    const synth::ClockDiagnostics second{
        .acquisition = synth::ClockAcquisitionState::FreeRun,
        .source = synth::ClockSource::Internal,
        .hasActiveExternalSource = false,
        .activeExternalSourceSlot = 97,
        .currentBpm = 222.5,
        .outputLatencyMicros = 50'002,
        .ignoredInputCount = 70,
        .lateEventCount = 110,
        .droppedOutputCount = 130,
    };
    publication.Publish(first);

    std::atomic<bool> start{false};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) {
        }
        for (int iteration = 0; iteration < 100'000; ++iteration) {
            publication.Publish((iteration & 1) == 0 ? second : first);
        }
    });
    start.store(true, std::memory_order_release);
    bool coherent = true;
    synth::ClockDiagnostics unexpected{};
    for (int iteration = 0; iteration < 100'000; ++iteration) {
        const synth::ClockDiagnostics snapshot = publication.Snapshot();
        if (!MatchesPublishedClockTuple(snapshot, first) &&
            !MatchesPublishedClockTuple(snapshot, second)) {
            coherent = false;
            unexpected = snapshot;
            break;
        }
    }
    writer.join();
    if (!coherent) {
        std::cerr << "unexpected tuple acquisition=" << static_cast<int>(unexpected.acquisition)
                  << " source=" << static_cast<int>(unexpected.source)
                  << " hasSource=" << unexpected.hasActiveExternalSource
                  << " slot=" << unexpected.activeExternalSourceSlot
                  << " bpm=" << unexpected.currentBpm
                  << " latency=" << unexpected.outputLatencyMicros
                  << " ignored=" << unexpected.ignoredInputCount
                  << " late=" << unexpected.lateEventCount
                  << " dropped=" << unexpected.droppedOutputCount << '\n';
    }
    REQUIRE_TRUE(coherent);
}

TEST_CASE(engine_sync_requests_are_atomic_next_block_latest_wins_and_save_requested_immediately) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-sync-request-handoff";
    std::filesystem::remove_all(dataRoot);

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{1'000}; });
    engine.SetRuntimeDataPaths(synth::RuntimeDataPaths::FromDataRoot(dataRoot));
    engine.Initialize();
    engine.Prepare(48'000.0, 64);

    const synth::SyncConfig defaults{};
    const synth::SyncConfig first{true, false, true, false, 48};
    const synth::SyncConfig latest{false, true, false, true, 96};
    REQUIRE_TRUE(!engine.RequestSyncConfiguration({true, true, true, true, 0}));
    REQUIRE_TRUE(engine.SyncConfigurationSnapshot() == defaults);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == defaults);

    REQUIRE_TRUE(engine.RequestSyncConfiguration(first));
    REQUIRE_TRUE(engine.RequestSyncConfiguration(latest));
    REQUIRE_TRUE(engine.SyncConfigurationSnapshot() == latest);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == defaults);

    REQUIRE_TRUE(engine.SaveRuntimeConfiguration() == synth::RuntimeConfigFileStatus::Ok);
    synth::MidiInstrumentConfig savedInstrument;
    synth::AudioDeviceState savedAudio;
    synth::SyncConfig savedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigFile(engine.DataPaths().configFile,
                                              savedInstrument,
                                              savedAudio,
                                              savedSync) ==
                 synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(savedSync == latest);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == defaults);

    TestBlockBuffers buffers(2, 64);
    synth::AudioBlock block = buffers.Block(64);
    engine.ProcessBlock(block, 1'000);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == latest);
    REQUIRE_TRUE(engine.SyncConfigurationSnapshot() == latest);

    REQUIRE_TRUE(!engine.RequestSyncConfiguration({false, false, false, false, 961}));
    REQUIRE_TRUE(engine.SyncConfigurationSnapshot() == latest);
    REQUIRE_TRUE(engine.Clock().SyncConfiguration() == latest);
    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_publishes_sensible_clock_diagnostics_before_and_after_audio) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{10'000}; });
    engine.Initialize();
    REQUIRE_TRUE(engine.ClockDiagnosticsSnapshot().acquisition ==
                 synth::ClockAcquisitionState::Internal);
    REQUIRE_TRUE(engine.ClockDiagnosticsSnapshot().currentBpm == 120.0);

    engine.Prepare(48'000.0, 128);
    REQUIRE_TRUE(engine.ClockDiagnosticsSnapshot().outputLatencyMicros == 5'334);

    REQUIRE_TRUE(engine.RequestSyncConfiguration({false, true, false, true, 24}));
    TestBlockBuffers buffers(2, 128);
    synth::AudioBlock block = buffers.Block(128);
    engine.ProcessBlock(block, 10'000);
    REQUIRE_TRUE(engine.ClockDiagnosticsSnapshot().acquisition ==
                 synth::ClockAcquisitionState::Acquiring);
}

TEST_CASE(engine_owns_one_stable_clock_prepares_before_app_and_publishes_exact_current_plan) {
    EngineTestApp::processLiteAlpha = 1.0f;
    gScheduledEventCount = 0;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{1000}; });
    engine.Initialize();

    synth::MasterClock* const contextClock = engine.Context().masterClock;
    REQUIRE_TRUE(contextClock != nullptr);
    REQUIRE_TRUE(contextClock == &engine.Clock());
    REQUIRE_TRUE(!contextClock->IsPrepared());

    EngineScheduledEventSink sink;
    engine.SetScheduledMidiEventSink(&sink);
    REQUIRE_TRUE(engine.Clock().ApplySyncConfig({
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = true,
        .receiveTransport = false,
        .ppqn = 24,
    }));
    engine.Prepare(48000.0, 64);
    REQUIRE_TRUE(engine.Application().clockPreparedDuringPrepare);
    REQUIRE_TRUE(engine.Context().masterClock == contextClock);

    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Start(1000)));
    TestBlockBuffers buffers(2, 64);
    synth::AudioBlock first = buffers.Block(64);
    engine.ProcessBlock(first, 1000);

    REQUIRE_TRUE(first.clockPlan != nullptr);
    REQUIRE_TRUE(first.clockPlan == engine.Clock().CurrentPlan());
    REQUIRE_TRUE(engine.Application().lastClockPlan == first.clockPlan);
    REQUIRE_TRUE(engine.Application().planStableDuringBlock);
    REQUIRE_TRUE(engine.Application().scheduledEventsDuringBlock == sink.size);
    REQUIRE_TRUE(sink.size > 0);
    REQUIRE_TRUE(first.clockPlan->StartSample() == 0);
    REQUIRE_TRUE(first.clockPlan->EndSample() == 64);
    REQUIRE_TRUE(first.clockPlan->FrameCount() == 64);

    const synth::ClockPlanDescriptor firstDescriptor = engine.Application().lastClockDescriptor;
    synth::AudioBlock second = buffers.Block(64);
    engine.ProcessBlock(second, 2000);
    REQUIRE_TRUE(second.clockPlan == engine.Clock().CurrentPlan());
    REQUIRE_TRUE(second.clockPlan->StartSample() == 64);
    REQUIRE_TRUE(second.clockPlan->EndSample() == 128);
    REQUIRE_TRUE(firstDescriptor.startSample == 0);
    REQUIRE_TRUE(firstDescriptor.endSample == 64);
    REQUIRE_TRUE(engine.Application().processBlockCalls == 2);
}

TEST_CASE(engine_wires_its_master_clock_to_its_concrete_midi_sender_by_default) {
    EngineTestApp::processLiteAlpha = 1.0f;
    std::atomic<std::uint64_t> nowMicros{3'000'000};
    synth::Engine<EngineTestApp> engine(
        [&nowMicros] { return nowMicros.load(std::memory_order_relaxed); });
    engine.Initialize();
    engine.Prepare(100.0, 25);
    REQUIRE_TRUE(engine.Clock().SetTempoBpm(60.0));
    REQUIRE_TRUE(engine.Clock().ApplySyncConfig({
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = false,
        .receiveTransport = false,
        .ppqn = 4,
    }));

    EngineMidiOutputSink sink;
    synth::MidiSender* const sender = engine.Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();

    TestBlockBuffers buffers(2, 25);
    synth::AudioBlock first = buffers.Block(25);
    synth::AudioBlock second = buffers.Block(25);
    synth::AudioBlock third = buffers.Block(25);
    engine.ProcessBlock(first, 1'000'000);
    engine.ProcessBlock(second, 1'250'000);
    engine.ProcessBlock(third, 1'500'000);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();

    std::lock_guard lock(sink.mutex);
    REQUIRE_TRUE(sink.delivered.size() == 2);
    REQUIRE_TRUE((sink.deadlines == std::vector<std::uint64_t>{1'750'000, 2'000'000}));
}

TEST_CASE(engine_delegates_with_null_clock_plan_before_prepare_and_for_zero_frame_blocks) {
    EngineTestApp::processLiteAlpha = 1.0f;

    synth::Engine<EngineTestApp> unprepared([] { return std::uint64_t{1000}; });
    unprepared.Initialize();
    TestBlockBuffers unpreparedBuffers(2, 64);
    synth::AudioBlock unpreparedBlock = unpreparedBuffers.Block(64);
    unprepared.ProcessBlock(unpreparedBlock, 1000);
    REQUIRE_TRUE(unpreparedBlock.clockPlan == nullptr);
    REQUIRE_TRUE(unprepared.Application().lastClockPlan == nullptr);
    REQUIRE_TRUE(unprepared.Application().processBlockCalls == 1);
    REQUIRE_TRUE(unprepared.Clock().CurrentPlan() == nullptr);

    synth::Engine<EngineTestApp> zeroFrames([] { return std::uint64_t{2000}; });
    zeroFrames.Initialize();
    zeroFrames.Prepare(48000.0, 64);
    TestBlockBuffers zeroFrameBuffers(2, 64);
    synth::AudioBlock zeroFrameBlock = zeroFrameBuffers.Block(0);
    zeroFrames.ProcessBlock(zeroFrameBlock, 2000);
    REQUIRE_TRUE(zeroFrameBlock.clockPlan == nullptr);
    REQUIRE_TRUE(zeroFrames.Application().lastClockPlan == nullptr);
    REQUIRE_TRUE(zeroFrames.Application().processBlockCalls == 1);
    REQUIRE_TRUE(zeroFrames.Clock().CurrentPlan() == nullptr);

    // A zero-frame callback consumes no sample position and does not prevent
    // the following ordinary block from committing the initial [0, 64) plan.
    synth::AudioBlock firstNonzeroBlock = zeroFrameBuffers.Block(64);
    zeroFrames.ProcessBlock(firstNonzeroBlock, 3000);
    REQUIRE_TRUE(firstNonzeroBlock.startSample == 0);
    REQUIRE_TRUE(firstNonzeroBlock.clockPlan != nullptr);
    REQUIRE_TRUE(firstNonzeroBlock.clockPlan == zeroFrames.Clock().CurrentPlan());
    REQUIRE_TRUE(zeroFrames.Application().processBlockCalls == 2);
}

TEST_CASE(engine_merges_realtime_messages_across_buses_with_deterministic_ties) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{100}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);
    REQUIRE_TRUE(engine.Clock().ApplySyncConfig({
        .sendClock = false,
        .receiveClock = true,
        .sendTransport = false,
        .receiveTransport = true,
        .ppqn = 24,
    }));

    // MIDI's earlier timestamp wins even though UI is drained first; Start
    // then Stop leaves the first committed plan stopped.
    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Stop(90)));
    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::Start(80)));

    // UI arrives first, but external slot identity is the earlier tie-break.
    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Clock(
        100, synth::MessageIn::Origin::ExternalMidi, 9)));
    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::Clock(
        100, synth::MessageIn::Origin::ExternalMidi, 2)));
    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock first = buffers.Block(32);
    engine.ProcessBlock(first, 100);
    REQUIRE_TRUE(first.clockPlan->TransportState() == synth::ClockTransportState::Stopped);
    const synth::ClockDiagnostics diagnostics = engine.Clock().DiagnosticsSnapshot();
    REQUIRE_TRUE(diagnostics.hasActiveExternalSource);
    REQUIRE_TRUE(diagnostics.activeExternalSourceSlot == 2);
    REQUIRE_TRUE(diagnostics.acceptedExternalClockCount == 1);

    // Exact ties preserve FIFO arrival order within one bus.
    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Start(200)));
    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Stop(200)));
    synth::AudioBlock second = buffers.Block(32);
    engine.ProcessBlock(second, 200);
    REQUIRE_TRUE(second.clockPlan->TransportState() == synth::ClockTransportState::Stopped);

}

TEST_CASE(engine_realtime_batch_overflow_is_observable_and_retains_earliest_ordered_messages) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{100}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);
    REQUIRE_TRUE(engine.Clock().ApplySyncConfig({
        .sendClock = false,
        .receiveClock = true,
        .sendTransport = false,
        .receiveTransport = false,
        .ppqn = 24,
    }));

    for (std::size_t slot = synth::Engine<EngineTestApp>::kRealtimeBatchCapacity; slot > 0; --slot) {
        REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::Clock(
            100, synth::MessageIn::Origin::ExternalMidi, slot)));
    }
    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::Clock(
        100, synth::MessageIn::Origin::ExternalMidi, 0)));

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);
    engine.ProcessBlock(block, 100);
    REQUIRE_TRUE(engine.DroppedRealtimeInputCount() == 1);
    REQUIRE_TRUE(engine.Clock().DiagnosticsSnapshot().activeExternalSourceSlot == 0);
}

TEST_CASE(engine_clock_commit_query_crossing_and_enqueue_allocate_nothing_after_prepare) {
    EngineTestApp::processLiteAlpha = 1.0f;
    gScheduledEventCount = 0;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{1000}; });
    engine.Initialize();
    EngineScheduledEventSink sink;
    engine.SetScheduledMidiEventSink(&sink);
    REQUIRE_TRUE(engine.Clock().ApplySyncConfig({
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = true,
        .receiveTransport = false,
        .ppqn = 960,
    }));
    engine.Prepare(48000.0, 64);
    REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::Start(1000)));

    TestBlockBuffers buffers(2, 64);
    synth::AudioBlock warmup = buffers.Block(64);
    engine.ProcessBlock(warmup, 1000);
    REQUIRE_TRUE(warmup.clockPlan != nullptr);

    engine_allocation_probe::count.store(0, std::memory_order_relaxed);
    engine_allocation_probe::enabled.store(true, std::memory_order_release);
    for (std::uint64_t blockIx = 1; blockIx <= 128; ++blockIx) {
        synth::AudioBlock block = buffers.Block(64);
        engine.ProcessBlock(block, 1000 + blockIx * 1333);
        const double fractionalSample = static_cast<double>(block.startSample) + 0.25;
        const auto direct = engine.Clock().TimeAtSample(fractionalSample);
        if (!direct.has_value() || block.clockPlan == nullptr ||
            !block.clockPlan->TryLifetimeQuarterNotesAt(fractionalSample).has_value()) {
            engine_allocation_probe::enabled.store(false, std::memory_order_release);
            throw std::runtime_error("clock query unexpectedly failed during allocation probe");
        }
    }
    engine_allocation_probe::enabled.store(false, std::memory_order_release);
    REQUIRE_TRUE(engine_allocation_probe::count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE(engine_steady_state_block_does_not_acquire_configuration_mutex) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{1000}; });
    engine.Initialize();
    engine.Prepare(48000.0, 64);
    TestBlockBuffers buffers(2, 64);

    std::atomic<bool> editHoldsMutex{false};
    std::atomic<bool> releaseEdit{false};
    std::thread editor([&] {
        engine.EditInstrument([&](synth::MidiInstrumentConfig&) {
            editHoldsMutex.store(true, std::memory_order_release);
            while (!releaseEdit.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    });
    while (!editHoldsMutex.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    std::atomic<bool> blockStarted{false};
    std::atomic<bool> blockCompleted{false};
    std::thread audio([&] {
        blockStarted.store(true, std::memory_order_release);
        synth::AudioBlock block = buffers.Block(64);
        engine.ProcessBlock(block, 1000);
        blockCompleted.store(true, std::memory_order_release);
    });
    while (!blockStarted.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!blockCompleted.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool completedWhileMutexHeld = blockCompleted.load(std::memory_order_acquire);
    releaseEdit.store(true, std::memory_order_release);
    audio.join();
    editor.join();
    REQUIRE_TRUE(completedWhileMutexHeld);
}

namespace {

struct EngineGridProbe {
    int presses = 0;
    int releases = 0;
    int pressureChanges = 0;
    int destructions = 0;
    std::uint8_t lastVelocity = 0;
    std::uint8_t lastPressure = 0;
};

class EngineGridProbeCell final : public synth::Cell {
public:
    explicit EngineGridProbeCell(EngineGridProbe* probe) : probe_(probe) {}
    ~EngineGridProbeCell() override { ++probe_->destructions; }

    void OnPress(std::uint8_t velocity) override {
        ++probe_->presses;
        probe_->lastVelocity = velocity;
    }
    void OnRelease() override { ++probe_->releases; }
    void OnPressureChange(std::uint8_t pressure) override {
        ++probe_->pressureChanges;
        probe_->lastPressure = pressure;
    }
    synth::Color GetColor() const override { return synth::Color::Rgb(10, 20, 30); }
    bool GetOnOff() const override { return true; }

private:
    EngineGridProbe* probe_;
};

}  // namespace

TEST_CASE(engine_owns_stable_runtime_grid_state_and_routes_both_buses) {
    EngineTestApp::wantEncoderMidiInput = true;
    EngineGridProbe probe;
    const synth::RuntimeUIState* facadeAddress = nullptr;
    synth::ParameterManager::UIState* parameterStateAddress = nullptr;
    synth::GridManager::UIState* gridStateAddress = nullptr;

    {
        synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
        const auto range = synth::GridRange::Create(-1, 1, 0, 1);
        REQUIRE_TRUE(range.has_value());
        auto& grids = engine.GridManagerForTest();
        const auto gridIx = grids.CreateGrid(*range);
        const auto slotIx = grids.CreateSlot(*range);
        REQUIRE_TRUE(gridIx.has_value());
        REQUIRE_TRUE(slotIx.has_value());
        REQUIRE_TRUE(grids.GridAt(*gridIx)->RegisterCell(
            -1, 0, std::make_unique<EngineGridProbeCell>(&probe)));
        REQUIRE_TRUE(grids.SelectGridForSlot(*slotIx, *gridIx));

        engine.Initialize();
        engine.Prepare(48000.0, 256);

        const synth::RuntimeUIState& state = engine.RuntimeUIStateForTest();
        facadeAddress = &state;
        parameterStateAddress = state.parameters;
        gridStateAddress = state.grids;
        REQUIRE_TRUE(grids.Finalized());
        REQUIRE_TRUE(state.parameters != nullptr);
        REQUIRE_TRUE(state.grids != nullptr);
        REQUIRE_TRUE(engine.Context().uiState == state.parameters);
        REQUIRE_TRUE(engine.MidiControllerCount() == 1);
        REQUIRE_TRUE(engine.MidiInputProcessor(0) != nullptr);
        REQUIRE_TRUE(state.grids->slots[*slotIx]->colors[0].Load() ==
                     synth::Color::Rgba(10, 20, 30, 1));

        REQUIRE_TRUE(engine.UiBus().Push(synth::MessageIn::GridPress(0, *slotIx, -1, 0, 101)));
        REQUIRE_TRUE(engine.MidiBus().Push(
            synth::MessageIn::GridPressureChange(0, *slotIx, -1, 0, 63)));
        REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::GridRelease(0, *slotIx, -1, 0)));
        TestBlockBuffers buffers(2, 4);
        for (int blockIx = 0; blockIx < 6; ++blockIx) {
            synth::AudioBlock block = buffers.Block(4);
            engine.ProcessBlock(block, 0);
        }

        REQUIRE_TRUE(probe.presses == 1);
        REQUIRE_TRUE(probe.lastVelocity == 101);
        REQUIRE_TRUE(probe.pressureChanges == 1);
        REQUIRE_TRUE(probe.lastPressure == 63);
        REQUIRE_TRUE(probe.releases == 1);
        REQUIRE_TRUE(state.grids->slots[*slotIx]->colors[0].Load() ==
                     synth::Color::Rgba(10, 20, 30, 1));

        engine.RebuildMidiProcessorsForTest();
        REQUIRE_TRUE(&engine.RuntimeUIStateForTest() == facadeAddress);
        REQUIRE_TRUE(engine.RuntimeUIStateForTest().parameters == parameterStateAddress);
        REQUIRE_TRUE(engine.RuntimeUIStateForTest().grids == gridStateAddress);
        REQUIRE_TRUE(probe.destructions == 0);
    }

    REQUIRE_TRUE(probe.destructions == 1);
    REQUIRE_TRUE(probe.presses == 1);
    REQUIRE_TRUE(probe.pressureChanges == 1);
    REQUIRE_TRUE(probe.releases == 1);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(sheaf_patch_runtime_configuration_saves_shared_config_without_patch_values) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-sheaf-patch-runtime-config-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::SheafPatchDataPathsForApp(dataRoot, "miniapp");

    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "sheaf-controller"));
    });
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Sheaf Output",
                                                          .inputDeviceName = "Sheaf Input"});

    TestBlockBuffers buffers(2, 4);
    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/0, /*slotIx=*/0, /*position=*/0, /*delta=*/0.3f));
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0), 0.55f, 1e-4f);

    REQUIRE_TRUE(engine.SaveRuntimeConfiguration() == synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(std::filesystem::exists(paths.configFile));
    REQUIRE_TRUE(paths.configFile == dataRoot / "synth" / "sheaf-patch" / "config");
    REQUIRE_TRUE(!std::filesystem::exists(paths.dataRoot / "config.json"));

    const std::string contents = ReadTextFile(paths.configFile);
    REQUIRE_TRUE(contents.find("sheaf-controller") != std::string::npos);
    REQUIRE_TRUE(contents.find("Sheaf Output") != std::string::npos);
    REQUIRE_TRUE(contents.find("Probe") == std::string::npos);
    REQUIRE_TRUE(contents.find("parameters") == std::string::npos);
    REQUIRE_TRUE(contents.find("0.55") == std::string::npos);

    std::filesystem::remove_all(dataRoot);
    EngineTestApp::wantEncoderMidiInput = false;
}

TEST_CASE(sheaf_patch_startup_discovers_only_selected_app_patch_root) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-sheaf-patch-patch-isolation-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths miniappPaths = synth::SheafPatchDataPathsForApp(dataRoot, "miniapp");
    const synth::RuntimeDataPaths otherAppPaths = synth::SheafPatchDataPathsForApp(dataRoot, "otherapp");
    std::filesystem::create_directories(miniappPaths.patchesRoot);
    std::filesystem::create_directories(otherAppPaths.patchesRoot);

    const auto earlier = std::chrono::system_clock::from_time_t(1700000000);
    const auto later = earlier + std::chrono::seconds(2);
    WriteProbePatchVersion(miniappPaths.patchesRoot / "MiniPatch", 0.75f, earlier);
    WriteProbePatchVersion(otherAppPaths.patchesRoot / "OtherPatch", 0.95f, later);

    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(miniappPaths);
    engine.Initialize();
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.75f, 1e-5f);

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(sheaf_patch_patch_save_as_writes_under_selected_app_patch_root) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-sheaf-patch-patch-save-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths miniappPaths = synth::SheafPatchDataPathsForApp(dataRoot, "miniapp");
    const synth::RuntimeDataPaths otherAppPaths = synth::SheafPatchDataPathsForApp(dataRoot, "otherapp");

    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(miniappPaths);
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    const std::filesystem::path saveDir = miniappPaths.patchesRoot / "SavedPatch";
    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);

    TestBlockBuffers buffers(2, 4);
    bool written = false;
    for (int iteration = 0; iteration < 16 && !written; ++iteration) {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
        engine.MessageThreadTick();
        written = synth::LatestPatchVersion(saveDir).has_value();
    }

    REQUIRE_TRUE(written);
    REQUIRE_TRUE(saveDir.parent_path() == miniappPaths.patchesRoot);
    REQUIRE_TRUE(!std::filesystem::exists(otherAppPaths.patchesRoot));

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_pump_applies_messages_before_app_block) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{2}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    const float before = engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0);

    // Push a ParamIncDec against the slot/position registered in Init (slot
    // 0, position 0 maps to the probe parameter via the bank/slot wiring).
    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/2, /*slotIx=*/0, /*position=*/0, /*delta=*/0.3f));

    TestBlockBuffers buffers(2, 4);
    synth::AudioBlock block = buffers.Block(4);
    engine.ProcessBlock(block, /*timestamp=*/2);

    REQUIRE_TRUE(engine.Application().processBlockCalls == 1);
    REQUIRE_TRUE(engine.Application().lastProbeSceneCenterDuringBlock != before);
    REQUIRE_NEAR(engine.Application().lastProbeSceneCenterDuringBlock, before + 0.3f, 1e-4f);
    REQUIRE_NEAR(engine.Application().lastProbeDuringBlock, before, 1e-4f);
}

TEST_CASE(engine_audio_block_exposes_monotonic_start_sample) {
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 10);

    TestBlockBuffers buffers(2, 10);
    {
        synth::AudioBlock block = buffers.Block(10);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_TRUE(engine.Application().lastBlockStartSample == 0);

    {
        synth::AudioBlock block = buffers.Block(10);
        engine.ProcessBlock(block, /*timestamp=*/1);
    }
    REQUIRE_TRUE(engine.Application().lastBlockStartSample == 10);
}

TEST_CASE(engine_process_block_does_not_compute_targets_at_host_block_boundary) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 10);

    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/0, /*slotIx=*/0, /*position=*/0, /*delta=*/0.3f));

    TestBlockBuffers buffers(2, 10);
    {
        synth::AudioBlock block = buffers.Block(10);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }

    REQUIRE_NEAR(engine.Application().lastProbeDuringBlock, 0.25f, 1e-4f);
    engine.Application().context->parameterManager->ParameterById(engine.Application().probeId).ProcessSample(0);
    REQUIRE_NEAR(engine.Application().context->parameterManager->ParameterById(engine.Application().probeId).CurrentCenter(),
                 0.55f, 1e-4f);
}

TEST_CASE(engine_process_sample_preserves_slew_after_engine_message_pump) {
    EngineTestApp::processLiteAlpha = 0.1f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{1}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    auto& probe = engine.Manager().ParameterById(engine.Application().probeId);
    const float start = probe.CurrentCenter();
    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/1, /*slotIx=*/0, /*position=*/0, /*delta=*/0.5f));
    const float target = std::clamp(start + 0.5f, 0.0f, 1.0f);

    TestBlockBuffers buffers(2, 4);

    synth::AudioBlock firstBlock = buffers.Block(4);
    engine.ProcessBlock(firstBlock, /*timestamp=*/1);
    REQUIRE_NEAR(engine.Application().lastProbeDuringBlock, start, 1e-6f);
    probe.ProcessSample(0);
    const float afterFirst = probe.CurrentCenter();
    REQUIRE_TRUE(afterFirst != target);  // no snap: slewed value must not equal target yet

    float previous = afterFirst;
    for (std::uint64_t sample = 1; sample < 64; ++sample) {
        probe.ProcessSample(sample);
        const float current = probe.CurrentCenter();
        REQUIRE_TRUE(current >= previous - 1e-6f);  // approaches monotonically
        previous = current;
    }
    REQUIRE_NEAR(previous, target, 1e-3f);
}

TEST_CASE(engine_pump_calls_app_exactly_once_per_block_and_advances_samples) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 64);

    TestBlockBuffers buffers(2, 64);

    synth::AudioBlock firstBlock = buffers.Block(64);
    engine.ProcessBlock(firstBlock, /*timestamp=*/10);
    synth::AudioBlock secondBlock = buffers.Block(64);
    engine.ProcessBlock(secondBlock, /*timestamp=*/11);

    REQUIRE_TRUE(engine.Application().processBlockCalls == 2);
    REQUIRE_TRUE(engine.SampleCount() == 128);
    REQUIRE_NEAR(buffers.channels[0][0], 0.5f, 1e-6f);
}

TEST_CASE(engine_pump_populates_ui_state_at_throttle_cadence) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{5}; });
    engine.Initialize();

    // config_.uiFrameHz defaults to 30 (EngineTestApp::Config() doesn't
    // override it), matching the brief's worked example:
    // interval = round(48000 / (30 * 256)) = 6.
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.Context().uiState != nullptr);
    auto& cell = engine.Context().uiState->slots[0].cells[0];
    const float initialDisplayCenter =
        engine.Manager().ParameterById(engine.Application().probeId).UIDisplayCenter(0);

    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/5, /*slotIx=*/0, /*position=*/0, /*delta=*/0.4f));

    TestBlockBuffers buffers(2, 4);

    // First block: message applied, but UI state must not be republished yet
    // (interval is 6, and PopulateUIState happens after Initialize() as well,
    // so we capture the initial published value first to compare against).
    const float publishedBeforeAnyBlock = cell.values[0].load();

    synth::AudioBlock block1 = buffers.Block(4);
    engine.ProcessBlock(block1, /*timestamp=*/5);
    REQUIRE_NEAR(cell.values[0].load(), publishedBeforeAnyBlock, 1e-6f);  // unchanged: cadence not hit

    auto& probe = engine.Manager().ParameterById(engine.Application().probeId);
    for (std::uint64_t sample = 0; sample < 8000; ++sample) {
        probe.ProcessSample(sample);
    }
    const float advancedCurrentCenter = probe.CurrentCenter();
    REQUIRE_TRUE(advancedCurrentCenter != initialDisplayCenter);

    for (int i = 0; i < 5; ++i) {  // blocks 2..6: the 6th block hits the cadence
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/5);
    }

    REQUIRE_NEAR(probe.SceneCenter(0), 0.65f, 1e-4f);
    REQUIRE_NEAR(cell.values[0].load(), advancedCurrentCenter, 1e-4f);
}

// ui-state-before-audio (openspec/changes/ui-state-before-audio): the
// following three tests cover the design's Testing section exactly --
// pre-audio population (positive control: identical content to a post-audio
// frame), the four-assertion transition test at the claim primitive's own
// seam, and null-safety. The "browser-level: freshly installed app ..."
// scenario needs no engine-level test of its own and no browser worker code
// change: browser/src/main.ts:355-356 already calls MessageThreadTick (via
// the "message-tick" request -> BrowserRuntime.hpp:717) unconditionally on
// every frame, including the very first (browser/src/main.ts:355-356, before the
// frame timer even starts and before any user activation), so the fix here
// is exercised by the existing browser frame loop with no seam changes
// needed there (task 1.1 trace obligation).

TEST_CASE(engine_message_thread_populates_ui_state_before_audio_ever_runs) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{7}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.Context().uiState != nullptr);
    auto& cell = engine.Context().uiState->slots[0].cells[0];
    auto& probe = engine.Manager().ParameterById(engine.Application().probeId);

    // Move the probe's target directly (Parameter::HandleIncDec is exactly
    // what a UI ParamIncDec message ultimately calls) and slew it, with NO
    // engine.ProcessBlock(...) call anywhere in this test -- the audio pump
    // must never run, so only MessageThreadTick's claim path can ever
    // publish here.
    probe.HandleIncDec(engine.Manager().Scene(), 0.4f);
    for (std::uint64_t sample = 0; sample < 8000; ++sample) {
        probe.ProcessSample(sample);
    }
    const float movedCenter = probe.UIDisplayCenter(0);

    // Positive control (omni §9.1): before any populate runs, the published
    // cell must NOT already carry the moved value, or ticking would prove
    // nothing.
    REQUIRE_TRUE(std::fabs(cell.values[0].load() - movedCenter) > 1e-4f);
    REQUIRE_TRUE(engine.UiStatePublisherIsQuiescentForTest());
    REQUIRE_TRUE(!engine.AudioOwnsUiStateForTest());

    engine.MessageThreadTick();

    // Content is identical to what a post-audio (ProcessBlock) publish would
    // have produced -- same PopulateUIState pair, same buffers.
    REQUIRE_NEAR(cell.values[0].load(), movedCenter, 1e-4f);
    REQUIRE_TRUE(!engine.AudioOwnsUiStateForTest());
    // The message thread releases its claim back to Quiescent after
    // publishing (design: "then store Quiescent (release)"). Proven below,
    // not merely asserted here: a second, independently-moved value is
    // published by a second tick.
    REQUIRE_TRUE(engine.UiStatePublisherIsQuiescentForTest());

    probe.HandleIncDec(engine.Manager().Scene(), -0.2f);
    for (std::uint64_t sample = 0; sample < 8000; ++sample) {
        probe.ProcessSample(sample);
    }
    const float secondMovedCenter = probe.UIDisplayCenter(0);
    REQUIRE_TRUE(std::fabs(secondMovedCenter - movedCenter) > 1e-4f);
    engine.MessageThreadTick();
    REQUIRE_NEAR(cell.values[0].load(), secondMovedCenter, 1e-4f);
}

TEST_CASE(engine_ui_state_claim_skips_then_latches_then_blocks_message_thread_forever) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{11}; });
    engine.Initialize();
    // config_.uiFrameHz defaults to 30 (EngineTestApp::Config() doesn't
    // override it): uiPublishInterval_ = round(48000 / (30 * 256)) = 6,
    // matching engine_pump_populates_ui_state_at_throttle_cadence above.
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.Context().uiState != nullptr);
    auto& cell = engine.Context().uiState->slots[0].cells[0];
    auto& probe = engine.Manager().ParameterById(engine.Application().probeId);

    TestBlockBuffers buffers(2, 4);

    // --- (a) CAS-fail skip, without blocking ---
    // Force the claim into MessageThread state (design Testing: "hold the
    // claim in MessageThread state via a test hook"), simulating a
    // message-thread populate in flight, then run one full throttle window
    // on the audio side. The published cell must stay untouched and
    // sampleCounter_ must still advance by the full window -- audio never
    // waits on the held claim.
    const float beforeSkip = cell.values[0].load();
    const std::uint64_t sampleCountBeforeSkip = engine.SampleCount();
    engine.HoldUiStatePublisherAsMessageThreadForTest();
    for (int i = 0; i < 6; ++i) {  // exactly one uiPublishInterval_ window
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/11);
    }
    REQUIRE_NEAR(cell.values[0].load(), beforeSkip, 1e-6f);                    // no populate happened
    REQUIRE_TRUE(engine.SampleCount() == sampleCountBeforeSkip + 6 * 4);       // audio kept advancing, did not block
    REQUIRE_TRUE(!engine.AudioOwnsUiStateForTest());                          // no latch yet
    REQUIRE_TRUE(engine.UiStatePublisherIsMessageThreadForTest());            // the held claim is untouched by the failed CAS

    // --- (b) claim-and-latch at the next window ---
    // Release the held claim (simulating the message thread finishing its
    // populate and storing Quiescent per the design) and move the probe so
    // a real, observable value exists for the audio side to publish.
    engine.ReleaseUiStatePublisherHoldForTest();
    probe.HandleIncDec(engine.Manager().Scene(), 0.4f);
    for (std::uint64_t sample = 0; sample < 8000; ++sample) {
        probe.ProcessSample(sample);
    }
    const float movedCenter = probe.UIDisplayCenter(0);
    REQUIRE_TRUE(std::fabs(movedCenter - beforeSkip) > 1e-4f);  // positive control: this value would move the cell if published

    for (int i = 0; i < 6; ++i) {  // the next uiPublishInterval_ window
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/11);
    }
    REQUIRE_TRUE(engine.AudioOwnsUiStateForTest());               // latched
    REQUIRE_TRUE(engine.UiStatePublisherIsAudioThreadForTest());  // permanently AudioThread

    // --- (d) a frame built after the latch reflects audio-published state ---
    REQUIRE_NEAR(cell.values[0].load(), movedCenter, 1e-4f);

    // --- (c) message-thread CAS fails forever after the latch ---
    // If MessageThreadTick's CAS incorrectly succeeded post-latch, the
    // design has it end the tick by storing Quiescent -- directly
    // observable here, so this assertion has a real failure mode (it is not
    // a tautology: engine_message_thread_populates_ui_state_before_audio_
    // ever_runs above is the positive control proving the tick's claim path
    // actually runs and mutates this same state when it is allowed to).
    engine.MessageThreadTick();
    REQUIRE_TRUE(engine.UiStatePublisherIsAudioThreadForTest());
    REQUIRE_TRUE(engine.AudioOwnsUiStateForTest());
}

TEST_CASE(engine_message_thread_tick_before_initialize_does_not_crash_or_populate) {
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    // No Initialize() call: uiState_/gridUIState_ are both still nullptr, so
    // the message-thread claim path must null-check exactly as the
    // audio-thread publish site already does (Engine.hpp:433/:436 -- see
    // MessageThreadTick's mirrored checks).
    REQUIRE_TRUE(engine.Context().uiState == nullptr);
    REQUIRE_TRUE(engine.UiStatePublisherIsQuiescentForTest());

    engine.MessageThreadTick();  // must not crash

    REQUIRE_TRUE(engine.Context().uiState == nullptr);
    // The claim was still taken and released even with nothing to populate
    // (the CAS branch ran; it did not merely no-op on a load check), leaving
    // the machine exactly where a real Initialize() + populate later expects
    // to find it. That the claimed branch's populate calls actually write
    // real data when buffers ARE present is the positive control, proven by
    // engine_message_thread_populates_ui_state_before_audio_ever_runs above.
    REQUIRE_TRUE(engine.UiStatePublisherIsQuiescentForTest());
    REQUIRE_TRUE(!engine.AudioOwnsUiStateForTest());
}

TEST_CASE(engine_pump_stash_is_a_drain_barrier_with_retry_first_ordering) {
    EngineTestApp::processLiteAlpha = 1.0f;  // snap immediately so applied/reverted values are visible this block

    // Tiny arena: SerializeToJSON cannot fit even the patch-only document in
    // 512 bytes, so ApplyPatchMessage reports ArenaExhausted on the first
    // attempt. A single GrowAndReset() doubling is enough to fit it, so one
    // MessageThreadTick() call is enough to clear the barrier below.
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; }, /*initialArenaCapacity=*/512);
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    const float initial = engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0);
    REQUIRE_NEAR(initial, 0.25f, 1e-5f);

    TestBlockBuffers buffers(2, 4);

    // Move the probe away from its default via a normal UI message so a
    // later revert-to-default is visibly observable.
    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/0, /*slotIx=*/0, /*position=*/0, /*delta=*/0.3f));
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    const float moved = engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0);
    REQUIRE_NEAR(moved, initial + 0.3f, 1e-4f);

    // Enqueue a serialize request (SavePatchAs) via PatchManager. The next
    // ProcessBlock will pop it, exhaust the tiny arena, stash it, and set
    // the grow-pending flag; the drain phase stops for this block without
    // touching anything else queued.
    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "engine-drain-barrier-save-dir";
    std::filesystem::remove_all(saveDir);
    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);

    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_TRUE(engine.HasStashedPatchMessageForTest());  // exhausted and stashed
    REQUIRE_TRUE(engine.IsArenaGrowPendingForTest());      // grow flag set: barrier is up

    // Now enqueue a second patch command (revert to defaults) directly onto
    // patchInputBus_ via the engine's AppContext, bypassing PatchManager's
    // own SavePatchAs/RevertPatch bookkeeping entirely (which is orthogonal
    // to — and would otherwise confound observing — the engine's drain
    // barrier: e.g. PatchManager::RevertPatch()/NewPatch() reset
    // PatchManager's pendingSave_ synchronously at dispatch time, regardless
    // of whether the engine's drain has actually applied the pending save
    // yet). If the drain barrier holds, this RevertAllToDefault message must
    // NOT be applied while the stash is still pending: the probe value must
    // stay at `moved`, not reset back to `initial`.
    REQUIRE_TRUE(engine.Context().patchInputBus->Push(synth::PatchMessageIn::RevertAllToDefault()));

    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }

    // Barrier held: the revert was not applied (probe still at `moved`), and
    // the stash/grow-pending flag are still in force since MessageThreadTick
    // has not run yet.
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0), moved, 1e-4f);
    REQUIRE_TRUE(engine.HasStashedPatchMessageForTest());
    REQUIRE_TRUE(engine.IsArenaGrowPendingForTest());

    // Simulate Task 5's tick contract: grow the arena and clear
    // arenaGrowPending_ only (MessageThreadTick must not touch the stash
    // itself per the documented contract).
    engine.MessageThreadTick();
    REQUIRE_TRUE(engine.HasStashedPatchMessageForTest());  // tick must not touch the stash
    REQUIRE_TRUE(!engine.IsArenaGrowPendingForTest());     // tick cleared the grow flag

    // Next block: ProcessBlock must retry the stashed serialize FIRST. It
    // now fits in the grown arena, so it succeeds and the barrier lifts;
    // draining continues in the same block, so the previously-blocked
    // revert now applies too.
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }

    REQUIRE_TRUE(!engine.HasStashedPatchMessageForTest());  // stash retried and succeeded
    const synth::PatchCommandResult processed = engine.Patches().ProcessResponses();
    REQUIRE_TRUE(processed.status == synth::PatchCommandStatus::Written);  // serialize response was produced

    // The revert queued behind the stash has now applied too (drain
    // continued past the retried stash in the same block): the probe is
    // back at its default.
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).SceneCenter(0), initial, 1e-4f);

    std::filesystem::remove_all(saveDir);
}

TEST_CASE(engine_initialize_without_startup_patch_never_fires_rebuilt_callback) {
    // Property 1: Initialize()'s first, pre-startup-patch RebuildMidiProcessors()
    // call (sar-5 step 7) must never invoke midiProcessorsRebuiltCallback_,
    // and with no patchesRoot configured there is no startup patch to apply
    // either, so the callback must not fire at all across Initialize().
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });

    int callbackCalls = 0;
    engine.SetMidiProcessorsRebuiltCallback([&]() { ++callbackCalls; });

    engine.Initialize();

    REQUIRE_TRUE(callbackCalls == 0);
}

TEST_CASE(engine_initialize_applies_startup_patch_without_rebuilt_callback) {
    // Startup patch load applies synthesizer parameter values only. MIDI/audio
    // configuration now lives in the separate runtime config file, so applying
    // a patch during Initialize() must not rebuild MIDI processors or fire the
    // rebuilt callback.
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-initialize-callback-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);
    WriteProbePatchVersion(paths.patchesRoot / "AAA", 0.75f, std::chrono::system_clock::now());

    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);

    int callbackCalls = 0;
    engine.SetMidiProcessorsRebuiltCallback([&]() { ++callbackCalls; });

    engine.Initialize();

    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.75f, 1e-5f);
    REQUIRE_TRUE(callbackCalls == 0);

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_tick_does_not_rebuild_midi_processors_after_parameter_patch_load) {
    // Runtime patch load applies synthesizer parameter values only. The tick
    // should keep processing MIDI controllers, but it must not perform a MIDI
    // processor rebuild or call the host reopen callback for a parameter-only
    // patch.
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // so MidiInputProcessor(0) is non-null and identity-observable
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    int callbackCalls = 0;
    synth::MidiInProcessor* inputProcessorBeforeLoad = engine.MidiInputProcessor(0);
    engine.SetMidiProcessorsRebuiltCallback([&]() {
        ++callbackCalls;
    });

    // Write a patch version (reusing Task 3's WriteProbePatchVersion helper)
    // and enqueue it via PatchManager::LoadPatch, matching the brief's
    // "helper like Task 3's" instruction.
    const std::filesystem::path patchDir =
        std::filesystem::temp_directory_path() / "engine-tick-rebuild-patch-dir";
    std::filesystem::remove_all(patchDir);
    WriteProbePatchVersion(patchDir, 0.9f, std::chrono::system_clock::now());

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(patchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 4);
    {
        // ProcessBlock drains patchInputBus_ and applies the LoadFromJSON
        // message without queuing MIDI/audio side effects.
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_NEAR(engine.Manager().ParameterById(engine.Application().probeId).GetRaw(0), 0.9f, 1e-4f);
    REQUIRE_TRUE(callbackCalls == 0);

    engine.MessageThreadTick();

    REQUIRE_TRUE(callbackCalls == 0);
    REQUIRE_TRUE(engine.MidiInputProcessor(0) == inputProcessorBeforeLoad);

    // A second tick with nothing pending must not fire the callback either.
    engine.MessageThreadTick();
    REQUIRE_TRUE(callbackCalls == 0);

    std::filesystem::remove_all(patchDir);
    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_tick_replies_to_storage_batch_requests) {
    // Dedicated test app variant with a tiny maxParameters group and two
    // modulator slots, matching
    // modulation_view_requests_storage_batch_and_succeeds_after_reinforcement's
    // recipe in parameter_modulation_tests.cpp for provoking
    // ParameterStorageBatchNeeded: a bank/slot mapped to a carrier
    // parameter, with HandlePress(1) requesting the modulation view. The
    // group's storage starts too small to hold the extra modulation-depth
    // parameters, so the manager posts a ParameterStorageBatchNeeded
    // request onto parameterMessageOutBus_ instead of materializing them.
    struct TinyGroupApp {
        static synth::RuntimeConfig Config() {
            synth::RuntimeConfig config;
            config.appName = "EngineTinyGroupTest";
            config.numAudioOutputs = 2;
            return config;
        }
        synth::AppContext* context = nullptr;
        synth::ParameterGroup* group = nullptr;
        synth::Bank* bank = nullptr;
        synth::BankSlot* slot = nullptr;
        synth::Parameter* carrier = nullptr;

        void Init(synth::AppContext* ctx) {
            context = ctx;
            group = &ctx->parameterManager->CreateGroup(
                {.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 2});
            for (synth::ModulatorMetadata& metadata : group->GetModulators().Metadata()) {
                metadata.connected = true;
            }
            carrier = &ctx->parameterManager->CreateParameter(*group, {.name = "Carrier", .defaultValue = 0.5f});
            auto& filler = ctx->parameterManager->CreateParameter(*group, {.name = "Filler", .defaultValue = 0.25f});
            (void)filler;
            bank = &ctx->parameterManager->CreateBank();
            bank->AddMapping(1, *carrier);
            bank->AddMapping(2, filler);
            slot = &ctx->parameterManager->CreateBankSlot();
            slot->AddPhysicalEncoder(1);
            slot->AddPhysicalEncoder(2);
            slot->AddPhysicalEncoder(3);
            slot->SelectBank(bank);
        }
        void ProcessBlock(synth::AudioBlock&) {}
    };

    synth::Engine<TinyGroupApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    synth::Bank* bank = engine.Application().bank;
    synth::BankSlot* slot = engine.Application().slot;
    REQUIRE_TRUE(!bank->ShowingModulation());

    // Trigger the growth-request path: pressing the mapped encoder asks the
    // manager to show the modulation view, which needs storage the tiny
    // group doesn't have, so it posts ParameterStorageBatchNeeded instead of
    // materializing the depth parameters.
    slot->HandlePress(1);
    REQUIRE_TRUE(!bank->ShowingModulation());  // materialization deferred: no room yet

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }

    engine.MessageThreadTick();  // drains parameterMessageOutBus_, replies with a storage batch

    // A subsequent registration/materialization now succeeds since the
    // group has been reinforced with the storage batch the tick provided:
    // re-pressing the same encoder materializes and shows the depth
    // parameters instead of deferring again.
    slot->HandlePress(1);
    REQUIRE_TRUE(bank->ShowingModulation());
    synth::Parameter* carrier = engine.Application().carrier;
    REQUIRE_TRUE(carrier->ModulationDepthParameter(0) != nullptr);
    REQUIRE_TRUE(carrier->ModulationDepthParameter(1) != nullptr);
}

TEST_CASE(engine_tick_grows_arena_and_retries_stashed_patch_message) {
    EngineTestApp::processLiteAlpha = 1.0f;

    // Tiny starting arena (64 bytes), matching the brief's initialArenaCapacity
    // constructor-parameter approach: far too small to serialize a patch
    // document, so the first ProcessBlock after SavePatchAs is expected to
    // exhaust and stash. Each MessageThreadTick doubles the arena
    // (GrowSerializationArenaForTick), so this drives the real grow/retry
    // loop end to end through the actual MessageThreadTick, bounded at 10
    // iterations, until the version file appears on disk under saveDir.
    //
    // Deliberately does NOT call patchManager_.ProcessResponses() directly
    // anywhere in this loop: MessageThreadTick's own step 3 is the only
    // thing that drains patchOutputBus_ here, so if the tick ever skipped
    // response processing, this test would stall out and fail the
    // iteration bound rather than passing vacuously.
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; }, /*initialArenaCapacity=*/64);
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "engine-tick-arena-grow-save-dir";
    std::filesystem::remove_all(saveDir);

    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);

    TestBlockBuffers buffers(2, 4);
    bool written = false;
    for (int iteration = 0; iteration < 10 && !written; ++iteration) {
        {
            synth::AudioBlock block = buffers.Block(4);
            engine.ProcessBlock(block, /*timestamp=*/0);
        }

        // MessageThreadTick's step 2 grows the arena when grow-pending is
        // set (clearing the barrier so the next ProcessBlock retries the
        // stash), and its step 3 (patchManager_.ProcessResponses()) drains
        // whatever response arrived this tick -- including the
        // SerializedJSON response a successful retry inside the preceding
        // ProcessBlock pushed onto patchOutputBus_. The tick is the only
        // thing under test that can make the version file show up on disk.
        engine.MessageThreadTick();

        written = synth::LatestPatchVersion(saveDir).has_value();
    }

    REQUIRE_TRUE(written);
    REQUIRE_TRUE(!engine.HasStashedPatchMessageForTest());
    REQUIRE_TRUE(!engine.IsArenaGrowPendingForTest());

    const auto latestVersion = synth::LatestPatchVersion(saveDir);
    REQUIRE_TRUE(latestVersion.has_value());
    REQUIRE_TRUE(std::filesystem::exists(*latestVersion));

    std::filesystem::remove_all(saveDir);
}

TEST_CASE(engine_logs_patch_apply_and_storage_batch_activity_for_slog_7) {
    // Regression for slog-7: the audio-thread patch drain (ProcessBlock) must
    // INFO-log each ApplyPatchMessage outcome, and the message-thread tick
    // must INFO-log storage-batch provisioning. Both run on ThreadId::Unknown
    // in this JUCE-free test binary (no ScopedThreadId tagging here), so
    // QueueSizeForTesting(ThreadId::Unknown) is where both land.
    synth::AsyncLogQueue& log = synth::AsyncLogQueue::s_instance;
    log.ResetForTesting();

    // --- Patch apply outcome, logged from ProcessBlock's audio-thread drain ---
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    // NewPatch() enqueues a RevertAllToDefault message onto patchInputBus_;
    // ProcessBlock's drain applies it synchronously and must log the outcome.
    const synth::PatchCommandResult newPatchResult = engine.Patches().NewPatch();
    REQUIRE_TRUE(newPatchResult.status == synth::PatchCommandStatus::Ok);

    const std::size_t queueSizeBeforePatchDrain = log.QueueSizeForTesting(synth::ThreadId::Unknown);
    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_TRUE(log.QueueSizeForTesting(synth::ThreadId::Unknown) > queueSizeBeforePatchDrain);

    // --- Storage-batch provisioning, logged from MessageThreadTick ---
    struct TinyGroupApp {
        static synth::RuntimeConfig Config() {
            synth::RuntimeConfig config;
            config.appName = "EngineLoggingTinyGroupTest";
            config.numAudioOutputs = 2;
            return config;
        }
        synth::AppContext* context = nullptr;
        synth::ParameterGroup* group = nullptr;
        synth::Bank* bank = nullptr;
        synth::BankSlot* slot = nullptr;
        synth::Parameter* carrier = nullptr;

        void Init(synth::AppContext* ctx) {
            context = ctx;
            group = &ctx->parameterManager->CreateGroup(
                {.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 2});
            carrier = &ctx->parameterManager->CreateParameter(*group, {.name = "Carrier", .defaultValue = 0.5f});
            auto& filler = ctx->parameterManager->CreateParameter(*group, {.name = "Filler", .defaultValue = 0.25f});
            (void)filler;
            bank = &ctx->parameterManager->CreateBank();
            bank->AddMapping(1, *carrier);
            bank->AddMapping(2, filler);
            slot = &ctx->parameterManager->CreateBankSlot();
            slot->AddPhysicalEncoder(1);
            slot->AddPhysicalEncoder(2);
            slot->AddPhysicalEncoder(3);
            slot->SelectBank(bank);
        }
        void ProcessBlock(synth::AudioBlock&) {}
    };

    synth::Engine<TinyGroupApp> tinyEngine([] { return std::uint64_t{0}; });
    tinyEngine.Initialize();
    tinyEngine.Prepare(48000.0, 256);

    synth::BankSlot* slot = tinyEngine.Application().slot;
    slot->HandlePress(1);  // triggers the deferred-growth path (ParameterStorageBatchNeeded)

    TestBlockBuffers tinyBuffers(2, 4);
    {
        synth::AudioBlock block = tinyBuffers.Block(4);
        tinyEngine.ProcessBlock(block, /*timestamp=*/0);
    }

    const std::size_t queueSizeBeforeTick = log.QueueSizeForTesting(synth::ThreadId::Unknown);
    tinyEngine.MessageThreadTick();  // drains parameterMessageOutBus_, logs storage-batch provisioning
    REQUIRE_TRUE(log.QueueSizeForTesting(synth::ThreadId::Unknown) > queueSizeBeforeTick);

    log.ResetForTesting();
}

namespace {

// App variant exercising the optional HasProcessFrame<App> control-rate
// hook (Engine::ProcessBlock step 4a): ProcessFrame() must run exactly once
// per block, after this block's messages have been applied (so it observes
// post-message-manager state) and before the app's own ProcessBlock(block)
// call.
struct ProcessFrameApp {
    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "EngineProcessFrameTest";
        config.numAudioOutputs = 2;
        return config;
    }
    synth::AppContext* context = nullptr;
    synth::ParameterId probeId = 0;
    int processFrameCalls = 0;
    int processBlockCalls = 0;
    // Set by ProcessFrame() each call: the probe scene center after this
    // block's messages have been applied, as observed from inside the hook.
    float probeSceneCenterDuringProcessFrame = -1.0f;
    // Sequence-order flag: true only while ProcessFrame() is executing, and
    // observed (then cleared) by ProcessBlock() -- proves ProcessFrame() ran
    // BEFORE ProcessBlock() for the same block, not merely that both ran.
    bool insideProcessFrame = false;
    bool processFrameRanBeforeProcessBlockThisCall = false;

    void Init(synth::AppContext* ctx) {
        context = ctx;
        auto& group = ctx->parameterManager->CreateGroup(
            {.numVoices = 1, .numModulators = 0, .numScenes = 1, .maxParameters = 4, .processLiteAlpha = 1.0f});
        auto& probe = ctx->parameterManager->CreateParameter(group, {.name = "Probe", .defaultValue = 0.25f});
        probeId = probe.Id();

        auto& bank = ctx->parameterManager->CreateBank();
        bank.AddMapping(/*encoderId=*/0, probe);
        auto& slot = ctx->parameterManager->CreateBankSlot();
        slot.AddPhysicalEncoder(/*encoderId=*/0);
        slot.SelectBank(&bank);
    }
    void ProcessFrame() {
        ++processFrameCalls;
        insideProcessFrame = true;
        // uiBus_.Process(timestamp) (message-manager application) runs
        // earlier in Engine::ProcessBlock's binding order than ProcessFrame()
        // (uiBus_.Process -> midiBus_.Process -> ProcessFrame() ->
        // app_.ProcessBlock()), so SceneCenter(0) (which MessageInBus::Apply's
        // ParamIncDec handling writes directly, via Parameter::HandleIncDec)
        // already reflects any message applied earlier in this same block --
        // this is the "post-message-manager state" the hook is documented to
        // observe.
        probeSceneCenterDuringProcessFrame = context->parameterManager->ParameterById(probeId).SceneCenter(0);
        insideProcessFrame = false;
    }
    void ProcessBlock(synth::AudioBlock& block) {
        ++processBlockCalls;
        // ProcessFrame() for this block must already have run (and returned)
        // by the time ProcessBlock() is called.
        processFrameRanBeforeProcessBlockThisCall = (processFrameCalls == processBlockCalls) && !insideProcessFrame;
        for (int channel = 0; channel < block.numOutputChannels; ++channel) {
            float* out = block.outputs[channel];
            if (out == nullptr) {
                continue;
            }
            for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
                out[frame] = 0.0f;
            }
        }
    }
};

}  // namespace

TEST_CASE(engine_process_frame_hook_runs_once_per_block_after_messages_before_process_block) {
    REQUIRE_TRUE(synth::HasProcessFrame<ProcessFrameApp>);
    REQUIRE_TRUE(!synth::HasProcessFrame<EngineTestApp>);  // EngineTestApp opts out: concept must not false-positive

    synth::Engine<ProcessFrameApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    // Push a message so ProcessFrame() observing the post-message scene
    // center is a meaningful check (not just reading the untouched default).
    engine.UiBus().Push(synth::MessageIn::ParamIncDec(/*timestamp=*/0, /*slotIx=*/0, /*position=*/0, /*delta=*/0.3f));

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }

    REQUIRE_TRUE(engine.Application().processFrameCalls == 1);
    REQUIRE_TRUE(engine.Application().processBlockCalls == 1);
    REQUIRE_TRUE(engine.Application().processFrameRanBeforeProcessBlockThisCall);
    REQUIRE_NEAR(engine.Application().probeSceneCenterDuringProcessFrame, 0.55f, 1e-4f);

    // A second block: call counts stay in lockstep (exactly once per block
    // each), reconfirming the once-per-block contract, not a one-shot fluke.
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/1);
    }
    REQUIRE_TRUE(engine.Application().processFrameCalls == 2);
    REQUIRE_TRUE(engine.Application().processBlockCalls == 2);
    REQUIRE_TRUE(engine.Application().processFrameRanBeforeProcessBlockThisCall);
}

TEST_CASE(engine_revert_all_to_default_restores_app_init_midi_profile_not_empty) {
    // Regression for the default-MIDI-profile gap: Engine::Initialize() must
    // snapshot defaultInstrumentConfig_ from the live instrument the app's
    // Init() configured, BEFORE any startup patch applies. Without that
    // snapshot, RevertAllToDefault (dispatched by Patches().NewPatch()
    // below) resets instrumentConfig_ to a default-constructed (empty,
    // zero-controller) MidiInstrumentConfig instead of back to the app's
    // real default, silently dropping MIDI control surface responsiveness.
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // Init() adds a non-empty controller (encoderInput)

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    // Sanity: the live instrument really is non-empty right after
    // Initialize, matching what the app's Init() configured.
    REQUIRE_TRUE(!engine.Context().instrument->controllers.empty());
    REQUIRE_TRUE(engine.Context().instrument->controllers.front().config.encoderInput.has_value());

    TestBlockBuffers buffers(2, 4);

    // NewPatch() dispatches RevertAllToDefault onto patchInputBus_ (see
    // PatchManager::NewPatch() in src/PatchPersistence.cpp); no patch is
    // loaded/saved here, so this exercises the "brand new patch" / revert
    // path directly against whatever Initialize() snapshotted as default.
    // NewPatch() itself returns Ok synchronously (it only enqueues the
    // message); the actual revert happens when ProcessBlock drains it below.
    const synth::PatchCommandResult newPatchResult = engine.Patches().NewPatch();
    REQUIRE_TRUE(newPatchResult.status == synth::PatchCommandStatus::Ok);

    {
        // ProcessBlock drains patchInputBus_ and applies RevertAllToDefault
        // synchronously (no arena growth needed for this message type).
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    // The live instrument must still equal the app's Init-configured default
    // -- i.e. still have a controller with an encoderInput mapping -- NOT
    // have been reset to an empty (zero-controller) MidiInstrumentConfig{}.
    REQUIRE_TRUE(!engine.Context().instrument->controllers.empty());
    REQUIRE_TRUE(engine.Context().instrument->controllers.front().config.encoderInput.has_value());

    // Also confirm the default instrument snapshot itself carries the
    // controller (not just that the live instrument happens to still have
    // it): the revert path copies defaultInstrumentConfig_ into
    // instrumentConfig_, so if the snapshot were empty the assertion above
    // would already have failed; this checks the snapshot directly for a
    // clearer failure signal.
    REQUIRE_TRUE(!engine.Context().defaultInstrument->controllers.empty());
    REQUIRE_TRUE(engine.Context().defaultInstrument->controllers.front().config.encoderInput.has_value());

    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_revert_all_to_default_preserves_host_audio_selection) {
    // Patch new/revert is parameter-only: a host-selected audio device is
    // runtime configuration and must survive RevertAllToDefault.
    EngineTestApp::processLiteAlpha = 1.0f;

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Speakers", .inputDeviceName = "Mic"});
    engine.Prepare(48000.0, 256);

    // Sanity: the live audio device state carries the host-selected value
    // after Initialize.
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "Speakers");
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().inputDeviceName == "Mic");

    TestBlockBuffers buffers(2, 4);

    // NewPatch() dispatches RevertAllToDefault onto patchInputBus_; no patch
    // is loaded/saved here, so this exercises the "brand new patch" / revert
    // path directly against whatever Initialize() snapshotted as default.
    const synth::PatchCommandResult newPatchResult = engine.Patches().NewPatch();
    REQUIRE_TRUE(newPatchResult.status == synth::PatchCommandStatus::Ok);

    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "Speakers");
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().inputDeviceName == "Mic");
}

TEST_CASE(engine_tick_preserves_audio_device_when_patch_load_has_legacy_audio_section) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());  // sanity: starts empty

    int callbackCalls = 0;
    engine.SetAudioDeviceChangedCallback([&]() { ++callbackCalls; });

    const std::filesystem::path patchDir =
        std::filesystem::temp_directory_path() / "engine-tick-audio-device-changed-patch-dir";
    std::filesystem::remove_all(patchDir);
    WriteProbePatchVersion(patchDir, 0.9f, std::chrono::system_clock::now(),
                           synth::AudioDeviceState{.outputDeviceName = "Interface A", .inputDeviceName = ""});

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(patchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().inputDeviceName.empty());

    engine.MessageThreadTick();
    engine.MessageThreadTick();

    REQUIRE_TRUE(callbackCalls == 0);

    std::filesystem::remove_all(patchDir);
}

TEST_CASE(engine_tick_does_not_fire_audio_device_changed_callback_when_load_has_no_section) {
    // A runtime patch load whose document has no audioDevice section leaves
    // audioDeviceState_ untouched, so the callback must not fire.
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    int callbackCalls = 0;
    engine.SetAudioDeviceChangedCallback([&]() { ++callbackCalls; });

    const std::filesystem::path patchDir =
        std::filesystem::temp_directory_path() / "engine-tick-audio-device-unchanged-patch-dir";
    std::filesystem::remove_all(patchDir);
    WriteProbePatchVersion(patchDir, 0.9f, std::chrono::system_clock::now());  // no audioDevice section

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(patchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    REQUIRE_TRUE(callbackCalls == 0);
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());

    std::filesystem::remove_all(patchDir);
}

TEST_CASE(engine_initialize_preserves_audio_device_for_startup_patch_load) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-initialize-audio-device-callback-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);
    WriteProbePatchVersion(paths.patchesRoot / "AAA", 0.75f, std::chrono::system_clock::now(),
                           synth::AudioDeviceState{.outputDeviceName = "Startup Interface", .inputDeviceName = ""});

    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);

    int callbackCalls = 0;
    std::string outputNameAtCallback;
    engine.SetAudioDeviceChangedCallback([&]() {
        ++callbackCalls;
        outputNameAtCallback = engine.AudioDeviceSnapshot().outputDeviceName;
    });

    engine.Initialize();

    REQUIRE_TRUE(callbackCalls == 0);
    REQUIRE_TRUE(outputNameAtCallback.empty());
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());

    std::filesystem::remove_all(dataRoot);
}

TEST_CASE(engine_startup_and_runtime_patch_loads_do_not_change_audio_device_shadow) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "engine-audio-state-shadow-sync-data-root";
    std::filesystem::remove_all(dataRoot);
    const synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);
    // Startup patch with audioDevice section
    WriteProbePatchVersion(paths.patchesRoot / "AAA", 0.75f, std::chrono::system_clock::now(),
                           synth::AudioDeviceState{.outputDeviceName = "Startup Device", .inputDeviceName = ""});

    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.SetRuntimeDataPaths(paths);

    int callbackCalls = 0;
    engine.SetAudioDeviceChangedCallback([&]() {
        ++callbackCalls;
    });

    engine.Initialize();
    REQUIRE_TRUE(callbackCalls == 0);
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());

    engine.Prepare(48000.0, 256);

    // Now drive a runtime patch message WITHOUT an audioDevice section
    // through ProcessBlock + MessageThreadTick twice. The callback should
    // not fire again (shadow is already synced).
    const std::filesystem::path runtimePatchDir =
        std::filesystem::temp_directory_path() / "engine-audio-state-shadow-runtime-dir";
    std::filesystem::remove_all(runtimePatchDir);
    WriteProbePatchVersion(runtimePatchDir, 0.9f, std::chrono::system_clock::now());

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(runtimePatchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    REQUIRE_TRUE(callbackCalls == 0);

    engine.MessageThreadTick();
    REQUIRE_TRUE(callbackCalls == 0);

    engine.MessageThreadTick();
    REQUIRE_TRUE(callbackCalls == 0);

    std::filesystem::remove_all(dataRoot);
    std::filesystem::remove_all(runtimePatchDir);
}

TEST_CASE(engine_revert_after_host_selection_preserves_audio_device) {
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    // Sanity: startup default is empty (no app-configured device, no startup
    // patch).
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName.empty());

    int callbackCalls = 0;
    std::string outputNameAtCallback;
    engine.SetAudioDeviceChangedCallback([&]() {
        ++callbackCalls;
        outputNameAtCallback = engine.AudioDeviceSnapshot().outputDeviceName;
    });

    // Simulate a host-initiated selection (Runtime::ApplyAudioDeviceSelection's
    // path): the host picks "DeviceX" in its combo.
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "DeviceX", .inputDeviceName = ""});

    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "DeviceX");
    REQUIRE_TRUE(callbackCalls == 0);  // host-initiated changes never fire the callback (see (b) below)

    // Drive a patch REVERT through ProcessBlock + MessageThreadTick.
    const synth::PatchCommandResult revertResult = engine.Patches().RevertPatch();
    REQUIRE_TRUE(revertResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    REQUIRE_TRUE(callbackCalls == 0);
    REQUIRE_TRUE(outputNameAtCallback.empty());
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "DeviceX");
}

TEST_CASE(engine_set_audio_device_from_host_fires_no_callback) {
    // (b): SetAudioDeviceFromHost itself must never fire
    // audioDeviceChangedCallback_ -- host-initiated changes are by
    // definition already known to the host that just made them; the
    // callback exists solely to notify the host of future engine-sourced
    // runtime configuration changes IT did not originate.
    EngineTestApp::processLiteAlpha = 1.0f;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    int callbackCalls = 0;
    engine.SetAudioDeviceChangedCallback([&]() { ++callbackCalls; });

    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Interface B", .inputDeviceName = ""});
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "Interface B");
    REQUIRE_TRUE(callbackCalls == 0);

    // Also confirm no *pending* notification was left for the tick to
    // deliver later -- a tick with nothing else happening must stay at 0.
    engine.MessageThreadTick();
    REQUIRE_TRUE(callbackCalls == 0);

    // A second host-initiated call to a different value likewise fires
    // nothing.
    engine.SetAudioDeviceFromHost(synth::AudioDeviceState{.outputDeviceName = "Interface C", .inputDeviceName = ""});
    REQUIRE_TRUE(engine.AudioDeviceSnapshot().outputDeviceName == "Interface C");
    REQUIRE_TRUE(callbackCalls == 0);
    engine.MessageThreadTick();
    REQUIRE_TRUE(callbackCalls == 0);
}

// ---------------------------------------------------------------------------
// Task 4: engine-owned instrument with serialized edits (LiveInstrument(),
// DefaultInstrument(), EditInstrument()).
// ---------------------------------------------------------------------------

TEST_CASE(engine_default_instrument_equals_app_seeded_instrument_after_initialize) {
    // Property 1 (brief Step 1): the post-Init() default snapshot must equal
    // the instrument the app's Init() actually seeded -- not an empty
    // MidiInstrumentConfig{}. EngineTestApp::Init() adds a "test"/Generic
    // controller with encoderInput set when wantEncoderMidiInput is true.
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();

    const synth::MidiInstrumentConfig& live = engine.LiveInstrument();
    const synth::MidiInstrumentConfig& def = engine.DefaultInstrument();

    REQUIRE_TRUE(live.controllers.size() == 1);
    REQUIRE_TRUE(def.controllers.size() == 1);
    REQUIRE_TRUE(live.controllers.front().name == "test");
    REQUIRE_TRUE(def.controllers.front().name == "test");
    REQUIRE_TRUE(live.controllers.front().kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(def.controllers.front().kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(live.controllers.front().config.encoderInput.has_value());
    REQUIRE_TRUE(def.controllers.front().config.encoderInput.has_value());

    // Also reachable through AppContext (message-thread surface apps use).
    REQUIRE_TRUE(engine.Context().instrument == &engine.LiveInstrument());
    REQUIRE_TRUE(engine.Context().defaultInstrument == &engine.DefaultInstrument());

    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_edit_instrument_mutation_visible_and_fires_rebuilt_callback_once) {
    // Property 2 (brief Step 1): EditInstrument's mutation must be visible in
    // LiveInstrument() immediately afterward, and must trigger the
    // MIDI-processors-rebuilt callback exactly once (not zero, not twice).
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;  // start from an empty instrument

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.LiveInstrument().controllers.empty());

    int callbackCalls = 0;
    engine.SetMidiProcessorsRebuiltCallback([&]() { ++callbackCalls; });

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        synth::MidiControllerSlot slot;
        slot.name = "edited";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.config.encoderInput = synth::EncoderMidiInConfig{};
        instrument.controllers.push_back(std::move(slot));
    });

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "edited");
    REQUIRE_TRUE(callbackCalls == 1);  // fired exactly once

    // A second EditInstrument call with a no-op edit still rebuilds and
    // fires again (EditInstrument always rebuilds/fires after applying,
    // regardless of whether the edit actually changed anything -- same
    // unconditional-fire contract as RebuildMidiProcessors()'s other
    // callers).
    engine.EditInstrument([](synth::MidiInstrumentConfig&) {});
    REQUIRE_TRUE(callbackCalls == 2);
}

TEST_CASE(engine_patch_save_perturb_load_preserves_instrument_through_production_messages) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // Init() seeds one "test" controller

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    TestBlockBuffers buffers(2, 4);

    // Save a patch while the instrument is Init() configured ("test"/Generic).
    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "engine-instrument-round-trip-save-dir";
    std::filesystem::remove_all(saveDir);
    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);
    bool written = false;
    for (int i = 0; i < 16 && !written; ++i) {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
        engine.MessageThreadTick();
        written = synth::LatestPatchVersion(saveDir).has_value();
    }
    REQUIRE_TRUE(written);

    // Perturb the live instrument via EditInstrument (production entry
    // point): rename the controller and change its kind.
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "perturbed"));
    });
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "perturbed");

    // Load the saved patch back through PatchManager::LoadPatch, exactly as
    // a production host would.
    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(saveDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    // Patch load restores parameter state only; runtime MIDI configuration is
    // still the host-perturbed state.
    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "perturbed");
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().config.encoderInput.has_value());

    std::filesystem::remove_all(saveDir);
    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_revert_preserves_current_instrument) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);

    // Perturb: add a second controller and rename the first.
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "renamed"));
        synth::MidiControllerSlot extra;
        extra.name = "extra";
        extra.kind = synth::MidiProfileKind::Generic;
        REQUIRE_TRUE(instrument.AddController(std::move(extra)));
    });
    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 2);

    TestBlockBuffers buffers(2, 4);
    const synth::PatchCommandResult newPatchResult = engine.Patches().NewPatch();
    REQUIRE_TRUE(newPatchResult.status == synth::PatchCommandStatus::Ok);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 2);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "renamed");
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().kind == synth::MidiProfileKind::Generic);

    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_edit_instrument_and_pending_patch_load_same_tick_preserves_edit) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // Init() seeds "test"/Generic

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 256);

    // Build a patch document whose instrument differs from the live one:
    // rename the single controller to "loaded" and give it a different kind
    // (still Generic-compatible: only encoderInput is set, matching
    // EngineTestApp's Init() shape) so FromJSON/SlotValidForKind accepts it.
    synth::MidiInstrumentConfig loadedInstrument = engine.LiveInstrument();
    REQUIRE_TRUE(loadedInstrument.RenameController(0, "loaded"));

    const std::filesystem::path patchDir =
        std::filesystem::temp_directory_path() / "engine-instrument-serialized-order-patch-dir";
    std::filesystem::remove_all(patchDir);
    WriteProbePatchVersion(patchDir, 0.6f, std::chrono::system_clock::now(), synth::AudioDeviceState{},
                           loadedInstrument);

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(patchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    // Enqueue the load (queued onto patchInputBus_ by LoadPatch above, not
    // yet drained), then immediately race it with a host-initiated
    // EditInstrument renaming the controller to "edited" -- before the next
    // ProcessBlock has a chance to drain the load.
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "edited"));
    });

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, /*timestamp=*/0);
    }
    engine.MessageThreadTick();

    // Patch load is parameter-only, so the host edit must remain the final
    // runtime instrument state.
    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    const std::string& finalName = engine.LiveInstrument().controllers.front().name;
    REQUIRE_TRUE(finalName == "edited");
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().config.encoderInput.has_value());

    std::filesystem::remove_all(patchDir);
    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(engine_rebuild_midi_processors_observes_fully_applied_edit_snapshot) {
    // Critical-fix regression (RebuildMidiProcessors() data race): the fix
    // makes RebuildMidiProcessors() copy the WHOLE controllers vector out of
    // instrumentConfig_ while holding audioDeviceStateMutex_, the same lock
    // EditInstrument()/the audio-thread patch drain hold while mutating that
    // member (see audioDeviceStateMutex_'s doc comment). This single-threaded
    // harness cannot manufacture a real data race, but it can assert the
    // snapshot-then-build sequence is internally consistent: a rebuild
    // immediately after a serialized EditInstrument mutation must reflect
    // that mutation's fully-applied result (never an empty or partially
    // mutated profile), for both the empty-instrument (midiProcessors_ has
    // size 0) and populated-instrument (one result per controller slot)
    // cases.
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // Init() seeds one "test"/Generic controller

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();

    // Populated case: EditInstrument adds encoder-output config to the
    // existing controller's profile, then explicitly rebuilds (mirroring
    // EditInstrument's own post-edit rebuild, but isolating the call so this
    // test documents RebuildMidiProcessors()'s contract directly).
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(!instrument.controllers.empty());
        instrument.controllers.front().config.encoderOutput = synth::EncoderMidiOutConfig{};
    });
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().config.encoderOutput.has_value());
    engine.RebuildMidiProcessorsForTest();
    // A rebuilt processor chain must exist and reflect the edited (non-empty)
    // profile: MidiInputProcessor(0) stays non-null because encoderInput is
    // still set on the same controller slot.
    REQUIRE_TRUE(engine.MidiControllerCount() == 1);
    REQUIRE_TRUE(engine.MidiInputProcessor(0) != nullptr);

    // Empty case: remove the only controller, then rebuild again. The
    // snapshot copy must see the now-empty controllers list (not a stale or
    // torn view of the prior populated state) and yield zero processor
    // chains, so both MidiControllerCount() and the slot-0 accessor reflect
    // "no controllers".
    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) { instrument.controllers.clear(); });
    REQUIRE_TRUE(engine.LiveInstrument().controllers.empty());
    engine.RebuildMidiProcessorsForTest();
    REQUIRE_TRUE(engine.MidiControllerCount() == 0);
    REQUIRE_TRUE(engine.MidiInputProcessor(0) == nullptr);

    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

TEST_CASE(blacklisted_midi_controller_profile_is_drop_only_and_emits_nothing) {
    synth::MessageInBus bus(nullptr, 16);
    synth::MidiControllerProfileResult profile =
        synth::CreateBlacklistedMidiControllerProfile();

    REQUIRE_TRUE(profile.input != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::DropMidiInProcessor*>(profile.input.get()) != nullptr);
    REQUIRE_TRUE(profile.input->Thru() == nullptr);
    REQUIRE_TRUE(profile.inputThru.empty());
    REQUIRE_TRUE(profile.outputs.empty());

    profile.input->SetMessageInBus(&bus);
    profile.input->Process(synth::BasicMidi::CC(1, 2, 3, 127));
    profile.input->Process(
        synth::BasicMidi::SysEx(2, std::vector<std::uint8_t>{0xF0, 0x7D, 0x01, 0xF7}));
    profile.input->Process(synth::BasicMidi::Clock(3));
    profile.input->Process(synth::BasicMidi::TransportStart(4));
    profile.input->Process(synth::BasicMidi::TransportContinue(5));
    profile.input->Process(synth::BasicMidi::TransportStop(6));
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(engine_rebuild_switches_active_and_blacklisted_processors_without_reading_blacklisted_config) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{123}; });
    engine.Initialize();

    synth::MidiControllerSlot slot;
    slot.name = "switchable";
    slot.kind = synth::MidiProfileKind::Generic;
    slot.config.encoderInput = synth::EncoderMidiInConfig{
        .turns = {{.control = {.channel = 2, .cc = 3}, .slotIx = 0, .position = 0}},
    };
    engine.LiveInstrument().controllers = {slot};
    engine.RebuildMidiProcessorsForTest();

    synth::MidiInProcessor* active = engine.MidiInputProcessor(0);
    REQUIRE_TRUE(active != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::DropMidiInProcessor*>(active) == nullptr);
    REQUIRE_TRUE(active->Thru() != nullptr);
    active->Process(synth::BasicMidi::CC(1, 2, 3, 127));
    active->Process(synth::BasicMidi::Clock(2));
    REQUIRE_TRUE(engine.MidiBus().Size() == 2);

    synth::MessageIn drained;
    while (engine.MidiBus().Pop(drained, std::numeric_limits<std::uint64_t>::max())) {
    }

    engine.LiveInstrument().controllers.front().disposition =
        synth::MidiControllerDisposition::Blacklisted;
    engine.RebuildMidiProcessorsForTest();

    synth::MidiInProcessor* blacklisted = engine.MidiInputProcessor(0);
    REQUIRE_TRUE(blacklisted != nullptr);
    REQUIRE_TRUE(blacklisted != active);
    REQUIRE_TRUE(dynamic_cast<synth::DropMidiInProcessor*>(blacklisted) != nullptr);
    REQUIRE_TRUE(blacklisted->Thru() == nullptr);
    blacklisted->Process(synth::BasicMidi::CC(3, 2, 3, 127));
    blacklisted->Process(synth::BasicMidi::Clock(4));
    REQUIRE_TRUE(engine.MidiBus().Size() == 0);

    engine.LiveInstrument().controllers.front().disposition =
        synth::MidiControllerDisposition::Active;
    engine.RebuildMidiProcessorsForTest();

    synth::MidiInProcessor* reactivated = engine.MidiInputProcessor(0);
    REQUIRE_TRUE(reactivated != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::DropMidiInProcessor*>(reactivated) == nullptr);
    REQUIRE_TRUE(reactivated->Thru() != nullptr);
    reactivated->Process(synth::BasicMidi::CC(5, 2, 3, 127));
    reactivated->Process(synth::BasicMidi::Clock(6));
    REQUIRE_TRUE(engine.MidiBus().Size() == 2);
}

TEST_CASE(engine_rebuild_preserves_ordered_slots_when_blacklisted_middle_slot_is_deleted) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();

    synth::MidiControllerSlot first;
    first.name = "first";
    synth::MidiControllerSlot middle;
    middle.name = "middle";
    middle.disposition = synth::MidiControllerDisposition::Blacklisted;
    synth::MidiControllerSlot last;
    last.name = "last";
    engine.LiveInstrument().controllers = {first, middle, last};
    engine.RebuildMidiProcessorsForTest();

    REQUIRE_TRUE(engine.MidiControllerCount() == 3);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(
                     engine.MidiInputProcessor(0)) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::DropMidiInProcessor*>(
                     engine.MidiInputProcessor(1)) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(
                     engine.MidiInputProcessor(2)) != nullptr);

    engine.LiveInstrument().RemoveController(1);
    engine.RebuildMidiProcessorsForTest();

    REQUIRE_TRUE(engine.MidiControllerCount() == 2);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(
                     engine.MidiInputProcessor(0)) != nullptr);
    synth::MidiInProcessor* shifted = engine.MidiInputProcessor(1);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(shifted) != nullptr);
    REQUIRE_TRUE(engine.MidiInputProcessor(2) == nullptr);

    shifted->Process(synth::BasicMidi::Clock(99));
    synth::MessageIn clock;
    REQUIRE_TRUE(engine.MidiBus().Pop(
        clock, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(clock.type == synth::MessageIn::Type::Clock);
    REQUIRE_TRUE(clock.origin == synth::MessageIn::Origin::ExternalMidi);
    REQUIRE_TRUE(clock.externalControllerSlot == 1);
    REQUIRE_TRUE(engine.MidiBus().Size() == 0);
}

TEST_CASE(engine_rebuild_retains_pending_absolute_feedback_across_bank_route_change) {
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = false;
    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{77}; });
    engine.Initialize();

    synth::MidiControllerProfileConfig profile;
    profile.encoderInput = synth::EncoderMidiInConfig{
        .mode = synth::EncoderMode::Absolute,
        .turns = {{.control = {.channel = 7, .cc = 74}, .slotIx = 0, .position = 0}},
    };
    synth::MidiControllerSlot slot;
    slot.name = "absolute";
    slot.kind = synth::MidiProfileKind::Generic;
    slot.config = std::move(profile);
    engine.LiveInstrument().controllers = {std::move(slot)};
    engine.RebuildMidiProcessorsForTest();

    EngineFakeMidiSink sink;
    synth::MidiSender* sender = engine.Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();

    TestBlockBuffers buffers(2, 4);
    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, 77);
    }
    engine.MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sink.received.clear();

    synth::MidiInProcessor* input = engine.MidiInputProcessor(0);
    REQUIRE_TRUE(input != nullptr);
    input->Process(synth::BasicMidi::CC(0, 7, 74, 96));
    engine.RebuildMidiProcessors();
    REQUIRE_TRUE(engine.Application().emptyBank != nullptr);
    engine.Application().probeSlot->SelectBank(engine.Application().emptyBank);

    // The rebuilt output chain sees the still-pending expectation and must
    // remain gated before the audio thread consumes and publishes it.
    engine.MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.received.empty());

    {
        synth::AudioBlock block = buffers.Block(4);
        engine.ProcessBlock(block, 77);
    }
    engine.MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();

    REQUIRE_TRUE(sink.received.size() == 1);
    REQUIRE_TRUE(sink.received.front().IsCC());
    REQUIRE_TRUE(sink.received.front().Channel() == 7);
    REQUIRE_TRUE(sink.received.front().GetCC() == 74);
    REQUIRE_TRUE(sink.received.front().GetValue() == 0);
}

TEST_CASE(engine_instrument_snapshot_is_deep_copy_equal_to_live_instrument) {
    // Task 4 review, Critical fix regression: InstrumentSnapshot() is the
    // locked running-state read surface any message-thread reader concurrent
    // with running audio (e.g. ControllersPage's per-tick VM refresh) must
    // use instead of the unlocked LiveInstrument() reference -- see both
    // methods' doc comments. This single-threaded harness cannot manufacture
    // a real data race, but it can assert the copy is (a) equal in content to
    // the live instrument at the moment of the call, and (b) a true deep
    // copy: mutating the returned value must not alter instrumentConfig_
    // (i.e. the snapshot does not alias the live controllers vector or its
    // per-slot config/endpoint members).
    EngineTestApp::processLiteAlpha = 1.0f;
    EngineTestApp::wantEncoderMidiInput = true;  // Init() seeds one "test"/Generic controller

    synth::Engine<EngineTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(!instrument.controllers.empty());
        instrument.controllers.front().input = synth::MidiEndpointRef{"in-id", "In Device"};
        instrument.controllers.front().output = synth::MidiEndpointRef{"out-id", "Out Device"};
    });

    synth::MidiInstrumentConfig snapshot = engine.InstrumentSnapshot();

    // (a) Equal in content to the live instrument at the time of the call.
    const synth::MidiInstrumentConfig& live = engine.LiveInstrument();
    REQUIRE_TRUE(snapshot.controllers.size() == live.controllers.size());
    REQUIRE_TRUE(snapshot.controllers.front().name == live.controllers.front().name);
    REQUIRE_TRUE(snapshot.controllers.front().kind == live.controllers.front().kind);
    REQUIRE_TRUE(snapshot.controllers.front().config.encoderInput.has_value() ==
                 live.controllers.front().config.encoderInput.has_value());
    REQUIRE_TRUE(snapshot.controllers.front().input.identifier == live.controllers.front().input.identifier);
    REQUIRE_TRUE(snapshot.controllers.front().input.name == live.controllers.front().input.name);
    REQUIRE_TRUE(snapshot.controllers.front().output.identifier == live.controllers.front().output.identifier);
    REQUIRE_TRUE(snapshot.controllers.front().output.name == live.controllers.front().output.name);

    // (b) Deep copy: mutating the snapshot must not affect the live
    // instrument, whether by appending a controller (would alias a shared
    // vector buffer) or by editing a field on the existing slot (would alias
    // a shared string/optional).
    snapshot.controllers.front().input.identifier = "mutated-in-id";
    snapshot.controllers.front().name = "mutated-name";
    synth::MidiControllerSlot extra;
    extra.name = "extra";
    snapshot.controllers.push_back(std::move(extra));

    REQUIRE_TRUE(engine.LiveInstrument().controllers.size() == 1);
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().name == "test");
    REQUIRE_TRUE(engine.LiveInstrument().controllers.front().input.identifier == "in-id");

    EngineTestApp::wantEncoderMidiInput = false;  // restore default for subsequent tests
}

namespace {

// Records every dispatched action so tests can assert what the engine sent
// to the app's own surface.
struct MidiCatalogTestSurface final : synth::ui::Surface {
    std::vector<synth::ui::Action> dispatched;

    synth::ui::NodeTree BuildTree() override {
        constexpr synth::ui::Bounds bounds{0.0f, 0.0f, 100.0f, 100.0f};
        synth::ui::Builder builder;
        builder.Root("midi-catalog-test.root", bounds);
        return builder.Build(bounds);
    }
    void SetActionHandler(ActionHandler) override {}
    void DispatchAction(const synth::ui::Action& action) override { dispatched.push_back(action); }
};

// An app with a MIDI catalog: satisfies HasMidiCatalog<App> and, via
// PortableSurface(), SynthApplication<App>. Its bank/slot layout gives
// position 5 a pressable encoder with one modulator, so a ParamPush that
// reaches the library (catalog forwarding off) opens the modulation view,
// exactly as it does for an app without a catalog.
struct MidiCatalogTestApp {
    static inline synth::MidiAppCatalog catalog;
    synth::AppContext* context = nullptr;
    synth::BankSlot* probeSlot = nullptr;
    MidiCatalogTestSurface surface;

    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "MidiCatalogTest";
        config.numAudioOutputs = 2;
        return config;
    }

    void Init(synth::AppContext* ctx) {
        context = ctx;
        auto& group = ctx->parameterManager->CreateGroup({.numVoices = 1,
                                                           .numModulators = 1,
                                                           .numScenes = 1,
                                                           .maxParameters = 4});
        for (synth::ModulatorMetadata& metadata : group.GetModulators().Metadata()) {
            metadata.connected = true;
        }
        auto& carrier = ctx->parameterManager->CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
        auto& bank = ctx->parameterManager->CreateBank();
        bank.AddMapping(5, carrier);
        probeSlot = &ctx->parameterManager->CreateBankSlot();
        for (synth::PhysicalEncoderId encoderId = 0; encoderId <= 5; ++encoderId) {
            probeSlot->AddPhysicalEncoder(encoderId);
        }
        probeSlot->SelectBank(&bank);
    }
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface; }
    synth::MidiAppCatalog MidiCatalog() const { return catalog; }
};

}  // namespace

TEST_CASE(engine_dispatches_catalog_app_actions_to_surface) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.actions.push_back({.action = "test.plain", .value = "3", .label = "Plain"});
    MidiCatalogTestApp::catalog.actions.push_back({.action = "test.analog",
                                                    .value = "",
                                                    .label = "Analog",
                                                    .analogRange = std::make_pair(30.0f, 300.0f)});

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::AppAction(0, 0, 0.0f)));
    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::AppAction(0, 1, 0.5f)));

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();

    const auto& dispatched = engine.Application().surface.dispatched;
    REQUIRE_TRUE(dispatched.size() == 2);
    REQUIRE_TRUE(dispatched[0].name == "test.plain");
    REQUIRE_TRUE(dispatched[0].value == "3");
    REQUIRE_TRUE(dispatched[1].name == "test.analog");
    REQUIRE_TRUE(dispatched[1].value == std::to_string(165.0f));
}

TEST_CASE(engine_app_action_out_of_range_dispatches_nothing) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.actions.push_back({.action = "test.plain", .value = "3", .label = "Plain"});

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::AppAction(0, 5, 0.0f)));

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();

    REQUIRE_TRUE(engine.Application().surface.dispatched.empty());
}

TEST_CASE(engine_rebuild_resolves_app_action_rows_and_drops_unknown_ones) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.actions.push_back({.action = "test.known", .value = "1", .label = "Known"});

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        synth::MidiControllerSlot slot;
        slot.name = "catalog-controller";
        slot.kind = synth::MidiProfileKind::Generic;

        synth::MidiControllerSystemMessageAssociation known;
        known.control = synth::MidiControlAddress{.channel = 0, .cc = 10};
        known.press = synth::MessageIn::AppAction(0, 0, 0.0f);
        known.appAction = "test.known";
        known.appActionValue = "1";
        known.feedback = known.press;

        synth::MidiControllerSystemMessageAssociation unknown;
        unknown.control = synth::MidiControlAddress{.channel = 0, .cc = 11};
        unknown.press = synth::MessageIn::AppAction(0, 0, 0.0f);
        unknown.appAction = "test.unknown";
        unknown.appActionValue = "9";
        unknown.feedback = unknown.press;

        slot.config.systemMessages.push_back(known);
        slot.config.systemMessages.push_back(unknown);
        instrument.controllers.push_back(std::move(slot));
    });

    // The persisted instrument keeps both rows, including the one the
    // catalog cannot currently resolve, so a later catalog that knows it
    // gets it back.
    const synth::MidiInstrumentConfig snapshot = engine.InstrumentSnapshot();
    REQUIRE_TRUE(snapshot.controllers.size() == 1);
    REQUIRE_TRUE(snapshot.controllers.front().config.systemMessages.size() == 2);

    synth::MidiInProcessor* processor = engine.MidiInputProcessor(0);
    REQUIRE_TRUE(processor != nullptr);

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);

    processor->Process(synth::BasicMidi::CC(0, 0, 10, 127));
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();

    const auto& dispatched = engine.Application().surface.dispatched;
    REQUIRE_TRUE(dispatched.size() == 1);
    REQUIRE_TRUE(dispatched[0].name == "test.known");

    // The unresolved row was dropped from the built profile, so its control
    // address has no association left to match.
    processor->Process(synth::BasicMidi::CC(0, 0, 11, 127));
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();
    REQUIRE_TRUE(engine.Application().surface.dispatched.size() == 1);
}

TEST_CASE(engine_forwards_encoder_press_to_catalog_action_instead_of_opening_modulation_view) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.encoderPressAction = "test.press";

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::ParamPush(0, 0, 5)));

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();

    const auto& dispatched = engine.Application().surface.dispatched;
    REQUIRE_TRUE(dispatched.size() == 1);
    REQUIRE_TRUE(dispatched[0].name == "test.press");
    REQUIRE_TRUE(dispatched[0].value == "5");
    REQUIRE_TRUE(!engine.Application().probeSlot->SelectedBank()->ShowingModulation());
}

TEST_CASE(engine_encoder_press_without_catalog_forwarding_opens_modulation_view_as_today) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.encoderPressAction.clear();

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    REQUIRE_TRUE(engine.MidiBus().Push(synth::MessageIn::ParamPush(0, 0, 5)));

    TestBlockBuffers buffers(2, 32);
    synth::AudioBlock block = buffers.Block(32);
    engine.ProcessBlock(block, 0);
    engine.MessageThreadTick();

    REQUIRE_TRUE(engine.Application().surface.dispatched.empty());
    REQUIRE_TRUE(engine.Application().probeSlot->SelectedBank()->ShowingModulation());
}

TEST_CASE(engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.patchCarriesMappings = true;

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        synth::MidiControllerSlot slot;
        slot.name = "saved-controller";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.input.identifier = "saved-in";
        slot.output.identifier = "saved-out";
        instrument.controllers.push_back(std::move(slot));
    });

    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "engine-patch-carries-instrument-save-dir";
    std::filesystem::remove_all(saveDir);

    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);

    TestBlockBuffers buffers(2, 32);
    {
        synth::AudioBlock block = buffers.Block(32);
        engine.ProcessBlock(block, 0);
    }
    const synth::PatchCommandResult written = engine.Patches().ProcessResponses();
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::Written);

    // The saved file itself carries the instrument under schemaVersion 2 --
    // confirm the real file on disk, not just the engine's in-memory path.
    const std::string savedText = synth::LoadPatchVersionText(written.path);
    synth::JsonArena checkArena(65536);
    const synth::JSON savedRoot = checkArena.Loads(savedText.c_str());
    REQUIRE_TRUE(!savedRoot.IsNull());
    REQUIRE_TRUE(savedRoot.Get("schemaVersion").IntegerValue() == 2);
    REQUIRE_TRUE(!savedRoot.Get("midiInstrument").IsNull());

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        instrument.controllers.clear();
        synth::MidiControllerSlot slot;
        slot.name = "changed-controller";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.input.identifier = "changed-in";
        slot.output.identifier = "changed-out";
        instrument.controllers.push_back(std::move(slot));
    });
    REQUIRE_TRUE(engine.InstrumentSnapshot().controllers.front().name == "changed-controller");

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(saveDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    {
        // ProcessBlock's drain parses the midiInstrument section and stages
        // it; only MessageThreadTick's EditInstrument call actually replaces
        // the live instrument.
        synth::AudioBlock block = buffers.Block(32);
        engine.ProcessBlock(block, 0);
    }
    REQUIRE_TRUE(engine.InstrumentSnapshot().controllers.front().name == "changed-controller");
    engine.MessageThreadTick();

    const synth::MidiInstrumentConfig snapshot = engine.InstrumentSnapshot();
    REQUIRE_TRUE(snapshot.controllers.size() == 1);
    REQUIRE_TRUE(snapshot.controllers.front().name == "saved-controller");
    REQUIRE_TRUE(snapshot.controllers.front().input.identifier == "saved-in");
    REQUIRE_TRUE(snapshot.controllers.front().output.identifier == "saved-out");

    std::filesystem::remove_all(saveDir);
}

TEST_CASE(engine_patch_load_leaves_instrument_untouched_when_catalog_does_not_carry_mappings) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.patchCarriesMappings = false;

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        synth::MidiControllerSlot slot;
        slot.name = "saved-controller";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.input.identifier = "saved-in";
        slot.output.identifier = "saved-out";
        instrument.controllers.push_back(std::move(slot));
    });

    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "engine-patch-no-carry-instrument-save-dir";
    std::filesystem::remove_all(saveDir);

    const synth::PatchCommandResult saveResult = engine.Patches().SavePatchAs(saveDir);
    REQUIRE_TRUE(saveResult.status == synth::PatchCommandStatus::Pending);

    TestBlockBuffers buffers(2, 32);
    {
        synth::AudioBlock block = buffers.Block(32);
        engine.ProcessBlock(block, 0);
    }
    const synth::PatchCommandResult written = engine.Patches().ProcessResponses();
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::Written);

    // Without a catalog that carries mappings, the saved file stays
    // version 1 with no midiInstrument section, byte-for-byte what a
    // parameter-only patch has always written.
    const std::string savedText = synth::LoadPatchVersionText(written.path);
    synth::JsonArena checkArena(65536);
    const synth::JSON savedRoot = checkArena.Loads(savedText.c_str());
    REQUIRE_TRUE(!savedRoot.IsNull());
    REQUIRE_TRUE(savedRoot.Get("schemaVersion").IntegerValue() == 1);
    REQUIRE_TRUE(savedRoot.Get("midiInstrument").IsNull());

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        instrument.controllers.clear();
        synth::MidiControllerSlot slot;
        slot.name = "changed-controller";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.input.identifier = "changed-in";
        slot.output.identifier = "changed-out";
        instrument.controllers.push_back(std::move(slot));
    });

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(saveDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    {
        synth::AudioBlock block = buffers.Block(32);
        engine.ProcessBlock(block, 0);
    }
    engine.MessageThreadTick();

    // No section was ever written, so nothing is staged to apply: the
    // instrument the operator changed to after saving stays live.
    const synth::MidiInstrumentConfig snapshot = engine.InstrumentSnapshot();
    REQUIRE_TRUE(snapshot.controllers.size() == 1);
    REQUIRE_TRUE(snapshot.controllers.front().name == "changed-controller");
    REQUIRE_TRUE(snapshot.controllers.front().input.identifier == "changed-in");
    REQUIRE_TRUE(snapshot.controllers.front().output.identifier == "changed-out");

    std::filesystem::remove_all(saveDir);
}

TEST_CASE(engine_ignores_version_two_midi_instrument_section_when_catalog_does_not_carry_mappings) {
    MidiCatalogTestApp::catalog = synth::MidiAppCatalog{};
    MidiCatalogTestApp::catalog.patchCarriesMappings = false;

    synth::Engine<MidiCatalogTestApp> engine([] { return std::uint64_t{0}; });
    engine.Initialize();
    engine.Prepare(48000.0, 32);

    engine.EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        instrument.controllers.clear();
        synth::MidiControllerSlot slot;
        slot.name = "changed-controller";
        slot.kind = synth::MidiProfileKind::Generic;
        slot.input.identifier = "changed-in";
        slot.output.identifier = "changed-out";
        instrument.controllers.push_back(std::move(slot));
    });

    // A schemaVersion-2 patch file whose midiInstrument section is valid and
    // carries a different controller -- written directly, not through this
    // engine (whose catalog never asks BuildPatchJSON to carry the
    // section), so the file's presence alone is what this test exercises.
    synth::ParameterManager scratchManager;
    auto& scratchGroup = scratchManager.CreateGroup(
        {.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 4});
    scratchManager.CreateParameter(scratchGroup, {.name = "Carrier", .defaultValue = 0.4f});
    scratchManager.CaptureDefaultControlState();
    scratchManager.ComputeAllParameters();

    synth::MidiInstrumentConfig sectionInstrument;
    synth::MidiControllerSlot sectionSlot;
    sectionSlot.name = "smuggled-controller";
    sectionSlot.kind = synth::MidiProfileKind::Generic;
    sectionSlot.input.identifier = "smuggled-in";
    sectionSlot.output.identifier = "smuggled-out";
    sectionInstrument.controllers.push_back(std::move(sectionSlot));

    synth::JsonArena buildArena(64 * 1024);
    synth::JSON root = synth::BuildPatchJSON(buildArena, "Smuggled Patch", scratchManager, sectionInstrument,
                                              /*audioDevice=*/{}, /*carryInstrument=*/true);
    REQUIRE_TRUE(!root.IsNull());
    REQUIRE_TRUE(root.Get("schemaVersion").IntegerValue() == 2);
    char* dumped = root.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);
    const std::string jsonText(dumped);
    std::free(dumped);

    const std::filesystem::path patchDir =
        std::filesystem::temp_directory_path() / "engine-no-carry-smuggled-instrument-dir";
    std::filesystem::remove_all(patchDir);
    synth::SavePatchVersionInDirectory(patchDir, jsonText, std::chrono::system_clock::now());

    const synth::PatchCommandResult loadResult = engine.Patches().LoadPatch(patchDir);
    REQUIRE_TRUE(loadResult.status == synth::PatchCommandStatus::Ok);

    TestBlockBuffers buffers(2, 32);
    {
        synth::AudioBlock block = buffers.Block(32);
        engine.ProcessBlock(block, 0);
    }
    engine.MessageThreadTick();

    // The section was never even parsed (the catalog does not carry
    // mappings), so nothing was staged to apply: the live instrument stays
    // exactly what it was before the load.
    const synth::MidiInstrumentConfig snapshot = engine.InstrumentSnapshot();
    REQUIRE_TRUE(snapshot.controllers.size() == 1);
    REQUIRE_TRUE(snapshot.controllers.front().name == "changed-controller");
    REQUIRE_TRUE(snapshot.controllers.front().input.identifier == "changed-in");
    REQUIRE_TRUE(snapshot.controllers.front().output.identifier == "changed-out");

    std::filesystem::remove_all(patchDir);
}

int main() {
    int failed = 0;
    for (const auto& test : Registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    return failed == 0 ? 0 : 1;
}
