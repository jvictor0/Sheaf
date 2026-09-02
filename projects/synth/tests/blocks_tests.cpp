#include "synth/MidiConfigBlocks.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth module tests must not see JUCE headers"
#endif

#include <algorithm>
#include <iostream>
#include <limits>
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

using synth::AnalogBlock;
using synth::AnalogMidiMapping;
using synth::BlockableMessage;
using synth::ComputeSystemMessageSortKey;
using synth::EncoderBlock;
using synth::EncoderMidiMapping;
using synth::ExpandAnalogBlock;
using synth::ExpandEncoderBlock;
using synth::ExpandGridBlock;
using synth::ExpandGridButton;
using synth::ExpandSystemBlock;
using synth::GridBlock;
using synth::GridButton;
using synth::GridMappingExpansion;
using synth::LaunchpadController;
using synth::LaunchpadShapeSupports;
using synth::MessageIn;
using synth::MidiControlAddress;
using synth::MidiControlType;
using synth::MidiControllerSystemMessageAssociation;
using synth::MidiProfileKind;
using synth::NormalizeMidiProfileConfig;
using synth::MidiControllerProfileConfig;
using synth::PolyphonicPressureMapping;
using synth::ReconstructAnalogBlocks;
using synth::ReconstructedAnalogRow;
using synth::ReconstructedEncoderRow;
using synth::ReconstructedSystemRow;
using synth::ReconstructEncoderBlocks;
using synth::ReconstructGridMappings;
using synth::ReconstructSystemBlocks;
using synth::SystemAddressField;
using synth::SystemAddressSchema;
using synth::SystemBlock;
using synth::SystemMessageSortKey;
using synth::WrldBldrPositionToCC;
using synth::WrldBldrSystemPosition;

template <typename T>
concept HasToggleField = requires(T value) { value.toggle; };

// --- Finding 6: full structural equality helper for round-trip tests -------
//
// Every prior round-trip assertion in this file only checked press.type and
// (when present) control -- e.g. it would never have caught finding 1
// (press.boolValue silently flipped by expansion) or a release/feedback/
// address mismatch. This helper compares EVERY field that participates in
// an association's meaning: all three address variants (control,
// wrldBldrPosition incl. its own channel, launchpadPosition incl. its
// controller variant), press/release/feedback (per-type semantic fields,
// mirroring MidiConfigViewModel.cpp's MessageInEquivalent, which
// deliberately excludes `timestamp` -- every default factory and every
// Expand* function constructs MessageIns with timestamp 0, so timestamp
// carries no round-trip-relevant meaning here either), and outputFeedback.

bool MessageInFullyEquivalent(const MessageIn& a, const MessageIn& b) {
    if (a.type != b.type) {
        return false;
    }
    switch (a.type) {
        case MessageIn::Type::ParamIncDec:
            return a.slotIx == b.slotIx && a.position == b.position && a.delta == b.delta;
        case MessageIn::Type::ParamSetAbsolute:
            return a.slotIx == b.slotIx && a.position == b.position && a.value == b.value;
        case MessageIn::Type::ParamSetAbsoluteOnBank:
            return a.bankIx == b.bankIx && a.slotIx == b.slotIx && a.position == b.position &&
                   a.value == b.value;
        case MessageIn::Type::ParamPush:
            return a.slotIx == b.slotIx && a.position == b.position;
        case MessageIn::Type::ToggleReset:
        case MessageIn::Type::ToggleRandom:
        case MessageIn::Type::ToggleRandomMod:
            return a.hasBoolValue == b.hasBoolValue && (!a.hasBoolValue || a.boolValue == b.boolValue);
        case MessageIn::Type::Start:
        case MessageIn::Type::Continue:
        case MessageIn::Type::Stop:
        case MessageIn::Type::Clock:
            return true;
        case MessageIn::Type::ToggleGestureSelect:
            return a.gestureIx == b.gestureIx;
        case MessageIn::Type::SetGestureSelect:
            // Finding 1's regression is caught HERE: boolValue must match.
            return a.gestureIx == b.gestureIx && a.hasBoolValue == b.hasBoolValue && a.boolValue == b.boolValue;
        case MessageIn::Type::SelectParamBank:
            return a.slotIx == b.slotIx && a.bankIx == b.bankIx;
        case MessageIn::Type::NextParamBank:
        case MessageIn::Type::PrevParamBank:
            return a.slotIx == b.slotIx;
        case MessageIn::Type::SetGestureValue:
            return a.gestureIx == b.gestureIx && a.value == b.value;
        case MessageIn::Type::SceneSelect:
            return a.sceneIx == b.sceneIx;
        case MessageIn::Type::SetSceneBlend:
            return a.value == b.value;
        case MessageIn::Type::GridPress:
        case MessageIn::Type::GridPressureChange:
            return a.gridSlotIx == b.gridSlotIx && a.gridX == b.gridX && a.gridY == b.gridY &&
                   a.velocity == b.velocity;
        case MessageIn::Type::GridRelease:
            return a.gridSlotIx == b.gridSlotIx && a.gridX == b.gridX && a.gridY == b.gridY;
        case MessageIn::Type::SelectGrid:
            return a.gridSlotIx == b.gridSlotIx && a.gridIx == b.gridIx;
        case MessageIn::Type::AppAction:
            return a.appActionIx == b.appActionIx && a.value == b.value;
        case MessageIn::Type::HoldDrill:
            return a.hasBoolValue == b.hasBoolValue && (!a.hasBoolValue || a.boolValue == b.boolValue);
    }
    return false;
}

bool OptionalMessageInFullyEquivalent(const std::optional<MessageIn>& a, const std::optional<MessageIn>& b) {
    if (a.has_value() != b.has_value()) {
        return false;
    }
    return !a.has_value() || MessageInFullyEquivalent(*a, *b);
}

// Full structural equality over a system-message association: every address
// variant, press/release/feedback, and outputFeedback -- the "element-by-
// element" comparison finding 6 asks Expand(Reconstruct(x)) == x round-trips
// to use, rather than the previous checks (size + press.type + control).
bool AssociationEqual(const MidiControllerSystemMessageAssociation& a,
                      const MidiControllerSystemMessageAssociation& b) {
    if (a.control.has_value() != b.control.has_value()) {
        return false;
    }
    if (a.control.has_value() && !(*a.control == *b.control)) {
        return false;
    }
    if (a.wrldBldrPosition.has_value() != b.wrldBldrPosition.has_value()) {
        return false;
    }
    if (a.wrldBldrPosition.has_value() &&
        !(a.wrldBldrPosition->channel == b.wrldBldrPosition->channel && a.wrldBldrPosition->x == b.wrldBldrPosition->x &&
          a.wrldBldrPosition->y == b.wrldBldrPosition->y)) {
        return false;
    }
    if (a.launchpadPosition.has_value() != b.launchpadPosition.has_value()) {
        return false;
    }
    if (a.launchpadPosition.has_value() && !(*a.launchpadPosition == *b.launchpadPosition)) {
        return false;
    }
    if (!MessageInFullyEquivalent(a.press, b.press)) {
        return false;
    }
    if (!OptionalMessageInFullyEquivalent(a.release, b.release)) {
        return false;
    }
    if (!MessageInFullyEquivalent(a.feedback, b.feedback)) {
        return false;
    }
    if (a.outputFeedback != b.outputFeedback) {
        return false;
    }
    return true;
}

void RequireAssociationsEqual(const std::vector<MidiControllerSystemMessageAssociation>& actual,
                              const std::vector<MidiControllerSystemMessageAssociation>& expected) {
    REQUIRE_TRUE(actual.size() == expected.size());
    for (std::size_t ix = 0; ix < expected.size(); ++ix) {
        REQUIRE_TRUE(AssociationEqual(actual[ix], expected[ix]));
    }
}

// --- D1: SystemAddressSchema ------------------------------------------------

TEST_CASE(SchemaWrldBldrIsChannelXY) {
    const auto schema = SystemAddressSchema(MidiProfileKind::WrldBldr);
    const std::vector<SystemAddressField> expected = {SystemAddressField::Channel, SystemAddressField::WrldBldrX,
                                                       SystemAddressField::WrldBldrY};
    REQUIRE_TRUE(schema == expected);
}

TEST_CASE(SchemaLaunchpadIsXYOnly) {
    const auto schema = SystemAddressSchema(MidiProfileKind::Launchpad);
    const std::vector<SystemAddressField> expected = {SystemAddressField::LaunchpadX, SystemAddressField::LaunchpadY};
    REQUIRE_TRUE(schema == expected);
}

TEST_CASE(SchemaTwisterIsButtonOnly) {
    const auto schema = SystemAddressSchema(MidiProfileKind::MfTwister);
    const std::vector<SystemAddressField> expected = {SystemAddressField::Button};
    REQUIRE_TRUE(schema == expected);
}

TEST_CASE(SchemaGenericIsChannelCc) {
    const auto schema = SystemAddressSchema(MidiProfileKind::Generic);
    const std::vector<SystemAddressField> expected = {SystemAddressField::AddressType, SystemAddressField::Channel,
                                                       SystemAddressField::Cc};
    REQUIRE_TRUE(schema == expected);
}

// --- D2: SystemMessageSortKey / NormalizeMidiProfileConfig ------------------

TEST_CASE(SortKeyOrdersByMessageTypeDeclarationOrder) {
    MidiControllerSystemMessageAssociation sceneAssoc;
    sceneAssoc.control = MidiControlAddress{.channel = 0, .cc = 0};
    sceneAssoc.press = MessageIn::SceneSelect(0, 0);
    sceneAssoc.feedback = sceneAssoc.press;

    MidiControllerSystemMessageAssociation bankAssoc;
    bankAssoc.control = MidiControlAddress{.channel = 0, .cc = 0};
    bankAssoc.press = MessageIn::SelectParamBank(0, 0, 0);
    bankAssoc.feedback = bankAssoc.press;

    const auto sceneKey = ComputeSystemMessageSortKey(sceneAssoc, MidiProfileKind::Generic);
    const auto bankKey = ComputeSystemMessageSortKey(bankAssoc, MidiProfileKind::Generic);
    // SelectParamBank (8) precedes SceneSelect (13) in MessageIn::Type's
    // declaration order.
    REQUIRE_TRUE(bankKey < sceneKey);
}

TEST_CASE(SortKeyIncludesAbsolutePayloadAddressAdjacentToRelativeTurns) {
    MidiControllerSystemMessageAssociation relative;
    relative.control = MidiControlAddress{.channel = 0, .cc = 0};
    relative.press = MessageIn::ParamIncDec(0, 2, 3, 0.1f);
    relative.feedback = relative.press;

    MidiControllerSystemMessageAssociation absolute;
    absolute.control = MidiControlAddress{.channel = 0, .cc = 0};
    absolute.press = MessageIn::ParamSetAbsolute(0, 2, 3, 0.75f);
    absolute.feedback = absolute.press;

    const auto relativeKey = ComputeSystemMessageSortKey(relative, MidiProfileKind::Generic);
    const auto absoluteKey = ComputeSystemMessageSortKey(absolute, MidiProfileKind::Generic);
    REQUIRE_TRUE(relativeKey < absoluteKey);
    REQUIRE_TRUE(absoluteKey.arg1 == 2);
    REQUIRE_TRUE(absoluteKey.arg2 == 3);
}

TEST_CASE(SortKeyOrdersSceneSelectByArgument) {
    MidiControllerSystemMessageAssociation a;
    a.control = MidiControlAddress{.channel = 0, .cc = 5};
    a.press = MessageIn::SceneSelect(0, 3);
    a.feedback = a.press;

    MidiControllerSystemMessageAssociation b;
    b.control = MidiControlAddress{.channel = 0, .cc = 0};
    b.press = MessageIn::SceneSelect(0, 1);
    b.feedback = b.press;

    const auto keyA = ComputeSystemMessageSortKey(a, MidiProfileKind::Generic);
    const auto keyB = ComputeSystemMessageSortKey(b, MidiProfileKind::Generic);
    REQUIRE_TRUE(keyB < keyA);  // sceneIx 1 < 3, regardless of address
}

TEST_CASE(SortKeyOrdersBankSelectBySlotThenBank) {
    MidiControllerSystemMessageAssociation a;
    a.control = MidiControlAddress{.channel = 0, .cc = 0};
    a.press = MessageIn::SelectParamBank(0, 1, 0);  // slot 1, bank 0
    a.feedback = a.press;

    MidiControllerSystemMessageAssociation b;
    b.control = MidiControlAddress{.channel = 0, .cc = 0};
    b.press = MessageIn::SelectParamBank(0, 0, 5);  // slot 0, bank 5
    b.feedback = b.press;

    const auto keyA = ComputeSystemMessageSortKey(a, MidiProfileKind::Generic);
    const auto keyB = ComputeSystemMessageSortKey(b, MidiProfileKind::Generic);
    REQUIRE_TRUE(keyB < keyA);  // slot 0 < slot 1, regardless of bank
}

TEST_CASE(SortKeyDistinguishesSetResetHeldVariants) {
    MidiControllerSystemMessageAssociation press;
    press.control = MidiControlAddress{.channel = 0, .cc = 0};
    press.press = MessageIn::SetReset(0, true);
    press.feedback = press.press;

    MidiControllerSystemMessageAssociation release;
    release.control = MidiControlAddress{.channel = 0, .cc = 1};
    release.press = MessageIn::SetReset(0, false);
    release.feedback = release.press;

    const auto keyPress = ComputeSystemMessageSortKey(press, MidiProfileKind::Generic);
    const auto keyRelease = ComputeSystemMessageSortKey(release, MidiProfileKind::Generic);
    REQUIRE_TRUE(!(keyPress == keyRelease));
    // Deterministic: false sorts before true.
    REQUIRE_TRUE(keyRelease < keyPress);
}

TEST_CASE(SortKeyTiesBreakByAddressTuple) {
    MidiControllerSystemMessageAssociation a;
    a.control = MidiControlAddress{.channel = 0, .cc = 9};
    a.press = MessageIn::SceneSelect(0, 0);
    a.feedback = a.press;

    MidiControllerSystemMessageAssociation b;
    b.control = MidiControlAddress{.channel = 0, .cc = 1};
    b.press = MessageIn::SceneSelect(0, 0);
    b.feedback = b.press;

    const auto keyA = ComputeSystemMessageSortKey(a, MidiProfileKind::Generic);
    const auto keyB = ComputeSystemMessageSortKey(b, MidiProfileKind::Generic);
    REQUIRE_TRUE(keyB < keyA);  // cc 1 < cc 9, same message
}

TEST_CASE(SortKeyDistinguishesGenericCcAndNoteAddresses) {
    MidiControllerSystemMessageAssociation cc;
    cc.control = MidiControlAddress{.channel = 1, .cc = 60, .type = MidiControlType::Cc};
    cc.press = MessageIn::SceneSelect(0, 0);
    cc.feedback = cc.press;

    MidiControllerSystemMessageAssociation note = cc;
    note.control->type = MidiControlType::Note;

    const auto ccKey = ComputeSystemMessageSortKey(cc, MidiProfileKind::Generic);
    const auto noteKey = ComputeSystemMessageSortKey(note, MidiProfileKind::Generic);
    REQUIRE_TRUE(!(ccKey == noteKey));
    REQUIRE_TRUE(ccKey < noteKey);
}

TEST_CASE(SortKeyOrdersGridMessagesByEverySemanticFieldBeforeAddress) {
    auto associationFor = [](const MessageIn& message, std::uint8_t cc) {
        MidiControllerSystemMessageAssociation association;
        association.control = MidiControlAddress{.channel = 0, .cc = cc};
        association.press = message;
        association.feedback = message;
        return association;
    };

    const auto slotOne = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 1, -1, 7, 100), 0), MidiProfileKind::Generic);
    const auto slotTwo = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 2, -5, 0, 0), 0), MidiProfileKind::Generic);
    REQUIRE_TRUE(slotOne < slotTwo);

    const auto xNegative = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 1, -2, 7, 100), 127), MidiProfileKind::Generic);
    const auto xPositive = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 1, 1, -9, 0), 0), MidiProfileKind::Generic);
    REQUIRE_TRUE(xNegative < xPositive);

    const auto yLow = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPressureChange(0, 1, -1, -3, 200), 127), MidiProfileKind::Generic);
    const auto yHigh = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPressureChange(0, 1, -1, 4, 0), 0), MidiProfileKind::Generic);
    REQUIRE_TRUE(yLow < yHigh);

    const auto velocityLow = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 1, -1, 7, 3), 127), MidiProfileKind::Generic);
    const auto velocityHigh = ComputeSystemMessageSortKey(
        associationFor(MessageIn::GridPress(0, 1, -1, 7, 200), 0), MidiProfileKind::Generic);
    REQUIRE_TRUE(velocityLow < velocityHigh);

    const auto gridOne = ComputeSystemMessageSortKey(
        associationFor(MessageIn::SelectGrid(0, 2, 1), 127), MidiProfileKind::Generic);
    const auto gridFive = ComputeSystemMessageSortKey(
        associationFor(MessageIn::SelectGrid(0, 2, 5), 0), MidiProfileKind::Generic);
    REQUIRE_TRUE(gridOne < gridFive);
}

TEST_CASE(NormalizeSortsEncoderTurnsAndPushesBySlotThenPosition) {
    MidiControllerProfileConfig config;
    config.encoderInput = synth::EncoderMidiInConfig{};
    config.encoderInput->turns = {
        EncoderMidiMapping{.control = {.channel = 0, .cc = 5}, .slotIx = 1, .position = 0},
        EncoderMidiMapping{.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 2},
        EncoderMidiMapping{.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0},
    };
    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);
    REQUIRE_TRUE(config.encoderInput->turns[0].slotIx == 0 && config.encoderInput->turns[0].position == 0);
    REQUIRE_TRUE(config.encoderInput->turns[1].slotIx == 0 && config.encoderInput->turns[1].position == 2);
    REQUIRE_TRUE(config.encoderInput->turns[2].slotIx == 1 && config.encoderInput->turns[2].position == 0);
}

TEST_CASE(NormalizeSortsAnalogGesturesByIndex) {
    MidiControllerProfileConfig config;
    config.analogInput = synth::AnalogMidiInConfig{};
    config.analogInput->gestures = {
        AnalogMidiMapping{.control = {.channel = 0, .cc = 5}, .gestureIx = 3},
        AnalogMidiMapping{.control = {.channel = 0, .cc = 1}, .gestureIx = 0},
    };
    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);
    REQUIRE_TRUE(config.analogInput->gestures[0].gestureIx == 0);
    REQUIRE_TRUE(config.analogInput->gestures[1].gestureIx == 3);
}

TEST_CASE(NormalizeSortsSystemMessagesByKeyStably) {
    MidiControllerProfileConfig config;
    MidiControllerSystemMessageAssociation scene2;
    scene2.control = MidiControlAddress{.channel = 0, .cc = 0};
    scene2.press = MessageIn::SceneSelect(0, 2);
    scene2.feedback = scene2.press;

    MidiControllerSystemMessageAssociation scene0;
    scene0.control = MidiControlAddress{.channel = 0, .cc = 1};
    scene0.press = MessageIn::SceneSelect(0, 0);
    scene0.feedback = scene0.press;

    MidiControllerSystemMessageAssociation bank0;
    bank0.control = MidiControlAddress{.channel = 0, .cc = 2};
    bank0.press = MessageIn::SelectParamBank(0, 0, 0);
    bank0.feedback = bank0.press;

    config.systemMessages = {scene2, scene0, bank0};
    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);

    REQUIRE_TRUE(config.systemMessages[0].press.type == MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(config.systemMessages[1].press.type == MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(config.systemMessages[1].press.sceneIx == 0);
    REQUIRE_TRUE(config.systemMessages[2].press.type == MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(config.systemMessages[2].press.sceneIx == 2);
}

TEST_CASE(NormalizeIsStableForExactDuplicates) {
    MidiControllerProfileConfig config;
    MidiControllerSystemMessageAssociation first;
    first.control = MidiControlAddress{.channel = 0, .cc = 3};
    first.press = MessageIn::SceneSelect(0, 0);
    first.feedback = first.press;

    MidiControllerSystemMessageAssociation second = first;  // exact duplicate

    config.systemMessages = {first, second};
    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);
    REQUIRE_TRUE(config.systemMessages.size() == 2);
    // Stable sort: original relative order preserved among exact duplicates.
    REQUIRE_TRUE(config.systemMessages[0].control->cc == 3);
    REQUIRE_TRUE(config.systemMessages[1].control->cc == 3);
}

TEST_CASE(RelativeBankMessagesSortBySlotAndReconstructAsIndividualCanonicalRows) {
    MidiControllerSystemMessageAssociation nextHigh;
    nextHigh.control = MidiControlAddress{.channel = 0, .cc = 4};
    nextHigh.press = MessageIn::NextParamBank(0, 3);
    nextHigh.feedback = nextHigh.press;

    MidiControllerSystemMessageAssociation nextLow = nextHigh;
    nextLow.control->cc = 5;
    nextLow.press = MessageIn::NextParamBank(0, 1);
    nextLow.feedback = nextLow.press;

    MidiControllerSystemMessageAssociation previous;
    previous.control = MidiControlAddress{.channel = 0, .cc = 6};
    previous.press = MessageIn::PrevParamBank(0, 2);
    previous.feedback = previous.press;

    REQUIRE_TRUE(ComputeSystemMessageSortKey(nextLow, MidiProfileKind::Generic) <
                 ComputeSystemMessageSortKey(nextHigh, MidiProfileKind::Generic));

    MidiControllerProfileConfig config;
    config.systemMessages = {nextHigh, previous, nextLow, nextLow};
    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);
    REQUIRE_TRUE(config.systemMessages[0].press.type == MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(config.systemMessages[0].press.slotIx == 1);
    REQUIRE_TRUE(config.systemMessages[1].press.type == MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(config.systemMessages[1].press.slotIx == 1);
    REQUIRE_TRUE(config.systemMessages[2].press.type == MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(config.systemMessages[2].press.slotIx == 3);
    REQUIRE_TRUE(config.systemMessages[3].press.type == MessageIn::Type::PrevParamBank);
    REQUIRE_TRUE(config.systemMessages[3].press.slotIx == 2);

    const auto rows = ReconstructSystemBlocks(config.systemMessages, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == config.systemMessages.size());
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        REQUIRE_TRUE(!rows[ix].isBlock);
        REQUIRE_TRUE(rows[ix].indices.size() == 1);
        REQUIRE_TRUE(rows[ix].indices[0] == ix);
    }
}

TEST_CASE(NormalizeSortsPressureMappingsByLogicalTargetThenPhysicalAddress) {
    auto mapping = [](std::uint8_t channel, std::uint8_t note, std::size_t slot,
                      int x, int y, std::uint64_t timestamp = 0) {
        return PolyphonicPressureMapping{
            .address = {.channel = channel, .note = note},
            .pressure = MessageIn::GridPressureChange(timestamp, slot, x, y, 0),
        };
    };

    MidiControllerProfileConfig config;
    config.pressureInput = synth::PolyphonicPressureMidiInConfig{{
        mapping(0, 1, 1, -4, 0),
        mapping(2, 9, 0, 2, -1),
        mapping(2, 7, 0, 2, -1),
        mapping(1, 9, 0, 2, -1),
        mapping(7, 7, 0, -2, 4),
        mapping(1, 1, 0, -2, -3),
    }};

    NormalizeMidiProfileConfig(config, MidiProfileKind::Generic);
    const auto& sorted = config.pressureInput->mappings;
    REQUIRE_TRUE(sorted.size() == 6);
    REQUIRE_TRUE(sorted[0] == mapping(1, 1, 0, -2, -3));
    REQUIRE_TRUE(sorted[1] == mapping(7, 7, 0, -2, 4));
    REQUIRE_TRUE(sorted[2] == mapping(1, 9, 0, 2, -1));
    REQUIRE_TRUE(sorted[3] == mapping(2, 7, 0, 2, -1));
    REQUIRE_TRUE(sorted[4] == mapping(2, 9, 0, 2, -1));
    REQUIRE_TRUE(sorted[5] == mapping(0, 1, 1, -4, 0));
}

TEST_CASE(NormalizePressureOrderingConvergesAndKeepsEqualKeysStable) {
    auto mapping = [](std::uint8_t channel, std::uint8_t note, std::size_t slot,
                      int x, int y, std::uint64_t timestamp = 0, std::uint8_t pressure = 0) {
        return PolyphonicPressureMapping{
            .address = {.channel = channel, .note = note},
            .pressure = MessageIn::GridPressureChange(timestamp, slot, x, y, pressure),
        };
    };

    const PolyphonicPressureMapping a = mapping(3, 42, 0, -1, 7);
    const PolyphonicPressureMapping b = mapping(2, 41, 0, -1, 7);
    const PolyphonicPressureMapping c = mapping(0, 3, 2, 4, -5);
    MidiControllerProfileConfig first;
    first.pressureInput = synth::PolyphonicPressureMidiInConfig{{c, a, b}};
    MidiControllerProfileConfig second;
    second.pressureInput = synth::PolyphonicPressureMidiInConfig{{b, c, a}};
    NormalizeMidiProfileConfig(first, MidiProfileKind::WrldBldr);
    NormalizeMidiProfileConfig(second, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(first.pressureInput->mappings == second.pressureInput->mappings);

    MidiControllerProfileConfig duplicates;
    duplicates.pressureInput = synth::PolyphonicPressureMidiInConfig{{
        mapping(3, 42, 0, -1, 7, 10, 1),
        mapping(3, 42, 0, -1, 7, 20, 2),
    }};
    NormalizeMidiProfileConfig(duplicates, MidiProfileKind::Launchpad);
    REQUIRE_TRUE(duplicates.pressureInput->mappings[0].pressure.timestamp == 10);
    REQUIRE_TRUE(duplicates.pressureInput->mappings[0].pressure.velocity == 1);
    REQUIRE_TRUE(duplicates.pressureInput->mappings[1].pressure.timestamp == 20);
    REQUIRE_TRUE(duplicates.pressureInput->mappings[1].pressure.velocity == 2);
}

// --- Runtime button-grid expansion ---------------------------------------

TEST_CASE(ExpandGridButtonProducesAtomicMomentarySystemAndPressurePair) {
    static_assert(!HasToggleField<GridButton>);

    GridButton button;
    button.kind = MidiProfileKind::WrldBldr;
    button.channel = 5;
    button.x = 3;
    button.y = 6;
    button.gridSlotIx = 2;
    button.outputFeedback = false;

    GridMappingExpansion out;
    std::string reason;
    REQUIRE_TRUE(ExpandGridButton(button, out, &reason));
    REQUIRE_TRUE(out.systemMessages.size() == 1);
    REQUIRE_TRUE(out.pressureMappings.size() == 1);

    const auto& system = out.systemMessages[0];
    REQUIRE_TRUE(system.control == std::optional<MidiControlAddress>(
                                       MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(3, 6)}));
    REQUIRE_TRUE(system.wrldBldrPosition.has_value());
    REQUIRE_TRUE(system.wrldBldrPosition->channel == 5);
    REQUIRE_TRUE(system.wrldBldrPosition->x == 3);
    REQUIRE_TRUE(system.wrldBldrPosition->y == 6);
    REQUIRE_TRUE(system.press.type == MessageIn::Type::GridPress);
    REQUIRE_TRUE(system.press.gridSlotIx == 2);
    REQUIRE_TRUE(system.press.gridX == 3);
    REQUIRE_TRUE(system.press.gridY == 6);
    REQUIRE_TRUE(system.release.has_value());
    REQUIRE_TRUE(system.release->type == MessageIn::Type::GridRelease);
    REQUIRE_TRUE(system.release->gridSlotIx == 2);
    REQUIRE_TRUE(system.release->gridX == 3);
    REQUIRE_TRUE(system.release->gridY == 6);
    REQUIRE_TRUE(system.feedback.type == MessageIn::Type::GridPress);
    REQUIRE_TRUE(system.feedback.gridSlotIx == 2);
    REQUIRE_TRUE(system.feedback.gridX == 3);
    REQUIRE_TRUE(system.feedback.gridY == 6);
    REQUIRE_TRUE(system.feedback.velocity == 0);
    REQUIRE_TRUE(!system.outputFeedback);

    const auto& pressure = out.pressureMappings[0];
    REQUIRE_TRUE(pressure.address.channel == 5);
    REQUIRE_TRUE(pressure.address.note == WrldBldrPositionToCC(3, 6));
    REQUIRE_TRUE(pressure.pressure.type == MessageIn::Type::GridPressureChange);
    REQUIRE_TRUE(pressure.pressure.gridSlotIx == 2);
    REQUIRE_TRUE(pressure.pressure.gridX == 3);
    REQUIRE_TRUE(pressure.pressure.gridY == 6);
    REQUIRE_TRUE(pressure.pressure.velocity == 0);
}

TEST_CASE(ExpandGridBlockTraversesSignedExclusiveRangeXFastAndPairsEveryCell) {
    static_assert(!HasToggleField<GridBlock>);

    GridBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.launchpadController = LaunchpadController::LaunchpadX;
    block.startX = 0;
    block.endX = 8;
    block.startY = -1;
    block.endY = 7;
    block.gridSlotIx = 1;
    block.outputFeedback = true;

    GridMappingExpansion out;
    std::string reason;
    REQUIRE_TRUE(ExpandGridBlock(block, out, &reason));
    REQUIRE_TRUE(out.systemMessages.size() == 64);
    REQUIRE_TRUE(out.pressureMappings.size() == 64);

    for (std::size_t ix = 0; ix < 64; ++ix) {
        const int expectedX = static_cast<int>(ix % 8);
        const int expectedY = -1 + static_cast<int>(ix / 8);
        const auto& system = out.systemMessages[ix];
        const auto& pressure = out.pressureMappings[ix];
        REQUIRE_TRUE(system.launchpadPosition == std::optional<synth::LaunchpadGridPosition>(
                                                    synth::LaunchpadGridPosition{
                                                        .controller = LaunchpadController::LaunchpadX,
                                                        .x = expectedX,
                                                        .y = expectedY,
                                                    }));
        REQUIRE_TRUE(system.press.type == MessageIn::Type::GridPress);
        REQUIRE_TRUE(system.press.gridSlotIx == 1);
        REQUIRE_TRUE(system.press.gridX == expectedX);
        REQUIRE_TRUE(system.press.gridY == expectedY);
        REQUIRE_TRUE(system.release.has_value());
        REQUIRE_TRUE(system.release->type == MessageIn::Type::GridRelease);
        REQUIRE_TRUE(system.release->gridSlotIx == 1);
        REQUIRE_TRUE(system.release->gridX == expectedX);
        REQUIRE_TRUE(system.release->gridY == expectedY);
        REQUIRE_TRUE(system.feedback.type == MessageIn::Type::GridPress);
        REQUIRE_TRUE(system.feedback.gridX == expectedX);
        REQUIRE_TRUE(system.feedback.gridY == expectedY);
        REQUIRE_TRUE(system.feedback.velocity == 0);
        REQUIRE_TRUE(system.outputFeedback);

        const auto expectedNote = synth::LaunchpadPositionToNote(
            LaunchpadController::LaunchpadX, expectedX, expectedY);
        REQUIRE_TRUE(expectedNote.has_value());
        REQUIRE_TRUE(pressure.address.channel == 0);
        REQUIRE_TRUE(pressure.address.note == *expectedNote);
        REQUIRE_TRUE(pressure.pressure.type == MessageIn::Type::GridPressureChange);
        REQUIRE_TRUE(pressure.pressure.gridSlotIx == 1);
        REQUIRE_TRUE(pressure.pressure.gridX == expectedX);
        REQUIRE_TRUE(pressure.pressure.gridY == expectedY);
        REQUIRE_TRUE(pressure.pressure.velocity == 0);
    }
}

TEST_CASE(ExpandGridBlockFailureLeavesBothOutputVectorsUntouched) {
    GridMappingExpansion out;
    MidiControllerSystemMessageAssociation sentinelSystem;
    sentinelSystem.press = MessageIn::Clock(91);
    sentinelSystem.feedback = sentinelSystem.press;
    const PolyphonicPressureMapping sentinelPressure{
        .address = {.channel = 9, .note = 99},
        .pressure = MessageIn::GridPressureChange(92, 8, -7, 6, 5),
    };
    out.systemMessages.push_back(sentinelSystem);
    out.pressureMappings.push_back(sentinelPressure);

    GridBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.launchpadController = LaunchpadController::LaunchpadX;
    block.startX = 0;
    block.endX = 10;
    block.startY = -1;
    block.endY = 8;  // includes unsupported x=9 cells

    std::string reason;
    REQUIRE_TRUE(!ExpandGridBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.systemMessages.size() == 1);
    REQUIRE_TRUE(out.pressureMappings.size() == 1);
    REQUIRE_TRUE(MessageInFullyEquivalent(out.systemMessages[0].press, sentinelSystem.press));
    REQUIRE_TRUE(out.pressureMappings[0] == sentinelPressure);
}

TEST_CASE(ExpandGridBlockRejectsImpossibleHugeRangeBeforeAllocating) {
    GridBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.startX = std::numeric_limits<int>::min();
    block.endX = std::numeric_limits<int>::max();
    block.startY = std::numeric_limits<int>::max();
    block.endY = std::numeric_limits<int>::min();
    GridMappingExpansion out;
    std::string reason;
    bool returned = false;
    try {
        returned = !ExpandGridBlock(block, out, &reason);
    } catch (...) {
        returned = false;
    }
    REQUIRE_TRUE(returned);
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.systemMessages.empty());
    REQUIRE_TRUE(out.pressureMappings.empty());
}

TEST_CASE(ExpandGridButtonRejectsPhysicalAddressAlreadyPresentInOutput) {
    GridButton button;
    button.kind = MidiProfileKind::WrldBldr;
    button.channel = 5;
    button.x = 1;
    button.y = 2;
    GridMappingExpansion out;
    REQUIRE_TRUE(ExpandGridButton(button, out));
    const GridMappingExpansion before = out;
    std::string reason;
    REQUIRE_TRUE(!ExpandGridButton(button, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    RequireAssociationsEqual(out.systemMessages, before.systemMessages);
    REQUIRE_TRUE(out.pressureMappings == before.pressureMappings);
}

TEST_CASE(ReconstructGridMappingsRecognizesOnlyExactPairAndPreservesOrphanBytes) {
    GridButton button;
    button.kind = MidiProfileKind::WrldBldr;
    button.channel = 5;
    button.x = 2;
    button.y = 4;
    button.gridSlotIx = 3;
    button.outputFeedback = false;
    GridMappingExpansion expanded;
    REQUIRE_TRUE(ExpandGridButton(button, expanded));

    const PolyphonicPressureMapping orphan{
        .address = {.channel = 13, .note = 117},
        .pressure = MessageIn::GridPressureChange(0x123456789ULL, 9, -12, 34, 87),
    };
    expanded.pressureMappings.push_back(orphan);

    const auto reconstructed = ReconstructGridMappings(
        expanded.systemMessages, expanded.pressureMappings, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(reconstructed.rows.size() == 1);
    REQUIRE_TRUE(!reconstructed.rows[0].isBlock);
    REQUIRE_TRUE(reconstructed.rows[0].button.kind == button.kind);
    REQUIRE_TRUE(reconstructed.rows[0].button.channel == button.channel);
    REQUIRE_TRUE(reconstructed.rows[0].button.x == button.x);
    REQUIRE_TRUE(reconstructed.rows[0].button.y == button.y);
    REQUIRE_TRUE(reconstructed.rows[0].button.gridSlotIx == button.gridSlotIx);
    REQUIRE_TRUE(reconstructed.rows[0].button.outputFeedback == button.outputFeedback);
    REQUIRE_TRUE(reconstructed.remainingSystemMessages.empty());
    REQUIRE_TRUE(reconstructed.orphanPressureMappings.size() == 1);
    REQUIRE_TRUE(reconstructed.orphanPressureMappings[0] == orphan);
}

TEST_CASE(ReconstructGridMappingsCoalescesShuffledExactPairsIntoMaximalRectangle) {
    GridBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.launchpadController = LaunchpadController::LaunchpadProMk3;
    block.startX = -1;
    block.endX = 2;
    block.startY = -1;
    block.endY = 1;
    block.gridSlotIx = 4;
    block.outputFeedback = true;
    GridMappingExpansion expanded;
    REQUIRE_TRUE(ExpandGridBlock(block, expanded));
    std::reverse(expanded.systemMessages.begin(), expanded.systemMessages.end());
    std::rotate(expanded.pressureMappings.begin(), expanded.pressureMappings.begin() + 2,
                expanded.pressureMappings.end());

    const auto reconstructed = ReconstructGridMappings(
        expanded.systemMessages, expanded.pressureMappings, MidiProfileKind::Launchpad);
    REQUIRE_TRUE(reconstructed.rows.size() == 1);
    REQUIRE_TRUE(reconstructed.rows[0].isBlock);
    const GridBlock& actual = reconstructed.rows[0].block;
    REQUIRE_TRUE(actual.kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(actual.launchpadController == LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(actual.startX == -1);
    REQUIRE_TRUE(actual.endX == 2);
    REQUIRE_TRUE(actual.startY == -1);
    REQUIRE_TRUE(actual.endY == 1);
    REQUIRE_TRUE(actual.gridSlotIx == 4);
    REQUIRE_TRUE(actual.outputFeedback);
    REQUIRE_TRUE(reconstructed.rows[0].systemIndices.size() == 6);
    REQUIRE_TRUE(reconstructed.rows[0].pressureIndices.size() == 6);
    REQUIRE_TRUE(reconstructed.remainingSystemMessages.empty());
    REQUIRE_TRUE(reconstructed.orphanPressureMappings.empty());
}

TEST_CASE(ReconstructGridMappingsSplitsBrokenRectangleWithoutInventingMissingCell) {
    GridMappingExpansion input;
    for (const std::pair<int, int> coordinate :
         std::vector<std::pair<int, int>>{{0, -1}, {1, -1}, {2, -1}, {0, 0}, {2, 0}}) {
        GridButton button;
        button.kind = MidiProfileKind::Launchpad;
        button.x = coordinate.first;
        button.y = coordinate.second;
        button.gridSlotIx = 1;
        REQUIRE_TRUE(ExpandGridButton(button, input));
    }

    const auto reconstructed = ReconstructGridMappings(
        input.systemMessages, input.pressureMappings, MidiProfileKind::Launchpad);
    REQUIRE_TRUE(reconstructed.rows.size() == 3);
    std::size_t covered = 0;
    for (const auto& row : reconstructed.rows) {
        covered += row.systemIndices.size();
        GridMappingExpansion roundTrip;
        if (row.isBlock) {
            REQUIRE_TRUE(ExpandGridBlock(row.block, roundTrip));
        } else {
            REQUIRE_TRUE(ExpandGridButton(row.button, roundTrip));
        }
        for (const auto& association : roundTrip.systemMessages) {
            REQUIRE_TRUE(!(association.press.gridX == 1 && association.press.gridY == 0));
        }
    }
    REQUIRE_TRUE(covered == 5);
}

TEST_CASE(ReconstructGridMappingsLeavesDuplicatePhysicalSystemsAndPressureUnconsumed) {
    GridButton button;
    button.kind = MidiProfileKind::WrldBldr;
    button.channel = 5;
    button.x = 7;
    button.y = 6;
    GridMappingExpansion expanded;
    REQUIRE_TRUE(ExpandGridButton(button, expanded));
    expanded.systemMessages.push_back(expanded.systemMessages.front());
    expanded.pressureMappings.push_back(expanded.pressureMappings.front());

    const auto reconstructed = ReconstructGridMappings(
        expanded.systemMessages, expanded.pressureMappings, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(reconstructed.rows.empty());
    REQUIRE_TRUE(reconstructed.remainingSystemMessages.size() == 2);
    REQUIRE_TRUE(reconstructed.orphanPressureMappings == expanded.pressureMappings);
}

TEST_CASE(ReconstructGridMappingsDescendingExpansionRoundTripsCanonicalMeaning) {
    GridBlock descending;
    descending.kind = MidiProfileKind::WrldBldr;
    descending.channel = 5;
    descending.startX = 1;
    descending.endX = 4;
    descending.startY = 3;
    descending.endY = 0;
    descending.gridSlotIx = 2;
    descending.outputFeedback = false;
    GridMappingExpansion input;
    REQUIRE_TRUE(ExpandGridBlock(descending, input));

    const auto reconstructed = ReconstructGridMappings(
        input.systemMessages, input.pressureMappings, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(reconstructed.rows.size() == 1);
    REQUIRE_TRUE(reconstructed.rows[0].isBlock);
    GridMappingExpansion output;
    REQUIRE_TRUE(ExpandGridBlock(reconstructed.rows[0].block, output));

    MidiControllerProfileConfig expected;
    expected.systemMessages = input.systemMessages;
    expected.pressureInput = synth::PolyphonicPressureMidiInConfig{input.pressureMappings};
    NormalizeMidiProfileConfig(expected, MidiProfileKind::WrldBldr);
    MidiControllerProfileConfig actual;
    actual.systemMessages = output.systemMessages;
    actual.pressureInput = synth::PolyphonicPressureMidiInConfig{output.pressureMappings};
    NormalizeMidiProfileConfig(actual, MidiProfileKind::WrldBldr);
    RequireAssociationsEqual(actual.systemMessages, expected.systemMessages);
    REQUIRE_TRUE(actual.pressureInput->mappings == expected.pressureInput->mappings);
}

// --- D3: ExpandEncoderBlock --------------------------------------------

TEST_CASE(ExpandEncoderBlockProducesConsecutiveCcToPositionMapping) {
    EncoderBlock block;
    block.isPush = false;
    block.channel = 2;
    block.startCc = 10;
    block.endCc = 14;  // 4 cells: cc 10,11,12,13
    block.slotIx = 5;
    block.startPosition = 100;

    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(ExpandEncoderBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 4);
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].control.channel == 2);
        REQUIRE_TRUE(out[ix].control.cc == 10 + ix);
        REQUIRE_TRUE(out[ix].slotIx == 5);
        REQUIRE_TRUE(out[ix].position == 100 + ix);
    }
}

TEST_CASE(ExpandEncoderPushBlockPreservesNoteControlType) {
    EncoderBlock pushBlock{.isPush = true,
                           .channel = 1,
                           .startCc = 60,
                           .endCc = 62,
                           .slotIx = 0,
                           .startPosition = 0,
                           .controlType = MidiControlType::Note};

    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(ExpandEncoderBlock(pushBlock, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    REQUIRE_TRUE(out[0].control.type == MidiControlType::Note);
    REQUIRE_TRUE(out[1].control.type == MidiControlType::Note);
}

TEST_CASE(ExpandEncoderTurnBlockRejectsNoteControlTypeWithoutMutatingOutput) {
    EncoderBlock turnBlock{.isPush = false,
                           .channel = 1,
                           .startCc = 60,
                           .endCc = 62,
                           .slotIx = 0,
                           .startPosition = 0,
                           .controlType = MidiControlType::Note};
    std::vector<EncoderMidiMapping> out = {
        {.control = {.channel = 9, .cc = 9}, .slotIx = 9, .position = 9},
    };
    std::string reason;
    REQUIRE_TRUE(!ExpandEncoderBlock(turnBlock, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.size() == 1);
    REQUIRE_TRUE((out[0].control == MidiControlAddress{.channel = 9, .cc = 9}));
    REQUIRE_TRUE(out[0].slotIx == 9);
    REQUIRE_TRUE(out[0].position == 9);
}

TEST_CASE(ExpandEncoderBlockRejectsInvertedRange) {
    EncoderBlock block;
    block.startCc = 10;
    block.endCc = 5;  // invalid: end <= start
    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandEncoderBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandEncoderBlockRejectsCcOutsideMidiDomain) {
    EncoderBlock block;
    block.startCc = 125;
    block.endCc = 200;  // last cell cc=199 exceeds the 0-127 MIDI CC domain
    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandEncoderBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ExpandEncoderBlockRejectsStartPositionNearSizeMaxThatWouldWrap) {
    // Finding 3: startPosition + index can wrap SIZE_MAX -> 0. Expansion
    // must refuse a block whose startPosition + cellCount would exceed a
    // sane domain cap rather than silently wrapping.
    EncoderBlock block;
    block.channel = 0;
    block.startCc = 0;
    block.endCc = 4;  // 4 cells
    block.slotIx = 0;
    block.startPosition = std::numeric_limits<std::size_t>::max() - 1;  // + 4 would wrap

    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandEncoderBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandEncoderBlockRejectsChannelAbove15) {
    // D3: expansion mirrors the edit rules (ApplyMappingEdit refuses channel
    // outside 0-15) -- all-or-nothing, with a reason.
    EncoderBlock block;
    block.channel = 16;  // one past the valid 0-15 MIDI channel domain
    block.startCc = 0;
    block.endCc = 4;
    block.slotIx = 0;
    block.startPosition = 0;
    std::vector<EncoderMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandEncoderBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

// --- D3: ExpandAnalogBlock -----------------------------------------------

TEST_CASE(ExpandAnalogBlockProducesConsecutiveCcToGestureMapping) {
    AnalogBlock block;
    block.channel = 3;
    block.startCc = 0;
    block.endCc = 3;
    block.startGestureIx = 7;

    std::vector<AnalogMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(ExpandAnalogBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 3);
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].control.channel == 3);
        REQUIRE_TRUE(out[ix].control.cc == ix);
        REQUIRE_TRUE(out[ix].gestureIx == 7 + ix);
    }
}

TEST_CASE(ExpandAnalogBlockRejectsSingleCellRangeIsStillValidButEmptyIsNot) {
    AnalogBlock block;
    block.startCc = 5;
    block.endCc = 5;  // empty range
    std::vector<AnalogMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandAnalogBlock(block, out, &reason));
}

TEST_CASE(ExpandAnalogBlockRejectsStartGestureIxNearSizeMaxThatWouldWrap) {
    AnalogBlock block;
    block.channel = 0;
    block.startCc = 0;
    block.endCc = 4;  // 4 cells
    block.startGestureIx = std::numeric_limits<std::size_t>::max() - 1;  // + 4 would wrap

    std::vector<AnalogMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandAnalogBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandAnalogBlockRejectsChannelAbove15) {
    AnalogBlock block;
    block.channel = 200;
    block.startCc = 0;
    block.endCc = 3;
    block.startGestureIx = 0;
    std::vector<AnalogMidiMapping> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandAnalogBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

// --- D3: ExpandSystemBlock: generic (1-D cc run) --------------------------

TEST_CASE(ExpandSystemBlockGenericSceneSelectProducesCcRunWithFeedbackEqualsPress) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 2;
    block.channel = 4;
    block.startCc = 10;
    block.endCc = 13;  // 3 cells
    block.outputFeedback = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 3);
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].control.has_value());
        REQUIRE_TRUE(out[ix].control->channel == 4);
        REQUIRE_TRUE(out[ix].control->cc == 10 + ix);
        REQUIRE_TRUE(out[ix].press.type == MessageIn::Type::SceneSelect);
        REQUIRE_TRUE(out[ix].press.sceneIx == 2 + ix);
        REQUIRE_TRUE(!out[ix].release.has_value());
        // feedback == press (required field, matching every default factory).
        REQUIRE_TRUE(out[ix].feedback.type == MessageIn::Type::SceneSelect);
        REQUIRE_TRUE(out[ix].feedback.sceneIx == 2 + ix);
        REQUIRE_TRUE(out[ix].outputFeedback == true);
    }
}

TEST_CASE(ExpandSystemBlockGenericPreservesNoteControlType) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 0;
    block.channel = 1;
    block.startCc = 60;
    block.endCc = 62;
    block.controlType = MidiControlType::Note;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    REQUIRE_TRUE(out[0].control->type == MidiControlType::Note);
    REQUIRE_TRUE(out[1].control->type == MidiControlType::Note);
}

TEST_CASE(ExpandSystemBlockGenericBankSelectCarriesBankSlotIx) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::BankSelect;
    block.startArg = 0;
    block.bankSlotIx = 7;
    block.channel = 1;
    block.startCc = 0;
    block.endCc = 2;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].press.type == MessageIn::Type::SelectParamBank);
        REQUIRE_TRUE(out[ix].press.slotIx == 7);
        REQUIRE_TRUE(out[ix].press.bankIx == ix);
    }
}

TEST_CASE(ExpandSystemBlockGenericGestureSelectHasPairedRelease) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::GestureSelect;
    block.startArg = 3;
    block.channel = 1;
    block.startCc = 0;
    block.endCc = 2;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].press.type == MessageIn::Type::SetGestureSelect);
        REQUIRE_TRUE(out[ix].press.gestureIx == 3 + ix);
        REQUIRE_TRUE(out[ix].press.boolValue == true);
        REQUIRE_TRUE(out[ix].release.has_value());
        REQUIRE_TRUE(out[ix].release->type == MessageIn::Type::SetGestureSelect);
        REQUIRE_TRUE(out[ix].release->gestureIx == 3 + ix);
        REQUIRE_TRUE(out[ix].release->boolValue == false);
    }
}

// --- D3: ExpandSystemBlock: wrldbldr (2-D rectangle, row-major/col-major) --

TEST_CASE(ExpandSystemBlockWrldBldrRowMajorTraversesXAscendingWithinRow) {
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 0;
    block.channel = 5;
    block.startX = 0;
    block.startY = 0;
    block.endX = 2;
    block.endY = 2;  // 2x2 rectangle: (0,0)(1,0)(0,1)(1,1) -- exclusive ends
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 4);
    // Row-major, x ascending within a row, y stepping start->end.
    const std::vector<std::pair<int, int>> expectedXY = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].wrldBldrPosition.has_value());
        REQUIRE_TRUE(out[ix].wrldBldrPosition->x == expectedXY[ix].first);
        REQUIRE_TRUE(out[ix].wrldBldrPosition->y == expectedXY[ix].second);
        REQUIRE_TRUE(out[ix].press.sceneIx == ix);
        // Block channel is authoritative; control->cc derives from position.
        REQUIRE_TRUE(out[ix].control.has_value());
        REQUIRE_TRUE(out[ix].control->channel == 5);
        REQUIRE_TRUE(out[ix].control->cc ==
                     WrldBldrPositionToCC(static_cast<std::uint8_t>(expectedXY[ix].first),
                                          static_cast<std::uint8_t>(expectedXY[ix].second)));
        REQUIRE_TRUE(out[ix].wrldBldrPosition->channel == 5);
    }
}

TEST_CASE(ExpandSystemBlockWrldBldrColumnMajorTraversesYWithinColumn) {
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 0;
    block.channel = 5;
    block.startX = 0;
    block.startY = 0;
    block.endX = 2;
    block.endY = 2;
    block.rowMajor = false;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 4);
    const std::vector<std::pair<int, int>> expectedXY = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};
    for (std::size_t ix = 0; ix < out.size(); ++ix) {
        REQUIRE_TRUE(out[ix].wrldBldrPosition->x == expectedXY[ix].first);
        REQUIRE_TRUE(out[ix].wrldBldrPosition->y == expectedXY[ix].second);
    }
}

TEST_CASE(ExpandSystemBlockWrldBldrDescendingRowTraversesYDownward) {
    // The default WRLD.Bldr bank grid: banks 0..7 at y=3, banks 8..15 at
    // y=2 -- exclusive ends: endX = 8 (one past max x=7), endY = 1 (one past
    // the descending traversal's last row y=2, stepping -1), so
    // traversal covers y in {3,2}.
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::BankSelect;
    block.startArg = 0;
    block.bankSlotIx = 0;
    block.channel = 5;
    block.startX = 0;
    block.startY = 3;
    block.endX = 8;
    block.endY = 1;
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 16);
    for (std::size_t x = 0; x < 8; ++x) {
        REQUIRE_TRUE(out[x].wrldBldrPosition->x == x);
        REQUIRE_TRUE(out[x].wrldBldrPosition->y == 3);
        REQUIRE_TRUE(out[x].press.bankIx == x);
    }
    for (std::size_t x = 0; x < 8; ++x) {
        REQUIRE_TRUE(out[8 + x].wrldBldrPosition->x == x);
        REQUIRE_TRUE(out[8 + x].wrldBldrPosition->y == 2);
        REQUIRE_TRUE(out[8 + x].press.bankIx == 8 + x);
    }
}

TEST_CASE(ExpandSystemBlockLaunchpadHasNoChannelField) {
    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 0;
    block.startX = 0;
    block.startY = 0;
    block.endX = 2;
    block.endY = 1;
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    for (const auto& assoc : out) {
        REQUIRE_TRUE(assoc.launchpadPosition.has_value());
        REQUIRE_TRUE(!assoc.control.has_value());
        REQUIRE_TRUE(!assoc.wrldBldrPosition.has_value());
    }
}

TEST_CASE(ExpandSystemBlockLaunchpadStampsCellsWithTheBlocksControllerVariant) {
    // Finding 4: SystemBlock::launchpadController selects which variant
    // every expanded cell's launchpadPosition.controller is stamped with
    // (and which variant's shape validates the coordinates) -- not a
    // hardcoded LaunchpadX regardless of the block's actual variant.
    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.launchpadController = LaunchpadController::LaunchpadMiniMk3;
    block.startArg = 0;
    block.startX = 0;
    block.startY = 0;
    block.endX = 2;
    block.endY = 1;
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    for (const auto& assoc : out) {
        REQUIRE_TRUE(assoc.launchpadPosition->controller == LaunchpadController::LaunchpadMiniMk3);
    }
}

TEST_CASE(ExpandSystemBlockLaunchpadProMk3AcceptsEdgeCoordinatesLaunchpadXRejects) {
    // ProMk3's shape is a strict superset of LaunchpadX/MiniMk3's -- e.g.
    // x=8 at y=-1 is valid on ProMk3 (LaunchpadShapeSupports: x in -1..8)
    // but outside LaunchpadX's x in 0..8 range combined with... actually
    // LaunchpadX supports x=8 too; use y=-1,x=-1 (ProMk3-only: x >= -1)
    // which LaunchpadX (x >= 0) rejects outright, proving the block's own
    // variant -- not a hardcoded LaunchpadX -- gates validation.
    REQUIRE_TRUE(LaunchpadShapeSupports(LaunchpadController::LaunchpadProMk3, -1, -1));
    REQUIRE_TRUE(!LaunchpadShapeSupports(LaunchpadController::LaunchpadX, -1, -1));

    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.launchpadController = LaunchpadController::LaunchpadProMk3;
    block.startArg = 0;
    block.startX = -1;
    block.startY = -1;
    block.endX = 1;   // exclusive: one past x=0
    block.endY = 0;    // exclusive: single row, d=+1, one past y=-1
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(out.size() == 2);
    REQUIRE_TRUE(out[0].launchpadPosition->x == -1 && out[0].launchpadPosition->y == -1);
    REQUIRE_TRUE(out[0].launchpadPosition->controller == LaunchpadController::LaunchpadProMk3);
}

TEST_CASE(ExpandSystemBlockLaunchpadXRejectsProMk3OnlyCoordinates) {
    // The same rectangle, but with the default LaunchpadX variant -- must
    // be rejected since x=-1 is outside LaunchpadX's shape.
    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.launchpadController = LaunchpadController::LaunchpadX;
    block.startArg = 0;
    block.startX = -1;
    block.startY = -1;
    block.endX = 1;
    block.endY = 0;
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ReconstructOfExpandedLaunchpadProMk3EdgeBlockRoundTrips) {
    // sru-10 round-trip property, specifically for the ProMk3 edge
    // coordinates called out in finding 4: (y=-1, x=8) must round-trip
    // through a ProMk3 block, both as a coordinate a non-ProMk3 block could
    // never validly reach and as a reconstruction identity check.
    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.launchpadController = LaunchpadController::LaunchpadProMk3;
    block.startArg = 0;
    block.startX = 7;
    block.startY = -1;
    block.endX = 9;    // exclusive: one past x=8; 2 cells: (7,-1) and (8,-1)
    block.endY = 0;     // exclusive: single row, d=+1, one past y=-1
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, expanded, &reason));
    REQUIRE_TRUE(expanded.size() == 2);
    REQUIRE_TRUE(expanded[1].launchpadPosition->x == 8 && expanded[1].launchpadPosition->y == -1);
    REQUIRE_TRUE(expanded[1].launchpadPosition->controller == LaunchpadController::LaunchpadProMk3);

    const auto rows = ReconstructSystemBlocks(expanded, MidiProfileKind::Launchpad);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.launchpadController == LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(rows[0].block.startX == 7 && rows[0].block.endX == 9);
    REQUIRE_TRUE(rows[0].block.startY == -1 && rows[0].block.endY == 0);
}

TEST_CASE(ReconstructSystemBlocksMixedLaunchpadVariantsStayIndividual) {
    // A run whose cells carry different LaunchpadController variants at
    // otherwise-consecutive positions must NOT reconstruct as one block --
    // ReconstructSystemBlocks requires a consistent variant across the run.
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::uint8_t x = 0; x < 4; ++x) {
        MidiControllerSystemMessageAssociation a;
        a.launchpadPosition = synth::LaunchpadGridPosition{
            .controller = (x == 2) ? LaunchpadController::LaunchpadMiniMk3 : LaunchpadController::LaunchpadX,
            .x = x,
            .y = 0};
        a.press = MessageIn::SceneSelect(0, x);
        a.feedback = a.press;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Launchpad);
    // No single block may span all 4 cells (the variant changes at x=2).
    for (const auto& row : rows) {
        if (row.isBlock) {
            REQUIRE_TRUE(row.indices.size() < 4);
        }
    }
}

TEST_CASE(ExpandSystemBlockTwisterAlwaysRejected) {
    // Twister never blocks (D4 point 3) -- ExpandSystemBlock refuses a
    // twister-kind block outright rather than producing cells nothing can
    // reconstruct.
    SystemBlock block;
    block.kind = MidiProfileKind::MfTwister;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 0;
    block.startCc = 0;
    block.endCc = 2;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ExpandSystemBlockRejectsSingleCellRectangle) {
    // D4's >= 2 threshold applies at reconstruction; ExpandSystemBlock
    // itself does not forbid a 1-cell block being expanded on its own (a
    // block struct with only 1 cell is a valid, if degenerate, expansion
    // target) -- but an inverted/empty range must still fail.
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.startCc = 5;
    block.endCc = 5;  // empty
    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
}

TEST_CASE(ExpandSystemBlockWrldBldrRejectsOutOfGridCoordinates) {
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::SceneSelect;
    block.channel = 5;
    block.startX = 0;
    block.startY = 0;
    block.endX = 9;  // exclusive: max x visited = 8, out of the 0-7 grid
    block.endY = 1;   // exclusive: single row, d=+1
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ExpandSystemBlockWrldBldrRejectsChannelAbove15) {
    // D3: expansion mirrors the edit rules -- channel > 15 is refused
    // all-or-nothing, with a reason, even when every coordinate is valid.
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::SceneSelect;
    block.channel = 16;
    block.startX = 0;
    block.startY = 0;
    block.endX = 2;
    block.endY = 1;
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandSystemBlockRejectsStartArgNearSizeMaxThatWouldWrap) {
    // Finding 3: startArg + cellIndex can wrap SIZE_MAX -> 0.
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.channel = 0;
    block.startCc = 0;
    block.endCc = 4;  // 4 cells
    block.startArg = std::numeric_limits<std::size_t>::max() - 1;  // + 4 would wrap

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandSystemBlockGenericRejectsChannelAbove15) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.channel = 255;
    block.startCc = 0;
    block.endCc = 3;

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.empty());
}

TEST_CASE(ExpandSystemBlockLaunchpadRejectsShapeUnsupportedCoordinates) {
    SystemBlock block;
    block.kind = MidiProfileKind::Launchpad;
    block.message = BlockableMessage::SceneSelect;
    block.startX = 0;
    block.startY = 0;
    block.endX = 20;  // far outside any launchpad shape
    block.endY = 1;    // exclusive: single row, d=+1

    std::vector<MidiControllerSystemMessageAssociation> out;
    std::string reason;
    REQUIRE_TRUE(!ExpandSystemBlock(block, out, &reason));
}

TEST_CASE(SystemBlockCellCountMatchesGenericRun) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.startCc = 3;
    block.endCc = 9;
    REQUIRE_TRUE(block.CellCount() == 6);
}

TEST_CASE(SystemBlockCellCountMatchesRectangle) {
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.startX = 0;
    block.endX = 8;   // exclusive: one past max x=7
    block.startY = 3;
    block.endY = 1;    // exclusive: descending, one past y=2
    REQUIRE_TRUE(block.CellCount() == 16);  // 8 wide x 2 tall
}

TEST_CASE(SystemBlockEndXEndYAreExclusive) {
    // A 2-column, 1-row block: endX = startX + 2 (exclusive, one past the
    // last column), endY = startY + 1 (exclusive, single row, d=+1).
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.startX = 3;
    block.endX = 5;
    block.startY = 0;
    block.endY = 1;
    REQUIRE_TRUE(block.CellCount() == 2);
    REQUIRE_TRUE(block.endX == block.startX + 2);
    REQUIRE_TRUE(block.endY == block.startY + 1);
}

// --- D4: ReconstructEncoderBlocks -----------------------------------------

TEST_CASE(ReconstructEncoderBlocksMergesSixteenConsecutiveIntoOneBlock) {
    std::vector<EncoderMidiMapping> mappings;
    for (std::size_t ix = 0; ix < 16; ++ix) {
        mappings.push_back({.control = {.channel = 0, .cc = static_cast<std::uint8_t>(ix)}, .slotIx = 0, .position = ix});
    }
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.channel == 0);
    REQUIRE_TRUE(rows[0].block.startCc == 0);
    REQUIRE_TRUE(rows[0].block.endCc == 16);
    REQUIRE_TRUE(rows[0].block.slotIx == 0);
    REQUIRE_TRUE(rows[0].block.startPosition == 0);
    REQUIRE_TRUE(rows[0].indices.size() == 16);
}

TEST_CASE(ReconstructEncoderBlocksSplitsBrokenRunAtOutlier) {
    std::vector<EncoderMidiMapping> mappings;
    for (std::size_t ix = 0; ix < 5; ++ix) {
        mappings.push_back({.control = {.channel = 0, .cc = static_cast<std::uint8_t>(ix)}, .slotIx = 0, .position = ix});
    }
    // Outlier: position jumps (breaks consecutiveness) at index 5.
    mappings.push_back({.control = {.channel = 0, .cc = 5}, .slotIx = 0, .position = 99});
    for (std::size_t ix = 6; ix < 10; ++ix) {
        mappings.push_back({.control = {.channel = 0, .cc = static_cast<std::uint8_t>(ix)}, .slotIx = 0, .position = ix - 1});
    }

    const auto rows = ReconstructEncoderBlocks(mappings, false);
    // Expect: block covering indices 0-4 (len 5 >= 2), individual row for
    // index 5 (the outlier), block covering indices 6-9 (len 4 >= 2).
    REQUIRE_TRUE(rows.size() == 3);
    REQUIRE_TRUE(rows[0].isBlock && rows[0].indices.size() == 5);
    REQUIRE_TRUE(!rows[1].isBlock && rows[1].indices.size() == 1 && rows[1].indices[0] == 5);
    REQUIRE_TRUE(rows[2].isBlock && rows[2].indices.size() == 4);
}

TEST_CASE(ReconstructEncoderBlocksSingleMappingStaysIndividual) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0},
    };
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(!rows[0].isBlock);
}

TEST_CASE(ReconstructEncoderBlocksRequiresConstantSlotAndChannel) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0},
        {.control = {.channel = 0, .cc = 1}, .slotIx = 1, .position = 1},  // slot changes -> breaks run
    };
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(!rows[0].isBlock);
    REQUIRE_TRUE(!rows[1].isBlock);
}

TEST_CASE(ReconstructEncoderBlocksRequiresConstantControlType) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 1, .cc = 60, .type = MidiControlType::Cc}, .slotIx = 0, .position = 0},
        {.control = {.channel = 1, .cc = 61, .type = MidiControlType::Note}, .slotIx = 0, .position = 1},
    };
    const auto rows = ReconstructEncoderBlocks(mappings, true);
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(!rows[0].isBlock);
    REQUIRE_TRUE(!rows[1].isBlock);
}

TEST_CASE(ReconstructedEncoderNoteBlockExpandsBackToNotes) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 1, .cc = 60, .type = MidiControlType::Note}, .slotIx = 0, .position = 0},
        {.control = {.channel = 1, .cc = 61, .type = MidiControlType::Note}, .slotIx = 0, .position = 1},
    };
    const auto rows = ReconstructEncoderBlocks(mappings, true);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.controlType == MidiControlType::Note);

    std::vector<EncoderMidiMapping> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandEncoderBlock(rows[0].block, expanded, &reason));
    REQUIRE_TRUE(expanded.size() == 2);
    REQUIRE_TRUE(expanded[0].control.type == MidiControlType::Note);
    REQUIRE_TRUE(expanded[1].control.type == MidiControlType::Note);
}

TEST_CASE(ReconstructEncoderBlocksRequiresConstantCcOffset) {
    // Positions consecutive, but cc jumps by 2 instead of 1 -- not a
    // constant-offset run.
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0},
        {.control = {.channel = 0, .cc = 2}, .slotIx = 0, .position = 1},
        {.control = {.channel = 0, .cc = 4}, .slotIx = 0, .position = 2},
    };
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.size() == 3);
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

TEST_CASE(ReconstructEncoderBlocksTreatsPositionWrapAsRunBreakNotMatch) {
    // Finding 3: `prev.position + 1` computed in unsigned arithmetic wraps
    // SIZE_MAX -> 0. Without an explicit check, a cell at position SIZE_MAX
    // followed by a cell at position 0 would look "consecutive" and merge
    // into a block -- reconstruction must instead treat this as a run
    // break, never a match.
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0},
         .slotIx = 0,
         .position = std::numeric_limits<std::size_t>::max()},
        {.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 0},  // would "wrap-match" prev+1
    };
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.size() == 2);
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

// --- D4: ReconstructAnalogBlocks ------------------------------------------

TEST_CASE(ReconstructAnalogBlocksMergesConsecutiveGestures) {
    std::vector<AnalogMidiMapping> mappings;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        mappings.push_back({.control = {.channel = 2, .cc = static_cast<std::uint8_t>(ix)}, .gestureIx = ix});
    }
    const auto rows = ReconstructAnalogBlocks(mappings);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.startGestureIx == 0);
    REQUIRE_TRUE(rows[0].block.startCc == 0);
    REQUIRE_TRUE(rows[0].block.endCc == 4);
}

TEST_CASE(ReconstructAnalogBlocksTreatsGestureIxWrapAsRunBreakNotMatch) {
    std::vector<AnalogMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .gestureIx = std::numeric_limits<std::size_t>::max()},
        {.control = {.channel = 0, .cc = 1}, .gestureIx = 0},  // would "wrap-match" prev+1
    };
    const auto rows = ReconstructAnalogBlocks(mappings);
    REQUIRE_TRUE(rows.size() == 2);
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

// --- D4: ReconstructSystemBlocks: generic (1-D) ---------------------------

TEST_CASE(ReconstructSystemBlocksGenericSceneSelectRun) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(10 + ix)};
        a.press = MessageIn::SceneSelect(0, ix);
        a.feedback = a.press;
        a.outputFeedback = true;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.message == BlockableMessage::SceneSelect);
    REQUIRE_TRUE(rows[0].block.startArg == 0);
    REQUIRE_TRUE(rows[0].block.startCc == 10);
    REQUIRE_TRUE(rows[0].block.endCc == 14);
}

TEST_CASE(ReconstructSystemBlocksRequiresConstantControlType) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 2; ++ix) {
        MidiControllerSystemMessageAssociation association;
        association.control = MidiControlAddress{.channel = 1,
                                                  .cc = static_cast<std::uint8_t>(60 + ix),
                                                  .type = ix == 0 ? MidiControlType::Cc : MidiControlType::Note};
        association.press = MessageIn::SceneSelect(0, ix);
        association.feedback = association.press;
        associations.push_back(association);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(!rows[0].isBlock);
    REQUIRE_TRUE(!rows[1].isBlock);
}

TEST_CASE(ReconstructedSystemNoteBlockExpandsBackToNotes) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 2; ++ix) {
        MidiControllerSystemMessageAssociation association;
        association.control = MidiControlAddress{.channel = 1,
                                                  .cc = static_cast<std::uint8_t>(60 + ix),
                                                  .type = MidiControlType::Note};
        association.press = MessageIn::SceneSelect(0, ix);
        association.feedback = association.press;
        associations.push_back(association);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.controlType == MidiControlType::Note);

    std::vector<MidiControllerSystemMessageAssociation> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(rows[0].block, expanded, &reason));
    REQUIRE_TRUE(expanded.size() == 2);
    REQUIRE_TRUE(expanded[0].control->type == MidiControlType::Note);
    REQUIRE_TRUE(expanded[1].control->type == MidiControlType::Note);
}

TEST_CASE(ReconstructSystemBlocksNonBlockableStayIndividual) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    MidiControllerSystemMessageAssociation reset;
    reset.control = MidiControlAddress{.channel = 0, .cc = 0};
    reset.press = MessageIn::SetReset(0, true);
    reset.release = MessageIn::SetReset(0, false);
    reset.feedback = reset.press;
    associations.push_back(reset);

    MidiControllerSystemMessageAssociation random;
    random.control = MidiControlAddress{.channel = 0, .cc = 1};
    random.press = MessageIn::SetRandom(0, true);
    random.release = MessageIn::SetRandom(0, false);
    random.feedback = random.press;
    associations.push_back(random);

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(!rows[0].isBlock);
    REQUIRE_TRUE(!rows[1].isBlock);
}

TEST_CASE(ReconstructSystemBlocksTwisterNeverBlocks) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 3, .cc = static_cast<std::uint8_t>(8 + ix)};
        a.press = MessageIn::SceneSelect(0, ix);
        a.feedback = a.press;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::MfTwister);
    REQUIRE_TRUE(rows.size() == 4);
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

TEST_CASE(ReconstructSystemBlocksUnsortedInputStillReconstructs) {
    // sru-9 "unsorted input still reconstructs" scenario: a bank-selector
    // run stored out of order reconstructs the same block as the sorted
    // equivalent.
    std::vector<MidiControllerSystemMessageAssociation> sorted;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SelectParamBank(0, 0, ix);
        a.feedback = a.press;
        sorted.push_back(a);
    }
    std::vector<MidiControllerSystemMessageAssociation> shuffled = {sorted[2], sorted[0], sorted[3], sorted[1]};

    const auto sortedRows = ReconstructSystemBlocks(sorted, MidiProfileKind::Generic);
    const auto shuffledRows = ReconstructSystemBlocks(shuffled, MidiProfileKind::Generic);
    REQUIRE_TRUE(sortedRows.size() == 1 && sortedRows[0].isBlock);
    REQUIRE_TRUE(shuffledRows.size() == 1 && shuffledRows[0].isBlock);
    REQUIRE_TRUE(shuffledRows[0].block.startArg == sortedRows[0].block.startArg);
    REQUIRE_TRUE(shuffledRows[0].block.startCc == sortedRows[0].block.startCc);
    REQUIRE_TRUE(shuffledRows[0].block.endCc == sortedRows[0].block.endCc);
}

// --- D4: ReconstructSystemBlocks: rectangles (wrldbldr) -------------------

std::vector<MidiControllerSystemMessageAssociation> MakeWrldBldrSceneRun(std::uint8_t startX, std::uint8_t endX,
                                                                          std::uint8_t y, std::size_t startArg) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::uint8_t x = startX; x <= endX; ++x) {
        MidiControllerSystemMessageAssociation a;
        a.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = x, .y = y};
        a.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(x, y)};
        a.press = MessageIn::SceneSelect(0, startArg + (x - startX));
        a.feedback = a.press;
        associations.push_back(a);
    }
    return associations;
}

TEST_CASE(ReconstructSystemBlocksWrldBldrOneRowNByOne) {
    const auto associations = MakeWrldBldrSceneRun(0, 7, 6, 0);  // 8 wide, 1 tall
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.startX == 0 && rows[0].block.endX == 8);
    REQUIRE_TRUE(rows[0].block.startY == 6 && rows[0].block.endY == 7);
}

TEST_CASE(ReconstructSystemBlocksWrldBldrBankRectangleDescendingRows) {
    // The default WRLD.Bldr profile's bank grid: banks 0..7 at y=3, banks
    // 8..15 at y=2 -- an 8x2 rectangle, descending (sru-10 scenario):
    // exclusive endX=8 (one past max x=7), exclusive endY=1 (one past y=2,
    // stepping -1 from startY=3).
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::uint8_t x = 0; x < 8; ++x) {
        MidiControllerSystemMessageAssociation a;
        a.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = x, .y = 3};
        a.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(x, 3)};
        a.press = MessageIn::SelectParamBank(0, 0, x);
        a.feedback = a.press;
        associations.push_back(a);
    }
    for (std::uint8_t x = 0; x < 8; ++x) {
        MidiControllerSystemMessageAssociation a;
        a.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = x, .y = 2};
        a.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(x, 2)};
        a.press = MessageIn::SelectParamBank(0, 0, 8 + x);
        a.feedback = a.press;
        associations.push_back(a);
    }

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.message == BlockableMessage::BankSelect);
    REQUIRE_TRUE(rows[0].block.startX == 0 && rows[0].block.endX == 8);
    REQUIRE_TRUE(rows[0].block.startY == 3 && rows[0].block.endY == 1);
    REQUIRE_TRUE(rows[0].block.startArg == 0);
    REQUIRE_TRUE(rows[0].indices.size() == 16);
}

TEST_CASE(ReconstructSystemBlocksWrldBldrOneColumnOneByN) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::uint8_t y = 0; y < 3; ++y) {
        MidiControllerSystemMessageAssociation a;
        a.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = 2, .y = y};
        a.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(2, y)};
        a.press = MessageIn::SceneSelect(0, y);
        a.feedback = a.press;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.startX == 2 && rows[0].block.endX == 3);
    REQUIRE_TRUE(rows[0].block.startY == 0 && rows[0].block.endY == 3);
}

TEST_CASE(ReconstructSystemBlocksWrldBldrRaggedRemainderSplitsIntoTwoRowBlocks) {
    // A 2-row run where the second physical row is wider than the first --
    // D4's greedy fit uses the FIRST row's width and only extends height
    // while later rows repeat that EXACT x-range, so this reconstructs as
    // two separate row blocks (3x1 then 6x1), not one ragged rectangle.
    std::vector<MidiControllerSystemMessageAssociation> associations = MakeWrldBldrSceneRun(0, 2, 0, 0);  // row0: x0-2, arg0-2
    auto row1 = MakeWrldBldrSceneRun(0, 5, 1, 3);  // row1: x0-5, arg3-8 (wider)
    associations.insert(associations.end(), row1.begin(), row1.end());

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);
    std::size_t blockCells = 0;
    std::size_t individualCount = 0;
    for (const auto& row : rows) {
        if (row.isBlock) {
            blockCells += row.indices.size();
        } else {
            ++individualCount;
        }
    }
    // Every input cell is covered exactly once, block or individual.
    REQUIRE_TRUE(blockCells + individualCount == associations.size());

    bool foundRow0Block = false;
    bool foundRow1Block = false;
    for (const auto& row : rows) {
        if (!row.isBlock) {
            continue;
        }
        if (row.block.startX == 0 && row.block.endX == 3 && row.block.startY == 0 && row.block.endY == 1) {
            foundRow0Block = true;
        }
        if (row.block.startX == 0 && row.block.endX == 6 && row.block.startY == 1 && row.block.endY == 2) {
            foundRow1Block = true;
        }
    }
    REQUIRE_TRUE(foundRow0Block);
    REQUIRE_TRUE(foundRow1Block);
}

TEST_CASE(ReconstructSystemBlocksColumnMajorAuthoredReconstructsAsOneBlockPerColumn) {
    // D4's explicit non-goal: a column-major-authored 2x2 block (args
    // increasing down each column, then to the next column) does NOT
    // reconstruct as one 2x2 block under this row-major reconstruction --
    // it reconstructs as one block per column, since row-major traversal
    // sees column 0's two cells as args {0,1} (consecutive, forms a run) but
    // column 1 continues with args {2,3} at the SAME two y's -- the
    // candidate run itself (arg consecutive) spans all 4 cells, but the
    // physical-row grouping (by y) sees two 1-wide "rows" per y (x0 has arg
    // 0 and x1 has arg 2 at y=0 -- NOT x-consecutive with each other), so
    // FitRectangles's row split naturally isolates each column.
    std::vector<MidiControllerSystemMessageAssociation> associations;
    auto addCell = [&](std::uint8_t x, std::uint8_t y, std::size_t arg) {
        MidiControllerSystemMessageAssociation a;
        a.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = x, .y = y};
        a.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(x, y)};
        a.press = MessageIn::SceneSelect(0, arg);
        a.feedback = a.press;
        associations.push_back(a);
    };
    // Column-major authoring: column x=0 gets args 0,1 (y=0,1); column x=1
    // gets args 2,3 (y=0,1).
    addCell(0, 0, 0);
    addCell(0, 1, 1);
    addCell(1, 0, 2);
    addCell(1, 1, 3);

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);
    // Flatten and confirm the round-trip property still holds even though
    // the grouping differs from the authored column-major shape.
    MidiControllerProfileConfig sortedConfig;
    sortedConfig.systemMessages = associations;
    NormalizeMidiProfileConfig(sortedConfig, MidiProfileKind::WrldBldr);

    std::size_t totalCells = 0;
    for (const auto& row : rows) {
        totalCells += row.indices.size();
    }
    REQUIRE_TRUE(totalCells == associations.size());
}

// --- D4: default WRLD.Bldr profile round-trip -----------------------------

TEST_CASE(RoundTripDefaultWrldBldrProfileEncodersAndSystemMessages) {
    const auto config = synth::WrldBldrDefaultProfileConfig();

    // Encoders: 16 turns + 16 pushes, each contiguous -> one block apiece.
    const auto turnRows = ReconstructEncoderBlocks(config.encoderInput->turns, false);
    REQUIRE_TRUE(turnRows.size() == 1 && turnRows[0].isBlock);
    const auto pushRows = ReconstructEncoderBlocks(config.encoderInput->pushes, true);
    REQUIRE_TRUE(pushRows.size() == 1 && pushRows[0].isBlock);

    // System messages: reconstruct then expand every row and confirm the
    // flattened result equals the sorted config exactly (D4 round-trip
    // property) -- covers reset/random/random-mod (individual), scene
    // selectors (block), and bank selectors (the descending 8x2 block) all
    // in one pass over the real default factory.
    MidiControllerProfileConfig sortedConfig;
    sortedConfig.systemMessages = config.systemMessages;
    NormalizeMidiProfileConfig(sortedConfig, MidiProfileKind::WrldBldr);

    const auto systemRows = ReconstructSystemBlocks(config.systemMessages, MidiProfileKind::WrldBldr);
    std::vector<MidiControllerSystemMessageAssociation> flattened;
    for (const auto& row : systemRows) {
        if (row.isBlock) {
            std::vector<MidiControllerSystemMessageAssociation> expanded;
            std::string reason;
            REQUIRE_TRUE(ExpandSystemBlock(row.block, expanded, &reason));
            for (auto& e : expanded) {
                flattened.push_back(e);
            }
        } else {
            for (std::size_t ix : row.indices) {
                flattened.push_back(sortedConfig.systemMessages[ix]);
            }
        }
    }
    // Finding 6: full structural equality, not just press.type + control --
    // catches address-variant, release, feedback, or outputFeedback drift
    // that a press.type-only comparison would miss.
    RequireAssociationsEqual(flattened, sortedConfig.systemMessages);

    // Explicitly confirm the descending 8x2 bank block (sru-10 scenario):
    // banks 0..7 at y=3, banks 8..15 at y=2, one block, startArg 0. Exclusive
    // ends: endX=8 (one past max x=7), endY=1 (one past y=2, descending).
    bool foundBankBlock = false;
    for (const auto& row : systemRows) {
        if (row.isBlock && row.block.message == BlockableMessage::BankSelect) {
            REQUIRE_TRUE(row.block.startX == 0 && row.block.endX == 8);
            REQUIRE_TRUE(row.block.startY == 3 && row.block.endY == 1);
            REQUIRE_TRUE(row.block.startArg == 0);
            REQUIRE_TRUE(row.indices.size() == 16);
            foundBankBlock = true;
        }
    }
    REQUIRE_TRUE(foundBankBlock);
}

TEST_CASE(RoundTripDefaultLaunchpadProfileSystemMessages) {
    synth::LaunchpadDefaultProfileOptions options;
    options.gestureSelectorCount = 4;
    const auto config = synth::LaunchpadDefaultProfileConfig(options);

    MidiControllerProfileConfig sortedConfig;
    sortedConfig.systemMessages = config.systemMessages;
    NormalizeMidiProfileConfig(sortedConfig, MidiProfileKind::Launchpad);

    const auto rows = ReconstructSystemBlocks(config.systemMessages, MidiProfileKind::Launchpad);
    std::vector<MidiControllerSystemMessageAssociation> flattened;
    for (const auto& row : rows) {
        if (row.isBlock) {
            std::vector<MidiControllerSystemMessageAssociation> expanded;
            std::string reason;
            REQUIRE_TRUE(ExpandSystemBlock(row.block, expanded, &reason));
            for (auto& e : expanded) {
                flattened.push_back(e);
            }
        } else {
            for (std::size_t ix : row.indices) {
                flattened.push_back(sortedConfig.systemMessages[ix]);
            }
        }
    }
    // Finding 6: full structural equality (address incl. launchpadPosition's
    // controller variant, press/release/feedback, outputFeedback).
    RequireAssociationsEqual(flattened, sortedConfig.systemMessages);
}

TEST_CASE(RoundTripDefaultTwisterProfileSystemMessagesAllIndividual) {
    synth::MfTwisterDefaultProfileOptions options;
    for (std::size_t ix = 0; ix < options.sideButtons.size(); ++ix) {
        options.sideButtons[ix] = MidiControllerSystemMessageAssociation{
            .press = MessageIn::SceneSelect(0, ix),
            .feedback = MessageIn::SceneSelect(0, ix),
        };
    }
    const auto config = synth::MfTwisterDefaultProfileConfig(options);
    const auto rows = ReconstructSystemBlocks(config.systemMessages, MidiProfileKind::MfTwister);
    REQUIRE_TRUE(rows.size() == config.systemMessages.size());
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

// --- D4: run-consistency rejections ---------------------------------------

TEST_CASE(ReconstructSystemBlocksRejectsMixedOutputFeedback) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SceneSelect(0, ix);
        a.feedback = a.press;
        a.outputFeedback = (ix == 2) ? false : true;  // one cell differs
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    // The run breaks at the differing cell -- no single 4-cell block.
    for (const auto& row : rows) {
        if (row.isBlock) {
            REQUIRE_TRUE(row.indices.size() < 4);
        }
    }
}

TEST_CASE(ReconstructSystemBlocksRejectsFeedbackNotEqualToPress) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SceneSelect(0, ix);
        a.feedback = (ix == 1) ? MessageIn::SceneSelect(0, 99) : a.press;  // feedback != press on one cell
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    for (const auto& row : rows) {
        if (row.isBlock) {
            REQUIRE_TRUE(row.indices.size() < 4);
        }
    }
}

TEST_CASE(ReconstructSystemBlocksRejectsMixedReleasePattern) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SetGestureSelect(0, ix, true);
        a.feedback = a.press;
        if (ix != 2) {
            a.release = MessageIn::SetGestureSelect(0, ix, false);
        }  // cell 2 is missing its release -- breaks the paired pattern
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    for (const auto& row : rows) {
        if (row.isBlock) {
            REQUIRE_TRUE(row.indices.size() < 4);
        }
    }
}

TEST_CASE(ReconstructSystemBlocksRejectsGestureCellWherePressCarriesFalse) {
    // Finding 1: a gesture-select cell is blockable ONLY when press =
    // SetGestureSelect(arg,true) AND release = SetGestureSelect(arg,false).
    // CellSatisfiesRunPattern's GestureSelect case checked that `release`
    // carries boolValue=false (the "paired set-false variant") but never
    // checked that `press` carries boolValue=true -- so a cell storing the
    // release-sense message as its PRESS (with a redundant boolValue=false
    // "release") wrongly passed the pattern. Expand(Reconstruct(config))
    // would then change behavior: ExpandSystemBlock always emits
    // press=SetGestureSelect(arg,true), silently flipping this cell's sense.
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SetGestureSelect(0, ix, false);    // press carries boolValue=false (bug trigger)
        a.release = MessageIn::SetGestureSelect(0, ix, false);  // "paired set-false" check alone would accept this
        a.feedback = a.press;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    // Every cell must stay individual -- none of these satisfy the run
    // pattern (press.boolValue must be true), so no block should form.
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

TEST_CASE(ExpandOfReconstructedGestureRunNeverFlipsPressReleaseSense) {
    // Companion round-trip check for finding 1: build a run whose PRESS
    // carries boolValue=false; reconstruct, expand every row, and confirm
    // the flattened result equals the sorted input exactly -- if
    // CellSatisfiesRunPattern wrongly accepted these as a block,
    // ExpandSystemBlock would emit press.boolValue=true for every cell,
    // silently flipping the stored sense (Expand(Reconstruct(x)) != x).
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SetGestureSelect(0, ix, false);
        a.release = MessageIn::SetGestureSelect(0, ix, false);
        a.feedback = a.press;
        associations.push_back(a);
    }

    MidiControllerProfileConfig sortedConfig;
    sortedConfig.systemMessages = associations;
    NormalizeMidiProfileConfig(sortedConfig, MidiProfileKind::Generic);

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    std::vector<MidiControllerSystemMessageAssociation> flattened;
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);  // must stay individual (see prior test)
        for (std::size_t ix : row.indices) {
            flattened.push_back(sortedConfig.systemMessages[ix]);
        }
    }
    // Finding 6: full structural equality -- this is the fixture finding 1's
    // bug must be caught by (verified by temporarily reverting the
    // CellSatisfiesRunPattern fix: without it, this run wrongly blocks and
    // ExpandSystemBlock flips press.boolValue to true, which
    // RequireAssociationsEqual's MessageInFullyEquivalent detects.
    RequireAssociationsEqual(flattened, sortedConfig.systemMessages);
}

TEST_CASE(ReconstructSystemBlocksRejectsBankRunSpanningTwoSlots) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation a;
        a.control = MidiControlAddress{.channel = 1, .cc = static_cast<std::uint8_t>(ix)};
        a.press = MessageIn::SelectParamBank(0, ix < 2 ? 0 : 1, ix);  // slot changes mid-run
        a.feedback = a.press;
        associations.push_back(a);
    }
    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    for (const auto& row : rows) {
        if (row.isBlock) {
            REQUIRE_TRUE(row.indices.size() < 4);
        }
    }
}

// Note: ReconstructSystemBlocks defensively sorts its input by
// SystemMessageSortKey (ascending arg) before ContinuesCandidateRun ever
// runs (sru-9), so a SIZE_MAX-then-0 arg pair can never end up adjacent in
// the sorted view -- SIZE_MAX always sorts last. ContinuesCandidateRun's
// `prevArg + 1` wrap guard (finding 3) is therefore unreachable through this
// public API, but the guard stays as defense-in-depth for the shared helper
// pattern; ExpandSystemBlockRejectsStartArgNearSizeMaxThatWouldWrap above
// covers the reachable system-block wrap case (expansion's startArg +
// cellIndex), and ReconstructEncoderBlocksTreatsPositionWrapAsRunBreakNotMatch
// / ReconstructAnalogBlocksTreatsGestureIxWrapAsRunBreakNotMatch cover the
// analogous (and reachable, since those reconstructors take no defensive
// sorting pass) wrap guard for the identical `prev + 1` pattern.

TEST_CASE(ReconstructSystemBlocksDuplicateAddressesStayIndividual) {
    std::vector<MidiControllerSystemMessageAssociation> associations;
    MidiControllerSystemMessageAssociation a;
    a.control = MidiControlAddress{.channel = 1, .cc = 5};
    a.press = MessageIn::SceneSelect(0, 0);
    a.feedback = a.press;
    MidiControllerSystemMessageAssociation b = a;  // exact duplicate (same address, same message)
    associations = {a, b};

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 2);
    for (const auto& row : rows) {
        REQUIRE_TRUE(!row.isBlock);
    }
}

// --- D4: round-trip properties ---------------------------------------------

TEST_CASE(RoundTripExpandReconstructEncoderTurnsForArbitraryConfig) {
    std::vector<EncoderMidiMapping> mappings;
    // Two separate runs plus an outlier -- exercises multiple block/individual
    // rows in one reconstruction.
    for (std::size_t ix = 0; ix < 5; ++ix) {
        mappings.push_back({.control = {.channel = 0, .cc = static_cast<std::uint8_t>(ix)}, .slotIx = 0, .position = ix});
    }
    mappings.push_back({.control = {.channel = 1, .cc = 50}, .slotIx = 3, .position = 7});  // lone outlier
    for (std::size_t ix = 0; ix < 3; ++ix) {
        mappings.push_back(
            {.control = {.channel = 2, .cc = static_cast<std::uint8_t>(20 + ix)}, .slotIx = 1, .position = 10 + ix});
    }

    const auto rows = ReconstructEncoderBlocks(mappings, false);
    std::vector<EncoderMidiMapping> flattened;
    for (const auto& row : rows) {
        if (row.isBlock) {
            std::vector<EncoderMidiMapping> expanded;
            std::string reason;
            REQUIRE_TRUE(ExpandEncoderBlock(row.block, expanded, &reason));
            for (const auto& m : expanded) {
                flattened.push_back(m);
            }
        } else {
            for (std::size_t ix : row.indices) {
                flattened.push_back(mappings[ix]);
            }
        }
    }
    REQUIRE_TRUE(flattened.size() == mappings.size());
    for (std::size_t ix = 0; ix < mappings.size(); ++ix) {
        REQUIRE_TRUE(flattened[ix].control.channel == mappings[ix].control.channel);
        REQUIRE_TRUE(flattened[ix].control.cc == mappings[ix].control.cc);
        REQUIRE_TRUE(flattened[ix].slotIx == mappings[ix].slotIx);
        REQUIRE_TRUE(flattened[ix].position == mappings[ix].position);
    }
}

TEST_CASE(RoundTripExpandReconstructSystemBlocksIncludingDuplicates) {
    std::vector<MidiControllerSystemMessageAssociation> associations = MakeWrldBldrSceneRun(0, 4, 3, 0);
    // Add a duplicate-message-different-address pair (legitimately two
    // buttons sending SceneSelect(0)) that must not be silently dropped or
    // merged incorrectly.
    MidiControllerSystemMessageAssociation dup1;
    dup1.wrldBldrPosition = WrldBldrSystemPosition{.channel = 5, .x = 7, .y = 6};
    dup1.control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(7, 6)};
    dup1.press = MessageIn::SceneSelect(0, 0);
    dup1.feedback = dup1.press;
    associations.push_back(dup1);

    const auto rows = ReconstructSystemBlocks(associations, MidiProfileKind::WrldBldr);

    // Flatten: expand blocks, keep individuals, then compare against the
    // same sorted view ReconstructSystemBlocks itself operates over.
    MidiControllerProfileConfig sortedConfig;
    sortedConfig.systemMessages = associations;
    NormalizeMidiProfileConfig(sortedConfig, MidiProfileKind::WrldBldr);

    std::vector<MidiControllerSystemMessageAssociation> flattened;
    for (const auto& row : rows) {
        if (row.isBlock) {
            std::vector<MidiControllerSystemMessageAssociation> expanded;
            std::string reason;
            REQUIRE_TRUE(ExpandSystemBlock(row.block, expanded, &reason));
            for (auto& e : expanded) {
                flattened.push_back(e);
            }
        } else {
            for (std::size_t ix : row.indices) {
                // indices refer to the reconstruction's own sorted view --
                // rebuild it the same way ReconstructSystemBlocks does.
                flattened.push_back(sortedConfig.systemMessages[ix]);
            }
        }
    }
    // Finding 6: full structural equality, element-by-element.
    RequireAssociationsEqual(flattened, sortedConfig.systemMessages);
}

// --- D4: Reconstruct(Expand(block)) == block, for reconstruction-producible blocks --

TEST_CASE(ReconstructOfExpandedEncoderBlockYieldsSameBlock) {
    EncoderBlock block;
    block.isPush = true;
    block.channel = 4;
    block.startCc = 20;
    block.endCc = 28;
    block.slotIx = 2;
    block.startPosition = 5;

    std::vector<EncoderMidiMapping> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandEncoderBlock(block, expanded, &reason));

    const auto rows = ReconstructEncoderBlocks(expanded, true);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.isPush == block.isPush);
    REQUIRE_TRUE(rows[0].block.channel == block.channel);
    REQUIRE_TRUE(rows[0].block.startCc == block.startCc);
    REQUIRE_TRUE(rows[0].block.endCc == block.endCc);
    REQUIRE_TRUE(rows[0].block.slotIx == block.slotIx);
    REQUIRE_TRUE(rows[0].block.startPosition == block.startPosition);
}

TEST_CASE(ReconstructOfExpandedSystemBlockYieldsSameBlockGeneric) {
    SystemBlock block;
    block.kind = MidiProfileKind::Generic;
    block.message = BlockableMessage::SceneSelect;
    block.startArg = 4;
    block.channel = 6;
    block.startCc = 30;
    block.endCc = 35;
    block.outputFeedback = false;

    std::vector<MidiControllerSystemMessageAssociation> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, expanded, &reason));

    const auto rows = ReconstructSystemBlocks(expanded, MidiProfileKind::Generic);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.message == block.message);
    REQUIRE_TRUE(rows[0].block.startArg == block.startArg);
    REQUIRE_TRUE(rows[0].block.channel == block.channel);
    REQUIRE_TRUE(rows[0].block.startCc == block.startCc);
    REQUIRE_TRUE(rows[0].block.endCc == block.endCc);
    REQUIRE_TRUE(rows[0].block.outputFeedback == block.outputFeedback);
}

TEST_CASE(ReconstructOfExpandedSystemBlockYieldsSameBlockWrldBldrRectangle) {
    SystemBlock block;
    block.kind = MidiProfileKind::WrldBldr;
    block.message = BlockableMessage::BankSelect;
    block.startArg = 0;
    block.bankSlotIx = 2;
    block.channel = 5;
    block.startX = 0;
    block.startY = 3;
    block.endX = 8;   // exclusive: one past max x=7
    block.endY = 1;    // exclusive: descending, one past y=2, matches the default WRLD.Bldr shape
    block.rowMajor = true;

    std::vector<MidiControllerSystemMessageAssociation> expanded;
    std::string reason;
    REQUIRE_TRUE(ExpandSystemBlock(block, expanded, &reason));

    const auto rows = ReconstructSystemBlocks(expanded, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].isBlock);
    REQUIRE_TRUE(rows[0].block.startX == block.startX && rows[0].block.endX == block.endX);
    REQUIRE_TRUE(rows[0].block.startY == block.startY && rows[0].block.endY == block.endY);
    REQUIRE_TRUE(rows[0].block.bankSlotIx == block.bankSlotIx);
}

// --- D4: broader round-trip property sweep across arbitrary configs --------

void RequireEncoderRoundTrip(const std::vector<EncoderMidiMapping>& mappings, bool isPush) {
    const auto rows = ReconstructEncoderBlocks(mappings, isPush);
    std::vector<EncoderMidiMapping> flattened;
    for (const auto& row : rows) {
        if (row.isBlock) {
            std::vector<EncoderMidiMapping> expanded;
            std::string reason;
            REQUIRE_TRUE(ExpandEncoderBlock(row.block, expanded, &reason));
            flattened.insert(flattened.end(), expanded.begin(), expanded.end());
        } else {
            for (std::size_t ix : row.indices) {
                flattened.push_back(mappings[ix]);
            }
        }
    }
    REQUIRE_TRUE(flattened.size() == mappings.size());
    for (std::size_t ix = 0; ix < mappings.size(); ++ix) {
        REQUIRE_TRUE(flattened[ix].control.channel == mappings[ix].control.channel);
        REQUIRE_TRUE(flattened[ix].control.cc == mappings[ix].control.cc);
        REQUIRE_TRUE(flattened[ix].slotIx == mappings[ix].slotIx);
        REQUIRE_TRUE(flattened[ix].position == mappings[ix].position);
    }
}

TEST_CASE(RoundTripEncoderBlocksWithDuplicateAddressesPassThroughAsIndividual) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 0},
        {.control = {.channel = 0, .cc = 0}, .slotIx = 0, .position = 1},  // duplicate address, breaks cc-consecutive
    };
    RequireEncoderRoundTrip(mappings, false);
}

TEST_CASE(RoundTripEncoderBlocksEmptyInput) {
    std::vector<EncoderMidiMapping> mappings;
    const auto rows = ReconstructEncoderBlocks(mappings, false);
    REQUIRE_TRUE(rows.empty());
}

TEST_CASE(RoundTripEncoderBlocksAllIndividualNoRunFits) {
    std::vector<EncoderMidiMapping> mappings = {
        {.control = {.channel = 0, .cc = 0}, .slotIx = 5, .position = 100},
        {.control = {.channel = 1, .cc = 50}, .slotIx = 2, .position = 3},
        {.control = {.channel = 2, .cc = 99}, .slotIx = 9, .position = 1},
    };
    RequireEncoderRoundTrip(mappings, false);
}

int Main() {
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

}  // namespace

int main() {
    return Main();
}
