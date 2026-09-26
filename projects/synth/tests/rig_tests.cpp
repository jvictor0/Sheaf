#include "support/SynthRig.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth rig tests must not see JUCE headers"
#endif

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

// Minimal SynthApplicationCore used by the rig smoke tests: one group with
// two parameters ("Level" default 0.25, "Tone" default 0.5) mapped to
// physical encoders 0/1 through a single bank+slot. ProcessBlock processes
// the group once per sample (so per-frame slewing is exercised, matching the
// engine's own audio-thread contract), then writes Level's post-slew value to
// every channel of every output frame.
struct RigTestApp {
    // When set, ProcessBlock writes a NaN into the very first output frame
    // of the very next ProcessBlock call instead of the normal value, then
    // clears itself. Exercises the rig's sticky-NaN detection.
    static inline bool injectNanNextBlock = false;

    synth::AppContext* context = nullptr;
    synth::ParameterGroup* group = nullptr;
    synth::ParameterId levelId = 0;
    synth::ParameterId toneId = 0;
    synth::Parameter* levelModDepth = nullptr;

    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "RigTest";
        config.numAudioInputs = 0;
        config.numAudioOutputs = 2;
        config.preferredSampleRate = 48000.0;
        config.preferredBlockSize = 32;
        return config;
    }

    void Init(synth::AppContext* ctx) {
        context = ctx;
        group = &ctx->parameterManager->CreateGroup({.numVoices = 1,
                                                     .numModulators = 1,
                                                     .numScenes = 2,
                                                     .maxParameters = 8,
                                                     .processLiteAlpha = 0.5f,
                                                     .targetCenterAlpha = 1.0f});
        auto& level = ctx->parameterManager->CreateParameter(*group, {.name = "Level", .defaultValue = 0.25f});
        auto& tone = ctx->parameterManager->CreateParameter(*group, {.name = "Tone", .defaultValue = 0.5f});
        levelId = level.Id();
        toneId = tone.Id();
        levelModDepth = level.EnsureModulationDepth(0);

        auto& bank = ctx->parameterManager->CreateBank();
        bank.AddMapping(/*encoderId=*/0, level);
        bank.AddMapping(/*encoderId=*/1, tone);
        auto& slot = ctx->parameterManager->CreateBankSlot();
        slot.AddPhysicalEncoder(/*encoderId=*/0);
        slot.AddPhysicalEncoder(/*encoderId=*/1);
        slot.SelectBank(&bank);
    }

    void ProcessBlock(synth::AudioBlock& block) {
        for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
            group->ProcessSample(block.startSample + frame);
        }
        const float levelValue = context->parameterManager->ParameterById(levelId).GetRaw(0);
        for (int channel = 0; channel < block.numOutputChannels; ++channel) {
            float* out = block.outputs[channel];
            if (out == nullptr) {
                continue;
            }
            for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
                out[frame] = levelValue;
            }
        }
        if (injectNanNextBlock) {
            injectNanNextBlock = false;
            if (block.numOutputChannels > 0 && block.outputs[0] != nullptr && block.numFrames > 0) {
                block.outputs[0][0] = std::nanf("");
            }
        }
    }

    void EnableLevelModulation(float source = 1.0f) {
        levelModDepth->SceneCenter(0) = 0.75f;
        levelModDepth->SceneCenter(1) = 0.75f;
        group->GetModulators().Value(0, 0) = source;
    }
};

// Records every BasicMidi handed to it via IMidiOutputSink::Send, tagged
// with a fixed sinkLabel so a test can distinguish which fake sink (i.e.
// which controller slot's MidiSender::SetSink registration) actually
// received a given message -- the whole point of the per-controller sink
// routing under test.
struct FakeSink final : synth::IMidiOutputSink {
    std::vector<synth::BasicMidi> received;

    void Send(const synth::BasicMidi& midi) override {
        received.push_back(midi);
    }
};

// Two-controller app (Task 2 Step 1): two independent bank slots (slotIx 0
// and 1), each with its own parameter ("Alpha"/"Beta") mapped to physical
// encoder 0 of its own bank slot, so a controller's encoder turn can be
// routed at MidiInstrumentConfig-build time to drive exactly one parameter.
// ProcessBlock mirrors RigTestApp's minimal per-sample group processing but
// has no audio-output dependency on either parameter (these tests only care
// about parameter/UI-state observation, not captured audio).
struct TwoControllerRigApp {
    synth::AppContext* context = nullptr;
    synth::ParameterGroup* group = nullptr;
    synth::ParameterId alphaId = 0;
    synth::ParameterId betaId = 0;

    static synth::RuntimeConfig Config() {
        synth::RuntimeConfig config;
        config.appName = "TwoControllerRigTest";
        config.numAudioInputs = 0;
        config.numAudioOutputs = 2;
        config.preferredSampleRate = 48000.0;
        config.preferredBlockSize = 32;
        return config;
    }

    void Init(synth::AppContext* ctx) {
        context = ctx;
        group = &ctx->parameterManager->CreateGroup({.numVoices = 1,
                                                     .numModulators = 0,
                                                     .numScenes = 1,
                                                     .maxParameters = 4,
                                                     .processLiteAlpha = 1.0f});
        auto& alpha = ctx->parameterManager->CreateParameter(*group, {.name = "Alpha", .defaultValue = 0.25f});
        auto& beta = ctx->parameterManager->CreateParameter(*group, {.name = "Beta", .defaultValue = 0.25f});
        alphaId = alpha.Id();
        betaId = beta.Id();

        auto& bankA = ctx->parameterManager->CreateBank();
        bankA.AddMapping(/*encoderId=*/0, alpha);
        auto& slotA = ctx->parameterManager->CreateBankSlot();
        slotA.AddPhysicalEncoder(/*encoderId=*/0);
        slotA.SelectBank(&bankA);

        auto& bankB = ctx->parameterManager->CreateBank();
        bankB.AddMapping(/*encoderId=*/0, beta);
        auto& slotB = ctx->parameterManager->CreateBankSlot();
        slotB.AddPhysicalEncoder(/*encoderId=*/0);
        slotB.SelectBank(&bankB);
    }

    void ProcessBlock(synth::AudioBlock& block) {
        for (std::size_t frame = 0; frame < block.numFrames; ++frame) {
            group->ProcessSample(block.startSample + frame);
        }
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

// Builds a two-controller instrument for TwoControllerRigApp: slot 0 drives
// bank slot 0 (Alpha) via a minimal encoder-input config on channel 0 CC 0,
// plus a system CC association (channel 4 CC 10) whose feedback is
// ToggleReset -- genuinely global UI state (uiState_->resetHeld), so
// toggling it is observable independent of any bank/gesture wiring. Slot 1
// is the same encoder-input shape shifted onto bank slot 1 (Beta) and a
// distinct system CC address (channel 4 CC 11), but its feedback is
// ToggleGestureSelect(gestureIx=0) instead of ToggleReset: TwoControllerRigApp
// never configures any gestures, so SystemMessageOutputInfo::Evaluate's
// ToggleGestureSelect/SetGestureSelect case always short-circuits to {} (its
// gestureCapacity guard is never satisfied) -- controller 1's cached feedback
// is therefore stable (never re-emits after its first Process() pass)
// regardless of what controller 0's reset toggling does. This deliberately
// gives the two controllers' feedback triggers independent state so a
// per-sink-routing test can toggle ONE controller's feedback and assert the
// OTHER controller's sink stays silent, without the "isolation" being
// confounded by both associations happening to watch the same UI-state bit.
// Both controllers' encoderOutput stays unset (deliberately): the
// WrldBldr/Twister encoder-out cell-cache tests belong to
// CreateMidiControllerProfile's own unit tests; this rig-level instrument
// only needs the simpler always-cache-driven system CC feedback path to
// exercise per-sink routing/reset end to end.
synth::MidiInstrumentConfig TwoControllerInstrument() {
    synth::MidiInstrumentConfig instrument;

    synth::MidiControllerSlot slot0;
    slot0.name = "controller-0";
    slot0.kind = synth::MidiProfileKind::Generic;
    slot0.config.encoderInput = synth::EncoderMidiInConfig{};
    slot0.config.encoderInput->turns.push_back(
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0});
    slot0.config.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 4, .cc = 10},
        .press = synth::MessageIn::ToggleReset(0),
        .feedback = synth::MessageIn::ToggleReset(0),
    });
    instrument.controllers.push_back(std::move(slot0));

    synth::MidiControllerSlot slot1;
    slot1.name = "controller-1";
    slot1.kind = synth::MidiProfileKind::Generic;
    slot1.config.encoderInput = synth::EncoderMidiInConfig{};
    slot1.config.encoderInput->turns.push_back(
        {.control = {.channel = 0, .cc = 0}, .slotIx = 1, .position = 0});
    slot1.config.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 4, .cc = 11},
        .press = synth::MessageIn::ToggleGestureSelect(0, 0),
        .feedback = synth::MessageIn::ToggleGestureSelect(0, 0),
    });
    instrument.controllers.push_back(std::move(slot1));

    return instrument;
}

}  // namespace

TEST_CASE(rig_existing_app_keeps_parameter_ui_contract_with_empty_grid_snapshot) {
    synth_rig::SynthRig<RigTestApp> rig;
    REQUIRE_TRUE(rig.Engine().Context().uiState == rig.Engine().RuntimeUIStateForTest().parameters);
    REQUIRE_TRUE(rig.Engine().RuntimeUIStateForTest().grids != nullptr);
    REQUIRE_TRUE(rig.Engine().RuntimeUIStateForTest().grids->slots.empty());
    REQUIRE_TRUE(rig.Engine().GridManagerForTest().Finalized());
}

TEST_CASE(rig_runs_blocks_and_captures_output) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.RunBlocks(4);
    REQUIRE_TRUE(!rig.SawNaN());
    REQUIRE_TRUE(rig.Output().size() == 4 * static_cast<std::size_t>(RigTestApp::Config().preferredBlockSize));
    REQUIRE_NEAR(rig.LastOutput().channels.at(0), 0.25f, 1e-4f);
    REQUIRE_NEAR(rig.OutputPeak(), 0.25f, 1e-4f);
}

TEST_CASE(rig_exposes_deterministic_clock_injection_queries_and_scheduled_output) {
    synth_rig::SynthRig<RigTestApp> rig(
        64, {}, {.sampleRate = 44100.0, .blockSize = 7});
    REQUIRE_TRUE(rig.SetSyncConfig({
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = true,
        .receiveTransport = false,
        .ppqn = 96,
    }));

    rig.StartAt(1000);
    rig.RunBlockAt(1000);
    const synth::ClockBlockPlan* first = rig.CurrentClockPlan();
    REQUIRE_TRUE(first != nullptr);
    REQUIRE_TRUE(first->StartSample() == 0);
    REQUIRE_TRUE(first->EndSample() == 7);
    REQUIRE_TRUE(first->IsTransportRunning());
    REQUIRE_TRUE(!rig.ScheduledMidiEvents().empty());

    // Braid 4's four-times-oversampled convention queries clock time at
    // block.startSample + internalSample / 4.0, with fractional output-domain
    // positions intact.
    const double internalSample = 13.0;
    const double outputSample = static_cast<double>(first->StartSample()) + internalSample / 4.0;
    const auto direct = rig.ClockTimeAtSample(outputSample);
    REQUIRE_TRUE(direct.has_value());
    REQUIRE_TRUE(first->Contains(outputSample));
    REQUIRE_TRUE(first->TryLifetimeQuarterNotesAt(outputSample).has_value());
    REQUIRE_TRUE(std::fabs(*first->TryLifetimeQuarterNotesAt(outputSample) -
                           direct->lifetimeQuarterNotes) < 1e-12);

    rig.ContinueAt(2000);
    rig.StopAt(2000);
    rig.RunBlockAt(2000);
    REQUIRE_TRUE(rig.CurrentClockPlan()->TransportState() == synth::ClockTransportState::Stopped);
    REQUIRE_TRUE(rig.DroppedScheduledMidiEventCount() == 0);
}

TEST_CASE(rig_clock_time_is_independent_of_block_partitioning) {
    synth_rig::SynthRig<RigTestApp> sevenFrameRig(
        64, {}, {.sampleRate = 48000.0, .blockSize = 7});
    synth_rig::SynthRig<RigTestApp> thirteenFrameRig(
        64, {}, {.sampleRate = 48000.0, .blockSize = 13});

    sevenFrameRig.StartAt(1000);
    thirteenFrameRig.StartAt(1000);
    for (std::uint64_t block = 0; block < 13; ++block) {
        sevenFrameRig.RunBlockAt(1000 + block);
    }
    for (std::uint64_t block = 0; block < 7; ++block) {
        thirteenFrameRig.RunBlockAt(1000 + block);
    }

    REQUIRE_TRUE(sevenFrameRig.CurrentClockPlan()->EndSample() == 91);
    REQUIRE_TRUE(thirteenFrameRig.CurrentClockPlan()->EndSample() == 91);
    REQUIRE_TRUE(std::fabs(
        sevenFrameRig.CurrentClockPlan()->LifetimeEndQuarterNotes() -
        thirteenFrameRig.CurrentClockPlan()->LifetimeEndQuarterNotes()) < 1e-12);
    const auto sevenFrameQuery = sevenFrameRig.ClockTimeAtSample(90.5);
    const auto thirteenFrameQuery = thirteenFrameRig.ClockTimeAtSample(90.5);
    REQUIRE_TRUE(sevenFrameQuery.has_value());
    REQUIRE_TRUE(thirteenFrameQuery.has_value());
    REQUIRE_TRUE(std::fabs(
        sevenFrameQuery->lifetimeQuarterNotes -
        thirteenFrameQuery->lifetimeQuarterNotes) < 1e-12);
}

TEST_CASE(rig_empty_controller_accepts_raw_external_realtime_and_long_run_stays_finite) {
    synth_rig::SynthRig<RigTestApp> rig(
        64, {}, {.sampleRate = 48000.0, .blockSize = 13});
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot slot;
    slot.name = "clock-only";
    slot.kind = synth::MidiProfileKind::Generic;
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    rig.InstallInstrumentForTest(std::move(instrument));
    REQUIRE_TRUE(rig.SetSyncConfig({
        .sendClock = false,
        .receiveClock = true,
        .sendTransport = false,
        .receiveTransport = true,
        .ppqn = 24,
    }));

    rig.SendMidi(0, synth::BasicMidi::TransportStart(10'000));
    rig.SendMidi(0, synth::BasicMidi::Clock(10'001));
    rig.RunBlockAt(10'001);
    REQUIRE_TRUE(rig.Engine().Clock().DiagnosticsSnapshot().activeExternalSourceSlot == 0);
    REQUIRE_TRUE(rig.CurrentClockPlan()->IsTransportRunning());

    rig.ExternalStopAt(0, 10'002);
    rig.RunBlockAt(10'002);
    REQUIRE_TRUE(rig.CurrentClockPlan()->TransportState() == synth::ClockTransportState::Stopped);
    rig.ExternalContinueAt(0, 10'003);
    rig.ExternalClockAt(0, 10'004);
    rig.RunBlockAt(10'004);
    REQUIRE_TRUE(rig.CurrentClockPlan()->IsTransportRunning());

    std::uint64_t timestamp = 10'004;
    double previousLifetime = rig.CurrentClockPlan()->LifetimeEndQuarterNotes();
    for (std::size_t block = 0; block < 2000; ++block) {
        timestamp += 271;
        if ((block % 77) == 0) {
            rig.ExternalClockAt(0, timestamp);
        }
        rig.RunBlockAt(timestamp);
        const double lifetime = rig.CurrentClockPlan()->LifetimeEndQuarterNotes();
        REQUIRE_TRUE(std::isfinite(lifetime));
        REQUIRE_TRUE(lifetime >= previousLifetime);
        previousLifetime = lifetime;
    }
    REQUIRE_TRUE(!rig.SawNaN());
    REQUIRE_TRUE(rig.CurrentClockPlan() != nullptr);
    REQUIRE_TRUE(std::isfinite(rig.CurrentClockPlan()->LifetimeEndQuarterNotes()));
    REQUIRE_TRUE(rig.CurrentClockPlan()->FrameCount() == 13);
}

TEST_CASE(rig_turn_reaches_parameter_through_production_bus) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.Turn(0, 0, 0.5f);  // Level encoder
    rig.RunBlocks(8);      // settle slew
    REQUIRE_TRUE(rig.LastOutput().channels.at(0) > 0.25f);
    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(rig_run_samples_and_seconds_convert_to_blocks) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.RunSamples(1);  // rounds up to one block
    const auto oneBlock = rig.Output().size();
    REQUIRE_TRUE(oneBlock == static_cast<std::size_t>(RigTestApp::Config().preferredBlockSize));
}

TEST_CASE(rig_zero_input_rejects_injection_helpers) {
    synth_rig::SynthRig<RigTestApp> rig;
    REQUIRE_TRUE(rig.NumInputChannels() == 0);
    REQUIRE_TRUE(rig.InputBlockSize() ==
                 static_cast<std::size_t>(RigTestApp::Config().preferredBlockSize));
    REQUIRE_TRUE(!rig.SetInputSample(0, 0, 1.0f));
    REQUIRE_TRUE(!rig.SetInputChannel(0, std::array<float, 32>{}));
    REQUIRE_TRUE(!rig.SetInputFrame(0, std::array<float, 1>{1.0f}));
    const std::span<const float> emptyBlock[1] = {std::span<const float>{}};
    REQUIRE_TRUE(!rig.SetInputBlock(emptyBlock));
    rig.ClearAudioInputs();
    rig.RunSamples(1);
    REQUIRE_TRUE(rig.Output().size() ==
                 static_cast<std::size_t>(RigTestApp::Config().preferredBlockSize));
}

TEST_CASE(rig_nan_flag_is_sticky) {
    synth_rig::SynthRig<RigTestApp> rig;
    RigTestApp::injectNanNextBlock = true;
    rig.RunBlocks(1);
    REQUIRE_TRUE(rig.SawNaN());
    RigTestApp::injectNanNextBlock = false;
    rig.RunBlocks(4);
    REQUIRE_TRUE(rig.SawNaN());  // still true: sticky across subsequent clean blocks
    rig.ClearNaN();
    REQUIRE_TRUE(!rig.SawNaN());
}

// Regression test for the save-pump result race fixed via
// Engine::ConsumeLastTickPatchResult(): RunBlocks(1) already drains the
// pending save's response through MessageThreadTick()'s internal
// ProcessResponses() call, so a rig that called ProcessResponses() again
// afterward would only ever observe NoCompletion and report TimedOut even
// though the save actually succeeded. This edits a parameter, saves via
// SavePatchAs, and asserts the rig correctly reports Written with the
// version file present on disk.
TEST_CASE(rig_save_patch_as_reports_written_and_creates_version_file) {
    const std::filesystem::path saveDir =
        std::filesystem::temp_directory_path() / "rig-tests-save-patch-as-dir";
    std::error_code ec;
    std::filesystem::remove_all(saveDir, ec);

    synth_rig::SynthRig<RigTestApp> rig;
    rig.Turn(0, 0, 0.5f);  // Level encoder
    rig.RunBlocks(4);      // let the edit land before saving

    const synth_rig::RigPatchStatus status = rig.SavePatchAs(saveDir);
    REQUIRE_TRUE(status == synth_rig::RigPatchStatus::Written);

    bool foundVersionFile = false;
    if (std::filesystem::is_directory(saveDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(saveDir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                foundVersionFile = true;
                break;
            }
        }
    }
    REQUIRE_TRUE(foundVersionFile);

    std::filesystem::remove_all(saveDir, ec);
}

// Rig-driven system tests (Plan 2 Task 7). Each test drives RigTestApp
// through synth_rig::SynthRig exactly as a production host would: no direct
// pokes into engine internals beyond the documented test-support surface
// (SynthRig::InstallInstrumentForTest / Engine::RebuildMidiProcessorsForTest,
// and AppContext::patchOutputBus, which is already a production-shaped
// escape hatch on AppContext, not a new accessor).

// Wraps a single MidiControllerProfileConfig into a one-controller
// MidiInstrumentConfig, matching the pre-Task-2 InstallMidiProfileForTest
// helper's single-slot shape for tests that only need one controller.
synth::MidiInstrumentConfig SingleControllerInstrument(synth::MidiControllerProfileConfig config) {
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot slot;
    slot.name = "test";
    slot.kind = synth::MidiProfileKind::Generic;
    slot.config = std::move(config);
    instrument.controllers.push_back(std::move(slot));
    return instrument;
}

synth::MidiControllerProfileConfig EncoderFeedbackProfile(synth::EncoderMode mode) {
    synth::MidiControllerProfileConfig config;
    config.encoderInput = synth::EncoderMidiInConfig{
        .mode = mode,
        .turnStep = 1.0f / 128.0f,
        .turns = {{.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0}},
    };
    config.encoderOutput = synth::EncoderMidiOutConfig{
        .protocol = synth::EncoderMidiOutProtocol::Twister,
        .mappings = {{.slotIx = 0, .position = 0, .cc = 0}},
    };
    return config;
}

synth::MidiControllerProfileConfig GenericEncoderFeedbackProfile(synth::EncoderMode mode) {
    synth::MidiControllerProfileConfig config;
    config.encoderInput = synth::EncoderMidiInConfig{
        .mode = mode,
        .turnStep = 1.0f / 128.0f,
        .turns = {
            {.control = {.channel = 7, .cc = 74}, .slotIx = 0, .position = 0},
            {.control = {.channel = 12, .cc = 3}, .slotIx = 0, .position = 1},
        },
    };
    return config;
}

synth::MidiInstrumentConfig TwoAbsoluteControllersOneCellInstrument() {
    synth::MidiInstrumentConfig instrument;
    for (std::size_t ix = 0; ix < 2; ++ix) {
        synth::MidiControllerSlot slot;
        slot.name = "absolute-" + std::to_string(ix);
        slot.kind = synth::MidiProfileKind::Generic;
        slot.config = EncoderFeedbackProfile(synth::EncoderMode::Absolute);
        instrument.controllers.push_back(std::move(slot));
    }
    return instrument;
}

std::vector<std::uint8_t> PositionValues(const FakeSink& sink) {
    std::vector<std::uint8_t> values;
    for (const synth::BasicMidi& midi : sink.received) {
        if (midi.IsCC() && midi.Channel() == 0) {
            values.push_back(midi.GetValue());
        }
    }
    return values;
}

TEST_CASE(rig_runtime_config_round_trips_nested_pressure_profile_without_changing_envelopes) {
    synth::MidiControllerProfileConfig profile;
    profile.pressureInput = synth::PolyphonicPressureMidiInConfig{{
        {
            .address = {.channel = 3, .note = 42},
            .pressure = synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0),
        },
    }};
    synth::MidiInstrumentConfig instrument = SingleControllerInstrument(profile);
    synth::AudioDeviceState audio{
        .outputDeviceName = "Pressure Out",
        .inputDeviceName = "Pressure In",
    };
    const synth::SyncConfig sync{.sendClock = true, .receiveClock = true, .ppqn = 96};

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::BuildRuntimeConfigJSON(arena, instrument, audio, sync);
    REQUIRE_TRUE(json.Get("schema").StringValue() == std::string_view(synth::kRuntimeConfigSchema));
    REQUIRE_TRUE(json.Get("schemaVersion").IntegerValue() == synth::kRuntimeConfigSchemaVersion);
    const synth::JSON nestedInstrument = json.Get("midiInstrument");
    REQUIRE_TRUE(nestedInstrument.Get("schemaVersion").IntegerValue() == synth::kMidiInstrumentSchemaVersion);
    const synth::JSON nestedProfile = nestedInstrument.Get("controllers").GetAt(0).Get("profile");
    REQUIRE_TRUE(nestedProfile.Get("schemaVersion").IntegerValue() ==
                 synth::kMidiControllerProfileSchemaVersion);

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(json, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedAudio == audio);
    REQUIRE_TRUE(loadedSync == sync);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.pressureInput.has_value());
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.pressureInput->mappings ==
                 profile.pressureInput->mappings);

    synth::ParameterManager manager;
    synth::JsonArena patchArena(1024 * 1024);
    const synth::JSON patch = synth::BuildPatchJSON(patchArena, "pressure", manager, instrument, audio);
    REQUIRE_TRUE(patch.Get("midiInstrument").IsNull());
    REQUIRE_TRUE(patch.Get("audioDevice").IsNull());
    REQUIRE_TRUE(patch.Get("sync").IsNull());
}

TEST_CASE(rig_installs_pressure_only_profile_and_processes_poly_pressure) {
    synth::MidiControllerProfileConfig profile;
    profile.pressureInput = synth::PolyphonicPressureMidiInConfig{{
        {
            .address = {.channel = 3, .note = 42},
            .pressure = synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0),
        },
    }};

    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(SingleControllerInstrument(profile));
    REQUIRE_TRUE(dynamic_cast<synth::PolyphonicPressureMidiInProcessor*>(
                     rig.Engine().MidiInputProcessor(0)) != nullptr);
    rig.SendMidi(0, synth::BasicMidi::PolyPressure(0, 3, 42, 88));
    rig.RunBlocks(1);
    REQUIRE_TRUE(!rig.SawNaN());
}

// MIDI CC -> profile -> parameter: install the default WrldBldr profile
// (encoder turns on channel 0, CC 0..15 -> slot 0 positions 0..15, see
// EncoderMidiInConfig::WrldBldrDefault / RowMajorInputDefault), then send a
// correctly encoded relative +1 turn for slot 0 position 0 (Level's
// physical encoder) and confirm the parameter actually rises once the
// engine settles.
TEST_CASE(rig_midi_cc_routes_through_profile_to_parameter) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(SingleControllerInstrument(synth::WrldBldrDefaultProfileConfig({})));

    const float before = rig.ParameterValue(rig.Application().levelId);

    synth::BasicMidi turn;
    turn.timestamp = 0;
    // Channel 0 (0xB0), CC 0 (slot 0 position 0's turn address), value 0x41
    // (65). EncoderMidiInProcessor::DecodeDelta with the default
    // EncoderMode::Signed7Bit computes ticks = value - 64, so 65 =>
    // +1 tick => delta = +1 * turnStep (turnStep defaults to 1/128).
    turn.raw = {0xB0, 0x00, 0x41};
    rig.SendMidi(0, turn);
    rig.RunBlocks(8);

    REQUIRE_TRUE(rig.ParameterValue(rig.Application().levelId) > before);
}

TEST_CASE(rig_absolute_feedback_follows_real_acknowledgement_and_ignores_modulation) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(
        SingleControllerInstrument(EncoderFeedbackProfile(synth::EncoderMode::Absolute)));

    FakeSink sink;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sink.received.clear();

    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 96));
    // Output polls before the audio consumer has run. The alert published by
    // the callback must already gate the old UI snapshot.
    rig.Engine().MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(PositionValues(sink).empty());

    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(PositionValues(sink).empty());
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), 0.753488443f, 0.00001f);

    // Modulation changes the published display value, but absolute feedback
    // remains debounced at the modulation-free raw center.
    rig.Application().EnableLevelModulation();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    auto& cell = rig.UIState().slots[0].cells[0];
    REQUIRE_TRUE(std::fabs(cell.values[0].load() - cell.rawKnobValue.load()) > 0.05f);
    REQUIRE_TRUE(PositionValues(sink).empty());

    // Reset rejects the new absolute edit. Its epoch is still acknowledged,
    // forcing one correction to the actual center even though that byte is
    // already the output processor's cached value.
    rig.SetReset(true);
    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 20));
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();
    const auto corrections = PositionValues(sink);
    REQUIRE_TRUE(corrections.size() == 1);
    REQUIRE_TRUE(corrections.front() == 96);
}

TEST_CASE(rig_generic_absolute_feedback_round_trips_same_address_without_auxiliary_traffic) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(
        SingleControllerInstrument(GenericEncoderFeedbackProfile(synth::EncoderMode::Absolute)));

    FakeSink sink;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(!sink.received.empty());
    bool sawFirstAddress = false;
    bool sawSecondAddress = false;
    for (const synth::BasicMidi& midi : sink.received) {
        REQUIRE_TRUE(midi.IsCC());
        const bool firstAddress = midi.Channel() == 7 && midi.GetCC() == 74;
        const bool secondAddress = midi.Channel() == 12 && midi.GetCC() == 3;
        REQUIRE_TRUE(firstAddress || secondAddress);
        sawFirstAddress = sawFirstAddress || firstAddress;
        sawSecondAddress = sawSecondAddress || secondAddress;
    }
    REQUIRE_TRUE(sawFirstAddress);
    REQUIRE_TRUE(sawSecondAddress);
    sink.received.clear();

    rig.SendMidi(0, synth::BasicMidi::CC(0, 7, 74, 96));
    rig.Engine().MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.received.empty());

    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.received.empty());
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), 0.753488443f, 0.00001f);

    rig.SetReset(true);
    rig.SendMidi(0, synth::BasicMidi::CC(0, 7, 74, 20));
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();
    REQUIRE_TRUE(sink.received.size() == 1);
    REQUIRE_TRUE(sink.received[0].IsCC());
    REQUIRE_TRUE(sink.received[0].Channel() == 7);
    REQUIRE_TRUE(sink.received[0].GetCC() == 74);
    REQUIRE_TRUE(sink.received[0].GetValue() == 96);
}

TEST_CASE(rig_absolute_feedback_resolves_only_latest_same_route_rapid_input) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(
        SingleControllerInstrument(EncoderFeedbackProfile(synth::EncoderMode::Absolute)));

    FakeSink sink;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sink.received.clear();

    // All three alerts become visible before either DSP processing or UI
    // publication. The coordinator must retain only the final expectation.
    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 16));
    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 48));
    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 80));
    rig.Engine().MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(PositionValues(sink).empty());

    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();
    REQUIRE_TRUE(PositionValues(sink).empty());
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), 0.626598188f, 0.00001f);
}

TEST_CASE(rig_relative_feedback_stays_modulation_aware_through_real_engine_path) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(
        SingleControllerInstrument(EncoderFeedbackProfile(synth::EncoderMode::Signed7Bit)));

    FakeSink sink;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink);
    sender->Start();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sink.received.clear();

    rig.Application().EnableLevelModulation();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();
    const auto values = PositionValues(sink);
    REQUIRE_TRUE(!values.empty());
    REQUIRE_TRUE(values.back() != 32);
}

TEST_CASE(rig_two_absolute_controllers_share_cell_with_independent_pending_feedback) {
    synth_rig::SynthRig<RigTestApp> rig;
    rig.InstallInstrumentForTest(TwoAbsoluteControllersOneCellInstrument());

    FakeSink sink0;
    FakeSink sink1;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink0);
    sender->SetSink(1, &sink1);
    sender->Start();
    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sink0.received.clear();
    sink1.received.clear();

    rig.SendMidi(0, synth::BasicMidi::CC(0, 0, 0, 32));
    rig.SendMidi(1, synth::BasicMidi::CC(0, 0, 0, 96));
    rig.Engine().MessageThreadTick();
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(PositionValues(sink0).empty());
    REQUIRE_TRUE(PositionValues(sink1).empty());

    rig.RunBlocks(64);
    REQUIRE_TRUE(sender->FlushForTests(std::chrono::milliseconds(500)));
    sender->Stop();
    const auto controller0 = PositionValues(sink0);
    const auto controller1 = PositionValues(sink1);
    REQUIRE_TRUE(controller0.size() == 1);
    REQUIRE_TRUE(controller0.front() == 96);
    REQUIRE_TRUE(controller1.empty());
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), 0.753488443f, 0.00001f);
}

// Determinism: two rigs driven through an identical script (turns, scene
// selection/blend, more turns) must produce bit-identical captured output
// and equal final parameter values. Nothing in the engine may depend on
// wall-clock time, addresses, or other non-deterministic state.
TEST_CASE(rig_two_identical_runs_are_deterministic) {
    auto script = [](synth_rig::SynthRig<RigTestApp>& rig) {
        rig.Turn(0, 0, 0.3f); rig.RunBlocks(3);
        rig.SetSceneBlend(0.5f); rig.SelectScene(1); rig.RunBlocks(3);
        rig.Turn(0, 1, -0.2f); rig.RunBlocks(3);
    };
    synth_rig::SynthRig<RigTestApp> a, b;
    script(a); script(b);
    REQUIRE_TRUE(a.Output().size() == b.Output().size());
    for (std::size_t i = 0; i < a.Output().size(); ++i) {
        REQUIRE_TRUE(a.Output()[i].channels == b.Output()[i].channels);  // bit-identical
    }
    REQUIRE_NEAR(a.ParameterValue(a.Application().levelId), b.ParameterValue(b.Application().levelId), 0.0f);
}

// Patch round-trip through the production save/revert/load flow.
// RevertPatch reloads the latest saved version from the current patch
// directory once set (per spp-6); only resets to defaults when no current
// patch directory exists yet.
TEST_CASE(rig_patch_round_trip_through_production_flow) {
    synth_rig::SynthRig<RigTestApp> rig;
    const auto root = std::filesystem::temp_directory_path() / "rig-patch-roundtrip";
    std::filesystem::remove_all(root); std::filesystem::create_directories(root);
    rig.InstallInstrumentForTest(SingleControllerInstrument(synth::WrldBldrDefaultProfileConfig({})));
    rig.Engine().SetAudioDeviceFromHost(synth::AudioDeviceState{
        .outputDeviceName = "Saved Out",
        .inputDeviceName = "Saved In",
    });

    const float defaultValue = rig.ParameterValue(rig.Application().levelId);
    REQUIRE_TRUE(rig.SavePatchAs(root / "Take1") == synth_rig::RigPatchStatus::Written);
    rig.Engine().EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "runtime-edited"));
    });
    rig.Engine().SetAudioDeviceFromHost(synth::AudioDeviceState{
        .outputDeviceName = "Runtime Out",
        .inputDeviceName = "Runtime In",
    });

    rig.Turn(0, 0, 0.4f); rig.RunBlocks(8);
    const float edited = rig.ParameterValue(rig.Application().levelId);
    REQUIRE_TRUE(edited > defaultValue + 1e-3f);

    REQUIRE_TRUE(rig.RevertPatch() == synth_rig::RigPatchStatus::Ok);
    rig.RunBlocks(4);
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), 0.25f, 1e-3f);

    rig.Turn(0, 0, 0.4f); rig.RunBlocks(8);
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), edited, 1e-3f);
    REQUIRE_TRUE(rig.SavePatch() == synth_rig::RigPatchStatus::Written);

    // Perturb state to ensure LoadPatch actually restores from disk,
    // not just verifying it matches an already-cached value.
    rig.Turn(0, 0, -0.3f); rig.RunBlocks(8);
    const float perturbed = rig.ParameterValue(rig.Application().levelId);
    REQUIRE_TRUE(perturbed < edited - 1e-3f);

    REQUIRE_TRUE(rig.LoadPatch(root / "Take1") == synth_rig::RigPatchStatus::Ok);
    rig.RunBlocks(8);
    REQUIRE_NEAR(rig.ParameterValue(rig.Application().levelId), edited, 1e-3f);
    REQUIRE_TRUE(rig.Engine().InstrumentSnapshot().controllers.size() == 1);
    REQUIRE_TRUE(rig.Engine().InstrumentSnapshot().controllers[0].name == "runtime-edited");
    REQUIRE_TRUE(rig.Engine().AudioDeviceSnapshot().outputDeviceName == "Runtime Out");
    REQUIRE_TRUE(rig.Engine().AudioDeviceSnapshot().inputDeviceName == "Runtime In");
    std::filesystem::remove_all(root);
}

// Timeout instead of hang: with a small patch-pump budget, starve the
// serialize response path by filling patchOutputBus_ to capacity through
// AppContext::patchOutputBus (a production-shaped pointer already exposed
// on AppContext; not a new accessor) so ApplyPatchMessage's SerializeToJSON
// handling can never push its MessageOut::SerializedJSON response.
// SavePatchAs must give up and report TimedOut once the budget is
// exhausted, never hang.
TEST_CASE(rig_patch_helper_times_out_instead_of_hanging) {
    synth_rig::SynthRig<RigTestApp> rig(/*patchPumpBudgetBlocks=*/8);

    synth::MessageOutBus& bus = *rig.Engine().Context().patchOutputBus;
    while (bus.Push(synth::MessageOut{})) {}

    const auto root = std::filesystem::temp_directory_path() / "rig-patch-timeout";
    std::filesystem::create_directories(root);
    REQUIRE_TRUE(rig.SavePatchAs(root / "Stuck") == synth_rig::RigPatchStatus::TimedOut);
    std::filesystem::remove_all(root);
}

// Bus drain order: a patch message (RevertPatch's RevertAllToDefault) and a
// UI turn queued for the same pending block must both take effect, with the
// patch applying FIRST. Engine::ProcessBlock's binding order drains
// patchInputBus_ before uiBus_.Process(), so queuing both without pumping
// in between and then running exactly one block proves the ordering
// directly: the final value is default (0.25) moved by the turn's slew
// direction (upward, since the turn's delta is positive) -- not exactly
// 0.25 (the turn was lost/ignored) and not the pre-revert edited value (the
// turn applied before the revert).
TEST_CASE(rig_bus_drain_order_patch_before_ui_before_midi) {
    synth_rig::SynthRig<RigTestApp> rig;

    // Establish an edited, non-default value so a "turn ignored" outcome
    // (final value == pre-revert edited value) is distinguishable from a
    // "revert ignored" outcome (final value == edited) and from the correct
    // "patch-then-turn" outcome (final value == moved off default).
    rig.Turn(0, 0, 0.4f);
    rig.RunBlocks(8);
    const float edited = rig.ParameterValue(rig.Application().levelId);
    REQUIRE_TRUE(edited > 0.25f + 1e-3f);

    // Queue a patch RevertPatch command (pushes RevertAllToDefault onto
    // patchInputBus_) and a UI turn (pushes ParamIncDec onto uiBus_)
    // without running any blocks in between, so both are pending for the
    // same ProcessBlock call.
    const synth::PatchCommandResult revertResult = rig.Engine().Patches().RevertPatch();
    REQUIRE_TRUE(revertResult.status == synth::PatchCommandStatus::Ok);
    rig.Turn(0, 0, 0.1f);

    rig.RunBlocks(1);

    const float afterOneBlock = rig.ParameterValue(rig.Application().levelId);
    REQUIRE_TRUE(afterOneBlock > 0.25f);            // turn survived the revert
    REQUIRE_TRUE(afterOneBlock < edited - 1e-3f);    // revert applied before the turn
}

// Task 2 Step 1 (per-controller MIDI processor rebuild): a two-controller
// instrument produces two independent processor chains, both fed by the
// SINGLE MIDI input bus (per the plan's global constraint) but each with its
// own MidiInputProcessor(ix) and its own output sink. A mapped encoder turn
// sent through controller 0's chain drives Alpha (bank slot 0); the
// identically-encoded turn sent through controller 1's chain drives Beta
// (bank slot 1) instead -- proving the two chains are wired to distinct
// parameter routing despite decoding the same raw MIDI bytes.
TEST_CASE(rig_two_controllers_both_drive_parameters_through_single_bus) {
    synth_rig::SynthRig<TwoControllerRigApp> rig;
    rig.InstallInstrumentForTest(TwoControllerInstrument());

    REQUIRE_TRUE(rig.Engine().MidiControllerCount() == 2);
    REQUIRE_TRUE(rig.Engine().MidiInputProcessor(0) != nullptr);
    REQUIRE_TRUE(rig.Engine().MidiInputProcessor(1) != nullptr);

    const float alphaBefore = rig.ParameterValue(rig.Application().alphaId);
    const float betaBefore = rig.ParameterValue(rig.Application().betaId);

    // Channel 0 (0xB0), CC 0, value 0x41 (65): a +1 relative tick under the
    // default Signed7Bit decode (see rig_midi_cc_routes_through_profile_to_parameter).
    synth::BasicMidi turn;
    turn.timestamp = 0;
    turn.raw = {0xB0, 0x00, 0x41};

    rig.SendMidi(0, turn);
    rig.RunBlocks(8);
    REQUIRE_TRUE(rig.ParameterValue(rig.Application().alphaId) > alphaBefore);
    REQUIRE_TRUE(rig.ParameterValue(rig.Application().betaId) == betaBefore);  // controller 1 untouched

    rig.SendMidi(1, turn);
    rig.RunBlocks(8);
    REQUIRE_TRUE(rig.ParameterValue(rig.Application().betaId) > betaBefore);
}

// Per-sink feedback isolation: each controller's output feedback must land
// only on its own registered sink index, never on another controller's sink.
// Controller 0's feedback watches uiState_->shiftHeld (genuinely global UI
// state); controller 1's watches ToggleGestureSelect(0), which
// TwoControllerRigApp never wires up, so SystemMessageOutputInfo::Evaluate
// always returns {} for it -- controller 1's cache is stable once primed, no
// matter what controller 0's reset toggling does (see TwoControllerInstrument()'s
// doc comment). The very first Process() pass after install primes BOTH
// caches (cache.valid starts false for every mapped association, regardless
// of the value it observes) and emits on both sinks once -- that priming
// round is not what's under test. What IS under test: toggling shift via
// controller 0's press must resend feedback on sink 0 only, once the
// publish-throttled UIState snapshot actually reflects the toggle (see
// uiPublishInterval_'s doc comment -- RunBlocks must cross at least one
// publish boundary for uiState_->shiftHeld to update); sink 1 has nothing new
// to report and must stay silent throughout.
TEST_CASE(rig_two_controllers_output_feedback_isolated_per_sink) {
    synth_rig::SynthRig<TwoControllerRigApp> rig;
    rig.InstallInstrumentForTest(TwoControllerInstrument());

    FakeSink sink0;
    FakeSink sink1;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink0);
    sender->SetSink(1, &sink1);
    sender->Start();

    // Priming round: both output processors report their initial (shift-off)
    // state once, regardless of sink. Not part of the assertion below. Runs
    // enough blocks to also clear the initial UIState publish so the
    // subsequent press's effect is unambiguously the next observed change.
    rig.RunBlocks(64);
    sender->FlushForTests(std::chrono::milliseconds(500));
    sink0.received.clear();
    sink1.received.clear();

    // Controller 0's system-CC press address: channel 4, CC 10, press value
    // > 0 toggles shift (see TwoControllerInstrument()).
    synth::BasicMidi press0;
    press0.timestamp = 0;
    press0.raw = {0xB4, 0x0A, 0x7F};
    rig.SendMidi(0, press0);
    // Run enough blocks to (a) apply the press (immediate, via midiBus_.Process
    // inside the very next ProcessBlock) and (b) cross a UIState publish
    // boundary so uiState_->shiftHeld actually reflects the toggle before the
    // output processors' Process() pass evaluates it.
    rig.RunBlocks(64);

    sender->FlushForTests(std::chrono::milliseconds(500));
    sender->Stop();

    REQUIRE_TRUE(!sink0.received.empty());
    REQUIRE_TRUE(sink1.received.empty());
}

// ResetMidiOutputProcessors(ix) is per-controller: forcing controller 1's
// output caches to clear must resend all of controller 1's mapped feedback
// on its next Process() pass, while controller 0's caches -- untouched --
// suppress a redundant resend of a value it already reported.
TEST_CASE(rig_reset_midi_output_processors_is_scoped_to_one_controller) {
    synth_rig::SynthRig<TwoControllerRigApp> rig;
    rig.InstallInstrumentForTest(TwoControllerInstrument());

    FakeSink sink0;
    FakeSink sink1;
    synth::MidiSender* sender = rig.Engine().Context().midiSender;
    REQUIRE_TRUE(sender != nullptr);
    sender->SetSink(0, &sink0);
    sender->SetSink(1, &sink1);
    sender->Start();

    // Toggle shift on (via controller 0's press) so both controllers' output
    // processors have something to report, then let a tick flush the first
    // (cache-priming) round of feedback through both sinks.
    synth::BasicMidi press0;
    press0.timestamp = 0;
    press0.raw = {0xB4, 0x0A, 0x7F};
    rig.SendMidi(0, press0);
    rig.RunBlocks(1);
    sender->FlushForTests(std::chrono::milliseconds(500));

    REQUIRE_TRUE(!sink0.received.empty());
    REQUIRE_TRUE(!sink1.received.empty());
    sink0.received.clear();
    sink1.received.clear();

    // Steady state, ui-state-before-audio-corrected (openspec/changes/
    // ui-state-before-audio): every uiState_-backed association's LIVE value
    // hasn't changed since the priming round, but the priming round's own
    // output pass ran BEFORE that same tick's message-thread claim/populate
    // step -- MessageThreadTick runs the midi output processors' Process()
    // pass first, as one of its "existing duties", and only attempts the
    // ui-state-before-audio claim/populate AFTER that (Engine.hpp,
    // MessageThreadTick, design "Mechanism"). So the priming round's
    // Process() observed every uiState_-backed value still at its
    // never-populated default (resetHeld false, both controllers' encoder
    // echo 0), and only that priming tick's trailing claim first published
    // the real content. This next tick's output pass is therefore the first
    // to observe the caught-up state, so TWO independent, deterministic
    // catch-ups occur here, traced empirically (git-stash bisect + direct
    // content inspection):
    //   1. BOTH controllers' encoder-echo association (channel 0 CC 0,
    //      mirroring each controller's own turn-input address; see
    //      TwoControllerRigApp::Init) resends once, symmetrically, because
    //      Alpha/Beta's untouched default (0.25 -> byte 32) only just became
    //      visible -- unrelated to controller 0's press, present on sink0
    //      AND sink1 alike.
    //   2. Controller 0's system-CC association ADDITIONALLY resends once
    //      because resetHeld itself went false->true. THIS is the part
    //      actually scoped to one controller: controller 1 watches
    //      ToggleGestureSelect, which never fires in this instrument (see
    //      TwoControllerInstrument()'s doc comment) and has no dependency on
    //      resetHeld at all, so sink1 carries no reset-related content in
    //      either round.
    // Before this change, only ProcessBlock's 50-block-throttled publish
    // ever populated uiState_, and this test's total block count (3) never
    // reached it, so uiState_ stayed frozen at its pre-press default the
    // whole test and neither catch-up existed -- the old "both sinks empty"
    // assertion here passed only by comparing that frozen stale value to
    // itself, not by exercising real steady-state (no-op) behavior. Do NOT
    // restore an empty-sink assertion here: these one-tick catch-up resends
    // after the publisher claim first activates are correct, intended
    // behavior.
    rig.RunBlocks(1);
    sender->FlushForTests(std::chrono::milliseconds(500));

    REQUIRE_TRUE(sink0.received.size() == 2);
    bool sawEncoderEcho = false;
    bool sawResetToggle = false;
    for (const synth::BasicMidi& midi : sink0.received) {
        REQUIRE_TRUE(midi.IsCC());
        const bool encoderEcho = midi.Channel() == 0 && midi.GetCC() == 0 && midi.GetValue() == 32;
        const bool resetToggle = midi.Channel() == 4 && midi.GetCC() == 10 && midi.GetValue() == 127;
        REQUIRE_TRUE(encoderEcho || resetToggle);
        sawEncoderEcho = sawEncoderEcho || encoderEcho;
        sawResetToggle = sawResetToggle || resetToggle;
    }
    REQUIRE_TRUE(sawEncoderEcho);  // Alpha's default-value echo, unrelated to the reset press
    REQUIRE_TRUE(sawResetToggle);  // resetHeld caught up to "on" -- scoped to controller 0

    REQUIRE_TRUE(sink1.received.size() == 1);
    REQUIRE_TRUE(sink1.received[0].IsCC());
    REQUIRE_TRUE(sink1.received[0].Channel() == 0);
    REQUIRE_TRUE(sink1.received[0].GetCC() == 0);
    REQUIRE_TRUE(sink1.received[0].GetValue() == 32);  // Beta's own encoder-echo catch-up, unrelated to reset
    sink0.received.clear();
    sink1.received.clear();

    // Force controller 1's cache to clear; controller 0's cache stays warm.
    rig.Engine().ResetMidiOutputProcessors(1);
    rig.RunBlocks(1);
    sender->FlushForTests(std::chrono::milliseconds(500));
    sender->Stop();

    REQUIRE_TRUE(sink0.received.empty());   // controller 0: no resend, cache untouched
    REQUIRE_TRUE(!sink1.received.empty());  // controller 1: resent after its cache was cleared
}

// Removing a controller via EditInstrument shrinks MidiControllerCount() and
// nulls the accessor for the removed slot -- the rebuild path that keeps
// midiProcessors_ in lockstep with LiveInstrument().controllers.
TEST_CASE(rig_edit_instrument_removing_controller_shrinks_count_and_nulls_accessor) {
    synth_rig::SynthRig<TwoControllerRigApp> rig;
    rig.InstallInstrumentForTest(TwoControllerInstrument());
    REQUIRE_TRUE(rig.Engine().MidiControllerCount() == 2);
    REQUIRE_TRUE(rig.Engine().MidiInputProcessor(1) != nullptr);

    rig.Engine().EditInstrument([](synth::MidiInstrumentConfig& instrument) { instrument.RemoveController(1); });

    REQUIRE_TRUE(rig.Engine().MidiControllerCount() == 1);
    REQUIRE_TRUE(rig.Engine().MidiInputProcessor(0) != nullptr);
    REQUIRE_TRUE(rig.Engine().MidiInputProcessor(1) == nullptr);
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
