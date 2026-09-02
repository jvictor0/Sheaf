#include "synth/ControllerWizard.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "synth/MidiController.hpp"
#include "synth/RuntimeUIState.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth module tests must not see JUCE headers"
#endif

#include <chrono>
#include <iostream>
#include <limits>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
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

using synth::MidiControllerProfileConfig;
using synth::MidiControllerDisposition;
using synth::MidiControllerSlot;
using synth::MidiControllerSystemMessageAssociation;
using synth::MidiControlAddress;
using synth::MidiEndpointRef;
using synth::MidiInstrumentConfig;
using synth::MidiKindSupport;
using synth::MidiProfileKind;
using synth::WrldBldrSystemPosition;

MidiControllerSystemMessageAssociation MakeControlOnlyAssociation() {
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 3, .cc = 8};
    association.press = synth::MessageIn::SetReset(0, true);
    association.feedback = association.press;
    return association;
}

MidiControllerSystemMessageAssociation MakeWrldBldrAssociation() {
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 5, .cc = 0};
    association.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 0};
    association.press = synth::MessageIn::SetReset(0, true);
    association.feedback = association.press;
    return association;
}

MidiControllerSystemMessageAssociation MakeLaunchpadAssociation() {
    MidiControllerSystemMessageAssociation association;
    association.launchpadPosition = synth::LaunchpadGridPosition{
        .controller = synth::LaunchpadController::LaunchpadX, .x = 0, .y = 0};
    association.press = synth::MessageIn::SetReset(0, true);
    association.feedback = association.press;
    return association;
}

MidiControllerSystemMessageAssociation MakeNoAddressAssociation() {
    MidiControllerSystemMessageAssociation association;
    association.press = synth::MessageIn::SetReset(0, true);
    association.feedback = association.press;
    return association;
}

MidiControllerSystemMessageAssociation MakeWrldBldrPositionOnlyAssociation() {
    MidiControllerSystemMessageAssociation association;
    association.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 0};
    association.press = synth::MessageIn::SetReset(0, true);
    association.feedback = association.press;
    return association;
}

MidiControllerSlot MakeGenericSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::Generic;
    slot.disposition = MidiControllerDisposition::Active;
    return slot;
}

MidiEndpointRef MakeEndpointRef(const char* identifier, const char* name) {
    MidiEndpointRef ref;
    ref.identifier = identifier;
    ref.name = name;
    return ref;
}

bool JsonObjectHasKey(synth::JSON json, const char* key) {
    if (json.m_node == nullptr || json.m_node->m_type != synth::JsonType::Object) {
        return false;
    }
    const synth::JsonContainer& container = json.m_node->m_container;
    const auto* members = static_cast<const synth::JsonMember*>(container.m_entries);
    for (std::uint32_t ix = 0; ix < container.m_size; ++ix) {
        if (members[ix].m_key != nullptr && std::strcmp(members[ix].m_key, key) == 0) {
            return true;
        }
    }
    return false;
}

MidiControllerSlot MakeBlacklistedSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::MfTwister;
    slot.disposition = MidiControllerDisposition::Blacklisted;
    slot.wizardId = "com.sheaf.midi-fighter-twister";
    slot.input = MakeEndpointRef("twister-in-id", "MIDI Fighter Twister In");
    slot.output = MakeEndpointRef("twister-out-id", "MIDI Fighter Twister Out");
    return slot;
}

class RecordingMidiInProcessor final : public synth::MidiInProcessor {
public:
    void Process(const synth::BasicMidi& midi) override { received.push_back(midi); }

    std::vector<synth::BasicMidi> received;
};

struct FakeMidiOutputSink final : synth::IMidiOutputSink {
    std::vector<synth::BasicMidi> sent;

    void Send(const synth::BasicMidi& midi) override { sent.push_back(midi); }
};

synth::PolyphonicPressureMapping MakePressureMapping(
    std::uint8_t channel = 3, std::uint8_t note = 42,
    synth::MessageIn pressure = synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0)) {
    return {
        .address = {.channel = channel, .note = note},
        .pressure = pressure,
    };
}

MidiControllerProfileConfig MakeHoldDrillProfileConfig(synth::EncoderMode mode) {
    MidiControllerProfileConfig config;
    config.encoderInput = synth::EncoderMidiInConfig{};
    config.encoderInput->mode = mode;
    config.encoderInput->turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 4});
    config.encoderInput->turns.push_back({.control = {.channel = 0, .cc = 2}, .slotIx = 0, .position = 5});
    config.systemMessages.push_back({
        .control = MidiControlAddress{.channel = 0, .cc = 20},
        .press = synth::MessageIn::HoldDrill(0, true),
        .release = synth::MessageIn::HoldDrill(0, false),
    });
    return config;
}

TEST_CASE(KindNameRoundTrip) {
    REQUIRE_TRUE(std::string(synth::MidiProfileKindName(MidiProfileKind::WrldBldr)) == "wrldbldr");
    REQUIRE_TRUE(std::string(synth::MidiProfileKindName(MidiProfileKind::MfTwister)) == "twister");
    REQUIRE_TRUE(std::string(synth::MidiProfileKindName(MidiProfileKind::Launchpad)) == "launchpad");
    REQUIRE_TRUE(std::string(synth::MidiProfileKindName(MidiProfileKind::Generic)) == "generic");

    MidiProfileKind kind{};
    REQUIRE_TRUE(synth::MidiProfileKindFromName("wrldbldr", kind));
    REQUIRE_TRUE(kind == MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(synth::MidiProfileKindFromName("twister", kind));
    REQUIRE_TRUE(kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(synth::MidiProfileKindFromName("launchpad", kind));
    REQUIRE_TRUE(kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(synth::MidiProfileKindFromName("generic", kind));
    REQUIRE_TRUE(kind == MidiProfileKind::Generic);
}

TEST_CASE(MessageInJsonRoundTripsHighGestureIndex) {
    synth::JsonArena arena(4096);
    const synth::MessageIn source = synth::MessageIn::SetGestureSelect(17, 63, true);
    const synth::JSON json = synth::ToJSON(arena, source);
    synth::MessageIn target;
    REQUIRE_TRUE(synth::FromJSON(json, target));
    REQUIRE_TRUE(target.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(target.gestureIx == 63);
    REQUIRE_TRUE(target.boolValue);
    REQUIRE_TRUE(target.hasBoolValue);
}

TEST_CASE(MessageInRealtimeFactoriesPreserveInternalDefaultsAndExternalIdentity) {
    const synth::MessageIn internalStart = synth::MessageIn::Start(11);
    const synth::MessageIn internalContinue = synth::MessageIn::Continue(0);
    REQUIRE_TRUE(internalStart.origin == synth::MessageIn::Origin::Internal);
    REQUIRE_TRUE(internalContinue.origin == synth::MessageIn::Origin::Internal);
    REQUIRE_TRUE(internalContinue.type == synth::MessageIn::Type::Continue);

    const synth::MessageIn exactTimestamp = synth::MessageIn::Clock(
        123456, synth::MessageIn::Origin::ExternalMidi, 7);
    REQUIRE_TRUE(exactTimestamp.timestamp == 123456);
    const synth::MessageIn external = synth::MessageIn::Clock(
        0, synth::MessageIn::Origin::ExternalMidi, 7);
    REQUIRE_TRUE(external.origin == synth::MessageIn::Origin::ExternalMidi);
    REQUIRE_TRUE(external.externalControllerSlot == 7);

    synth::JsonArena arena(4096);
    const synth::JSON json = synth::ToJSON(arena, external);
    REQUIRE_TRUE(std::string_view(json.Get("origin").StringValue()) == "externalMidi");
    REQUIRE_TRUE(json.Get("externalControllerSlot").IntegerValue() == 7);
    synth::MessageIn roundTrip;
    REQUIRE_TRUE(synth::FromJSON(json, roundTrip));
    REQUIRE_TRUE(roundTrip == external);

    synth::JsonArena internalArena(4096);
    const synth::JSON internalJson = synth::ToJSON(internalArena, internalContinue);
    REQUIRE_TRUE(internalJson.Get("origin").IsNull());
    synth::MessageIn internalRoundTrip;
    REQUIRE_TRUE(synth::FromJSON(internalJson, internalRoundTrip));
    REQUIRE_TRUE(internalRoundTrip == internalContinue);
}

TEST_CASE(RealtimeMidiProcessorTranslatesExactSingleByteMessagesWithOriginalTimestampAndSlot) {
    synth::MessageInBus bus(nullptr, 8);
    synth::RealtimeMidiInProcessor processor(/*controllerSlot=*/3, &bus);
    processor.SetTimestampProvider([] { return std::uint64_t{999999}; });
    RecordingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::Clock(101));
    processor.Process(synth::BasicMidi::TransportStart(102));
    processor.Process(synth::BasicMidi::TransportContinue(103));
    processor.Process(synth::BasicMidi::TransportStop(104));
    REQUIRE_TRUE(bus.Size() == 4);
    REQUIRE_TRUE(thru.received.empty());

    const synth::MessageIn::Type expectedTypes[] = {
        synth::MessageIn::Type::Clock,
        synth::MessageIn::Type::Start,
        synth::MessageIn::Type::Continue,
        synth::MessageIn::Type::Stop,
    };
    for (std::size_t ix = 0; ix < 4; ++ix) {
        synth::MessageIn message;
        REQUIRE_TRUE(bus.Pop(message, std::numeric_limits<std::uint64_t>::max()));
        REQUIRE_TRUE(message.type == expectedTypes[ix]);
        REQUIRE_TRUE(message.timestamp == 101 + ix);
        REQUIRE_TRUE(message.origin == synth::MessageIn::Origin::ExternalMidi);
        REQUIRE_TRUE(message.externalControllerSlot == 3);
    }

    const synth::BasicMidi multiByteRealtime(201, std::vector<std::uint8_t>{0xF8, 0x00});
    const synth::BasicMidi unsupportedRealtime(202, std::vector<std::uint8_t>{0xF9});
    const synth::BasicMidi ordinary = synth::BasicMidi::CC(203, 2, 3, 4);
    processor.Process(multiByteRealtime);
    processor.Process(unsupportedRealtime);
    processor.Process(ordinary);
    REQUIRE_TRUE(bus.Size() == 0);
    REQUIRE_TRUE(thru.received.size() == 3);
    REQUIRE_TRUE(thru.received[0].timestamp == 201);
    REQUIRE_TRUE(thru.received[1].timestamp == 202);
    REQUIRE_TRUE(thru.received[2].timestamp == 203);
}

TEST_CASE(EveryControllerProfileEndsInRealtimeMidiIncludingEmptyProfiles) {
    synth::MessageInBus bus(nullptr, 8);
    auto empty = synth::CreateMidiControllerProfile(
        MidiControllerProfileConfig{}, &bus, nullptr,
        static_cast<synth::ParameterManager::UIState*>(nullptr), {}, 0, nullptr, 5);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(empty.input.get()) != nullptr);
    REQUIRE_TRUE(empty.inputThru.empty());

    empty.input->Process(synth::BasicMidi::TransportContinue(777));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::Continue);
    REQUIRE_TRUE(message.timestamp == 777);
    REQUIRE_TRUE(message.externalControllerSlot == 5);
}

TEST_CASE(BasicMidiPolyPressureRecognizesOnlyCompletePolyphonicAftertouch) {
    const synth::BasicMidi midi = synth::BasicMidi::PolyPressure(9, 3, 42, 88);
    REQUIRE_TRUE(midi.timestamp == 9);
    REQUIRE_TRUE(midi.IsPolyPressure());
    REQUIRE_TRUE(midi.Channel() == 3);
    REQUIRE_TRUE(midi.GetNote() == 42);
    REQUIRE_TRUE(midi.GetPressure() == 88);

    REQUIRE_TRUE(!synth::BasicMidi(0, std::vector<std::uint8_t>{0xA3}).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi(0, std::vector<std::uint8_t>{0xA3, 42}).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi(0, std::vector<std::uint8_t>{0xD3, 88}).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi::Note(0, 3, 42, 88).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi::CC(0, 3, 42, 88).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi::PitchBend(0, 3, 8192).IsPolyPressure());
    REQUIRE_TRUE(!synth::BasicMidi::Clock(0).IsPolyPressure());
}

TEST_CASE(PolyPressureProcessorStampsMappedPressureAndConsumesExactMatch) {
    synth::MessageInBus bus(nullptr, 8);
    synth::PolyphonicPressureMidiInConfig config;
    config.mappings.push_back(MakePressureMapping());
    synth::PolyphonicPressureMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 1234; });
    RecordingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::PolyPressure(9, 3, 42, 88));
    REQUIRE_TRUE(bus.Size() == 1);
    REQUIRE_TRUE(thru.received.empty());

    synth::MessageIn mapped;
    REQUIRE_TRUE(bus.Pop(mapped, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(mapped.type == synth::MessageIn::Type::GridPressureChange);
    REQUIRE_TRUE(mapped.timestamp == 1234);
    REQUIRE_TRUE(mapped.gridSlotIx == 1);
    REQUIRE_TRUE(mapped.gridX == -1);
    REQUIRE_TRUE(mapped.gridY == 7);
    REQUIRE_TRUE(mapped.velocity == 88);
}

TEST_CASE(PolyPressureProcessorPassesUnmatchedAndNonPressureToThruExactlyOnce) {
    synth::MessageInBus bus(nullptr, 8);
    synth::PolyphonicPressureMidiInConfig config;
    config.mappings.push_back(MakePressureMapping());
    synth::PolyphonicPressureMidiInProcessor processor(config, &bus);
    RecordingMidiInProcessor thru;
    processor.SetThru(&thru);

    const std::vector<synth::BasicMidi> passthrough = {
        synth::BasicMidi::PolyPressure(0, 2, 42, 88),
        synth::BasicMidi::PolyPressure(0, 3, 41, 88),
        synth::BasicMidi::Note(0, 3, 42, 88),
        synth::BasicMidi::NoteOff(0, 3, 42),
        synth::BasicMidi::CC(0, 3, 42, 88),
        synth::BasicMidi(0, std::vector<std::uint8_t>{0xD3, 88}),
        synth::BasicMidi::PitchBend(0, 3, 8192),
        synth::BasicMidi::Clock(0),
    };
    for (const synth::BasicMidi& midi : passthrough) {
        processor.Process(midi);
    }
    REQUIRE_TRUE(bus.Size() == 0);
    REQUIRE_TRUE(thru.received.size() == passthrough.size());
    for (std::size_t ix = 0; ix < passthrough.size(); ++ix) {
        REQUIRE_TRUE(thru.received[ix].raw == passthrough[ix].raw);
    }

    synth::PolyphonicPressureMidiInProcessor noThru(config, &bus);
    noThru.SetTimestampProvider([] { return 99; });
    noThru.Process(synth::BasicMidi::PolyPressure(0, 3, 42, 12));
    noThru.Process(synth::BasicMidi::PolyPressure(0, 9, 9, 9));
    noThru.Process(synth::BasicMidi::CC(0, 9, 9, 9));
    REQUIRE_TRUE(bus.Size() == 1);
    synth::MessageIn noThruMapped;
    REQUIRE_TRUE(bus.Pop(noThruMapped, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(noThruMapped.timestamp == 99);
    REQUIRE_TRUE(noThruMapped.velocity == 12);
}

TEST_CASE(PolyPressureProcessorRejectsInvalidConfigWithoutReplacingPriorConfig) {
    synth::PolyphonicPressureMidiInConfig valid;
    valid.mappings.push_back(MakePressureMapping());
    synth::PolyphonicPressureMidiInProcessor processor({}, nullptr);
    REQUIRE_TRUE(processor.SetConfig(valid));
    REQUIRE_TRUE(processor.Config().mappings == valid.mappings);

    synth::PolyphonicPressureMidiInConfig duplicate = valid;
    duplicate.mappings.push_back(MakePressureMapping(
        3, 42, synth::MessageIn::GridPressureChange(0, 9, 2, 3, 0)));
    REQUIRE_TRUE(!processor.SetConfig(duplicate));
    REQUIRE_TRUE(processor.Config().mappings == valid.mappings);

    synth::PolyphonicPressureMidiInConfig wrongTarget = valid;
    wrongTarget.mappings[0].pressure = synth::MessageIn::GridPress(0, 1, -1, 7, 0);
    REQUIRE_TRUE(!processor.SetConfig(wrongTarget));
    REQUIRE_TRUE(processor.Config().mappings == valid.mappings);

    synth::PolyphonicPressureMidiInConfig outOfRange = valid;
    outOfRange.mappings[0].address = {.channel = 16, .note = 128};
    REQUIRE_TRUE(!processor.SetConfig(outOfRange));
    REQUIRE_TRUE(processor.Config().mappings == valid.mappings);
}

TEST_CASE(CreateMidiControllerProfileBuildsPressureOnlyAndSharedMixedThruChains) {
    synth::MessageInBus bus(nullptr, 16);
    MidiControllerProfileConfig pressureOnly;
    pressureOnly.pressureInput = synth::PolyphonicPressureMidiInConfig{{MakePressureMapping()}};
    auto only = synth::CreateMidiControllerProfile(
        pressureOnly, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 77; });
    REQUIRE_TRUE(dynamic_cast<synth::PolyphonicPressureMidiInProcessor*>(only.input.get()) != nullptr);
    REQUIRE_TRUE(only.inputThru.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(only.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(only.input->Bus() == &bus);

    MidiControllerProfileConfig mixed;
    mixed.encoderInput = synth::EncoderMidiInConfig{};
    mixed.encoderInput->turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 0});
    mixed.analogInput = synth::AnalogMidiInConfig{};
    mixed.analogInput->gestures.push_back({.control = {.channel = 0, .cc = 2}, .gestureIx = 0});
    mixed.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 0, .cc = 3},
        .press = synth::MessageIn::ToggleReset(0),
        .feedback = synth::MessageIn::ToggleReset(0),
    });
    mixed.pressureInput = synth::PolyphonicPressureMidiInConfig{{MakePressureMapping()}};

    auto chain = synth::CreateMidiControllerProfile(
        mixed, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 77; });
    REQUIRE_TRUE(dynamic_cast<synth::EncoderMidiInProcessor*>(chain.input.get()) != nullptr);
    REQUIRE_TRUE(chain.inputThru.size() == 4);
    REQUIRE_TRUE(dynamic_cast<synth::AnalogMidiInProcessor*>(chain.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(chain.inputThru[1].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::PolyphonicPressureMidiInProcessor*>(chain.inputThru[2].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(chain.inputThru[3].get()) != nullptr);
    REQUIRE_TRUE(chain.input->Bus() == &bus);
    REQUIRE_TRUE(chain.inputThru[0]->Bus() == &bus);
    REQUIRE_TRUE(chain.inputThru[1]->Bus() == &bus);
    REQUIRE_TRUE(chain.inputThru[2]->Bus() == &bus);

    RecordingMidiInProcessor end;
    chain.inputThru.back()->SetThru(&end);
    chain.input->Process(synth::BasicMidi::PolyPressure(0, 3, 42, 91));
    REQUIRE_TRUE(end.received.empty());
    synth::MessageIn mapped;
    REQUIRE_TRUE(bus.Pop(mapped, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(mapped.timestamp == 77);
    REQUIRE_TRUE(mapped.velocity == 91);

    chain.input->Process(synth::BasicMidi::PolyPressure(0, 3, 41, 91));
    chain.input->Process(synth::BasicMidi(0, std::vector<std::uint8_t>{0xD3, 91}));
    REQUIRE_TRUE(end.received.size() == 2);
}

TEST_CASE(HoldDrillTurnPushesOnceThenPlainTurnAfterRelease) {
    synth::MessageInBus bus(nullptr, 16);
    auto config = MakeHoldDrillProfileConfig(synth::EncoderMode::Signed7Bit);
    auto chain = synth::CreateMidiControllerProfile(
        config, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 500; });
    REQUIRE_TRUE(chain.holdDrill != nullptr);

    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 127));  // hold
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // drills the knob
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // already drilled this hold: nothing
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 0));    // release
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // plain turn again

    REQUIRE_TRUE(bus.Size() == 2);
    synth::MessageIn push;
    REQUIRE_TRUE(bus.Pop(push, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(push.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(push.slotIx == 0);
    REQUIRE_TRUE(push.position == 4);

    synth::MessageIn incDec;
    REQUIRE_TRUE(bus.Pop(incDec, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(incDec.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(incDec.slotIx == 0);
    REQUIRE_TRUE(incDec.position == 4);
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(HoldDrillDrillsEachTurnedKnobOnceDuringOneHold) {
    synth::MessageInBus bus(nullptr, 16);
    auto config = MakeHoldDrillProfileConfig(synth::EncoderMode::Signed7Bit);
    auto chain = synth::CreateMidiControllerProfile(
        config, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 501; });

    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 127));  // hold
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // drills position 4
    chain.input->Process(synth::BasicMidi::CC(0, 0, 2, 80));    // drills position 5
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 0));    // release

    REQUIRE_TRUE(bus.Size() == 2);
    synth::MessageIn first;
    REQUIRE_TRUE(bus.Pop(first, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(first.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(first.position == 4);

    synth::MessageIn second;
    REQUIRE_TRUE(bus.Pop(second, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(second.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(second.position == 5);
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(HoldDrillOnAbsoluteEncoderSkipsAbsoluteFeedbackUntilRelease) {
    synth::MessageInBus bus(nullptr, 16);
    auto config = MakeHoldDrillProfileConfig(synth::EncoderMode::Absolute);
    auto chain = synth::CreateMidiControllerProfile(
        config, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 502; });

    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 127));  // hold
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // drills instead of jumping
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 0));    // release
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // first plain absolute turn

    REQUIRE_TRUE(bus.Size() == 2);
    synth::MessageIn push;
    REQUIRE_TRUE(bus.Pop(push, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(push.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(push.position == 4);

    synth::MessageIn absolute;
    REQUIRE_TRUE(bus.Pop(absolute, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(absolute.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(absolute.position == 4);
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(HoldDrillResetsDrilledFlagsOnEachNewHold) {
    synth::MessageInBus bus(nullptr, 16);
    auto config = MakeHoldDrillProfileConfig(synth::EncoderMode::Signed7Bit);
    auto chain = synth::CreateMidiControllerProfile(
        config, &bus, nullptr, static_cast<synth::ParameterManager::UIState*>(nullptr), [] { return 503; });

    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 127));  // first hold
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // drills position 4
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 0));    // release
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 127));  // second hold
    chain.input->Process(synth::BasicMidi::CC(0, 0, 1, 70));    // drills position 4 again
    chain.input->Process(synth::BasicMidi::CC(0, 0, 20, 0));    // release

    REQUIRE_TRUE(bus.Size() == 2);
    synth::MessageIn first;
    REQUIRE_TRUE(bus.Pop(first, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(first.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(first.position == 4);

    synth::MessageIn second;
    REQUIRE_TRUE(bus.Pop(second, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(second.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(second.position == 4);
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(GridMessageInFactoriesCarryFlatSemanticFields) {
    const synth::MessageIn press = synth::MessageIn::GridPress(11, 1, -1, 7, 100);
    REQUIRE_TRUE(press.timestamp == 11);
    REQUIRE_TRUE(press.type == synth::MessageIn::Type::GridPress);
    REQUIRE_TRUE(press.gridSlotIx == 1);
    REQUIRE_TRUE(press.gridX == -1);
    REQUIRE_TRUE(press.gridY == 7);
    REQUIRE_TRUE(press.velocity == 100);

    const synth::MessageIn release = synth::MessageIn::GridRelease(12, 1, -1, 7);
    REQUIRE_TRUE(release.timestamp == 12);
    REQUIRE_TRUE(release.type == synth::MessageIn::Type::GridRelease);
    REQUIRE_TRUE(release.gridSlotIx == 1);
    REQUIRE_TRUE(release.gridX == -1);
    REQUIRE_TRUE(release.gridY == 7);

    const synth::MessageIn pressure = synth::MessageIn::GridPressureChange(13, 1, -1, 7, 64);
    REQUIRE_TRUE(pressure.timestamp == 13);
    REQUIRE_TRUE(pressure.type == synth::MessageIn::Type::GridPressureChange);
    REQUIRE_TRUE(pressure.gridSlotIx == 1);
    REQUIRE_TRUE(pressure.gridX == -1);
    REQUIRE_TRUE(pressure.gridY == 7);
    REQUIRE_TRUE(pressure.velocity == 64);

    const synth::MessageIn select = synth::MessageIn::SelectGrid(14, 2, 5);
    REQUIRE_TRUE(select.timestamp == 14);
    REQUIRE_TRUE(select.type == synth::MessageIn::Type::SelectGrid);
    REQUIRE_TRUE(select.gridSlotIx == 2);
    REQUIRE_TRUE(select.gridIx == 5);
}

TEST_CASE(GridMessageInJsonUsesFlatPerVariantShapeAndRoundTrips) {
    synth::JsonArena arena(4096);
    const synth::MessageIn messages[] = {
        synth::MessageIn::GridPress(11, 1, -1, 7, 100),
        synth::MessageIn::GridRelease(12, 1, -1, 7),
        synth::MessageIn::GridPressureChange(13, 1, -1, 7, 64),
        synth::MessageIn::SelectGrid(14, 2, 5),
    };

    for (const synth::MessageIn& source : messages) {
        const synth::JSON json = synth::ToJSON(arena, source);
        REQUIRE_TRUE(json.Get("gridSlot").IntegerValue() == static_cast<std::int64_t>(source.gridSlotIx));
        REQUIRE_TRUE(json.Get("slotIx").IsNull());

        synth::MessageIn target;
        REQUIRE_TRUE(synth::FromJSON(json, target));
        REQUIRE_TRUE(target.type == source.type);
        REQUIRE_TRUE(target.gridSlotIx == source.gridSlotIx);
        if (source.type == synth::MessageIn::Type::SelectGrid) {
            REQUIRE_TRUE(json.Get("type").StringValue() == std::string_view("selectGrid"));
            REQUIRE_TRUE(json.Get("grid").IntegerValue() == 5);
            REQUIRE_TRUE(json.Get("x").IsNull());
            REQUIRE_TRUE(json.Get("y").IsNull());
            REQUIRE_TRUE(json.Get("velocity").IsNull());
            REQUIRE_TRUE(target.gridIx == source.gridIx);
        } else {
            REQUIRE_TRUE(json.Get("grid").IsNull());
            REQUIRE_TRUE(json.Get("x").IntegerValue() == -1);
            REQUIRE_TRUE(json.Get("y").IntegerValue() == 7);
            REQUIRE_TRUE(target.gridX == source.gridX);
            REQUIRE_TRUE(target.gridY == source.gridY);
            if (source.type == synth::MessageIn::Type::GridRelease) {
                REQUIRE_TRUE(json.Get("type").StringValue() == std::string_view("gridRelease"));
                REQUIRE_TRUE(json.Get("velocity").IsNull());
            } else {
                const std::string_view expectedType = source.type == synth::MessageIn::Type::GridPress
                                                          ? "gridPress"
                                                          : "gridPressureChange";
                REQUIRE_TRUE(json.Get("type").StringValue() == expectedType);
                REQUIRE_TRUE(json.Get("velocity").IntegerValue() == source.velocity);
                REQUIRE_TRUE(target.velocity == source.velocity);
            }
        }
    }
}

TEST_CASE(GridFeedbackReadsOnlyTheImmutableRuntimeSnapshot) {
    std::unique_ptr<synth::GridManager::UIState> gridState;
    bool on = true;
    bool off = false;
    {
        synth::GridManager manager;
        const auto range = synth::GridRange::Create(-2, 0, -1, 1);
        REQUIRE_TRUE(range.has_value());
        const auto gridIx = manager.CreateGrid(*range);
        const auto connectedSlotIx = manager.CreateSlot(*range);
        const auto disconnectedSlotIx = manager.CreateSlot(*range);
        REQUIRE_TRUE(gridIx.has_value());
        REQUIRE_TRUE(connectedSlotIx.has_value());
        REQUIRE_TRUE(disconnectedSlotIx.has_value());

        using Cell = synth::StateCell<bool>;
        REQUIRE_TRUE(manager.GridAt(*gridIx)->RegisterCell(
            -1, -1, std::make_unique<Cell>(synth::Color::Off, synth::Color::Rgb(10, 20, 30),
                                           &on, true, false, Cell::Mode::ShowOnly)));
        REQUIRE_TRUE(manager.GridAt(*gridIx)->RegisterCell(
            -2, 0, std::make_unique<Cell>(synth::Color::Rgb(40, 50, 60), synth::Color::White,
                                          &off, true, false, Cell::Mode::ShowOnly)));
        REQUIRE_TRUE(manager.SelectGridForSlot(*connectedSlotIx, *gridIx));
        gridState = manager.CreateUIState();
        manager.PopulateUIState(*gridState);
    }

    synth::ParameterManager::UIState parameters;
    parameters.Configure(0, 0, 0, 0, 0, 0);
    synth::RuntimeUIState runtimeState{.parameters = &parameters, .grids = gridState.get()};
    synth::SystemMessageOutputInfo info(&runtimeState);

    auto state = info.Evaluate(synth::MessageIn::GridPress(0, 0, -1, -1, 127));
    REQUIRE_TRUE(state.color == synth::Color::Rgb(10, 20, 30));
    REQUIRE_TRUE(state.isOn);

    state = info.Evaluate(synth::MessageIn::GridRelease(0, 0, -2, 0));
    REQUIRE_TRUE(state.color == synth::Color::Rgb(40, 50, 60));
    REQUIRE_TRUE(!state.isOn);

    state = info.Evaluate(synth::MessageIn::GridPressureChange(0, 0, -1, -1, 64));
    REQUIRE_TRUE(state.color == synth::Color::Rgb(10, 20, 30));
    REQUIRE_TRUE(state.isOn);

    for (const synth::MessageIn& missing : {
             synth::MessageIn::GridPress(0, 0, 0, -1, 127),
             synth::MessageIn::GridPress(0, 0, -1, 1, 127),
             synth::MessageIn::GridPress(0, 1, -1, -1, 127),
             synth::MessageIn::GridPress(0, 99, -1, -1, 127),
         }) {
        state = info.Evaluate(missing);
        REQUIRE_TRUE(state.color == synth::Color::Off);
        REQUIRE_TRUE(!state.isOn);
    }
}

TEST_CASE(GridMessageInJsonRejectsVelocityOutsideByteRangeWithoutMutation) {
    auto makeMessage = [](synth::JsonArena& arena, std::int64_t velocity) {
        synth::JSON json = arena.Object();
        json.SetNew("type", arena.String("gridPress"));
        json.SetNew("gridSlot", arena.Integer(1));
        json.SetNew("x", arena.Integer(-1));
        json.SetNew("y", arena.Integer(7));
        json.SetNew("velocity", arena.Integer(velocity));
        return json;
    };

    synth::MessageIn target = synth::MessageIn::SceneSelect(99, 3);
    synth::JsonArena negativeArena(1024);
    REQUIRE_TRUE(!synth::FromJSON(makeMessage(negativeArena, -1), target));
    REQUIRE_TRUE(target.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(target.sceneIx == 3);

    synth::JsonArena overflowArena(1024);
    REQUIRE_TRUE(!synth::FromJSON(makeMessage(overflowArena, 256), target));
    REQUIRE_TRUE(target.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(target.sceneIx == 3);
}

TEST_CASE(MessageInBusCapacityConstructorRemainsSourceCompatibleWithLiteralZero) {
    synth::ParameterManager manager;
    synth::MessageInBus bus(&manager, 0);
    REQUIRE_TRUE(bus.Capacity() == 1);
    bus.SetParameterManager(&manager);
}

TEST_CASE(ControllerGesture63SelectsAndEditsManagerGestureWhileBankMaskRemains32Bit) {
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.SetGestureCount(64));
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "High Gesture", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(77, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(77);
    slot.SelectBank(&bank);

    synth::MessageInBus bus(&manager, 16);
    synth::SystemButtonMidiInConfig buttonConfig;
    buttonConfig.associations.push_back({
        .control = synth::MidiControlAddress{.channel = 2, .cc = 9},
        .press = synth::MessageIn::SetGestureSelect(0, 63, true),
        .release = synth::MessageIn::SetGestureSelect(0, 63, false),
    });
    synth::SystemButtonMidiInProcessor buttons(buttonConfig, &bus);
    buttons.SetTimestampProvider([] { return 41; });
    buttons.Process(synth::BasicMidi::CC(0, 2, 9, 127));
    bus.Process(41);
    REQUIRE_TRUE(manager.GestureSelected(63));

    synth::AnalogMidiInConfig analogConfig;
    analogConfig.gestures.push_back({.control = {.channel = 2, .cc = 10}, .gestureIx = 63});
    synth::AnalogMidiInProcessor analog(analogConfig, &bus);
    analog.SetTimestampProvider([] { return 42; });
    analog.Process(synth::BasicMidi::CC(0, 2, 10, 127));
    bus.Process(42);
    REQUIRE_TRUE(manager.GestureValue(63) == 1.0f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(43, 0, 0, 0.1f)));
    bus.Process(43); // first turn arms the selected gesture
    REQUIRE_TRUE(parameter.GestureActive(0, 63));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(44, 0, 0, 0.1f)));
    bus.Process(44); // second turn edits that same active manager gesture
    REQUIRE_TRUE(parameter.GestureValue(0, 63) > 0.25f);

    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    static_assert(std::is_same_v<decltype(ui->gestures.bankAffectingMask[0].load()), std::uint32_t>);
    REQUIRE_TRUE(ui->gestures.bankAffectingMask[63].load() == 1u);
    REQUIRE_TRUE(ui->gestures.bankAffectingCount[63].load() == 1);
}

TEST_CASE(AnalogMidiInProcessorPushesAppActionForMatchingControlAndGestureOtherwise) {
    synth::MessageInBus bus(nullptr, 8);
    synth::AnalogMidiInConfig config;
    config.gestures.push_back({.control = {.channel = 1, .cc = 5}, .gestureIx = 3});
    config.appActions.push_back({
        .control = {.channel = 1, .cc = 6},
        .appAction = "app.bank",
        .appActionValue = "2",
        .appActionIx = 5,
    });
    synth::AnalogMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 99; });

    processor.Process(synth::BasicMidi::CC(0, 1, 6, 127));
    synth::MessageIn appAction;
    REQUIRE_TRUE(bus.Pop(appAction, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(appAction.type == synth::MessageIn::Type::AppAction);
    REQUIRE_TRUE(appAction.appActionIx == 5);
    REQUIRE_TRUE(appAction.value == 127.0f / 127.0f);

    processor.Process(synth::BasicMidi::CC(0, 1, 5, 64));
    synth::MessageIn gesture;
    REQUIRE_TRUE(bus.Pop(gesture, std::numeric_limits<std::uint64_t>::max()));
    REQUIRE_TRUE(gesture.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(gesture.gestureIx == 3);
}

TEST_CASE(KindNameFromUnknownRejected) {
    MidiProfileKind kind = MidiProfileKind::Generic;
    REQUIRE_TRUE(!synth::MidiProfileKindFromName("bogus", kind));
    REQUIRE_TRUE(!synth::MidiProfileKindFromName("", kind));
    REQUIRE_TRUE(!synth::MidiProfileKindFromName("WrldBldr", kind));
}

TEST_CASE(KindSupportMatrix) {
    const MidiKindSupport wrldbldr = synth::KindSupport(MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(wrldbldr.encoders);
    REQUIRE_TRUE(wrldbldr.systemMessages);
    REQUIRE_TRUE(wrldbldr.analogs);

    const MidiKindSupport twister = synth::KindSupport(MidiProfileKind::MfTwister);
    REQUIRE_TRUE(twister.encoders);
    REQUIRE_TRUE(twister.systemMessages);
    REQUIRE_TRUE(!twister.analogs);

    const MidiKindSupport launchpad = synth::KindSupport(MidiProfileKind::Launchpad);
    REQUIRE_TRUE(!launchpad.encoders);
    REQUIRE_TRUE(launchpad.systemMessages);
    REQUIRE_TRUE(!launchpad.analogs);

    const MidiKindSupport generic = synth::KindSupport(MidiProfileKind::Generic);
    REQUIRE_TRUE(generic.encoders);
    REQUIRE_TRUE(generic.systemMessages);
    REQUIRE_TRUE(generic.analogs);
}

TEST_CASE(ActiveSlotsAcceptManualAndOpaqueWizardIdentity) {
    MidiControllerSlot manual = MakeGenericSlot("manual");
    std::string reason;
    REQUIRE_TRUE(synth::IsActive(manual));
    REQUIRE_TRUE(!manual.wizardId.has_value());
    REQUIRE_TRUE(synth::SlotValidForKind(manual, &reason));

    MidiControllerSlot wizard = MakeGenericSlot("unknown wizard");
    wizard.wizardId = "third.party.future.wizard";
    wizard.input = MakeEndpointRef("unknown-in", "Unknown In");
    wizard.output = MakeEndpointRef("unknown-out", "Unknown Out");
    reason.clear();
    REQUIRE_TRUE(synth::IsActive(wizard));
    REQUIRE_TRUE(synth::SlotValidForKind(wizard, &reason));
}

TEST_CASE(BlacklistedIgnoredSlotsRequireIdentityButNoDormantProfile) {
    MidiControllerSlot ignored = MakeBlacklistedSlot("ignored");
    ignored.kind = MidiProfileKind::Launchpad;
    ignored.config.encoderInput = synth::EncoderMidiInConfig{};

    std::string reason;
    REQUIRE_TRUE(!synth::IsActive(ignored));
    REQUIRE_TRUE(!ignored.dormantConfig.has_value());
    REQUIRE_TRUE(synth::SlotValidForKind(ignored, &reason));
}

TEST_CASE(BlacklistedSlotsRejectMissingWizardOrEndpointRefs) {
    MidiControllerSlot missingWizard = MakeBlacklistedSlot("missing-wizard");
    missingWizard.wizardId.reset();
    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(missingWizard, &reason));
    REQUIRE_TRUE(!reason.empty());

    MidiControllerSlot emptyWizard = MakeBlacklistedSlot("empty-wizard");
    emptyWizard.wizardId = "";
    reason.clear();
    REQUIRE_TRUE(!synth::SlotValidForKind(emptyWizard, &reason));
    REQUIRE_TRUE(!reason.empty());

    MidiControllerSlot missingInput = MakeBlacklistedSlot("missing-input");
    missingInput.input = MidiEndpointRef{};
    reason.clear();
    REQUIRE_TRUE(!synth::SlotValidForKind(missingInput, &reason));
    REQUIRE_TRUE(!reason.empty());

    MidiControllerSlot missingOutput = MakeBlacklistedSlot("missing-output");
    missingOutput.output = MidiEndpointRef{};
    reason.clear();
    REQUIRE_TRUE(!synth::SlotValidForKind(missingOutput, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(BlacklistedSlotsRetainDormantProfileWithoutRuntimeActiveConfig) {
    MidiControllerProfileConfig prior = synth::MfTwisterDefaultProfileConfig();
    MidiControllerSlot slot = MakeBlacklistedSlot("retained");
    slot.config = MidiControllerProfileConfig{};
    slot.dormantConfig = prior;

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!slot.config.encoderInput.has_value());
    REQUIRE_TRUE(slot.dormantConfig.has_value());
    REQUIRE_TRUE(slot.dormantConfig->encoderInput.has_value());
    REQUIRE_TRUE(slot.dormantConfig->encoderInput->turns.size() == prior.encoderInput->turns.size());
}

TEST_CASE(BlacklistedDormantProfilesAreValidatedForKind) {
    MidiControllerSlot slot = MakeBlacklistedSlot("bad dormant");
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = MidiControllerProfileConfig{};
    slot.dormantConfig = MidiControllerProfileConfig{};
    slot.dormantConfig->encoderInput = synth::EncoderMidiInConfig{};

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsLaunchpadWithEncoders) {
    MidiControllerSlot slot = MakeGenericSlot("pad");
    slot.kind = MidiProfileKind::Launchpad;
    slot.config.encoderInput = synth::EncoderMidiInConfig{};
    slot.config.systemMessages.push_back(MakeLaunchpadAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsLaunchpadWithWrldBldrPosition) {
    MidiControllerSlot slot = MakeGenericSlot("pad");
    slot.kind = MidiProfileKind::Launchpad;
    slot.config.systemMessages.push_back(MakeWrldBldrAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsLaunchpadControlOnlyAssociation) {
    MidiControllerSlot slot = MakeGenericSlot("pad");
    slot.kind = MidiProfileKind::Launchpad;
    slot.config.systemMessages.push_back(MakeControlOnlyAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsTwisterWithLaunchpadPosition) {
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    slot.config.systemMessages.push_back(MakeLaunchpadAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsGenericWithLaunchpadPosition) {
    MidiControllerSlot slot = MakeGenericSlot("gen");
    slot.kind = MidiProfileKind::Generic;
    slot.config.systemMessages.push_back(MakeLaunchpadAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsTwisterWithNoAddress) {
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    slot.config.systemMessages.push_back(MakeNoAddressAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsWrldBldrWithWrldBldrPositionOnly) {
    MidiControllerSlot slot = MakeGenericSlot("wrld");
    slot.kind = MidiProfileKind::WrldBldr;
    slot.config.systemMessages.push_back(MakeWrldBldrPositionOnlyAssociation());

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindAcceptsWrldBldrWithWrldBldrPosition) {
    MidiControllerSlot slot = MakeGenericSlot("wrld");
    slot.kind = MidiProfileKind::WrldBldr;
    slot.config.systemMessages.push_back(MakeWrldBldrAssociation());

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(SlotValidForKindAcceptsTwisterSideButtonCcAssociation) {
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    slot.config.systemMessages.push_back(MakeControlOnlyAssociation());

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(SlotValidForKindRejectsNoteEncoderTurns) {
    MidiControllerSlot slot = MakeGenericSlot("gen");
    slot.config.encoderInput = synth::EncoderMidiInConfig{};
    slot.config.encoderInput->turns.push_back({
        .control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note},
        .slotIx = 0,
        .position = 0,
    });

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsNoteAnalogAddresses) {
    MidiControllerSlot gestureSlot = MakeGenericSlot("gesture");
    gestureSlot.config.analogInput = synth::AnalogMidiInConfig{};
    gestureSlot.config.analogInput->gestures.push_back({
        .control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note},
        .gestureIx = 0,
    });

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(gestureSlot, &reason));
    REQUIRE_TRUE(!reason.empty());

    MidiControllerSlot sceneBlendSlot = MakeGenericSlot("scene-blend");
    sceneBlendSlot.config.analogInput = synth::AnalogMidiInConfig{};
    sceneBlendSlot.config.analogInput->sceneBlend = MidiControlAddress{
        .channel = 1,
        .cc = 61,
        .type = synth::MidiControlType::Note,
    };
    reason.clear();
    REQUIRE_TRUE(!synth::SlotValidForKind(sceneBlendSlot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindAcceptsGenericNotePushesAndSystemControls) {
    MidiControllerSlot slot = MakeGenericSlot("gen");
    slot.config.encoderInput = synth::EncoderMidiInConfig{};
    slot.config.encoderInput->pushes.push_back({
        .control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note},
        .slotIx = 0,
        .position = 0,
    });
    MidiControllerSystemMessageAssociation association = MakeControlOnlyAssociation();
    association.control->type = synth::MidiControlType::Note;
    slot.config.systemMessages.push_back(association);

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(SlotValidForKindRejectsNoteSystemControlsForWrldBldrAndTwister) {
    MidiControllerSlot wrld = MakeGenericSlot("wrld");
    wrld.kind = MidiProfileKind::WrldBldr;
    MidiControllerSystemMessageAssociation wrldAssociation = MakeWrldBldrAssociation();
    wrldAssociation.control->type = synth::MidiControlType::Note;
    wrld.config.systemMessages.push_back(wrldAssociation);

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(wrld, &reason));
    REQUIRE_TRUE(!reason.empty());

    MidiControllerSlot twister = MakeGenericSlot("twister");
    twister.kind = MidiProfileKind::MfTwister;
    MidiControllerSystemMessageAssociation twisterAssociation = MakeControlOnlyAssociation();
    twisterAssociation.control->type = synth::MidiControlType::Note;
    twister.config.systemMessages.push_back(twisterAssociation);
    reason.clear();
    REQUIRE_TRUE(!synth::SlotValidForKind(twister, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsTwisterAssociationWithWrongChannel) {
    // Finding 5: MfTwister system associations must carry the fixed
    // hardware channel 3 -- SlotValidForKind previously accepted any
    // control-address association regardless of channel/cc, so a
    // malformed/externally-authored association with e.g. channel 5 (a
    // shape the twister hardware cannot produce) silently passed.
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    MidiControllerSystemMessageAssociation association = MakeControlOnlyAssociation();
    association.control->channel = 5;  // twister side buttons are fixed to channel 3
    slot.config.systemMessages.push_back(association);

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindRejectsTwisterAssociationWithCcOutsideSideButtonRange) {
    // The physical MF Twister side buttons are cc 8..13 (6 buttons) on
    // channel 3 -- a cc outside that range cannot come from real hardware.
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    MidiControllerSystemMessageAssociation association = MakeControlOnlyAssociation();
    association.control->cc = 20;  // outside 8..13
    slot.config.systemMessages.push_back(association);

    std::string reason;
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SlotValidForKindAcceptsFullyPopulatedTwisterSideButtons) {
    // All 6 physical side buttons (cc 8..13) populated -- confirms the
    // default factory's real shape still passes after finding 5's
    // tightened validation.
    synth::MfTwisterDefaultProfileOptions options;
    for (std::size_t ix = 0; ix < options.sideButtons.size(); ++ix) {
        options.sideButtons[ix] = MidiControllerSystemMessageAssociation{
            .press = synth::MessageIn::SceneSelect(0, ix),
            .feedback = synth::MessageIn::SceneSelect(0, ix),
        };
    }
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig(options);

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(SlotValidForKindAcceptsWrldBldrDefaultProfile) {
    MidiControllerSlot slot = MakeGenericSlot("wrld");
    slot.kind = MidiProfileKind::WrldBldr;
    slot.config = synth::WrldBldrDefaultProfileConfig();

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(DefaultMidiInstrumentConfigCarriesSharedWrldBldrProfile) {
    const synth::MidiInstrumentConfig instrument = synth::DefaultMidiInstrumentConfig();
    REQUIRE_TRUE(instrument.controllers.size() == 1);

    const synth::MidiControllerSlot& slot = instrument.controllers.front();
    REQUIRE_TRUE(slot.name == "wrldbldr");
    REQUIRE_TRUE(slot.kind == synth::MidiProfileKind::WrldBldr);

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(slot.config.encoderInput.has_value());
    REQUIRE_TRUE(slot.config.encoderInput->turns.size() == 16);
    REQUIRE_TRUE(slot.config.encoderInput->pushes.size() == 16);
    REQUIRE_TRUE(slot.config.systemMessages.size() == 28);
}

TEST_CASE(SlotValidForKindAcceptsMfTwisterDefaultProfile) {
    MidiControllerSlot slot = MakeGenericSlot("twist");
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig();

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(MfTwisterWizardGeneratesAnActiveKindValidInstrumentSlot) {
    std::unique_ptr<synth::ControllerWizard> wizard = synth::MakeControllerWizard(
        synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{}), "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(wizard != nullptr);
    std::unique_ptr<synth::ControllerConfigForm> baseForm = wizard->ConfigForm(std::nullopt);
    auto* form = dynamic_cast<synth::MfTwisterConfigForm*>(baseForm.get());
    REQUIRE_TRUE(form != nullptr);
    form->encoderSlotText = "4";

    const synth::WizardGenerationResult result = wizard->GenerateProfile(
        *form, {.name = "wizard twister",
                .input = {.identifier = "twister-in", .name = "MIDI Fighter Twister"},
                .output = {.identifier = "twister-out", .name = "MIDI Fighter Twister"}});

    REQUIRE_TRUE(result);
    REQUIRE_TRUE(result.controller->disposition == MidiControllerDisposition::Active);
    REQUIRE_TRUE(result.controller->wizardId == "com.sheaf.midi-fighter-twister");
    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(*result.controller, &reason));
}

TEST_CASE(SlotValidForKindAcceptsLaunchpadDefaultProfile) {
    MidiControllerSlot slot = MakeGenericSlot("pad");
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig();

    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));
}

TEST_CASE(AddControllerRejectsDuplicateName) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot first = MakeGenericSlot("dup");
    MidiControllerSlot second = MakeGenericSlot("dup");

    REQUIRE_TRUE(instrument.AddController(first));
    REQUIRE_TRUE(!instrument.AddController(second));
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].name == "dup");
    REQUIRE_TRUE(instrument.controllers[0].kind == MidiProfileKind::Generic);
}

TEST_CASE(AddControllerRejectsDuplicateNameAcrossDispositions) {
    MidiInstrumentConfig activeFirst;
    REQUIRE_TRUE(activeFirst.AddController(MakeGenericSlot("twist")));
    REQUIRE_TRUE(!activeFirst.AddController(MakeBlacklistedSlot("twist")));
    REQUIRE_TRUE(activeFirst.controllers.size() == 1);
    REQUIRE_TRUE(synth::IsActive(activeFirst.controllers[0]));

    MidiInstrumentConfig blacklistedFirst;
    REQUIRE_TRUE(blacklistedFirst.AddController(MakeBlacklistedSlot("twist")));
    REQUIRE_TRUE(!blacklistedFirst.AddController(MakeGenericSlot("twist")));
    REQUIRE_TRUE(blacklistedFirst.controllers.size() == 1);
    REQUIRE_TRUE(!synth::IsActive(blacklistedFirst.controllers[0]));
}

TEST_CASE(AddControllerRejectsInvalidSlot) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot invalid = MakeGenericSlot("bad");
    invalid.kind = MidiProfileKind::Launchpad;
    invalid.config.systemMessages.push_back(MakeControlOnlyAssociation());

    REQUIRE_TRUE(!instrument.AddController(invalid));
    REQUIRE_TRUE(instrument.controllers.empty());
}

TEST_CASE(OrderedIterationPreservedAfterAddRemove) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("a")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("b")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("c")));

    REQUIRE_TRUE(instrument.controllers.size() == 3);
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[1].name == "b");
    REQUIRE_TRUE(instrument.controllers[2].name == "c");

    instrument.RemoveController(1);
    REQUIRE_TRUE(instrument.controllers.size() == 2);
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[1].name == "c");

    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("d")));
    REQUIRE_TRUE(instrument.controllers.size() == 3);
    REQUIRE_TRUE(instrument.controllers[2].name == "d");

    REQUIRE_TRUE(instrument.FindController("a") != nullptr);
    REQUIRE_TRUE(instrument.FindController("c") != nullptr);
    REQUIRE_TRUE(instrument.FindController("d") != nullptr);
    REQUIRE_TRUE(instrument.FindController("b") == nullptr);
}

TEST_CASE(OrderedIterationPreservedAcrossReplaceAndMiddleAndTrailingRemove) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("a")));
    REQUIRE_TRUE(instrument.AddController(MakeBlacklistedSlot("b")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("c")));
    REQUIRE_TRUE(instrument.AddController(MakeBlacklistedSlot("d")));

    MidiControllerSlot replacement = MakeBlacklistedSlot("bb");
    replacement.input.identifier = "twister-in-bb";
    replacement.output.identifier = "twister-out-bb";
    REQUIRE_TRUE(instrument.ReplaceController(1, replacement));

    REQUIRE_TRUE(instrument.controllers.size() == 4);
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[1].name == "bb");
    REQUIRE_TRUE(!synth::IsActive(instrument.controllers[1]));
    REQUIRE_TRUE(instrument.controllers[2].name == "c");
    REQUIRE_TRUE(instrument.controllers[3].name == "d");

    instrument.RemoveController(2);
    REQUIRE_TRUE(instrument.controllers.size() == 3);
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[1].name == "bb");
    REQUIRE_TRUE(instrument.controllers[2].name == "d");

    instrument.RemoveController(2);
    REQUIRE_TRUE(instrument.controllers.size() == 2);
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[1].name == "bb");
}

TEST_CASE(RenameControllerRejectsDuplicate) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("a")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("b")));

    REQUIRE_TRUE(!instrument.RenameController(0, "b"));
    REQUIRE_TRUE(instrument.controllers[0].name == "a");

    REQUIRE_TRUE(instrument.RenameController(0, "c"));
    REQUIRE_TRUE(instrument.controllers[0].name == "c");
}

TEST_CASE(RenameControllerRejectsBadIndex) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("a")));

    REQUIRE_TRUE(!instrument.RenameController(5, "z"));
}

TEST_CASE(ReplaceControllerRejectsDuplicateAndInvalid) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("a")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("b")));

    MidiControllerSlot dupName = MakeGenericSlot("b");
    REQUIRE_TRUE(!instrument.ReplaceController(0, dupName));
    REQUIRE_TRUE(instrument.controllers[0].name == "a");

    MidiControllerSlot invalid = MakeGenericSlot("a");
    invalid.kind = MidiProfileKind::Launchpad;
    invalid.config.systemMessages.push_back(MakeControlOnlyAssociation());
    REQUIRE_TRUE(!instrument.ReplaceController(0, invalid));
    REQUIRE_TRUE(instrument.controllers[0].name == "a");
    REQUIRE_TRUE(instrument.controllers[0].kind == MidiProfileKind::Generic);

    MidiControllerSlot renamed = MakeGenericSlot("a-renamed");
    REQUIRE_TRUE(instrument.ReplaceController(0, renamed));
    REQUIRE_TRUE(instrument.controllers[0].name == "a-renamed");
}

TEST_CASE(MidiEndpointRefIsConfigured) {
    MidiEndpointRef empty;
    REQUIRE_TRUE(!empty.IsConfigured());

    MidiEndpointRef withIdentifier;
    withIdentifier.identifier = "abc";
    REQUIRE_TRUE(withIdentifier.IsConfigured());

    MidiEndpointRef withName;
    withName.name = "My Device";
    REQUIRE_TRUE(withName.IsConfigured());
}

MidiControllerSlot MakeWrldBldrSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::WrldBldr;
    slot.config = synth::WrldBldrDefaultProfileConfig();
    slot.input.identifier = "wrldbldr-in-id";
    slot.input.name = "WRLD.Bldr In";
    slot.output.identifier = "wrldbldr-out-id";
    slot.output.name = "WRLD.Bldr Out";
    return slot;
}

MidiControllerSlot MakeTwisterSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig();
    slot.input.identifier = "twister-in-id";
    slot.input.name = "MF Twister In";
    slot.output.identifier = "twister-out-id";
    slot.output.name = "MF Twister Out";
    return slot;
}

MidiControllerSlot MakeLaunchpadSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig();
    // Endpoint refs intentionally left unconfigured (empty identifier + name)
    // to cover the "unconfigured endpoint round-trips" case.
    return slot;
}

TEST_CASE(ControllerProfileJsonWritesSchemaTwoAndRoundTripsPressureInput) {
    MidiControllerProfileConfig source;
    source.encoderInput = synth::EncoderMidiInConfig::WrldBldrDefault(2);
    source.analogInput = synth::AnalogMidiInConfig{};
    source.analogInput->sceneBlend = synth::MidiControlAddress{.channel = 1, .cc = 7};
    source.systemMessages.push_back(MakeControlOnlyAssociation());
    source.pressureInput = synth::PolyphonicPressureMidiInConfig{{
        MakePressureMapping(3, 42, synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0)),
        MakePressureMapping(4, 43, synth::MessageIn::GridPressureChange(0, 2, 8, -2, 0)),
    }};

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(json.Get("schema").StringValue() == std::string_view(synth::kMidiControllerProfileSchema));
    REQUIRE_TRUE(json.Get("schemaVersion").IntegerValue() == synth::kMidiControllerProfileSchemaVersion);
    REQUIRE_TRUE(!json.Get("pressureInput").IsNull());
    REQUIRE_TRUE(json.Get("pressureInput").Get("mappings").Size() == 2);

    MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.encoderInput.has_value());
    REQUIRE_TRUE(loaded.encoderInput->turns.size() == source.encoderInput->turns.size());
    REQUIRE_TRUE(loaded.analogInput.has_value());
    REQUIRE_TRUE(loaded.analogInput->sceneBlend == source.analogInput->sceneBlend);
    REQUIRE_TRUE(loaded.systemMessages.size() == 1);
    REQUIRE_TRUE(loaded.pressureInput.has_value());
    REQUIRE_TRUE(loaded.pressureInput->mappings == source.pressureInput->mappings);

    const synth::PolyphonicPressureMapping& mapping = loaded.pressureInput->mappings[0];
    REQUIRE_TRUE(mapping.address.channel == 3);
    REQUIRE_TRUE(mapping.address.note == 42);
    REQUIRE_TRUE(mapping.pressure.type == synth::MessageIn::Type::GridPressureChange);
    REQUIRE_TRUE(mapping.pressure.gridSlotIx == 1);
    REQUIRE_TRUE(mapping.pressure.gridX == -1);
    REQUIRE_TRUE(mapping.pressure.gridY == 7);
}

TEST_CASE(ControllerProfileJsonRoundTripsOpenSysExByteForByte) {
    MidiControllerProfileConfig source;
    source.openSysEx.push_back({0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x41, 0x09, 0x07, 0x01, 0xF7});
    source.openSysEx.push_back({0xF0, 0x7E, 0x00, 0xF7});

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(json.Get("openSysEx").Size() == 2);

    MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.openSysEx == source.openSysEx);
}

TEST_CASE(ControllerProfileJsonWithoutOpenSysExKeyReadsBackEmpty) {
    // A file written before this field existed has no "openSysEx" key at
    // all; FromJSON must still succeed and leave the vector empty rather
    // than failing to parse.
    synth::JsonArena arena(1024 * 1024);
    synth::JSON stripped = arena.Object();
    stripped.SetNew("schema", arena.String(synth::kMidiControllerProfileSchema));
    stripped.SetNew("schemaVersion", arena.Integer(synth::kMidiControllerProfileSchemaVersion));
    REQUIRE_TRUE(!JsonObjectHasKey(stripped, "openSysEx"));

    MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(stripped, loaded));
    REQUIRE_TRUE(loaded.openSysEx.empty());
}

TEST_CASE(CreateMidiControllerProfileOmitsOpenSysExOutputWhenEmpty) {
    synth::MessageInBus bus(nullptr, 8);
    auto profile = synth::CreateMidiControllerProfile(
        MidiControllerProfileConfig{}, &bus, nullptr,
        static_cast<synth::ParameterManager::UIState*>(nullptr));
    REQUIRE_TRUE(profile.outputs.empty());
}

TEST_CASE(OpenSysExMidiOutProcessorSendsOnceThenWaitsForReset) {
    FakeMidiOutputSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    const std::vector<std::uint8_t> message = {0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00,
                                               0x04, 0x41, 0x09, 0x07, 0x01, 0xF7};
    synth::OpenSysExMidiOutProcessor processor({message}, &sender);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].raw == message);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);

    processor.Reset();
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[1].raw == message);
}

TEST_CASE(OpenSysExMidiOutProcessorSendsMultipleMessagesInOrder) {
    FakeMidiOutputSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    const std::vector<std::uint8_t> first = {0xF0, 0x7E, 0x00, 0xF7};
    const std::vector<std::uint8_t> second = {0xF0, 0x7E, 0x01, 0xF7};
    synth::OpenSysExMidiOutProcessor processor({first, second}, &sender);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[0].raw == first);
    REQUIRE_TRUE(sink.sent[1].raw == second);
}

TEST_CASE(AssociationJsonRoundTripsAppActionStringsButNotIndex) {
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 4, .cc = 20};
    association.press = synth::MessageIn::AppAction(0, 7, 0.0f);
    association.appAction = "app.test";
    association.appActionValue = "3";
    association.feedback = association.press;

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, association);
    REQUIRE_TRUE(JsonObjectHasKey(json, "appAction"));
    REQUIRE_TRUE(JsonObjectHasKey(json, "appActionValue"));

    MidiControllerSystemMessageAssociation loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.appAction == "app.test");
    REQUIRE_TRUE(loaded.appActionValue == "3");
    REQUIRE_TRUE(loaded.press.appActionIx == 0);
}

TEST_CASE(AssociationJsonOmitsAppActionKeysForNonAppActionPress) {
    MidiControllerSystemMessageAssociation association = MakeControlOnlyAssociation();

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, association);
    REQUIRE_TRUE(!JsonObjectHasKey(json, "appAction"));
    REQUIRE_TRUE(!JsonObjectHasKey(json, "appActionValue"));
}

TEST_CASE(AnalogMidiInConfigJsonRoundTripsAppActions) {
    synth::AnalogMidiInConfig source;
    source.gestures.push_back({.control = {.channel = 0, .cc = 2}, .gestureIx = 0});
    source.sceneBlend = MidiControlAddress{.channel = 1, .cc = 7};
    source.appActions.push_back({
        .control = {.channel = 2, .cc = 30},
        .appAction = "app.bank",
        .appActionValue = "4",
        .appActionIx = 9,
    });

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, source);
    synth::AnalogMidiInConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.gestures.size() == 1);
    REQUIRE_TRUE(loaded.gestures[0].control == source.gestures[0].control);
    REQUIRE_TRUE(loaded.gestures[0].gestureIx == source.gestures[0].gestureIx);
    REQUIRE_TRUE(loaded.sceneBlend == source.sceneBlend);
    REQUIRE_TRUE(loaded.appActions.size() == 1);
    REQUIRE_TRUE(loaded.appActions[0].control == source.appActions[0].control);
    REQUIRE_TRUE(loaded.appActions[0].appAction == "app.bank");
    REQUIRE_TRUE(loaded.appActions[0].appActionValue == "4");
}

TEST_CASE(ControllerProfileJsonReadsVersionOneWithoutPressureAndPreservesLegacyData) {
    MidiControllerProfileConfig source;
    source.encoderInput = synth::EncoderMidiInConfig::WrldBldrDefault(3);
    source.analogInput = synth::AnalogMidiInConfig{};
    source.analogInput->sceneBlend = synth::MidiControlAddress{.channel = 2, .cc = 11};
    source.systemMessages.push_back(MakeControlOnlyAssociation());

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON versionTwo = synth::ToJSON(arena, source);
    MidiControllerProfileConfig loadedVersionTwo;
    REQUIRE_TRUE(synth::FromJSON(versionTwo, loadedVersionTwo));
    REQUIRE_TRUE(!loadedVersionTwo.pressureInput.has_value());

    synth::JSON versionOne = arena.Object();
    versionOne.SetNew("schema", arena.String(synth::kMidiControllerProfileSchema));
    versionOne.SetNew("schemaVersion", arena.Integer(1));
    versionOne.SetNew("encoderInput", versionTwo.Get("encoderInput"));
    versionOne.SetNew("encoderOutput", versionTwo.Get("encoderOutput"));
    versionOne.SetNew("analogInput", versionTwo.Get("analogInput"));
    versionOne.SetNew("systemMessages", versionTwo.Get("systemMessages"));

    MidiControllerProfileConfig loaded;
    loaded.pressureInput = synth::PolyphonicPressureMidiInConfig{{MakePressureMapping()}};
    REQUIRE_TRUE(synth::FromJSON(versionOne, loaded));
    REQUIRE_TRUE(loaded.encoderInput.has_value());
    REQUIRE_TRUE(loaded.encoderInput->turns.size() == source.encoderInput->turns.size());
    REQUIRE_TRUE(loaded.analogInput.has_value());
    REQUIRE_TRUE(loaded.analogInput->sceneBlend == source.analogInput->sceneBlend);
    REQUIRE_TRUE(loaded.systemMessages.size() == 1);
    REQUIRE_TRUE(!loaded.pressureInput.has_value());
}

TEST_CASE(ControllerProfileJsonRejectsInvalidPressureShapesAtomically) {
    auto makeProfile = [](synth::JsonArena& arena, synth::JSON pressureInput) {
        synth::JSON profile = arena.Object();
        profile.SetNew("schema", arena.String(synth::kMidiControllerProfileSchema));
        profile.SetNew("schemaVersion", arena.Integer(synth::kMidiControllerProfileSchemaVersion));
        profile.SetNew("encoderInput", arena.Null());
        profile.SetNew("encoderOutput", arena.Null());
        profile.SetNew("analogInput", arena.Null());
        profile.SetNew("systemMessages", arena.Array());
        profile.SetNew("pressureInput", pressureInput);
        return profile;
    };
    auto makeMapping = [](synth::JsonArena& arena, std::int64_t channel, std::int64_t note,
                          const synth::MessageIn& target) {
        synth::JSON address = arena.Object();
        address.SetNew("channel", arena.Integer(channel));
        address.SetNew("note", arena.Integer(note));
        synth::JSON mapping = arena.Object();
        mapping.SetNew("address", address);
        mapping.SetNew("pressure", synth::ToJSON(arena, target));
        return mapping;
    };
    auto makeConfig = [](synth::JsonArena& arena, const std::vector<synth::JSON>& mappings) {
        synth::JSON array = arena.Array();
        for (synth::JSON mapping : mappings) {
            array.AppendNew(mapping);
        }
        synth::JSON config = arena.Object();
        config.SetNew("mappings", array);
        return config;
    };

    MidiControllerProfileConfig target;
    target.encoderInput = synth::EncoderMidiInConfig{};
    auto requireRejected = [&](synth::JSON profile) {
        REQUIRE_TRUE(!synth::FromJSON(profile, target));
        REQUIRE_TRUE(target.encoderInput.has_value());
        REQUIRE_TRUE(!target.pressureInput.has_value());
    };

    synth::JsonArena wrongTypeArena(4096);
    requireRejected(makeProfile(wrongTypeArena, wrongTypeArena.String("bad")));

    synth::JsonArena channelArena(4096);
    requireRejected(makeProfile(
        channelArena,
        makeConfig(channelArena, {makeMapping(channelArena, 16, 42,
                                               synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0))})));

    synth::JsonArena noteArena(4096);
    requireRejected(makeProfile(
        noteArena,
        makeConfig(noteArena, {makeMapping(noteArena, 3, 128,
                                           synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0))})));

    synth::JsonArena targetArena(4096);
    requireRejected(makeProfile(
        targetArena,
        makeConfig(targetArena, {makeMapping(targetArena, 3, 42, synth::MessageIn::SceneSelect(0, 1))})));

    synth::JsonArena duplicateArena(8192);
    synth::JSON first = makeMapping(duplicateArena, 3, 42,
                                    synth::MessageIn::GridPressureChange(0, 1, -1, 7, 0));
    synth::JSON second = makeMapping(duplicateArena, 3, 42,
                                     synth::MessageIn::GridPressureChange(0, 2, 4, 5, 0));
    requireRejected(makeProfile(duplicateArena, makeConfig(duplicateArena, {first, second})));
}

TEST_CASE(SlotValidForKindValidatesPressureMappingsWithoutExposingANewKindSection) {
    MidiControllerSlot slot = MakeGenericSlot("pressure");
    slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{{MakePressureMapping()}};
    std::string reason;
    REQUIRE_TRUE(synth::SlotValidForKind(slot, &reason));

    slot.config.pressureInput->mappings.push_back(MakePressureMapping());
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());

    slot.config.pressureInput->mappings.pop_back();
    slot.config.pressureInput->mappings[0].pressure = synth::MessageIn::GridPress(0, 1, -1, 7, 0);
    REQUIRE_TRUE(!synth::SlotValidForKind(slot, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(InstrumentJsonKeepsEnvelopeSchemaAndDelegatesPressureProfileVersion) {
    MidiControllerSlot slot = MakeGenericSlot("pressure");
    slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{{MakePressureMapping()}};
    MidiInstrumentConfig source;
    REQUIRE_TRUE(source.AddController(slot));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(json.Get("schemaVersion").IntegerValue() == 2);
    const synth::JSON nestedProfile = json.Get("controllers").GetAt(0).Get("profile");
    REQUIRE_TRUE(nestedProfile.Get("schemaVersion").IntegerValue() == synth::kMidiControllerProfileSchemaVersion);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(loaded.controllers[0].config.pressureInput.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.pressureInput->mappings == slot.config.pressureInput->mappings);
}

TEST_CASE(InstrumentJsonWritesActiveDispositionAndOmitsManualWizardId) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot manual = MakeTwisterSlot("manual twist");
    REQUIRE_TRUE(instrument.AddController(manual));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controller = json.Get("controllers").GetAt(0);

    REQUIRE_TRUE(!controller.Get("disposition").IsNull());
    REQUIRE_TRUE(controller.Get("disposition").StringValue() == std::string_view("active"));
    REQUIRE_TRUE(!JsonObjectHasKey(controller, "wizardId"));
    REQUIRE_TRUE(JsonObjectHasKey(controller, "profile"));

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(!loaded.controllers[0].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput.has_value());
}

TEST_CASE(InstrumentJsonRoundTripsWizardActiveWithOpaqueUnknownId) {
    MidiControllerSlot slot = MakeTwisterSlot("future wizard");
    slot.wizardId = "future.vendor/controller-wizard:alpha_01";
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controller = json.Get("controllers").GetAt(0);
    REQUIRE_TRUE(!controller.Get("disposition").IsNull());
    REQUIRE_TRUE(controller.Get("disposition").StringValue() == std::string_view("active"));
    REQUIRE_TRUE(!controller.Get("wizardId").IsNull());
    REQUIRE_TRUE(controller.Get("wizardId").StringValue() == std::string_view(*slot.wizardId));

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(loaded.controllers[0].wizardId == slot.wizardId);
    REQUIRE_TRUE(loaded.controllers[0].input.identifier == slot.input.identifier);
    REQUIRE_TRUE(loaded.controllers[0].output.identifier == slot.output.identifier);
}

TEST_CASE(InstrumentJsonRoundTripsBlacklistedIgnoredWithoutProfile) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot ignored = MakeBlacklistedSlot("ignored twister");
    REQUIRE_TRUE(instrument.AddController(ignored));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controller = json.Get("controllers").GetAt(0);
    REQUIRE_TRUE(!controller.Get("disposition").IsNull());
    REQUIRE_TRUE(controller.Get("disposition").StringValue() == std::string_view("blacklisted"));
    REQUIRE_TRUE(!controller.Get("wizardId").IsNull());
    REQUIRE_TRUE(controller.Get("wizardId").StringValue() == std::string_view(*ignored.wizardId));
    REQUIRE_TRUE(!JsonObjectHasKey(controller, "profile"));

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(!synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(loaded.controllers[0].wizardId == ignored.wizardId);
    REQUIRE_TRUE(loaded.controllers[0].input.identifier == ignored.input.identifier);
    REQUIRE_TRUE(loaded.controllers[0].output.identifier == ignored.output.identifier);
    REQUIRE_TRUE(!loaded.controllers[0].dormantConfig.has_value());
    REQUIRE_TRUE(!loaded.controllers[0].config.encoderInput.has_value());
}

TEST_CASE(InstrumentJsonRoundTripsBlacklistedDormantProfile) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot blacklisted = MakeBlacklistedSlot("dormant twister");
    blacklisted.dormantConfig = synth::MfTwisterDefaultProfileConfig();
    REQUIRE_TRUE(instrument.AddController(blacklisted));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controller = json.Get("controllers").GetAt(0);
    REQUIRE_TRUE(!controller.Get("disposition").IsNull());
    REQUIRE_TRUE(controller.Get("disposition").StringValue() == std::string_view("blacklisted"));
    REQUIRE_TRUE(JsonObjectHasKey(controller, "profile"));
    REQUIRE_TRUE(controller.Get("profile").Get("encoderInput").Get("turns").Size() == 16);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(!synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(loaded.controllers[0].dormantConfig.has_value());
    REQUIRE_TRUE(loaded.controllers[0].dormantConfig->encoderInput.has_value());
    REQUIRE_TRUE(loaded.controllers[0].dormantConfig->encoderInput->turns.size() == 16);
    REQUIRE_TRUE(!loaded.controllers[0].config.encoderInput.has_value());
}

TEST_CASE(InstrumentJsonRoundTripsControllersInOrder) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeWrldBldrSlot("wrld")));
    REQUIRE_TRUE(instrument.AddController(MakeTwisterSlot("twist")));
    REQUIRE_TRUE(instrument.AddController(MakeLaunchpadSlot("left pad")));
    REQUIRE_TRUE(instrument.AddController(MakeLaunchpadSlot("right pad")));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    REQUIRE_TRUE(!json.IsNull());
    REQUIRE_TRUE(json.Get("schemaVersion").IntegerValue() == 2);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));

    REQUIRE_TRUE(loaded.controllers.size() == 4);
    REQUIRE_TRUE(loaded.controllers[0].name == "wrld");
    REQUIRE_TRUE(loaded.controllers[0].kind == MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(loaded.controllers[0].input.identifier == "wrldbldr-in-id");
    REQUIRE_TRUE(loaded.controllers[0].input.name == "WRLD.Bldr In");
    REQUIRE_TRUE(loaded.controllers[0].output.identifier == "wrldbldr-out-id");
    REQUIRE_TRUE(loaded.controllers[0].output.name == "WRLD.Bldr Out");
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(!loaded.controllers[0].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput->turns.size() ==
                 instrument.controllers[0].config.encoderInput->turns.size());

    REQUIRE_TRUE(loaded.controllers[1].name == "twist");
    REQUIRE_TRUE(loaded.controllers[1].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(loaded.controllers[1].input.identifier == "twister-in-id");
    REQUIRE_TRUE(loaded.controllers[1].output.name == "MF Twister Out");
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[1]));
    REQUIRE_TRUE(!loaded.controllers[1].wizardId.has_value());

    REQUIRE_TRUE(loaded.controllers[2].name == "left pad");
    REQUIRE_TRUE(loaded.controllers[2].kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[2]));
    // Unconfigured endpoint refs (empty identifier + name) round-trip as unconfigured.
    REQUIRE_TRUE(!loaded.controllers[2].input.IsConfigured());
    REQUIRE_TRUE(!loaded.controllers[2].output.IsConfigured());

    REQUIRE_TRUE(loaded.controllers[3].name == "right pad");
    REQUIRE_TRUE(loaded.controllers[3].kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[3]));
}

TEST_CASE(InstrumentJsonRoundTripsMixedDispositionsInOrder) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeWrldBldrSlot("active wrld")));

    MidiControllerSlot ignored = MakeBlacklistedSlot("ignored twister");
    REQUIRE_TRUE(instrument.AddController(ignored));

    REQUIRE_TRUE(instrument.AddController(MakeLaunchpadSlot("active pad")));

    MidiControllerSlot dormant = MakeBlacklistedSlot("dormant twister");
    dormant.wizardId = "future.vendor/dormant-twister";
    dormant.dormantConfig = synth::MfTwisterDefaultProfileConfig();
    REQUIRE_TRUE(instrument.AddController(dormant));

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controllers = json.Get("controllers");
    REQUIRE_TRUE(controllers.Size() == 4);
    REQUIRE_TRUE(controllers.GetAt(0).Get("disposition").StringValue() == std::string_view("active"));
    REQUIRE_TRUE(JsonObjectHasKey(controllers.GetAt(0), "profile"));
    REQUIRE_TRUE(controllers.GetAt(1).Get("disposition").StringValue() == std::string_view("blacklisted"));
    REQUIRE_TRUE(!JsonObjectHasKey(controllers.GetAt(1), "profile"));
    REQUIRE_TRUE(controllers.GetAt(2).Get("disposition").StringValue() == std::string_view("active"));
    REQUIRE_TRUE(JsonObjectHasKey(controllers.GetAt(2), "profile"));
    REQUIRE_TRUE(controllers.GetAt(3).Get("disposition").StringValue() == std::string_view("blacklisted"));
    REQUIRE_TRUE(JsonObjectHasKey(controllers.GetAt(3), "profile"));

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));

    REQUIRE_TRUE(loaded.controllers.size() == 4);
    REQUIRE_TRUE(loaded.controllers[0].name == "active wrld");
    REQUIRE_TRUE(loaded.controllers[0].kind == MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(!loaded.controllers[0].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(!loaded.controllers[0].dormantConfig.has_value());

    REQUIRE_TRUE(loaded.controllers[1].name == "ignored twister");
    REQUIRE_TRUE(loaded.controllers[1].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(!synth::IsActive(loaded.controllers[1]));
    REQUIRE_TRUE(loaded.controllers[1].wizardId == ignored.wizardId);
    REQUIRE_TRUE(loaded.controllers[1].input.identifier == ignored.input.identifier);
    REQUIRE_TRUE(loaded.controllers[1].output.identifier == ignored.output.identifier);
    REQUIRE_TRUE(!loaded.controllers[1].config.encoderInput.has_value());
    REQUIRE_TRUE(!loaded.controllers[1].dormantConfig.has_value());

    REQUIRE_TRUE(loaded.controllers[2].name == "active pad");
    REQUIRE_TRUE(loaded.controllers[2].kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[2]));
    REQUIRE_TRUE(!loaded.controllers[2].wizardId.has_value());
    REQUIRE_TRUE(!loaded.controllers[2].config.systemMessages.empty());
    REQUIRE_TRUE(!loaded.controllers[2].dormantConfig.has_value());

    REQUIRE_TRUE(loaded.controllers[3].name == "dormant twister");
    REQUIRE_TRUE(loaded.controllers[3].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(!synth::IsActive(loaded.controllers[3]));
    REQUIRE_TRUE(loaded.controllers[3].wizardId == dormant.wizardId);
    REQUIRE_TRUE(loaded.controllers[3].input.identifier == dormant.input.identifier);
    REQUIRE_TRUE(loaded.controllers[3].output.identifier == dormant.output.identifier);
    REQUIRE_TRUE(!loaded.controllers[3].config.encoderInput.has_value());
    REQUIRE_TRUE(loaded.controllers[3].dormantConfig.has_value());
    REQUIRE_TRUE(loaded.controllers[3].dormantConfig->encoderInput.has_value());
    REQUIRE_TRUE(loaded.controllers[3].dormantConfig->encoderInput->turns.size() ==
                 dormant.dormantConfig->encoderInput->turns.size());
}

TEST_CASE(InstrumentJsonEmptyControllersRoundTrips) {
    MidiInstrumentConfig instrument;

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    REQUIRE_TRUE(!json.IsNull());

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.empty());
}

TEST_CASE(InstrumentJsonRejectsUnknownKind) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON controller = arena.Object();
    controller.SetNew("name", arena.String("pad"));
    controller.SetNew("kind", arena.String("theremin"));
    controller.SetNew("disposition", arena.String("active"));
    controller.SetNew("input", synth::ToJSON(arena, MidiEndpointRef{}));
    controller.SetNew("output", synth::ToJSON(arena, MidiEndpointRef{}));
    controller.SetNew("profile", synth::ToJSON(arena, MidiControllerProfileConfig{}));

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);

    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String(synth::kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(synth::kMidiInstrumentSchemaVersion));
    json.SetNew("controllers", controllers);

    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
}

TEST_CASE(InstrumentJsonRejectsDuplicateNames) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("pad")));

    synth::JsonArena arena(1024 * 1024);
    synth::JSON controller = synth::ToJSON(arena, instrument.controllers[0]);
    synth::JSON duplicate = synth::ToJSON(arena, instrument.controllers[0]);

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    controllers.AppendNew(duplicate);

    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String(synth::kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(synth::kMidiInstrumentSchemaVersion));
    json.SetNew("controllers", controllers);

    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
}

// Builds a "profile" JSON object directly (bypassing MidiControllerProfileConfig's
// ToJSON) so tests can construct shapes that SlotValidForKind must reject on load
// — the in-memory types can't represent these illegal combinations directly.
// NOTE: JSON::Get returns the first match for a repeated key, so each field must be
// set exactly once here.
synth::JSON MakeProfileJson(synth::JsonArena& arena, synth::JSON encoderInput, synth::JSON systemMessages) {
    synth::JSON profile = arena.Object();
    profile.SetNew("schema", arena.String("synth.midiControllerProfileConfig"));
    profile.SetNew("schemaVersion", arena.Integer(1));
    profile.SetNew("encoderInput", encoderInput);
    profile.SetNew("encoderOutput", arena.Null());
    profile.SetNew("analogInput", arena.Null());
    profile.SetNew("systemMessages", systemMessages);
    return profile;
}

synth::JSON MakeProfileJsonWithSystemMessages(synth::JsonArena& arena, synth::JSON systemMessages) {
    return MakeProfileJson(arena, arena.Null(), systemMessages);
}

synth::JSON MakeInstrumentControllerJson(synth::JsonArena& arena, const char* name, const char* kind,
                                         synth::JSON profile) {
    synth::JSON controller = arena.Object();
    controller.SetNew("name", arena.String(name));
    controller.SetNew("kind", arena.String(kind));
    controller.SetNew("input", synth::ToJSON(arena, MidiEndpointRef{}));
    controller.SetNew("output", synth::ToJSON(arena, MidiEndpointRef{}));
    controller.SetNew("profile", profile);
    return controller;
}

synth::JSON MakeInstrumentControllerJsonV2(synth::JsonArena& arena, const char* name, const char* kind,
                                           const char* disposition, MidiEndpointRef input,
                                           MidiEndpointRef output) {
    synth::JSON controller = arena.Object();
    controller.SetNew("name", arena.String(name));
    controller.SetNew("kind", arena.String(kind));
    controller.SetNew("disposition", arena.String(disposition));
    controller.SetNew("input", synth::ToJSON(arena, input));
    controller.SetNew("output", synth::ToJSON(arena, output));
    return controller;
}

synth::JSON MakeInstrumentJson(synth::JsonArena& arena, synth::JSON controllers) {
    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String(synth::kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(synth::kMidiInstrumentSchemaVersion));
    json.SetNew("controllers", controllers);
    return json;
}

void RequireInstrumentLoadRejectedAtomically(synth::JSON json) {
    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
    REQUIRE_TRUE(target.controllers[0].kind == MidiProfileKind::Generic);
    REQUIRE_TRUE(synth::IsActive(target.controllers[0]));
}

TEST_CASE(InstrumentJsonLoadsPreviousSchemaControllersAsActiveWithoutWizardId) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON controllers = arena.Array();
    controllers.AppendNew(MakeInstrumentControllerJson(
        arena, "legacy wrld", "wrldbldr", synth::ToJSON(arena, synth::WrldBldrDefaultProfileConfig())));
    controllers.AppendNew(MakeInstrumentControllerJson(
        arena, "legacy twist", "twister", synth::ToJSON(arena, synth::MfTwisterDefaultProfileConfig())));

    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String(synth::kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(1));
    json.SetNew("controllers", controllers);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 2);
    REQUIRE_TRUE(loaded.controllers[0].name == "legacy wrld");
    REQUIRE_TRUE(loaded.controllers[0].kind == MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(!loaded.controllers[0].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(loaded.controllers[1].name == "legacy twist");
    REQUIRE_TRUE(loaded.controllers[1].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[1]));
    REQUIRE_TRUE(!loaded.controllers[1].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[1].config.encoderInput.has_value());
}

TEST_CASE(InstrumentJsonIgnoresPreviousSchemaDispositionAndWizardIdExtensions) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON controller = MakeInstrumentControllerJson(
        arena, "legacy extension", "twister", synth::ToJSON(arena, synth::MfTwisterDefaultProfileConfig()));
    controller.SetNew("disposition", arena.String("blacklisted"));
    controller.SetNew("wizardId", arena.String("future.vendor/legacy-extension"));

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);

    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String(synth::kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(1));
    json.SetNew("controllers", controllers);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.controllers.size() == 1);
    REQUIRE_TRUE(loaded.controllers[0].name == "legacy extension");
    REQUIRE_TRUE(loaded.controllers[0].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(synth::IsActive(loaded.controllers[0]));
    REQUIRE_TRUE(!loaded.controllers[0].wizardId.has_value());
    REQUIRE_TRUE(loaded.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(!loaded.controllers[0].dormantConfig.has_value());
}

TEST_CASE(InstrumentJsonRejectsUnknownDispositionAtomically) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON controller = MakeInstrumentControllerJsonV2(
        arena, "paused", "twister", "paused",
        MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));
    controller.SetNew("profile", synth::ToJSON(arena, synth::MfTwisterDefaultProfileConfig()));

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
}

TEST_CASE(InstrumentJsonRejectsMalformedWizardIdsAtomically) {
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "empty active wizard", "twister", "active",
            MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));
        controller.SetNew("wizardId", arena.String(""));
        controller.SetNew("profile", synth::ToJSON(arena, synth::MfTwisterDefaultProfileConfig()));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "non-string active wizard", "twister", "active",
            MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));
        controller.SetNew("wizardId", arena.Integer(7));
        controller.SetNew("profile", synth::ToJSON(arena, synth::MfTwisterDefaultProfileConfig()));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
}

TEST_CASE(InstrumentJsonRejectsActiveWithoutProfileAtomically) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON controller = MakeInstrumentControllerJsonV2(
        arena, "missing active profile", "twister", "active",
        MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
}

TEST_CASE(InstrumentJsonRejectsIncompleteBlacklistedRecordsAtomically) {
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "missing blacklisted wizard", "twister", "blacklisted",
            MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "empty blacklisted wizard", "twister", "blacklisted",
            MakeEndpointRef("in-id", "Twister In"), MakeEndpointRef("out-id", "Twister Out"));
        controller.SetNew("wizardId", arena.String(""));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "missing blacklisted input", "twister", "blacklisted",
            MidiEndpointRef{}, MakeEndpointRef("out-id", "Twister Out"));
        controller.SetNew("wizardId", arena.String("com.sheaf.midi-fighter-twister"));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
    {
        synth::JsonArena arena(1024 * 1024);
        synth::JSON controller = MakeInstrumentControllerJsonV2(
            arena, "missing blacklisted output", "twister", "blacklisted",
            MakeEndpointRef("in-id", "Twister In"), MidiEndpointRef{});
        controller.SetNew("wizardId", arena.String("com.sheaf.midi-fighter-twister"));

        synth::JSON controllers = arena.Array();
        controllers.AppendNew(controller);
        RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
    }
}

TEST_CASE(InstrumentJsonRejectsKindInvalidDormantProfileAtomically) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON invalidDormantProfile =
        MakeProfileJson(arena, synth::ToJSON(arena, synth::EncoderMidiInConfig{}), arena.Array());
    synth::JSON controller = MakeInstrumentControllerJsonV2(
        arena, "bad dormant pad", "launchpad", "blacklisted",
        MakeEndpointRef("pad-in-id", "Launchpad In"), MakeEndpointRef("pad-out-id", "Launchpad Out"));
    controller.SetNew("wizardId", arena.String("future.launchpad.wizard"));
    controller.SetNew("profile", invalidDormantProfile);

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    RequireInstrumentLoadRejectedAtomically(MakeInstrumentJson(arena, controllers));
}

TEST_CASE(InstrumentJsonRejectsLaunchpadWithEncoderMappings) {
    synth::JsonArena arena(1024 * 1024);
    // Launchpad profile with an illegal encoderInput — launchpad supports no encoders.
    synth::JSON profile =
        MakeProfileJson(arena, synth::ToJSON(arena, synth::EncoderMidiInConfig{}), arena.Array());
    synth::JSON controller = MakeInstrumentControllerJsonV2(
        arena, "pad", "launchpad", "active", MidiEndpointRef{}, MidiEndpointRef{});
    controller.SetNew("profile", profile);

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    synth::JSON json = MakeInstrumentJson(arena, controllers);

    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
}

TEST_CASE(InstrumentJsonRejectsLaunchpadWithWrldBldrPosition) {
    synth::JsonArena arena(1024 * 1024);
    // A system-message entry that illegally carries both a launchpad position and a
    // WRLD.Bldr position; launchpad kind must reject the WRLD.Bldr position.
    synth::JSON association = arena.Object();
    association.SetNew("control", arena.Null());
    association.SetNew("wrldBldrPosition",
                        synth::ToJSON(arena, WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 0}));
    association.SetNew("launchpadPosition",
                        synth::ToJSON(arena, synth::LaunchpadGridPosition{
                                                  .controller = synth::LaunchpadController::LaunchpadX,
                                                  .x = 0,
                                                  .y = 0}));
    association.SetNew("press", synth::ToJSON(arena, synth::MessageIn::SetReset(0, true)));
    association.SetNew("release", arena.Null());
    association.SetNew("feedback", synth::ToJSON(arena, synth::MessageIn::SetReset(0, true)));
    association.SetNew("outputFeedback", arena.Boolean(true));

    synth::JSON systemMessages = arena.Array();
    systemMessages.AppendNew(association);
    synth::JSON profile = MakeProfileJsonWithSystemMessages(arena, systemMessages);
    synth::JSON controller = MakeInstrumentControllerJsonV2(
        arena, "pad", "launchpad", "active", MidiEndpointRef{}, MidiEndpointRef{});
    controller.SetNew("profile", profile);

    synth::JSON controllers = arena.Array();
    controllers.AppendNew(controller);
    synth::JSON json = MakeInstrumentJson(arena, controllers);

    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
}

TEST_CASE(InstrumentJsonRejectsBadSchema) {
    synth::JsonArena arena(1024 * 1024);
    synth::JSON json = arena.Object();
    json.SetNew("schema", arena.String("not.the.right.schema"));
    json.SetNew("schemaVersion", arena.Integer(synth::kMidiInstrumentSchemaVersion));
    json.SetNew("controllers", arena.Array());

    MidiInstrumentConfig target;
    REQUIRE_TRUE(target.AddController(MakeGenericSlot("existing")));
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target.controllers.size() == 1);
    REQUIRE_TRUE(target.controllers[0].name == "existing");
}

TEST_CASE(InstrumentJsonUnconfiguredEndpointRoundTrips) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("gen")));
    REQUIRE_TRUE(!instrument.controllers[0].input.IsConfigured());
    REQUIRE_TRUE(!instrument.controllers[0].output.IsConfigured());

    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);

    MidiInstrumentConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(!loaded.controllers[0].input.IsConfigured());
    REQUIRE_TRUE(!loaded.controllers[0].output.IsConfigured());
    REQUIRE_TRUE(loaded.controllers[0].input.identifier.empty());
    REQUIRE_TRUE(loaded.controllers[0].input.name.empty());
}

} // namespace

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
