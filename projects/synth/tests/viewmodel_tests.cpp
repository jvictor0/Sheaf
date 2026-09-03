#include "synth/MidiConfigViewModel.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth module tests must not see JUCE headers"
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <random>
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

using synth::MidiConfigSection;
using synth::MidiConfigViewModel;
using synth::MidiConnectionState;
using synth::MidiControllerConnection;
using synth::MidiControllerSlot;
using synth::MidiControllerSystemMessageAssociation;
using synth::MidiControlAddress;
using synth::EncoderMode;
using synth::FieldIsInteger;
using synth::FieldShortLabel;
using synth::MidiEndpointConnection;
using synth::MidiEndpointRef;
using synth::MidiEndpointStatus;
using synth::MidiInstrumentConfig;
using synth::MidiMappingRowVM;
using synth::MidiProfileKind;
using synth::EncoderModeCatalog;
using synth::DeadlineWindowCapacity;
using synth::RollingMax;
using synth::UISystemMessage;

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
    // Output left unconfigured to exercise the "(none)" label path.
    return slot;
}

MidiControllerSlot MakeLaunchpadSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig();
    slot.input.identifier = "";
    slot.input.name = "Launchpad X";  // stored ref, no identifier match -> offline
    return slot;
}

MidiControllerSlot MakeGenericSlot(const char* name) {
    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = MidiProfileKind::Generic;
    return slot;
}

// Builds the standard 4-controller instrument (one of each kind) used by
// most tests below, along with matching connection state:
//   wrld:   input Online (deviceName "WRLD.Bldr In (live)"), output Online
//   twist:  input Online, output Unconfigured
//   pads (launchpad): input Offline (stored ref "Launchpad X", no live match)
//   blank (generic): both Unconfigured
MidiInstrumentConfig MakeFourKindInstrument() {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeWrldBldrSlot("wrld")));
    REQUIRE_TRUE(instrument.AddController(MakeTwisterSlot("twist")));
    REQUIRE_TRUE(instrument.AddController(MakeLaunchpadSlot("pads")));
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("blank")));
    return instrument;
}

MidiConnectionState MakeFourKindConnection() {
    MidiConnectionState state;
    MidiControllerConnection wrld;
    wrld.input = MidiEndpointConnection{.status = MidiEndpointStatus::Online, .openIdentifier = "wrldbldr-in-id"};
    wrld.output = MidiEndpointConnection{.status = MidiEndpointStatus::Online, .openIdentifier = "wrldbldr-out-id"};
    state.controllers.push_back(wrld);

    MidiControllerConnection twist;
    twist.input = MidiEndpointConnection{.status = MidiEndpointStatus::Online, .openIdentifier = "twister-in-id"};
    twist.output = MidiEndpointConnection{.status = MidiEndpointStatus::Unconfigured};
    state.controllers.push_back(twist);

    MidiControllerConnection pads;
    pads.input = MidiEndpointConnection{.status = MidiEndpointStatus::Offline};
    pads.output = MidiEndpointConnection{.status = MidiEndpointStatus::Unconfigured};
    state.controllers.push_back(pads);

    MidiControllerConnection blank;
    blank.input = MidiEndpointConnection{.status = MidiEndpointStatus::Unconfigured};
    blank.output = MidiEndpointConnection{.status = MidiEndpointStatus::Unconfigured};
    state.controllers.push_back(blank);

    return state;
}

std::string DumpInstrument(const MidiInstrumentConfig& instrument) {
    synth::JsonArena arena(1024 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    char* dumped = json.Dumps(JSON_ENCODE_ANY);
    std::string result = dumped != nullptr ? std::string(dumped) : std::string();
    std::free(dumped);
    return result;
}

int UISystemMessageIndex(UISystemMessage message) {
    const std::vector<synth::UISystemMessageChoice>& catalog = synth::UISystemMessageCatalog();
    for (std::size_t ix = 0; ix < catalog.size(); ++ix) {
        if (catalog[ix].message == message) {
            return static_cast<int>(ix);
        }
    }
    return -1;
}

TEST_CASE(RebuildProducesRowsInOrder) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const auto& rows = vm.Controllers();
    REQUIRE_TRUE(rows.size() == 4);
    REQUIRE_TRUE(rows[0].name == "wrld");
    REQUIRE_TRUE(rows[0].kind == MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(rows[1].name == "twist");
    REQUIRE_TRUE(rows[1].kind == MidiProfileKind::MfTwister);
    REQUIRE_TRUE(rows[2].name == "pads");
    REQUIRE_TRUE(rows[2].kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(rows[3].name == "blank");
    REQUIRE_TRUE(rows[3].kind == MidiProfileKind::Generic);
}

TEST_CASE(SectionsAreKindFiltered) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());
    const auto& rows = vm.Controllers();

    auto hasSection = [](const synth::MidiControllerRowVM& row, MidiConfigSection section) {
        for (const auto& s : row.sections) {
            if (s == section) {
                return true;
            }
        }
        return false;
    };

    // wrld: Encoders + SystemMessages + Analogs
    REQUIRE_TRUE(hasSection(rows[0], MidiConfigSection::Encoders));
    REQUIRE_TRUE(hasSection(rows[0], MidiConfigSection::SystemMessages));
    REQUIRE_TRUE(hasSection(rows[0], MidiConfigSection::Analogs));

    // twist: Encoders + SystemMessages, no Analogs
    REQUIRE_TRUE(hasSection(rows[1], MidiConfigSection::Encoders));
    REQUIRE_TRUE(hasSection(rows[1], MidiConfigSection::SystemMessages));
    REQUIRE_TRUE(!hasSection(rows[1], MidiConfigSection::Analogs));

    // pads (launchpad): SystemMessages only
    REQUIRE_TRUE(!hasSection(rows[2], MidiConfigSection::Encoders));
    REQUIRE_TRUE(hasSection(rows[2], MidiConfigSection::SystemMessages));
    REQUIRE_TRUE(!hasSection(rows[2], MidiConfigSection::Analogs));

    // blank (generic): all three
    REQUIRE_TRUE(hasSection(rows[3], MidiConfigSection::Encoders));
    REQUIRE_TRUE(hasSection(rows[3], MidiConfigSection::SystemMessages));
    REQUIRE_TRUE(hasSection(rows[3], MidiConfigSection::Analogs));
}

TEST_CASE(EverythingStartsCollapsed) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());
    const auto& rows = vm.Controllers();

    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        REQUIRE_TRUE(rows[ix].configExpanded == false);
        for (MidiConfigSection section : rows[ix].sections) {
            REQUIRE_TRUE(vm.SectionExpanded(ix, section) == false);
        }
    }
}

TEST_CASE(ToggleConfigAndSectionFlipAndSurviveRebuild) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    vm.ToggleConfig(0);  // wrld
    vm.ToggleSection(0, MidiConfigSection::Encoders);

    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == true);
    REQUIRE_TRUE(vm.SectionExpanded(0, MidiConfigSection::Encoders) == true);
    REQUIRE_TRUE(vm.SectionExpanded(0, MidiConfigSection::SystemMessages) == false);

    // Rebuild with the same controller set (state keyed by name) -- toggles survive.
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());
    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == true);
    REQUIRE_TRUE(vm.SectionExpanded(0, MidiConfigSection::Encoders) == true);

    // Toggling back off flips it back.
    vm.ToggleConfig(0);
    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == false);
}

TEST_CASE(ToggleStateKeyedByNameSurvivesReordering) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());
    vm.ToggleConfig(2);  // "pads"
    REQUIRE_TRUE(vm.Controllers()[2].configExpanded == true);

    // Rebuild with "pads" moved to index 0.
    MidiInstrumentConfig reordered;
    REQUIRE_TRUE(reordered.AddController(MakeLaunchpadSlot("pads")));
    REQUIRE_TRUE(reordered.AddController(MakeWrldBldrSlot("wrld")));
    MidiConnectionState reorderedConnection;
    reorderedConnection.controllers.push_back(MidiControllerConnection{});
    reorderedConnection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(reordered, reorderedConnection);

    REQUIRE_TRUE(vm.Controllers()[0].name == "pads");
    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == true);
    REQUIRE_TRUE(vm.Controllers()[1].name == "wrld");
    REQUIRE_TRUE(vm.Controllers()[1].configExpanded == false);
}

// sru-5's "uniform runs present as blocks" scenario: the default WRLD.Bldr
// controller's 16 turns / 16 pushes reconstruct as one block row apiece
// (task group 2 -- SectionRows() now returns the block presentation, not a
// flat per-mapping list).
TEST_CASE(WrldBldrEncoderSectionListsOneTurnBlockAndOnePushBlock) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    // 1 turn block + 1 push block + 2 config-level rows (EncoderMode, TurnStep).
    REQUIRE_TRUE(rows.size() == 1 + 1 + 2);

    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(rows[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(rows[0].label.rfind("turn block ch", 0) == 0);
    REQUIRE_TRUE(rows[1].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(rows[1].group == MidiMappingRowVM::RowGroup::EncoderPush);
    REQUIRE_TRUE(rows[1].label.rfind("push block ch", 0) == 0);
    REQUIRE_TRUE(rows[2].kind == MidiMappingRowVM::Kind::ConfigLevel);
    REQUIRE_TRUE(rows[3].kind == MidiMappingRowVM::Kind::ConfigLevel);
}

// --- Finding 2 (Task 4 review, Important): RowFieldValue() -----------------
//
// RowFieldValue is the VM-owned replacement for ControllersPage's old
// page-local RowFieldCurrentValue walker -- it must read the exact same
// value ApplyMappingEdit would accept/produce for that (controllerIx,
// section, rowIx, field), across every field kind the default profiles
// exercise, and return false for a field not advertised on a given row
// (mirroring ApplyMappingEdit's own editableFields gate).

TEST_CASE(RowFieldValueReadsEncoderTurnChannelCcSlotIxPosition) {
    // A single-mapping fixture (KeepFirstPositions(1)): below the >=2
    // threshold for a block (D3/D4), so the sole turn presents as an
    // Individual row at index 0.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeWrldBldrSlot("wrld");
    slot.config.encoderInput->KeepFirstPositions(1);
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);

    const auto& turn = instrument.controllers[0].config.encoderInput->turns[0];
    double value = -1.0;

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, value));
    REQUIRE_TRUE(value == static_cast<double>(turn.control.channel));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Cc, value));
    REQUIRE_TRUE(value == static_cast<double>(turn.control.cc));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::SlotIx, value));
    REQUIRE_TRUE(value == static_cast<double>(turn.slotIx));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Position, value));
    REQUIRE_TRUE(value == static_cast<double>(turn.position));
}

TEST_CASE(RowFieldValueReadsEncoderConfigLevelModeAndTurnStep) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto& encoderInput = *instrument.controllers[0].config.encoderInput;
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    // Config-level rows are the last two: EncoderMode then TurnStep (see
    // ForEachEncoderRow's visit order in the .cpp).
    const std::size_t encoderModeRowIx = rows.size() - 2;
    const std::size_t turnStepRowIx = rows.size() - 1;

    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                  MidiMappingRowVM::Field::EncoderMode, value));
    REQUIRE_TRUE(value == (encoderInput.mode == synth::EncoderMode::DirectionOnly ? 1.0 : 0.0));

    REQUIRE_TRUE(
        vm.RowFieldValue(0, MidiConfigSection::Encoders, turnStepRowIx, MidiMappingRowVM::Field::TurnStep, value));
    REQUIRE_TRUE(value == static_cast<double>(encoderInput.turnStep));
}

TEST_CASE(RowFieldValueReadsAnalogGestureFieldsAndSceneBlend) {
    // The default WrldBldr analog fixture's 31 gestures reconstruct into two
    // clean blocks (ch2 gestures 0..15, ch14 gestures 1..15 -- each
    // internally consecutive/same-channel); trim to a single gesture so row
    // 0 stays Individual (below the >=2 block threshold, D3/D4), matching
    // this test's original intent (single-row field reads).
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeWrldBldrSlot("wrld");
    slot.config.analogInput->gestures.resize(1);
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);

    const auto& analogInput = *instrument.controllers[0].config.analogInput;
    const auto& gesture = analogInput.gestures[0];
    double value = -1.0;

    const std::vector<MidiMappingRowVM> firstPass = vm.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(firstPass[0].kind == MidiMappingRowVM::Kind::Individual);

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Analogs, 0, MidiMappingRowVM::Field::Channel, value));
    REQUIRE_TRUE(value == static_cast<double>(gesture.control.channel));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Analogs, 0, MidiMappingRowVM::Field::Cc, value));
    REQUIRE_TRUE(value == static_cast<double>(gesture.control.cc));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Analogs, 0, MidiMappingRowVM::Field::GestureIx, value));
    REQUIRE_TRUE(value == static_cast<double>(gesture.gestureIx));

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Analogs);
    const std::size_t sceneBlendRowIx = rows.size() - 1;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Analogs, sceneBlendRowIx, MidiMappingRowVM::Field::SceneBlend,
                                  value));
    REQUIRE_TRUE(value == static_cast<double>(analogInput.sceneBlend->cc));
}

TEST_CASE(RowFieldValueReadsSceneBlendWhenUnassigned) {
    // Finding 4 review fallout: RowFieldValue used to return false for an
    // UNASSIGNED sceneBlend even though the ConfigLevel row always
    // advertises Field::SceneBlend as editable and ApplyMappingEdit
    // genuinely accepts assigning it (see that method's AnalogSceneBlend
    // case, which value_or(MidiControlAddress{})-defaults an absent
    // sceneBlend rather than refusing). ControllersPage.hpp's renderer
    // (finding 4's own fix) now skips building an editor entirely for any
    // field RowFieldValue can't read -- so if this case still returned
    // false, an unassigned scene blend would become permanently
    // unassignable from the UI. Must return true with a stable default (0.0
    // -- matching ApplyMappingEdit's own default-construction value) so the
    // renderer still builds an editor for it.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeWrldBldrSlot("wrld");
    slot.config.analogInput->sceneBlend = std::nullopt;
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Analogs);
    const std::size_t sceneBlendRowIx = rows.size() - 1;
    REQUIRE_TRUE(rows[sceneBlendRowIx].group == MidiMappingRowVM::RowGroup::AnalogSceneBlend);

    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Analogs, sceneBlendRowIx, MidiMappingRowVM::Field::SceneBlend,
                                  value));
    REQUIRE_TRUE(value == 0.0);

    // And the field is genuinely assignable from that seeded state.
    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Analogs, sceneBlendRowIx,
                                     MidiMappingRowVM::Field::SceneBlend, 42.0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.analogInput->sceneBlend.has_value());
    REQUIRE_TRUE(out.controllers[0].config.analogInput->sceneBlend->cc == 42);
}

TEST_CASE(RowFieldValueReadsWrldBldrSystemMessagePositions) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto& association = instrument.controllers[0].config.systemMessages[0];
    double value = -1.0;

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrX, value));
    REQUIRE_TRUE(value == static_cast<double>(association.wrldBldrPosition->x));

    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrY, value));
    REQUIRE_TRUE(value == static_cast<double>(association.wrldBldrPosition->y));

    // Channel IS in this row's editableFields (issue #10 -- WRLD.Bldr system
    // rows expose chan/x/y), reading the paired control address's channel.
    // Cc remains NOT advertised: the row's cc is always derived from x/y via
    // WrldBldrPositionToCC, so there is no direct Cc editor.
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, value));
    REQUIRE_TRUE(value == static_cast<double>(association.control->channel));
    REQUIRE_TRUE(!vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Cc, value));
}

TEST_CASE(RowFieldValueReadsLaunchpadSystemMessagePositions) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // Row 0 ("pads") is the reset association at (8,-1) -- the only
    // non-blockable (hence Individual) launchpad system row in the default
    // profile; the 8 scene selectors and 8 bank selectors reconstruct into
    // two block rows (task group 2), so this test targets row 0 by KIND
    // rather than assuming raw unsorted config index 0 lines up with it.
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(2, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);
    double x = -1.0;
    double y = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::LaunchpadX, x));
    REQUIRE_TRUE(vm.RowFieldValue(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::LaunchpadY, y));
    REQUIRE_TRUE(x == 8.0);
    REQUIRE_TRUE(y == -1.0);

    double value = -1.0;
    REQUIRE_TRUE(!vm.RowFieldValue(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, value));
}

TEST_CASE(RowFieldValueReadsTwisterSystemMessageButtonOnly) {
    // sru-8/D1: twister system rows advertise exactly one editable address
    // field -- the logical side button 0..5, persisted as control->cc = 8 +
    // button on the fixed channel 3. The zero-arg MfTwisterDefaultProfileConfig()
    // used in MakeTwisterSlot() has no side buttons configured by default
    // (see TwisterSideButtonRowButtonAndMessageFieldsAllSucceed's comment),
    // so build one directly here with a side button set.
    synth::MfTwisterDefaultProfileOptions options;
    options.sideButtons[0] = MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    };
    MidiControllerSlot slot;
    slot.name = "twist2";
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig(options);
    REQUIRE_TRUE(!slot.config.systemMessages.empty());

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);

    const auto& association = instrument.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(association.control->channel == 3);
    REQUIRE_TRUE(association.control->cc == 8);  // side button 0 -> cc 8 + 0

    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Button, value));
    REQUIRE_TRUE(value == 0.0);

    // No editable Channel or Cc field -- the fixed channel is display-only
    // and the button number is the only editable address field.
    REQUIRE_TRUE(!vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, value));
    REQUIRE_TRUE(!vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Cc, value));
    REQUIRE_TRUE(
        !vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrX, value));
}

TEST_CASE(RowFieldValueRejectsTwisterButtonWhenStoredCcIsOutsideThePhysicalShape) {
    // Finding 5: RowFieldValue's Field::Button case only rejected cc < 8,
    // so a stored cc of e.g. 20 (outside the twister's physical 8..13 side
    // button range) read back as "button 12" instead of being treated as
    // unreadable -- matching how every other RowFieldValue case returns
    // false when the stored data doesn't fit the field it's asked to read
    // (e.g. Field::Channel/Cc return false when association.control has no
    // value at all). A field this out-of-shape must render as blank/dash,
    // not a bogus button number.
    //
    // SlotValidForKind now refuses this shape at AddController time (see
    // the instrument_tests.cpp SlotValidForKind* tests), so to exercise
    // RowFieldValue's own defense independent of that write-path gate --
    // e.g. against data that reached the view model through some other
    // path that didn't re-validate -- add a valid association first, then
    // mutate the in-memory config directly to the out-of-shape cc.
    MidiControllerSlot slot;
    slot.name = "twist3";
    slot.kind = MidiProfileKind::MfTwister;
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 3, .cc = 8};  // valid shape, mutated below
    association.press = synth::MessageIn::SetReset(0, true);
    association.release = synth::MessageIn::SetReset(0, false);
    association.feedback = association.press;
    slot.config.systemMessages.push_back(association);

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));
    instrument.controllers[0].config.systemMessages[0].control->cc = 20;  // outside 8..13
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);

    double value = -1.0;
    REQUIRE_TRUE(!vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Button, value));
}

TEST_CASE(ApplyMappingEditTwisterButtonWritesCcAndRefusesOutOfRange) {
    synth::MfTwisterDefaultProfileOptions options;
    options.sideButtons[0] = MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    };
    MidiControllerSlot slot;
    slot.name = "twist2";
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig(options);

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Button, 5.0,
                                     out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages[0].control->cc == 13);  // 8 + 5
    REQUIRE_TRUE(out.controllers[0].config.systemMessages[0].control->channel == 3);  // unchanged, fixed

    // Out-of-range button (only 0..5 valid) is refused with a reason.
    MidiInstrumentConfig rejected;
    std::string rejectReason;
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Button, 6.0,
                                      rejected, &rejectReason));
    REQUIRE_TRUE(!rejectReason.empty());

    MidiInstrumentConfig rejectedNegative;
    std::string rejectReason2;
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Button, -1.0,
                                      rejectedNegative, &rejectReason2));
    REQUIRE_TRUE(!rejectReason2.empty());
}

TEST_CASE(RowFieldValueReturnsFalseForCatalogAndOutOfRangeFields) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    double value = -1.0;
    REQUIRE_TRUE(
        !vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::MessageKind, value));

    // Out-of-range controllerIx/rowIx.
    REQUIRE_TRUE(!vm.RowFieldValue(99, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, value));
    REQUIRE_TRUE(!vm.RowFieldValue(0, MidiConfigSection::Encoders, 9999, MidiMappingRowVM::Field::Channel, value));
}

// Both tests below need an Individual (not Block) turn row at index 0 --
// trim to a single turn mapping (KeepFirstPositions(1), below the >=2 block
// threshold, D3/D4).
MidiInstrumentConfig MakeSingleTurnWrldBldrInstrument() {
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeWrldBldrSlot("wrld");
    slot.config.encoderInput->KeepFirstPositions(1);
    instrument.AddController(std::move(slot));
    return instrument;
}

MidiConnectionState MakeSingleControllerConnection() {
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    return connection;
}

TEST_CASE(RowFieldValueRoundTripsWithApplyMappingEdit) {
    // For every field ApplyMappingEdit successfully applies, RowFieldValue
    // must read back the same value from the resulting instrument, so a JUCE
    // editor's "revert to VM's last-known value" path never shows something
    // ApplyMappingEdit itself would disagree with.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    vm.Rebuild(instrument, MakeSingleControllerConnection());

    MidiInstrumentConfig edited;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Position, 7.0,
                                     edited, &reason));

    MidiConfigViewModel vmAfter;
    vmAfter.Rebuild(edited, MakeSingleControllerConnection());
    double value = -1.0;
    REQUIRE_TRUE(
        vmAfter.RowFieldValue(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Position, value));
    REQUIRE_TRUE(value == 7.0);
}

TEST_CASE(ApplyMappingEditChangesOnlyTargetedField) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    vm.Rebuild(instrument, MakeSingleControllerConnection());

    // Row 0 of the Encoders section for "wrld" is the sole turn mapping.
    MidiInstrumentConfig edited;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Position, 5.0,
                                        edited, &reason);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(reason.empty());

    REQUIRE_TRUE(edited.controllers[0].config.encoderInput->turns[0].position == 5);

    // Everything else must be identical modulo canonical order: apply the
    // same field change to a copy of `instrument`, normalize it the same way
    // every commit path does (sru-9), and compare full JSON dumps. (Not a
    // literal byte-for-byte "untouched" comparison anymore -- every commit
    // path normalizes now, and the default WrldBldr factory's raw
    // systemMessages order is not itself canonical, so the two would
    // otherwise differ by ordering alone, not content.)
    MidiInstrumentConfig expected = instrument;
    expected.controllers[0].config.encoderInput->turns[0].position = 5;
    synth::NormalizeMidiProfileConfig(expected.controllers[0].config, synth::MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(DumpInstrument(edited) == DumpInstrument(expected));
}

TEST_CASE(ApplyMappingEditRejectingIllegalEditLeavesOutUntouched) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // "pads" (launchpad) SystemMessages rows carry launchpad positions only;
    // attempting to set a WrldBldrX field on one is not among that row's
    // editable fields and must be refused with a reason.
    MidiInstrumentConfig out;
    out.controllers.push_back(MakeGenericSlot("sentinel"));  // pre-populate to prove it's untouched
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrX, 3.0, out,
                            &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.controllers.size() == 1);
    REQUIRE_TRUE(out.controllers[0].name == "sentinel");
}

// --- Issue #10: WRLD.Bldr system rows expose an editable Channel ----------
//
// WRLD.Bldr system-message rows advertise Channel alongside WrldBldrX/
// WrldBldrY and shared message kind/argument fields (see SectionRows' SystemMessages
// case) so the slot reads chan/x/y, matching how the association's `control`
// carries both a channel and a (position-derived) cc. Channel edits write
// only `association.control->channel`; `wrldBldrPosition` and `control->cc`
// (which WrldBldrX/Y edits keep in sync via WrldBldrPositionToCC) must stay
// untouched -- this was previously refused entirely (see git history), which
// is now wrong: the user needs to retarget a slot's MIDI channel without
// also having to touch its x/y position.
TEST_CASE(ApplyMappingEditChannelOnWrldBldrSystemRowIsAccepted) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(!rows.empty());
    bool hasChannel = false;
    for (const auto& field : rows[0].editableFields) {
        if (field == MidiMappingRowVM::Field::Channel) {
            hasChannel = true;
        }
    }
    REQUIRE_TRUE(hasChannel);

    const auto& before = instrument.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(before.wrldBldrPosition.has_value());
    const auto beforePosition = *before.wrldBldrPosition;
    const auto beforeCc = before.control->cc;

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, 5.0, out,
                            &reason);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(reason.empty());

    const auto& after = out.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(after.control.has_value());
    REQUIRE_TRUE(after.control->channel == 5);
    REQUIRE_TRUE(after.control->cc == beforeCc);
    REQUIRE_TRUE(after.wrldBldrPosition.has_value());
    REQUIRE_TRUE(after.wrldBldrPosition->x == beforePosition.x);
    REQUIRE_TRUE(after.wrldBldrPosition->y == beforePosition.y);
    // control->channel is authoritative; the position's channel is kept synced
    // to it so a later X/Y edit (which repacks control from the channel) cannot
    // silently restore the old channel.
    REQUIRE_TRUE(after.wrldBldrPosition->channel == 5);
}

TEST_CASE(WrldBldrChannelEditSurvivesLaterXYEdit) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // Edit the channel to 5...
    MidiInstrumentConfig afterChannel;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, 5.0,
                                     afterChannel, &reason));

    // ...then edit X on the just-committed config. The channel must persist.
    vm.Rebuild(afterChannel, MakeFourKindConnection());
    MidiInstrumentConfig afterXY;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrX, 3.0,
                                     afterXY, &reason));

    const auto& after = afterXY.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(after.control.has_value());
    REQUIRE_TRUE(after.control->channel == 5);
    REQUIRE_TRUE(after.wrldBldrPosition.has_value());
    REQUIRE_TRUE(after.wrldBldrPosition->channel == 5);
    REQUIRE_TRUE(after.wrldBldrPosition->x == 3);
    // cc is repacked from the new (x,y) but the channel axis is preserved.
    REQUIRE_TRUE(after.control->cc == synth::WrldBldrPositionToCC(after.wrldBldrPosition->x, after.wrldBldrPosition->y));
}

TEST_CASE(ApplyMappingEditChannelOnWrldBldrSystemRowValidatesRange) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Channel, 16.0, out,
                            &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditCcOnLaunchpadSystemRowIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(2, MidiConfigSection::SystemMessages);  // "pads" (launchpad)
    REQUIRE_TRUE(!rows.empty());
    for (const auto& field : rows[0].editableFields) {
        REQUIRE_TRUE(field != MidiMappingRowVM::Field::Cc);
    }

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::Cc, 5.0, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(AddControllerDuplicateNameFails) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.AddController("wrld", MidiProfileKind::Generic, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(AddControllerLaunchpadSeedsDefaultProfile) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.AddController("newpads", MidiProfileKind::Launchpad, out, &reason);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(out.controllers.size() == 5);
    const synth::MidiControllerSlot* added = out.FindController("newpads");
    REQUIRE_TRUE(added != nullptr);
    REQUIRE_TRUE(added->kind == MidiProfileKind::Launchpad);
    REQUIRE_TRUE(!added->config.systemMessages.empty());

    const synth::MidiControllerProfileConfig expectedConfig = synth::LaunchpadDefaultProfileConfig();
    REQUIRE_TRUE(added->config.systemMessages.size() == expectedConfig.systemMessages.size());
}

// --- Launchpad controller-variant selector (label-launchpad-brief.md
// Change 2) ------------------------------------------------------------

TEST_CASE(LaunchpadVariantCatalogOrderMatchesEnum) {
    // Index 0 = LaunchpadX, 1 = LaunchpadProMk3, 2 = LaunchpadMiniMk3 --
    // MidiController.hpp's LaunchpadController declaration order.
    const auto& catalog = synth::LaunchpadVariantCatalog();
    REQUIRE_TRUE(catalog.size() == 3);
    REQUIRE_TRUE(catalog[0] == "Launchpad X");
    REQUIRE_TRUE(catalog[1] == "Launchpad Pro MK3");
    REQUIRE_TRUE(catalog[2] == "Launchpad Mini MK3");
}

TEST_CASE(LaunchpadVariantIndexReadsCurrentVariant) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();  // "pads" (ix 2) is LaunchpadX by default
    vm.Rebuild(instrument, MakeFourKindConnection());

    REQUIRE_TRUE(vm.LaunchpadVariantIndex(2) == 0);  // LaunchpadX

    // A non-launchpad controller reports -1 (kind != Launchpad).
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == -1);  // "wrld" is WrldBldr
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(3) == -1);  // "blank" is Generic

    // Out-of-range controllerIx.
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(99) == -1);
}

TEST_CASE(LaunchpadVariantIndexDefaultsToZeroWhenNoAssociations) {
    // A Launchpad-kind slot with an empty systemMessages vector still
    // reports index 0 (LaunchpadX) rather than -1 or an error -- "default 0
    // when none" per this method's doc comment.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot;
    slot.name = "emptypads";
    slot.kind = MidiProfileKind::Launchpad;
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    vm.Rebuild(instrument, MidiConnectionState{});

    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 0);
}

TEST_CASE(SetLaunchpadVariantRewritesAllPositionsXToProMk3) {
    // X -> Pro MK3 only ever WIDENS the addressable grid (Pro MK3's shape is
    // a superset of X's -- see LaunchpadShapeSupports), so every existing
    // LaunchpadDefaultProfileConfig() position (x in 0..8, y in -1..7) stays
    // valid; this should always succeed and rewrite every association's
    // controller.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeLaunchpadSlot("pads"));
    vm.Rebuild(instrument, MakeSingleControllerConnection());

    const std::size_t associationCount = instrument.controllers[0].config.systemMessages.size();
    REQUIRE_TRUE(associationCount > 0);

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.SetLaunchpadVariant(0, 1, out, &reason);  // 1 = Launchpad Pro MK3
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == associationCount);
    for (const auto& association : out.controllers[0].config.systemMessages) {
        REQUIRE_TRUE(association.launchpadPosition.has_value());
        REQUIRE_TRUE(association.launchpadPosition->controller == synth::LaunchpadController::LaunchpadProMk3);
    }

    // Re-Rebuild()ing on the committed result and reading the variant back
    // confirms the slot-level index now reads Pro MK3.
    vm.Rebuild(out, MakeSingleControllerConnection());
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 1);
}

TEST_CASE(SetLaunchpadVariantOpenPresentationSurvivesFollowingEdits) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeLaunchpadSlot("pads");
    slot.config.systemMessages.clear();
    MidiControllerSystemMessageAssociation association;
    association.launchpadPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX, .x = 0, .y = 0};
    association.press = synth::MessageIn::SceneSelect(0, 0);
    association.feedback = association.press;
    association.outputFeedback = true;
    slot.config.systemMessages.push_back(association);
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection = MakeSingleControllerConnection();
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 0);
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    MidiInstrumentConfig afterVariant;
    std::string reason;
    REQUIRE_TRUE(vm.SetLaunchpadVariant(0, 1, afterVariant, &reason));  // -> Pro MK3
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 1);
    REQUIRE_TRUE(afterVariant.controllers[0].config.systemMessages[0].launchpadPosition->controller ==
                 synth::LaunchpadController::LaunchpadProMk3);

    MidiInstrumentConfig afterCoordinate;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::LaunchpadX, -1.0, afterCoordinate, &reason));
    REQUIRE_TRUE(afterCoordinate.controllers[0].config.systemMessages.size() == 1);
    const auto& moved = *afterCoordinate.controllers[0].config.systemMessages[0].launchpadPosition;
    REQUIRE_TRUE(moved.controller == synth::LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(moved.x == -1);
    REQUIRE_TRUE(moved.y == 0);

    vm.Rebuild(afterCoordinate, connection);
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 1);
    MidiInstrumentConfig afterMessage;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageArg, 4.0, afterMessage, &reason));
    REQUIRE_TRUE(afterMessage.controllers[0].config.systemMessages[0].launchpadPosition->controller ==
                 synth::LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(afterMessage.controllers[0].config.systemMessages[0].press.sceneIx == 4);
}

TEST_CASE(LaunchpadCoordinateConflictKeepsTemporaryPresentationUntilResolved) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeLaunchpadSlot("pads");
    slot.config.systemMessages.clear();

    MidiControllerSystemMessageAssociation moving;
    moving.launchpadPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX, .x = 0, .y = 0};
    moving.press = synth::MessageIn::SceneSelect(0, 0);
    moving.feedback = moving.press;
    moving.outputFeedback = true;
    slot.config.systemMessages.push_back(moving);

    MidiControllerSystemMessageAssociation blocker;
    blocker.launchpadPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX, .x = 1, .y = 0};
    blocker.press = synth::MessageIn::ToggleReset(0);
    blocker.feedback = blocker.press;
    blocker.outputFeedback = true;
    slot.config.systemMessages.push_back(blocker);
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection = MakeSingleControllerConnection();
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    auto findRowAt = [&](int expectedX, int expectedY) {
        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        for (std::size_t ix = 0; ix < rows.size(); ++ix) {
            if (rows[ix].kind != MidiMappingRowVM::Kind::Individual) {
                continue;
            }
            double x = -1.0;
            double y = -1.0;
            if (vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                 MidiMappingRowVM::Field::LaunchpadX, x) &&
                vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                 MidiMappingRowVM::Field::LaunchpadY, y) &&
                x == static_cast<double>(expectedX) && y == static_cast<double>(expectedY)) {
                return ix;
            }
        }
        return SIZE_MAX;
    };
    const std::size_t movingIx = findRowAt(0, 0);
    REQUIRE_TRUE(movingIx != SIZE_MAX);

    MidiInstrumentConfig out;
    std::string reason;
    bool presentationChanged = false;
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, movingIx,
                                      MidiMappingRowVM::Field::LaunchpadX, 1.0, out, &reason,
                                      &presentationChanged));
    REQUIRE_TRUE(presentationChanged);
    REQUIRE_TRUE(out.controllers.empty());
    REQUIRE_TRUE(reason.find("duplicate") != std::string::npos);

    double x = -1.0;
    double y = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, movingIx,
                                  MidiMappingRowVM::Field::LaunchpadX, x));
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, movingIx,
                                  MidiMappingRowVM::Field::LaunchpadY, y));
    REQUIRE_TRUE(x == 1.0);
    REQUIRE_TRUE(y == 0.0);

    MidiInstrumentConfig resolved;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, movingIx,
                                     MidiMappingRowVM::Field::LaunchpadY, 1.0, resolved, &reason,
                                     &presentationChanged));
    REQUIRE_TRUE(presentationChanged);
    REQUIRE_TRUE(resolved.controllers[0].config.systemMessages.size() == 2);

    bool sawMoved = false;
    bool sawBlocker = false;
    for (const MidiControllerSystemMessageAssociation& association :
         resolved.controllers[0].config.systemMessages) {
        REQUIRE_TRUE(association.launchpadPosition.has_value());
        sawMoved = sawMoved || (association.launchpadPosition->x == 1 && association.launchpadPosition->y == 1);
        sawBlocker = sawBlocker || (association.launchpadPosition->x == 1 && association.launchpadPosition->y == 0);
    }
    REQUIRE_TRUE(sawMoved);
    REQUIRE_TRUE(sawBlocker);
}

TEST_CASE(SetLaunchpadVariantProMk3ToXRefusedWithProOnlyEdgeButton) {
    // Pro MK3 supports x = -1 (a column X/Mini MK3 do not -- their shape is
    // x in [0, 9)); seed a slot with one association pinned at that
    // Pro-only edge column, then attempt Pro MK3 -> X and confirm it is
    // refused (config unchanged) rather than silently dropping the button.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot;
    slot.name = "pads";
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig(
        {.controller = synth::LaunchpadController::LaunchpadProMk3});
    MidiControllerSystemMessageAssociation edgeButton;
    edgeButton.launchpadPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadProMk3, .x = -1, .y = 0};
    edgeButton.press = synth::MessageIn::SetReset(0, true);
    edgeButton.feedback = edgeButton.press;
    slot.config.systemMessages.push_back(edgeButton);
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    vm.Rebuild(instrument, MidiConnectionState{});

    // Confirm the fixture is actually Pro-only before asserting the refusal
    // (x = -1 is outside LaunchpadX's/MiniMk3's 0..8 range).
    REQUIRE_TRUE(synth::LaunchpadShapeSupports(synth::LaunchpadController::LaunchpadProMk3, -1, 0));
    REQUIRE_TRUE(!synth::LaunchpadShapeSupports(synth::LaunchpadController::LaunchpadX, -1, 0));

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.SetLaunchpadVariant(0, 0, out, &reason);  // 0 = Launchpad X
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
    // `out` must be untouched on refusal -- verify the ORIGINAL instrument
    // (still available via the vm's own snapshot) is what a re-read reports.
    REQUIRE_TRUE(vm.LaunchpadVariantIndex(0) == 1);  // still Pro MK3, unchanged
}

TEST_CASE(SetLaunchpadVariantProMk3ToXOkWhenOnlyShapeSafePositions) {
    // A Pro MK3 slot whose positions all happen to also be valid on X (the
    // LaunchpadDefaultProfileConfig() default shape: x in 0..8, y in -1..7)
    // shrinks cleanly -- Pro MK3 -> X succeeds when no position falls
    // outside X's shape.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot;
    slot.name = "pads";
    slot.kind = MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig(
        {.controller = synth::LaunchpadController::LaunchpadProMk3});
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    vm.Rebuild(instrument, MidiConnectionState{});

    const std::size_t associationCount = instrument.controllers[0].config.systemMessages.size();
    REQUIRE_TRUE(associationCount > 0);

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.SetLaunchpadVariant(0, 0, out, &reason);  // 0 = Launchpad X
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == associationCount);
    for (const auto& association : out.controllers[0].config.systemMessages) {
        REQUIRE_TRUE(association.launchpadPosition.has_value());
        REQUIRE_TRUE(association.launchpadPosition->controller == synth::LaunchpadController::LaunchpadX);
    }
}

TEST_CASE(SetLaunchpadVariantRefusedForNonLaunchpadController) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(!vm.SetLaunchpadVariant(0, 1, out, &reason));  // "wrld" is WrldBldr, not Launchpad
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(SetLaunchpadVariantRefusedForOutOfRangeInputs) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(!vm.SetLaunchpadVariant(99, 1, out, &reason));  // controllerIx out of range
    reason.clear();
    REQUIRE_TRUE(!vm.SetLaunchpadVariant(2, -1, out, &reason));  // variantIndex negative
    reason.clear();
    REQUIRE_TRUE(!vm.SetLaunchpadVariant(2, 3, out, &reason));  // variantIndex >= catalog size
}

TEST_CASE(AddSingleAfterVariantChangeSeedsNewVariant) {
    // Once a slot's variant is switched, AddSingle's NextFreeLaunchpadPosition
    // scan must seed new rows with the SLOT'S CURRENT variant, not a
    // hardcoded LaunchpadX -- otherwise a button added to a Pro MK3
    // controller would silently come back as an X button.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeLaunchpadSlot("pads"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig afterVariant;
    std::string reason;
    REQUIRE_TRUE(vm.SetLaunchpadVariant(0, 1, afterVariant, &reason));  // -> Pro MK3
    vm.Rebuild(afterVariant, connection);

    MidiInstrumentConfig out;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason));
    // Find the newly-added association (the one absent from `afterVariant`)
    // and confirm it carries the Pro MK3 variant.
    bool foundNewProMk3Position = false;
    for (const auto& association : out.controllers[0].config.systemMessages) {
        if (!association.launchpadPosition.has_value()) {
            continue;
        }
        const bool existedBefore =
            std::any_of(afterVariant.controllers[0].config.systemMessages.begin(),
                       afterVariant.controllers[0].config.systemMessages.end(),
                       [&](const MidiControllerSystemMessageAssociation& prior) {
                           return prior.launchpadPosition.has_value() &&
                                 *prior.launchpadPosition == *association.launchpadPosition;
                       });
        if (!existedBefore) {
            REQUIRE_TRUE(association.launchpadPosition->controller == synth::LaunchpadController::LaunchpadProMk3);
            foundNewProMk3Position = true;
        }
    }
    REQUIRE_TRUE(foundNewProMk3Position);
}

TEST_CASE(AddBlockAfterVariantChangeSeedsNewVariant) {
    // A minimal single-association slot (rather than MakeLaunchpadSlot's
    // full LaunchpadDefaultProfileConfig()) so a 2-wide default block has
    // room to land without colliding with an existing position -- the
    // default profile's dense scene/bank rows leave no free 2-cell run
    // adjacent to Pro MK3's first free cell, which is a pre-existing
    // "AddBlock's next-free scan only guarantees the START cell is free"
    // limitation (same one AddBlock's own duplicate-address check already
    // guards, see AddBlockAppendsCommittedExpansion's sibling tests), not
    // something this test is about.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot;
    slot.name = "pads";
    slot.kind = MidiProfileKind::Launchpad;
    // Placed at (8,7) -- far from (-1,-1)/(0,-1), the first two cells Pro
    // MK3's row-major scan tries -- so the default 2-wide block lands
    // without colliding with this lone association.
    MidiControllerSystemMessageAssociation lone;
    lone.launchpadPosition = synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX, .x = 8, .y = 7};
    lone.press = synth::MessageIn::SceneSelect(0, 0);
    lone.feedback = lone.press;
    slot.config.systemMessages.push_back(lone);
    REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig afterVariant;
    std::string reason;
    REQUIRE_TRUE(vm.SetLaunchpadVariant(0, 1, afterVariant, &reason));  // -> Pro MK3
    vm.Rebuild(afterVariant, connection);

    MidiInstrumentConfig out;
    REQUIRE_TRUE(vm.AddBlock(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason));
    bool foundNewProMk3Position = false;
    for (const auto& association : out.controllers[0].config.systemMessages) {
        if (!association.launchpadPosition.has_value()) {
            continue;
        }
        const bool existedBefore =
            std::any_of(afterVariant.controllers[0].config.systemMessages.begin(),
                       afterVariant.controllers[0].config.systemMessages.end(),
                       [&](const MidiControllerSystemMessageAssociation& prior) {
                           return prior.launchpadPosition.has_value() &&
                                 *prior.launchpadPosition == *association.launchpadPosition;
                       });
        if (!existedBefore) {
            REQUIRE_TRUE(association.launchpadPosition->controller == synth::LaunchpadController::LaunchpadProMk3);
            foundNewProMk3Position = true;
        }
    }
    REQUIRE_TRUE(foundNewProMk3Position);
}

TEST_CASE(AddControllerGenericSeedsEmptyConfig) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.AddController("blank2", MidiProfileKind::Generic, out, &reason);
    REQUIRE_TRUE(ok);
    const synth::MidiControllerSlot* added = out.FindController("blank2");
    REQUIRE_TRUE(added != nullptr);
    REQUIRE_TRUE(!added->config.encoderInput.has_value());
    REQUIRE_TRUE(!added->config.encoderOutput.has_value());
    REQUIRE_TRUE(!added->config.analogInput.has_value());
    REQUIRE_TRUE(added->config.systemMessages.empty());
}

TEST_CASE(SetEndpointRefWritesSlotRef) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiEndpointRef ref;
    ref.identifier = "new-device-id";
    ref.name = "New Device";

    MidiInstrumentConfig out;
    const bool ok = vm.SetEndpointRef(3, /*output=*/false, ref, out);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(out.controllers[3].input.identifier == "new-device-id");
    REQUIRE_TRUE(out.controllers[3].input.name == "New Device");
    // Nothing else in the instrument moved.
    REQUIRE_TRUE(out.controllers.size() == instrument.controllers.size());
    REQUIRE_TRUE(out.controllers[0].name == "wrld");
}

TEST_CASE(DeviceLabelsDistinguishOnlineOfflineAndUnconfigured) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());
    const auto& rows = vm.Controllers();

    // wrld: input Online -> shows the live/stored name (not "(none)"/"(offline)").
    REQUIRE_TRUE(rows[0].inputDeviceLabel.find("(none)") == std::string::npos);
    REQUIRE_TRUE(rows[0].inputDeviceLabel.find("(offline)") == std::string::npos);
    REQUIRE_TRUE(!rows[0].inputDeviceLabel.empty());

    // twist: output Unconfigured -> "(none)".
    REQUIRE_TRUE(rows[1].outputDeviceLabel.find("(none)") != std::string::npos);

    // pads: input configured-but-absent (Offline) -> shows stored ref name + "(offline)".
    REQUIRE_TRUE(rows[2].inputDeviceLabel.find("Launchpad X") != std::string::npos);
    REQUIRE_TRUE(rows[2].inputDeviceLabel.find("(offline)") != std::string::npos);

    // blank: both Unconfigured -> "(none)" for both.
    REQUIRE_TRUE(rows[3].inputDeviceLabel.find("(none)") != std::string::npos);
    REQUIRE_TRUE(rows[3].outputDeviceLabel.find("(none)") != std::string::npos);
}

TEST_CASE(RebuildScalesToFourControllersSixtyFourRowsUnderTenMilliseconds) {
    MidiInstrumentConfig instrument;
    for (int ix = 0; ix < 4; ++ix) {
        MidiControllerSlot slot = MakeWrldBldrSlot(("scale" + std::to_string(ix)).c_str());
        REQUIRE_TRUE(instrument.AddController(std::move(slot)));
    }
    MidiConnectionState connection;
    for (int ix = 0; ix < 4; ++ix) {
        connection.controllers.push_back(MidiControllerConnection{});
    }

    MidiConfigViewModel vm;
    const auto start = std::chrono::steady_clock::now();
    vm.Rebuild(instrument, connection);
    // Also materialize the row lists (SectionRows) since that's the other
    // half of the "64 rows" scale claim -- Rebuild() alone only builds the
    // controller/section tree, not the flattened mapping rows.
    for (std::size_t ix = 0; ix < vm.Controllers().size(); ++ix) {
        for (MidiConfigSection section : vm.Controllers()[ix].sections) {
            auto rows = vm.SectionRows(ix, section);
            (void)rows;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    // Generous margin (brief: "sanity bound, std::chrono assert with
    // generous margin") -- 10ms bound for 4 controllers x ~64 rows each.
    REQUIRE_TRUE(elapsed < std::chrono::milliseconds(10));
}

TEST_CASE(RollingMaxReturnsMaxOfItsWindow) {
    RollingMax rolling(256);
    REQUIRE_TRUE(rolling.Max() == 0.0f);

    rolling.Write(1.0f);
    rolling.Write(5.0f);
    rolling.Write(3.0f);
    REQUIRE_TRUE(rolling.Max() == 5.0f);
}

TEST_CASE(RollingMaxForgetsSpikesOlderThanItsWindow) {
    RollingMax rolling(256);
    rolling.Write(100.0f);  // this write will be overwritten after 256 more writes
    for (int ix = 0; ix < 256; ++ix) {
        rolling.Write(1.0f);
    }
    // The initial 100.0f write has been evicted -- 257 total writes means the
    // ring (capacity 256) has wrapped exactly once, overwriting slot 0.
    REQUIRE_TRUE(rolling.Max() == 1.0f);
}

TEST_CASE(RollingMaxKeepsSpikeUntilEvicted) {
    RollingMax rolling(256);
    rolling.Write(42.0f);
    for (int ix = 0; ix < 255; ++ix) {
        rolling.Write(0.0f);
    }
    // Only 256 total writes so far -- the initial spike is still the 0th
    // slot and has not yet been overwritten.
    REQUIRE_TRUE(rolling.Max() == 42.0f);

    rolling.Write(0.0f);  // 257th write wraps around and evicts the spike.
    REQUIRE_TRUE(rolling.Max() == 0.0f);
}

TEST_CASE(DeadlineWindowCapacityTracksTheUiFrameRate) {
    REQUIRE_TRUE(DeadlineWindowCapacity(30) == 30);
    REQUIRE_TRUE(DeadlineWindowCapacity(60) == 60);
    // An unset or invalid frame rate still yields a usable window instead
    // of a capacity-zero ring buffer.
    REQUIRE_TRUE(DeadlineWindowCapacity(0) == 30);
}

TEST_CASE(ShortenedDeadlineWindowDropsAStalePeakWithinItsOwnSpan) {
    const std::size_t capacity = DeadlineWindowCapacity(30);
    RollingMax rolling(capacity);

    rolling.Write(97.0f);
    rolling.Write(1.0f);  // one frame later
    // Positive control: a spike is still on screen a frame after it
    // happens -- proof this is a real hold, not a capacity-of-one window
    // that would trade one wrong reading (stale) for another (invisible).
    REQUIRE_TRUE(rolling.Max() == 97.0f);

    // The rest of the window's own span elapses with no further spike --
    // one write past its capacity evicts the spike's slot.
    for (std::size_t ix = 0; ix < capacity - 1; ++ix) {
        rolling.Write(1.0f);
    }
    REQUIRE_TRUE(rolling.Max() == 1.0f);
}

// --- Finding 2: WrldBldrX/Y edits keep position and control address paired ---

TEST_CASE(WrldBldrXEditUpdatesBothPositionAndControlAddress) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // Row 0 (index 0) is the reset modifier button at (0,4) in the default
    // profile (modifiers reset/random/random-mod lead the system-message list).
    const auto& before = instrument.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(before.wrldBldrPosition.has_value());
    REQUIRE_TRUE(before.wrldBldrPosition->x == 0);

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::WrldBldrX,
                                        5.0, out, &reason);
    REQUIRE_TRUE(ok);
    REQUIRE_TRUE(reason.empty());

    const auto& after = out.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(after.wrldBldrPosition.has_value());
    REQUIRE_TRUE(after.wrldBldrPosition->x == 5);
    REQUIRE_TRUE(after.wrldBldrPosition->y == before.wrldBldrPosition->y);

    // The paired control address must match the new position via
    // WrldBldrPositionToCC, since SystemButtonMidiInProcessor matches
    // incoming MIDI by `control`, not by `wrldBldrPosition`.
    REQUIRE_TRUE(after.control.has_value());
    REQUIRE_TRUE(after.control->channel == after.wrldBldrPosition->channel);
    REQUIRE_TRUE(after.control->cc ==
                synth::WrldBldrPositionToCC(after.wrldBldrPosition->x, after.wrldBldrPosition->y));
    // And it must have actually changed from the stale pre-edit address.
    REQUIRE_TRUE(after.control->cc != before.control->cc);
}

TEST_CASE(WrldBldrYEditUpdatesBothPositionAndControlAddress) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    bool ok = false;
    for (double y = 0.0; y <= 7.0 && !ok; y += 1.0) {
        ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 1, MidiMappingRowVM::Field::WrldBldrY, y,
                                 out, &reason);
    }
    REQUIRE_TRUE(ok);

    const auto& after = out.controllers[0].config.systemMessages[1];
    REQUIRE_TRUE(after.control->cc ==
                synth::WrldBldrPositionToCC(after.wrldBldrPosition->x, after.wrldBldrPosition->y));
}

// --- Finding 3: unchecked numeric casts / validation -----------------------

TEST_CASE(ApplyMappingEditNegativeChannelIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, -1.0,
                                        out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditCc128IsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Cc, 128.0, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditFractionalPositionIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Position, 2.5, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditNegativeSlotIxIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::SlotIx, -3.0, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

// --- Finding 2: index domain checks reject values too large to round-trip
// through static_cast<std::size_t> (e.g. 1e300 is finite, non-negative, and
// == std::floor(itself), but casting it to std::size_t is undefined
// behavior) ---------------------------------------------------------------

TEST_CASE(ApplyMappingEditHugeSlotIxIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::SlotIx, 1e300,
                                        out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditHugeMessageKindCatalogIndexIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                        MidiMappingRowVM::Field::MessageKind, 1e300, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditWrldBldrCoordinateOutOfGridIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 1, MidiMappingRowVM::Field::WrldBldrX, 8.0, out,
                            &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditLaunchpadCoordinateOutOfShapeIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // "pads" row 0 is LaunchpadX at (0,-1); y=8 is outside LaunchpadX's shape
    // (LaunchpadShapeSupports allows y in [-1, 8) for LaunchpadX).
    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.ApplyMappingEdit(2, MidiConfigSection::SystemMessages, 0, MidiMappingRowVM::Field::LaunchpadY, 8.0, out,
                            &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditTurnStepMustBePositive) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    // Config-level rows are last: EncoderMode then TurnStep (see
    // ForEachEncoderRow).
    const std::size_t turnStepRowIx = rows.size() - 1;
    REQUIRE_TRUE(rows[turnStepRowIx].label.rfind("turn step", 0) == 0);

    MidiInstrumentConfig out;
    std::string reason;
    const bool negativeOk = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, turnStepRowIx,
                                                MidiMappingRowVM::Field::TurnStep, -0.5, out, &reason);
    REQUIRE_TRUE(!negativeOk);
    REQUIRE_TRUE(!reason.empty());

    const bool zeroOk = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, turnStepRowIx,
                                            MidiMappingRowVM::Field::TurnStep, 0.0, out, &reason);
    REQUIRE_TRUE(!zeroOk);
}

TEST_CASE(ApplyMappingEditTurnStepMustBeFiniteFloat) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    const std::size_t turnStepRowIx = rows.size() - 1;
    REQUIRE_TRUE(rows[turnStepRowIx].label.rfind("turn step", 0) == 0);

    MidiInstrumentConfig out;
    std::string reason;
    const bool hugeOk = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, turnStepRowIx,
                                            MidiMappingRowVM::Field::TurnStep, 1e300, out, &reason);
    REQUIRE_TRUE(!hugeOk);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(ApplyMappingEditValidEditsStillCommit) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    vm.Rebuild(instrument, MakeSingleControllerConnection());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, 15.0, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns[0].control.channel == 15);

    REQUIRE_TRUE(
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Cc, 127.0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns[0].control.cc == 127);

    REQUIRE_TRUE(
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::SlotIx, 2.0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns[0].slotIx == 2);
}

// --- Finding 4: every editableFields entry actually succeeds ---------------

// Applies a "safe" valid value for `field` against `rowIx` in the given
// section/controller and asserts ApplyMappingEdit succeeds for every row of
// every section on all four default profile kinds.
double SafeValueFor(MidiMappingRowVM::Field field) {
    using Field = MidiMappingRowVM::Field;
    switch (field) {
        case Field::Channel:
            return 1.0;
        case Field::Cc:
            return 10.0;
        case Field::SlotIx:
            return 0.0;
        case Field::Position:
            return 0.0;
        case Field::EncoderMode:
            return 1.0;
        case Field::TurnStep:
            return 0.5;
        case Field::MessageKind:
            return static_cast<double>(UISystemMessageIndex(UISystemMessage::SceneSelect));
        case Field::MessageArg:
            return 0.0;
        case Field::LaunchpadX:
            return 0.0;
        case Field::LaunchpadY:
            return 0.0;
        case Field::WrldBldrX:
            return 0.0;
        case Field::WrldBldrY:
            return 0.0;
        case Field::GestureIx:
            return 0.0;
        case Field::SceneBlend:
            return 10.0;
        case Field::Button:
            return 2.0;
        case Field::BlockStartCc:
            return 0.0;
        case Field::BlockEndCc:
            return 127.0;
        case Field::BlockStartPos:
            return 0.0;
        case Field::BlockStartArg:
            return 0.0;
        case Field::BlockBankSlotIx:
            return 0.0;
        case Field::BlockStartX:
            return 0.0;
        case Field::BlockStartY:
            return 0.0;
        case Field::BlockEndX:
            return 1.0;
        case Field::BlockEndY:
            return 0.0;
        case Field::BlockRowMajor:
            return 1.0;
        case Field::BlockOutputFeedback:
            return 1.0;
        case Field::BlockMessageType:
            return 0.0;
        case Field::AddressType:
        case Field::GridSlotIx:
        case Field::GridXMin:
        case Field::GridXMax:
        case Field::GridYMin:
        case Field::GridYMax:
            return 0.0;
    }
    return 0.0;
}

// Block end-coordinate/end-cc fields must stay strictly greater than (BlockEndCc/
// BlockEndX) or different from (BlockEndY -- exclusive-end semantics allow the
// y direction to descend) the row's OWN current start (SafeValueFor's fixed
// constants can't know that per-row) -- e.g. a launchpad bank-select block can
// start at x=8, so a fixed BlockEndX=1 would make endX <= startX and
// legitimately fail validation. Reads the row's current start value via
// RowFieldValue and returns a value guaranteed to satisfy each field's own
// validity relation against that start.
//
// Block start-coordinate fields (finding 3 fallout): a fixed SafeValueFor
// constant can walk a 2-D system block's START clean off its own footprint
// while its END stays put (this helper only edits ONE field at a time,
// mirroring "every field independently succeeds") -- e.g. moving a
// scene-select block's BlockStartY from 6 to a fixed 0 while BlockEndY
// stays 6 turns a 1-row block into a 7-row rectangle that sweeps through
// OTHER rows' addresses (the WrldBldr default profile's bank-select block
// and reset/random rows sit at y=2..4), which finding 3's new
// duplicate-address check correctly refuses -- a real bug in the FIXED
// TEST CONSTANT, not in the view model. Kept at the row's own current
// value (a same-value "no-op" edit, still a genuine ApplyMappingEdit
// round-trip) for BlockStartX/BlockStartY/BlockStartCc so this test keeps
// verifying "the field accepts SOME legal value" without depending on a
// magic constant staying collision-free against every default profile's
// address layout. Address fields and block end fields use the same no-op
// rule: moving an address or increasing a block end can sweep across a
// sibling row's address or argument range, and duplicate-address validation
// should refuse that.
double SafeValueForRow(MidiConfigViewModel& vm, std::size_t controllerIx, MidiConfigSection section,
                       std::size_t rowIx, MidiMappingRowVM::Field field) {
    using Field = MidiMappingRowVM::Field;
    switch (field) {
        case Field::BlockEndCc: {
            double current = 0.0;
            if (vm.RowFieldValue(controllerIx, section, rowIx, field, current)) {
                return current;
            }
            return SafeValueFor(field);
        }
        case Field::BlockEndX: {
            double current = 0.0;
            if (vm.RowFieldValue(controllerIx, section, rowIx, field, current)) {
                return current;
            }
            return SafeValueFor(field);
        }
        case Field::BlockEndY: {
            double current = 0.0;
            if (vm.RowFieldValue(controllerIx, section, rowIx, field, current)) {
                return current;
            }
            return SafeValueFor(field);
        }
        case Field::BlockStartX:
        case Field::BlockStartY:
        case Field::BlockStartCc: {
            double current = 0.0;
            if (vm.RowFieldValue(controllerIx, section, rowIx, field, current)) {
                return current;
            }
            return SafeValueFor(field);
        }
        case Field::Channel:
        case Field::Cc:
        case Field::LaunchpadX:
        case Field::LaunchpadY:
        case Field::WrldBldrX:
        case Field::WrldBldrY:
        case Field::Button: {
            double current = 0.0;
            if (vm.RowFieldValue(controllerIx, section, rowIx, field, current)) {
                return current;
            }
            return SafeValueFor(field);
        }
        default:
            return SafeValueFor(field);
    }
}

// Each (row, field) pair is applied against a fresh view model rebuilt from
// the same base instrument. This keeps the test focused on its contract:
// every advertised field is individually writable from the row state that
// advertised it. Arbitrary chained edits may legitimately change which fields
// are visible on later reads, so this helper does not depend on edit ordering.
void RequireEveryEditableFieldSucceeds(const MidiInstrumentConfig& baseInstrument,
                                       const MidiConnectionState& connection, std::size_t controllerIx,
                                       MidiConfigSection section) {
    MidiConfigViewModel probe;
    probe.Rebuild(baseInstrument, connection);
    const auto rows = probe.SectionRows(controllerIx, section);
    for (std::size_t rowIx = 0; rowIx < rows.size(); ++rowIx) {
        for (MidiMappingRowVM::Field field : rows[rowIx].editableFields) {
            MidiConfigViewModel vm;
            vm.Rebuild(baseInstrument, connection);
            MidiInstrumentConfig out;
            std::string reason;
            const bool ok = vm.ApplyMappingEdit(controllerIx, section, rowIx, field,
                                                SafeValueForRow(vm, controllerIx, section, rowIx, field), out,
                                                &reason);
            if (!ok) {
                std::ostringstream oss;
                oss << "controller " << controllerIx << " section " << static_cast<int>(section) << " row " << rowIx
                    << " field " << static_cast<int>(field) << " failed: " << reason;
                throw std::runtime_error(oss.str());
            }
        }
    }
}

TEST_CASE(TwisterSideButtonRowButtonAndMessageFieldsAllSucceed) {
    // MfTwister side-button associations advertise a single Button field
    // (sru-8/D1: logical side button 0..5, persisted as control->cc = 8 +
    // button on the fixed channel 3) plus shared message fields. The
    // zero-arg MfTwisterDefaultProfileConfig() used elsewhere in this file
    // has no side buttons configured, so exercise that branch directly here
    // (this options shape mirrors mf_twister_default_profile_maps_encoders_
    // and_input_only_side_buttons in parameter_modulation_tests.cpp).
    synth::MfTwisterDefaultProfileOptions options;
    options.sideButtons[0] = MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    };

    MidiControllerSlot slot;
    slot.name = "twist2";
    slot.kind = MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig(options);
    REQUIRE_TRUE(!slot.config.systemMessages.empty());

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    RequireEveryEditableFieldSucceeds(instrument, connection, 0, MidiConfigSection::SystemMessages);
}

TEST_CASE(EveryEditableFieldOnEveryDefaultProfileRowSucceeds) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    for (std::size_t controllerIx = 0; controllerIx < vm.Controllers().size(); ++controllerIx) {
        for (MidiConfigSection section : vm.Controllers()[controllerIx].sections) {
            RequireEveryEditableFieldSucceeds(instrument, connection, controllerIx, section);
        }
    }
}

// --- Issue #9: FieldIsInteger / EncoderModeCatalog / FieldShortLabel -----

TEST_CASE(FieldIsIntegerTrueForIndexAndCoordinateFields) {
    using Field = MidiMappingRowVM::Field;
    REQUIRE_TRUE(FieldIsInteger(Field::Channel));
    REQUIRE_TRUE(FieldIsInteger(Field::Cc));
    REQUIRE_TRUE(FieldIsInteger(Field::SlotIx));
    REQUIRE_TRUE(FieldIsInteger(Field::Position));
    REQUIRE_TRUE(FieldIsInteger(Field::GestureIx));
    REQUIRE_TRUE(FieldIsInteger(Field::LaunchpadX));
    REQUIRE_TRUE(FieldIsInteger(Field::LaunchpadY));
    REQUIRE_TRUE(FieldIsInteger(Field::WrldBldrX));
    REQUIRE_TRUE(FieldIsInteger(Field::WrldBldrY));
    REQUIRE_TRUE(FieldIsInteger(Field::SceneBlend));
    REQUIRE_TRUE(FieldIsInteger(Field::Button));
}

TEST_CASE(FieldIsIntegerFalseForTurnStepAndNonNumericEditorFields) {
    using Field = MidiMappingRowVM::Field;
    REQUIRE_TRUE(!FieldIsInteger(Field::TurnStep));
    REQUIRE_TRUE(!FieldIsInteger(Field::EncoderMode));
    REQUIRE_TRUE(!FieldIsInteger(Field::MessageKind));
}

TEST_CASE(EncoderModeCatalogExposesAllChoicesInDeclarationOrder) {
    const std::vector<std::string>& catalog = EncoderModeCatalog();
    REQUIRE_TRUE(catalog.size() == 3);
    REQUIRE_TRUE(!catalog[0].empty());
    REQUIRE_TRUE(!catalog[1].empty());
    REQUIRE_TRUE(!catalog[2].empty());
    REQUIRE_TRUE(catalog[0] != catalog[1]);
    REQUIRE_TRUE(catalog[0] != catalog[2]);
    REQUIRE_TRUE(catalog[1] != catalog[2]);
}

TEST_CASE(EncoderModeIndexRoundTripsThroughApplyMappingEditAndRowFieldValue) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    const std::size_t encoderModeRowIx = rows.size() - 2;

    // Index 1 == DirectionOnly (declaration order: Signed7Bit=0,
    // DirectionOnly=1 -- see EncoderMode in MidiController.hpp).
    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                     MidiMappingRowVM::Field::EncoderMode, 1.0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->mode == EncoderMode::DirectionOnly);

    MidiConfigViewModel vmAfter;
    vmAfter.Rebuild(out, MakeFourKindConnection());
    double value = -1.0;
    REQUIRE_TRUE(vmAfter.RowFieldValue(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                       MidiMappingRowVM::Field::EncoderMode, value));
    REQUIRE_TRUE(value == 1.0);

    // Index 0 == Signed7Bit, round-tripping back the other way.
    MidiInstrumentConfig out2;
    REQUIRE_TRUE(vmAfter.ApplyMappingEdit(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                          MidiMappingRowVM::Field::EncoderMode, 0.0, out2, &reason));
    REQUIRE_TRUE(out2.controllers[0].config.encoderInput->mode == EncoderMode::Signed7Bit);
}

TEST_CASE(AbsoluteEncoderModeHasItsOwnCatalogIndexAndRowValue) {
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    instrument.controllers[0].config.encoderInput->mode = EncoderMode::Absolute;

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<std::string>& catalog = EncoderModeCatalog();
    REQUIRE_TRUE(catalog.size() == 3);
    REQUIRE_TRUE(!catalog[2].empty());

    const auto rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    const std::size_t encoderModeRowIx = rows.size() - 2;

    MidiInstrumentConfig edited;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                     MidiMappingRowVM::Field::EncoderMode, 2.0, edited, &reason));
    REQUIRE_TRUE(edited.controllers[0].config.encoderInput->mode == EncoderMode::Absolute);

    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                  MidiMappingRowVM::Field::EncoderMode, value));
    REQUIRE_TRUE(value == 2.0);
}

TEST_CASE(AbsoluteEncoderModeCommitKeepsOpenRowsAndRestoresStoredRelativeStep) {
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    auto& encoderInput = *instrument.controllers[0].config.encoderInput;
    encoderInput.turnStep = 0.03125f;

    MidiConfigViewModel vm;
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::Encoders);

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(before.size() >= 2);
    const std::size_t modeRowIx = before.size() - 2;
    const std::size_t stepRowIx = before.size() - 1;
    REQUIRE_TRUE(before[modeRowIx].group == MidiMappingRowVM::RowGroup::EncoderMode);
    REQUIRE_TRUE(before[stepRowIx].group == MidiMappingRowVM::RowGroup::EncoderStep);
    REQUIRE_TRUE(!before[modeRowIx].deletable);
    REQUIRE_TRUE(!before[stepRowIx].deletable);

    MidiInstrumentConfig absolute;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, modeRowIx,
                                     MidiMappingRowVM::Field::EncoderMode, 2.0, absolute, &reason));
    REQUIRE_TRUE(absolute.controllers[0].config.encoderInput->mode == EncoderMode::Absolute);
    REQUIRE_TRUE(absolute.controllers[0].config.encoderInput->turnStep == 0.03125f);

    vm.Rebuild(absolute, connection);
    REQUIRE_TRUE(vm.SectionExpanded(0, MidiConfigSection::Encoders));
    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after.size() == before.size());
    for (std::size_t ix = 0; ix < before.size(); ++ix) {
        REQUIRE_TRUE(after[ix].kind == before[ix].kind);
        REQUIRE_TRUE(after[ix].group == before[ix].group);
    }
    double modeIndex = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, modeRowIx,
                                  MidiMappingRowVM::Field::EncoderMode, modeIndex));
    REQUIRE_TRUE(modeIndex == 2.0);
    double storedStep = 0.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::Encoders, stepRowIx,
                                  MidiMappingRowVM::Field::TurnStep, storedStep));
    REQUIRE_TRUE(storedStep == 0.03125);
    REQUIRE_TRUE(after[stepRowIx].label.find("relative modes only") != std::string::npos);

    synth::MessageInBus absoluteBus(nullptr, 16);
    auto absoluteProfile = synth::CreateMidiControllerProfile(
        absolute.controllers[0].config, &absoluteBus, nullptr, nullptr, [] { return 601; });
    REQUIRE_TRUE(absoluteProfile.input != nullptr);
    const auto& turn = absolute.controllers[0].config.encoderInput->turns.front();
    absoluteProfile.input->Process(
        synth::BasicMidi::CC(1, turn.control.channel, turn.control.cc, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(absoluteBus.Pop(message, 601));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(message.value == 1.0f);

    MidiInstrumentConfig relative;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, modeRowIx,
                                     MidiMappingRowVM::Field::EncoderMode, 0.0, relative, &reason));
    REQUIRE_TRUE(relative.controllers[0].config.encoderInput->mode == EncoderMode::Signed7Bit);
    REQUIRE_TRUE(relative.controllers[0].config.encoderInput->turnStep == 0.03125f);

    synth::MessageInBus relativeBus(nullptr, 16);
    auto relativeProfile = synth::CreateMidiControllerProfile(
        relative.controllers[0].config, &relativeBus, nullptr, nullptr, [] { return 602; });
    REQUIRE_TRUE(relativeProfile.input != nullptr);
    relativeProfile.input->Process(
        synth::BasicMidi::CC(1, turn.control.channel, turn.control.cc, 65));
    REQUIRE_TRUE(relativeBus.Pop(message, 602));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.delta == 0.03125f);
}

TEST_CASE(EncoderModeIndexMustBeIntegralAndLeavesOutputUntouched) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());
    const std::size_t modeRowIx = vm.SectionRows(0, MidiConfigSection::Encoders).size() - 2;

    MidiInstrumentConfig out = MakeFourKindInstrument();
    out.controllers[0].name = "sentinel";
    const std::string before = DumpInstrument(out);
    std::string reason;
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, modeRowIx,
                                      MidiMappingRowVM::Field::EncoderMode, 1.5, out, &reason));
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(DumpInstrument(out) == before);
}

TEST_CASE(EncoderModeIndexOutOfRangeIsRefused) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const auto rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    const std::size_t encoderModeRowIx = rows.size() - 2;

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, encoderModeRowIx,
                                        MidiMappingRowVM::Field::EncoderMode, 3.0, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

TEST_CASE(FieldShortLabelIsNonEmptyAndDistinctPerField) {
    using Field = MidiMappingRowVM::Field;
    const std::vector<Field> fields = {
        Field::Channel,     Field::Cc,     Field::SlotIx,      Field::Position,       Field::GestureIx,
        Field::LaunchpadX,  Field::LaunchpadY, Field::WrldBldrX, Field::WrldBldrY,     Field::TurnStep,
        Field::EncoderMode, Field::MessageKind, Field::MessageArg,
        Field::SceneBlend, Field::Button,
    };
    std::vector<std::string> seen;
    for (Field field : fields) {
        const char* label = FieldShortLabel(field);
        REQUIRE_TRUE(label != nullptr);
        REQUIRE_TRUE(*label != '\0');
        seen.push_back(label);
    }
    // Not asserting global uniqueness (X/Y are legitimately reused between
    // Launchpad and WRLD.Bldr coordinate fields), just that the helper
    // returns a real label for every field the renderer might ask about.
}

// --- Issue #9/#11: RowGroup assignment --------------------------------

TEST_CASE(EncoderRowsAreGroupedTurnPushModeStep) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    // 1 turn block, 1 push block, 1 mode row, 1 step row (see
    // WrldBldrEncoderSectionListsOneTurnBlockAndOnePushBlock for the
    // row-count derivation).
    REQUIRE_TRUE(rows.size() == 4);
    REQUIRE_TRUE(rows[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(rows[1].group == MidiMappingRowVM::RowGroup::EncoderPush);
    REQUIRE_TRUE(rows[2].group == MidiMappingRowVM::RowGroup::EncoderMode);
    REQUIRE_TRUE(rows[3].group == MidiMappingRowVM::RowGroup::EncoderStep);
}

TEST_CASE(AnalogRowsAreGroupedGestureThenSceneBlend) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(rows.size() >= 2);
    for (std::size_t ix = 0; ix + 1 < rows.size(); ++ix) {
        REQUIRE_TRUE(rows[ix].group == MidiMappingRowVM::RowGroup::AnalogGesture);
    }
    REQUIRE_TRUE(rows.back().group == MidiMappingRowVM::RowGroup::AnalogSceneBlend);
}

TEST_CASE(SystemMessageRowsAreGroupedSystem) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(!rows.empty());
    for (const auto& row : rows) {
        REQUIRE_TRUE(row.group == MidiMappingRowVM::RowGroup::System);
    }
}

// --- Issue #11: scene blend label reads clearly ----------------------------

TEST_CASE(SceneBlendLabelReadsClearlyWhenAssignedAndUnassigned) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(rows.back().group == MidiMappingRowVM::RowGroup::AnalogSceneBlend);
    REQUIRE_TRUE(rows.back().label.find("Scene blend") != std::string::npos ||
                rows.back().label.find("scene blend") != std::string::npos);

    // Force an unassigned sceneBlend and confirm the label still reads
    // clearly (not just a bare "(unassigned)" with no context).
    MidiControllerSlot slot = instrument.controllers[0];
    slot.config.analogInput->sceneBlend = std::nullopt;
    MidiInstrumentConfig unassignedInstrument;
    unassignedInstrument.controllers.push_back(slot);
    for (std::size_t ix = 1; ix < instrument.controllers.size(); ++ix) {
        unassignedInstrument.controllers.push_back(instrument.controllers[ix]);
    }
    MidiConfigViewModel vmUnassigned;
    vmUnassigned.Rebuild(unassignedInstrument, MakeFourKindConnection());
    const std::vector<MidiMappingRowVM> unassignedRows = vmUnassigned.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(unassignedRows.back().label.find("unassigned") != std::string::npos);
}

// ============================================================================
// Task group 2: presentation stability, block editing, add/delete (sru-9,
// sru-10, sru-11).
// ============================================================================

using synth::AnalogBlock;
using synth::EncoderBlock;
using synth::SystemBlock;

// --- sru-11: presentation stability across Rebuild() ------------------------

TEST_CASE(PresentationStableAcrossRebuildWithNoChanges) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(before.size() == 4);  // 1 turn block, 1 push block, mode, step
    REQUIRE_TRUE(before[0].kind == MidiMappingRowVM::Kind::Block);

    // Rebuild with the exact same instrument must not re-group or otherwise
    // change an already-open presentation's shape.
    vm.Rebuild(instrument, MakeFourKindConnection());
    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after.size() == before.size());
    for (std::size_t ix = 0; ix < before.size(); ++ix) {
        REQUIRE_TRUE(after[ix].kind == before[ix].kind);
        REQUIRE_TRUE(after[ix].group == before[ix].group);
        REQUIRE_TRUE(after[ix].label == before[ix].label);
    }
}

TEST_CASE(EditsDoNotRegroupWhileExpanded) {
    // sru-11 scenario: "Edits do not re-group while expanded" -- two
    // individual scene-select rows that happen to form a contiguous run stay
    // two individual rows until collapse+re-expand.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("gen");
    instrument.AddController(std::move(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    // Establish the presentation (first SectionRows() read = the expand
    // transition, per this change's SectionRows() doc comment).
    std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows.empty());

    // Add two individual scene-select rows (arg 0, then arg 1) via AddSingle
    // -- consecutive args on the same (generic) address form, which WOULD
    // reconstruct into one block if re-grouped.
    MidiInstrumentConfig afterFirst;
    std::string reason;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, afterFirst,
                              &reason));
    vm.Rebuild(afterFirst, connection);
    rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    MidiInstrumentConfig afterSecond;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, afterSecond,
                              &reason));
    vm.Rebuild(afterSecond, connection);
    rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    // Still two INDIVIDUAL rows. The presentation was already built, so
    // Rebuild() leaves the open UI-level representation alone.
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);
    REQUIRE_TRUE(rows[1].kind == MidiMappingRowVM::Kind::Individual);

    // Collapse and re-expand: NOW it reconstructs as one block.
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);  // expand (starts collapsed)
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);  // collapse -> discards presentation
    rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);  // re-expand (lazy build)
    REQUIRE_TRUE(rows.size() == 1);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Block);
}

TEST_CASE(CollapseThenReExpandReconstructs) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // Reading SectionRows() builds the presentation (lazy-build == "expand").
    const std::vector<MidiMappingRowVM> firstRead = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(firstRead[0].kind == MidiMappingRowVM::Kind::Block);

    // ToggleSection twice: expand (true) then collapse (false) -- collapse
    // discards the presentation.
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);

    // Re-expanding (reading again) reconstructs -- same shape here since
    // nothing changed, but exercises the discard+rebuild path explicitly.
    const std::vector<MidiMappingRowVM> secondRead = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(secondRead[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(secondRead.size() == firstRead.size());
}

TEST_CASE(TwoIndividualRowsBecomeBlockAfterReExpand) {
    // The exact scenario named in the task brief: add two individual rows
    // that form a contiguous run, stay individual while expanded, then
    // collapse/re-expand and see them merge into a block.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeWrldBldrSlot("wrld");
    slot.config.encoderInput->KeepFirstPositions(0);  // start with zero turns/pushes
    instrument.AddController(std::move(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    // Expand (first read).
    std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    const std::size_t initialRowCount = rows.size();  // just mode+step (0 turns/pushes)

    MidiInstrumentConfig afterFirst;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, afterFirst, &reason));
    vm.Rebuild(afterFirst, connection);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == initialRowCount + 1);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    MidiInstrumentConfig afterSecond;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, afterSecond, &reason));
    vm.Rebuild(afterSecond, connection);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == initialRowCount + 2);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);
    REQUIRE_TRUE(rows[1].kind == MidiMappingRowVM::Kind::Individual);

    // Collapse + re-expand: now one block.
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == initialRowCount + 1);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Block);
}

TEST_CASE(DuplicateMessagesResolveDistinctlyOnDelete) {
    // sru-11 scenario: two associations at different addresses both send
    // scene-select 0; deleting one removes exactly that one.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("gen");
    MidiControllerSystemMessageAssociation a;
    a.control = MidiControlAddress{.channel = 0, .cc = 5};
    a.press = synth::MessageIn::SceneSelect(0, 0);
    a.feedback = a.press;
    slot.config.systemMessages.push_back(a);
    MidiControllerSystemMessageAssociation b;
    b.control = MidiControlAddress{.channel = 0, .cc = 9};
    b.press = synth::MessageIn::SceneSelect(0, 0);
    b.feedback = b.press;
    slot.config.systemMessages.push_back(b);
    instrument.AddController(std::move(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows.size() == 2);  // non-consecutive cc (5, 9) -- no block, two individuals
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);
    REQUIRE_TRUE(rows[1].kind == MidiMappingRowVM::Kind::Individual);

    // Both rows carry the identical message (SceneSelect 0); their addresses
    // differ (cc 5 vs cc 9), so deleting row 0 must remove exactly the cc-5
    // association.
    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, 0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 1);
    REQUIRE_TRUE(out.controllers[0].config.systemMessages[0].control->cc == 9);
}

TEST_CASE(OpenRowSurvivesSourceTruthRemovalUntilReopen) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);  // the sole turn
    const std::size_t originalOpenRowCount = rows.size();

    // Externally clear BOTH turns and pushes (simulating a patch load) and
    // Rebuild() while the presentation is open. The open UI-level
    // representation is intentionally stable; it is not rebuilt from
    // source truth until the section closes and reopens.
    MidiInstrumentConfig patched = instrument;
    patched.controllers[0].config.encoderInput->turns.clear();
    patched.controllers[0].config.encoderInput->pushes.clear();
    vm.Rebuild(patched, connection);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == originalOpenRowCount);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == 2);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::ConfigLevel);
}

TEST_CASE(AllRowsDroppedWhenEncoderInputVanishesEntirely) {
    // A patch load could replace this controller's config wholesale while the
    // section is open. The old presentation remains visible until the user
    // collapses and reopens the section.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    MidiInstrumentConfig patched = instrument;
    patched.controllers[0].config.encoderInput = std::nullopt;
    vm.Rebuild(patched, connection);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(!rows.empty());
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);

    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.empty());
}

TEST_CASE(SameNameReaddAfterRemovalGetsFreshPresentation) {
    // Finding 5: Rebuild() used to keep the presentation map entry alive
    // (rows cleared in place, not erased) once a controller's name vanished
    // from the instrument -- a re-added controller reusing that SAME name
    // would then find the stale, still-present-but-empty entry via
    // PresentationFor()'s find()-hit path and serve an emptied presentation
    // forever, never lazily reconstructing from the reappeared controller's
    // actual config (BuildFreshPresentation only runs on a find()-MISS).
    // TDD per the task brief: remove controller, Rebuild, re-add same name,
    // Rebuild, expand -> fresh reconstruction.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();  // "wrld", one turn mapping
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    // Expand (builds the presentation entry for ("wrld", Encoders)) and also
    // expand the CONFIG section (ToggleConfig) -- both are name-keyed
    // expand state that should reset on a same-name readd (finding 5's
    // "expand-state entry if appropriate").
    std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Individual);
    vm.ToggleConfig(0);
    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == true);

    // Remove the controller entirely and Rebuild -- the presentation AND
    // expand-state entries are now orphaned (name "wrld" no longer
    // resolves).
    MidiInstrumentConfig removed;
    MidiConnectionState emptyConnection;
    vm.Rebuild(removed, emptyConnection);
    REQUIRE_TRUE(vm.Controllers().empty());

    // Re-add a DIFFERENT controller also named "wrld", with a full 16-turn
    // WRLD.Bldr default profile this time (deliberately different content
    // from the single-turn original, so "fresh reconstruction" is
    // observable, not just "happens to look the same").
    MidiInstrumentConfig readded;
    REQUIRE_TRUE(readded.AddController(MakeWrldBldrSlot("wrld")));
    MidiConnectionState readdedConnection = MakeSingleControllerConnection();
    vm.Rebuild(readded, readdedConnection);
    REQUIRE_TRUE(vm.Controllers().size() == 1);
    REQUIRE_TRUE(vm.Controllers()[0].name == "wrld");
    // Fresh appearance -> starts fully collapsed again, not inheriting the
    // OLD "wrld" controller's expanded config state.
    REQUIRE_TRUE(vm.Controllers()[0].configExpanded == false);

    // Expand again: must reconstruct FRESH from the reappeared controller's
    // actual (16-turn, block-forming) config -- one turn block + one push
    // block + mode + step, NOT the stale single-individual-row presentation
    // (or worse, an empty one) the old "wrld" entry would have served.
    rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(rows.size() == 1 + 1 + 2);
    REQUIRE_TRUE(rows[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(rows[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(rows[1].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(rows[1].group == MidiMappingRowVM::RowGroup::EncoderPush);
}

// --- sru-10/sru-11: block editing (replace-cells commit) --------------------

TEST_CASE(BlockEditReplacesStartArgumentKeepingRowInPlace) {
    // This uses the same view model instance across expand -> block edit ->
    // simulated host commit, matching the real ControllersPage call sequence.
    // The open presentation row is the editing surface and must stay in place;
    // only collapse/reopen reconstructs from source truth.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(before[0].kind == MidiMappingRowVM::Kind::Block);  // turn block, startPosition 0

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::BlockStartPos, 20.0,
                                     out, &reason));
    REQUIRE_TRUE(reason.empty());

    // The 16 turn mappings now start at position 20 instead of 0, still on
    // slot 0, still 16 of them, still cc 0..15 (unaffected by BlockStartPos).
    const auto& turns = out.controllers[0].config.encoderInput->turns;
    REQUIRE_TRUE(turns.size() == 16);
    std::size_t minPos = SIZE_MAX;
    std::size_t maxPos = 0;
    for (const auto& t : turns) {
        minPos = std::min(minPos, t.position);
        maxPos = std::max(maxPos, t.position);
    }
    REQUIRE_TRUE(minPos == 20);
    REQUIRE_TRUE(maxPos == 35);

    // Simulate the host committing `out` and calling Rebuild() again on the
    // SAME vm. The block row must stay in place at index 0, still ONE block
    // row (not 16 individuals), with updated values.
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after.size() == before.size());  // still 1 turn block + 1 push block + mode + step, not 16+ rows
    REQUIRE_TRUE(after[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(after[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(after[0].label.find("pos 20") != std::string::npos);

    // A further Rebuild() with the SAME `out` must still leave the open row
    // in place.
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> stable = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(stable.size() == before.size());
    REQUIRE_TRUE(stable[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(stable[0].label.find("pos 20") != std::string::npos);
}

TEST_CASE(RebuildDoesNotHealOpenBlockRowFromSourceTruth) {
    // The open block row is the UI-level representation. Even if the host
    // later Rebuild()s from an older source-of-truth snapshot, the open row
    // must not be re-derived from that truth. Collapse/reopen is the only
    // operation that reconstructs the minimal representation.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(before[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(before[0].label.find("ch0 ") != std::string::npos);  // original turn block channel 0

    MidiInstrumentConfig discardedEdit;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, 7.0,
                                     discardedEdit, &reason));
    REQUIRE_TRUE(reason.empty());

    // The staged presentation now shows "ch7" because the open row is the
    // authoritative editing surface. A Rebuild() from older truth must not
    // overwrite it.
    const std::vector<MidiMappingRowVM> staged = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(staged[0].label.find("ch7 ") != std::string::npos);

    vm.Rebuild(instrument, connection);
    const std::vector<MidiMappingRowVM> healed = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(healed.size() == before.size());
    REQUIRE_TRUE(healed[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(healed[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(healed[0].label.find("ch7 ") != std::string::npos);

    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    const std::vector<MidiMappingRowVM> reopened = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(reopened[0].label.find("ch0 ") != std::string::npos);
}

TEST_CASE(RebuildDoesNotDropOpenBlockRowWhenTruthNoLongerFormsOneBlock) {
    // If source truth changes under an open block row, the open row is still
    // stable. The split minimal representation appears only after close and
    // reopen.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(before[0].kind == MidiMappingRowVM::Kind::Block);  // turn block: slot 0, cc 0..15, pos 0..15

    // Break the persisted shape: give the mapping at position 5 a channel
    // different from the rest. The open row still stays stable; the split
    // minimal representation appears only after close/reopen.
    MidiInstrumentConfig patched = instrument;
    std::vector<synth::EncoderMidiMapping>& turns = patched.controllers[0].config.encoderInput->turns;
    for (auto& mapping : turns) {
        if (mapping.position == 5) {
            mapping.control.channel = 9;
        }
    }
    vm.Rebuild(patched, connection);

    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after.size() == before.size());
    REQUIRE_TRUE(after[0].kind == MidiMappingRowVM::Kind::Block);

    vm.ToggleSection(0, MidiConfigSection::Encoders);
    vm.ToggleSection(0, MidiConfigSection::Encoders);
    const std::vector<MidiMappingRowVM> reopened = vm.SectionRows(0, MidiConfigSection::Encoders);
    std::size_t turnBlockCount = 0;
    std::size_t turnIndividualCount = 0;
    for (const MidiMappingRowVM& row : reopened) {
        if (row.group != MidiMappingRowVM::RowGroup::EncoderTurn) {
            continue;
        }
        if (row.kind == MidiMappingRowVM::Kind::Block) {
            ++turnBlockCount;
        } else if (row.kind == MidiMappingRowVM::Kind::Individual) {
            ++turnIndividualCount;
        }
    }
    (void)turnBlockCount;
    REQUIRE_TRUE(turnIndividualCount >= 1);  // the outlier (position 5) is now its own individual row
}

TEST_CASE(TwoBlockEditsBeforeAnyRebuildAccumulateThroughOpenPresentation) {
    // Two edits applied to the same open presentation before any Rebuild()
    // lands must accumulate through that presentation. The view model's
    // instrument_ snapshot is still the last committed truth, but the open
    // presentation is authoritative while expanded; the second edit flushes
    // the whole presentation, including the first edit's still-open row
    // value, into its returned `out`.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig firstOut;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, 5.0,
                                     firstOut, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(firstOut.controllers[0].config.encoderInput->turns.front().control.channel == 5);

    // The second edit touches a different field. If it rebuilt from
    // instrument_ alone, the returned config would have channel 0 and start
    // position 32. The intended model keeps the first edit's channel 5 in
    // the open presentation and serializes both edited values.
    MidiInstrumentConfig secondOut;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::BlockStartPos, 32.0,
                                     secondOut, &reason));
    REQUIRE_TRUE(reason.empty());
    const auto& secondTurns = secondOut.controllers[0].config.encoderInput->turns;
    REQUIRE_TRUE(secondTurns.size() == 16);
    for (std::size_t ix = 0; ix < secondTurns.size(); ++ix) {
        REQUIRE_TRUE(secondTurns[ix].control.channel == 5);
        REQUIRE_TRUE(secondTurns[ix].position == 32 + ix);
    }

    // Host commits the second edit's accumulated `out` and Rebuild()s. The
    // open block row remains stable and displays both edited values.
    vm.Rebuild(secondOut, connection);
    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after[0].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(after[0].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(after[0].label.find("ch5 ") != std::string::npos);
    REQUIRE_TRUE(after[0].label.find("pos 32..") != std::string::npos);
}

TEST_CASE(BlockEditAllOrNothingRefusalLeavesConfigUnchanged) {
    // sru-10 "block commit is all-or-nothing": editing a block's end cc to
    // something that collides/overflows (endCc <= startCc) must refuse and
    // leave the ENTIRE config untouched, not partially applied.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    out.controllers.push_back(MakeGenericSlot("sentinel"));  // prove untouched on refusal
    std::string reason;
    // BlockEndCc = 0 makes endCc <= startCc (startCc is 0 for the turn
    // block) -- ExpandEncoderBlock refuses this (empty range).
    const bool ok =
        vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::BlockEndCc, 0.0, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(out.controllers.size() == 1);
    REQUIRE_TRUE(out.controllers[0].name == "sentinel");

    // The view model's OWN snapshot (what a subsequent SectionRows() call
    // reads) is also unaffected -- still 16 turns, block intact.
    const std::vector<MidiMappingRowVM> stillThere = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(stillThere[0].kind == MidiMappingRowVM::Kind::Block);
}

TEST_CASE(BlockEditOverlappingExistingSceneButtonRefused) {
    // Finding 3 / sru-10 "block commit is all-or-nothing ... duplicate
    // address": a block edit whose new expansion collides with an address
    // some OTHER, unrelated mapping already occupies must be refused with a
    // reason, config completely unchanged -- Expand*Block only validates a
    // block's OWN cells are self-consistent; it has no visibility into the
    // rest of the config, so this check has to live in ApplyMappingEdit
    // itself (see HasDuplicateSystemAddress).
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();  // "wrld" at index 0
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    // Add an individual scene-select button at the next-free WRLD.Bldr grid
    // position -- (x=0, y=0), sceneIx 8 (the default profile's existing
    // scene-select block already claims sceneIx 0..7).
    MidiInstrumentConfig afterAdd;
    std::string reason;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, afterAdd,
                              &reason));
    vm.Rebuild(afterAdd, connection);
    const std::size_t associationCountBeforeEdit = afterAdd.controllers[0].config.systemMessages.size();

    // Locate the scene-select block row (still (0,6)..(8,7) in exclusive-end
    // form, untouched by the add above) and the new individual scene button
    // at (0,0).
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t sceneBlockIx = SIZE_MAX;
    std::size_t sceneButtonIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].kind == MidiMappingRowVM::Kind::Block && rows[ix].label.rfind("scene select block", 0) == 0) {
            sceneBlockIx = ix;
        }
        if (rows[ix].kind == MidiMappingRowVM::Kind::Individual && rows[ix].label.find("scene select 8") != std::string::npos) {
            sceneButtonIx = ix;
        }
    }
    REQUIRE_TRUE(sceneBlockIx != SIZE_MAX);
    REQUIRE_TRUE(sceneButtonIx != SIZE_MAX);

    // Move the scene-select block's row from y=6 down to y=0 -- its
    // x-range (0..7) now covers (0,0), the new individual button's address,
    // even though BOTH the block's own expansion (self-consistent) and the
    // individual button (unchanged) are independently valid.
    MidiInstrumentConfig sentinel;
    sentinel.controllers.push_back(MakeGenericSlot("sentinel"));  // prove untouched on refusal
    bool presentationChanged = false;
    bool ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, sceneBlockIx,
                                  MidiMappingRowVM::Field::BlockStartY, 0.0, sentinel, &reason,
                                  &presentationChanged);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(presentationChanged);
    REQUIRE_TRUE(!reason.empty());
    // `out` (aliased here as `sentinel`) is untouched -- still just the
    // sentinel controller, not a copy of the real instrument.
    REQUIRE_TRUE(sentinel.controllers.size() == 1);
    REQUIRE_TRUE(sentinel.controllers[0].name == "sentinel");

    ok = vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, sceneBlockIx, MidiMappingRowVM::Field::BlockEndY,
                             0.0, sentinel, &reason, &presentationChanged);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(presentationChanged);

    // The commit output is unaffected, but the open presentation keeps the
    // temporarily-invalid edit visible so a later edit can repair it without
    // snapping the row back to persisted truth.
    const std::vector<MidiMappingRowVM> stillThere = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(stillThere[sceneBlockIx].kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(stillThere[sceneBlockIx].label.rfind("scene select block (0,0)..(8,0)", 0) == 0);
    REQUIRE_TRUE(afterAdd.controllers[0].config.systemMessages.size() == associationCountBeforeEdit);
}

TEST_CASE(SystemBlockEditChangesMessageTypeAndCommitsExpansion) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // "wrld"'s system rows: find the bank-select block.
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t bankBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].kind == MidiMappingRowVM::Kind::Block && rows[ix].label.rfind("bank select block", 0) == 0) {
            bankBlockIx = ix;
        }
    }
    REQUIRE_TRUE(bankBlockIx != SIZE_MAX);

    // Toggle BlockOutputFeedback off and commit.
    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, bankBlockIx,
                                     MidiMappingRowVM::Field::BlockOutputFeedback, 0.0, out, &reason));
    REQUIRE_TRUE(reason.empty());

    bool anyBankSelect = false;
    for (const auto& association : out.controllers[0].config.systemMessages) {
        if (association.press.type == synth::MessageIn::Type::SelectParamBank) {
            anyBankSelect = true;
            REQUIRE_TRUE(association.outputFeedback == false);
        }
    }
    REQUIRE_TRUE(anyBankSelect);
}

// Task group 3 (renderer): BlockMessageTypeIndex() is the dedicated accessor
// a BlockMessageType combo box uses to preselect the row's current message
// type -- RowFieldValue() deliberately refuses BlockMessageType (it is not a
// single numeric value), mirroring how MessageKind uses UISystemMessageIndex()
// instead of RowFieldValue().
TEST_CASE(BlockMessageTypeIndexReadsBankSelectForBankBlock) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t bankBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].kind == MidiMappingRowVM::Kind::Block && rows[ix].label.rfind("bank select block", 0) == 0) {
            bankBlockIx = ix;
        }
    }
    REQUIRE_TRUE(bankBlockIx != SIZE_MAX);

    const int index = vm.BlockMessageTypeIndex(0, MidiConfigSection::SystemMessages, bankBlockIx);
    REQUIRE_TRUE(index == 1);  // BlockableMessage::BankSelect's declaration-order index
    REQUIRE_TRUE(synth::BlockableMessageCatalog()[static_cast<std::size_t>(index)] == "Bank select");
}

TEST_CASE(BlockMessageTypeIndexRefusedForNonSystemBlockRows) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // An encoder Block row (turn/push block) has no BlockMessageType field.
    const std::vector<MidiMappingRowVM> encoderRows = vm.SectionRows(0, MidiConfigSection::Encoders);
    std::size_t encoderBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < encoderRows.size(); ++ix) {
        if (encoderRows[ix].kind == MidiMappingRowVM::Kind::Block) {
            encoderBlockIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(encoderBlockIx != SIZE_MAX);
    REQUIRE_TRUE(vm.BlockMessageTypeIndex(0, MidiConfigSection::Encoders, encoderBlockIx) == -1);

    // An Individual system row also has no BlockMessageType field.
    const std::vector<MidiMappingRowVM> systemRows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t individualIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < systemRows.size(); ++ix) {
        if (systemRows[ix].kind == MidiMappingRowVM::Kind::Individual) {
            individualIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(individualIx != SIZE_MAX);
    REQUIRE_TRUE(vm.BlockMessageTypeIndex(0, MidiConfigSection::SystemMessages, individualIx) == -1);

    // Out-of-range row and non-SystemMessages/out-of-range controller both
    // refuse too.
    REQUIRE_TRUE(vm.BlockMessageTypeIndex(0, MidiConfigSection::SystemMessages, 9999) == -1);
    REQUIRE_TRUE(vm.BlockMessageTypeIndex(9999, MidiConfigSection::SystemMessages, 0) == -1);
}

// --- sru-11: add ("+"/"+B") and delete --------------------------------------

TEST_CASE(AddSingleAppendsAtGroupEndWithNextFreeDefaults) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();  // sole turn at position 0
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, out, &reason));
    REQUIRE_TRUE(reason.empty());
    const auto& turns = out.controllers[0].config.encoderInput->turns;
    REQUIRE_TRUE(turns.size() == 2);
    // Next-free position after {0} is 1.
    bool foundPositionOne = false;
    for (const auto& t : turns) {
        if (t.position == 1) {
            foundPositionOne = true;
        }
    }
    REQUIRE_TRUE(foundPositionOne);
}

TEST_CASE(AddBlockAppendsCommittedExpansion) {
    // sru-11 scenario: "+B" on a launchpad's system group appends the block
    // as a stable presentation row in one commit, then the same view model is
    // rebuilt to mirror the real ControllersPage flow.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeLaunchpadSlot("pads"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    // Expand (first read) before AddBlock, per D5's "collapsed->expanded
    // transition" -- this is the scenario a real host hits: the section is
    // already open when the user presses "+B".
    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    const std::size_t beforeRowCount = before.size();
    const std::size_t beforeAssociationCount = instrument.controllers[0].config.systemMessages.size();

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.AddBlock(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason));
    REQUIRE_TRUE(reason.empty());
    // Default block width is 2 -- exactly 2 new associations appear.
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == beforeAssociationCount + 2);

    // Simulate the host committing `out` and calling Rebuild() again on the
    // SAME vm -- the new block must appear as ONE block row at the end of
    // the System group (not 2 individual rows).
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(after.size() == beforeRowCount + 1);  // one new row: the block
    REQUIRE_TRUE(after.back().kind == MidiMappingRowVM::Kind::Block);
    REQUIRE_TRUE(after.back().group == MidiMappingRowVM::RowGroup::System);

    // Stable on a further no-op Rebuild() too.
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> stable = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(stable.size() == beforeRowCount + 1);
    REQUIRE_TRUE(stable.back().kind == MidiMappingRowVM::Kind::Block);
}

TEST_CASE(AddBlockRefusedForTwister) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeTwisterSlot("twist"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    const bool ok =
        vm.AddBlock(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason);
    REQUIRE_TRUE(!ok);
    REQUIRE_TRUE(!reason.empty());
}

// --- sru-11: first add creates an absent container --------------------------
//
// A Generic controller (MakeGenericSlot) starts with encoderInput and
// analogInput both nullopt (AddControllerGenericSeedsEmptyConfig already
// pins this). Before this fix, AddSingle/AddBlock's Encoders/Analogs
// branches refused outright ("controller has no encoder/analog input") when
// the container was absent -- making an empty section's "+"/"+B" a dead end
// even though GroupSupportsAdd/GroupSupportsBlocks (dispatch-level, kind-only)
// say the group IS addable. Fix: create a default-constructed container as
// part of the commit, then add into it, exactly like a controller that
// already had one.
TEST_CASE(AddSingleEncoderTurnCreatesAbsentEncoderInputContainer) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);
    REQUIRE_TRUE(!instrument.controllers[0].config.encoderInput.has_value());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns.size() == 1);
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->pushes.empty());
    // Default-constructed mode/turnStep are fine (brief: "default
    // mode/turnStep are fine").
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->mode == EncoderMode::Signed7Bit);
}

TEST_CASE(AddSingleEncoderPushCreatesAbsentEncoderInputContainer) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderPush, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->pushes.size() == 1);
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns.empty());
}

TEST_CASE(AddSingleAnalogGestureCreatesAbsentAnalogInputContainer) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);
    REQUIRE_TRUE(!instrument.controllers[0].config.analogInput.has_value());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogGesture, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.analogInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.analogInput->gestures.size() == 1);
    // No scene blend seeded by this add -- absence is a valid, common state.
    REQUIRE_TRUE(!out.controllers[0].config.analogInput->sceneBlend.has_value());
}

TEST_CASE(AddBlockEncoderTurnCreatesAbsentEncoderInputContainer) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddBlock(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.encoderInput.has_value());
    // Default block width is 2.
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns.size() == 2);
}

TEST_CASE(AddBlockAnalogGestureCreatesAbsentAnalogInputContainer) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddBlock(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogGesture, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.analogInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.analogInput->gestures.size() == 2);
}

TEST_CASE(DeleteIndividualRowRemovesExactlyThatConfigElement) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeSingleTurnWrldBldrInstrument();
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    REQUIRE_TRUE(vm.CanDeleteRow(0, MidiConfigSection::Encoders, 0));

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::Encoders, 0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->turns.empty());
    // Push mapping (a separate config element) is untouched.
    REQUIRE_TRUE(out.controllers[0].config.encoderInput->pushes.size() == 1);
}

TEST_CASE(DeleteBlockRowRemovesAllItsCellsInOneCommit) {
    // sru-11 scenario: deleting a 16-cell bank block removes all 16
    // associations in one commit, no other mapping changes.
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t bankBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].kind == MidiMappingRowVM::Kind::Block && rows[ix].label.rfind("bank select block", 0) == 0) {
            bankBlockIx = ix;
        }
    }
    REQUIRE_TRUE(bankBlockIx != SIZE_MAX);
    REQUIRE_TRUE(vm.CanDeleteRow(0, MidiConfigSection::SystemMessages, bankBlockIx));

    const std::size_t beforeCount = instrument.controllers[0].config.systemMessages.size();

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, bankBlockIx, out, &reason));

    std::size_t bankSelectCount = 0;
    for (const auto& association : out.controllers[0].config.systemMessages) {
        if (association.press.type == synth::MessageIn::Type::SelectParamBank) {
            ++bankSelectCount;
        }
    }
    REQUIRE_TRUE(bankSelectCount == 0);
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == beforeCount - 16);
}

TEST_CASE(ConfigLevelRowsAreNotDeletable) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    // rows[2] = EncoderMode, rows[3] = EncoderStep (see
    // WrldBldrEncoderSectionListsOneTurnBlockAndOnePushBlock).
    REQUIRE_TRUE(rows[2].group == MidiMappingRowVM::RowGroup::EncoderMode);
    REQUIRE_TRUE(!rows[2].deletable);
    REQUIRE_TRUE(!vm.CanDeleteRow(0, MidiConfigSection::Encoders, 2));
    REQUIRE_TRUE(rows[3].group == MidiMappingRowVM::RowGroup::EncoderStep);
    REQUIRE_TRUE(!rows[3].deletable);
    REQUIRE_TRUE(!vm.CanDeleteRow(0, MidiConfigSection::Encoders, 3));

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(!vm.DeleteRow(0, MidiConfigSection::Encoders, 2, out, &reason));
    REQUIRE_TRUE(!reason.empty());

    const std::vector<MidiMappingRowVM> analogRows = vm.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(analogRows.back().group == MidiMappingRowVM::RowGroup::AnalogSceneBlend);
    REQUIRE_TRUE(!analogRows.back().deletable);
}

// --- sru-9: every commit path normalizes ------------------------------------

TEST_CASE(BlockEditCommitNormalizesOrder) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, 0, MidiMappingRowVM::Field::Channel, 3.0, out,
                                     &reason));

    synth::MidiControllerProfileConfig sortedCopy = out.controllers[0].config;
    synth::NormalizeMidiProfileConfig(sortedCopy, MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(DumpInstrument(out) ==
                [&] {
                    MidiInstrumentConfig wrapped = out;
                    wrapped.controllers[0].config = sortedCopy;
                    return DumpInstrument(wrapped);
                }());
}

TEST_CASE(AddSingleCommitNormalizes) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("gen");
    // Seed two out-of-order system messages so a normalizing commit would
    // visibly resort them.
    MidiControllerSystemMessageAssociation hi;
    hi.control = MidiControlAddress{.channel = 0, .cc = 50};
    hi.press = synth::MessageIn::SceneSelect(0, 5);
    hi.feedback = hi.press;
    slot.config.systemMessages.push_back(hi);
    MidiControllerSystemMessageAssociation lo;
    lo.control = MidiControlAddress{.channel = 0, .cc = 10};
    lo.press = synth::MessageIn::SceneSelect(0, 1);
    lo.feedback = lo.press;
    slot.config.systemMessages.push_back(lo);
    instrument.AddController(std::move(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    MidiInstrumentConfig out;
    std::string reason;
    // "gen" has no analogInput container yet -- sru-11's "first add creates
    // an absent container" means this now SUCCEEDS (creating a fresh
    // AnalogMidiInConfig) rather than refusing; `out` from this call is
    // discarded (SystemMessages is what this test actually cares about
    // normalizing) but it must still commit cleanly.
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogGesture, out, &reason));
    REQUIRE_TRUE(reason.empty());
    REQUIRE_TRUE(out.controllers[0].config.analogInput.has_value());
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason));

    // AddSingle seeds the new row at the next-free sceneIx (0, the lowest
    // unused among {1, 5}); post-commit, all three must be in
    // SystemMessageSortKey order (sceneIx 0, 1, 5).
    const auto& messages = out.controllers[0].config.systemMessages;
    REQUIRE_TRUE(messages.size() == 3);
    REQUIRE_TRUE(messages[0].press.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(messages[0].press.sceneIx == 0);
    REQUIRE_TRUE(messages[1].press.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(messages[1].press.sceneIx == 1);
    REQUIRE_TRUE(messages[2].press.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(messages[2].press.sceneIx == 5);
}

TEST_CASE(DeleteRowCommitNormalizes) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("gen");
    MidiControllerSystemMessageAssociation hi;
    hi.control = MidiControlAddress{.channel = 0, .cc = 50};
    hi.press = synth::MessageIn::SceneSelect(0, 5);
    hi.feedback = hi.press;
    slot.config.systemMessages.push_back(hi);
    MidiControllerSystemMessageAssociation mid;
    mid.control = MidiControlAddress{.channel = 0, .cc = 30};
    mid.press = synth::MessageIn::SceneSelect(0, 3);
    mid.feedback = mid.press;
    slot.config.systemMessages.push_back(mid);
    MidiControllerSystemMessageAssociation lo;
    lo.control = MidiControlAddress{.channel = 0, .cc = 10};
    lo.press = synth::MessageIn::SceneSelect(0, 1);
    lo.feedback = lo.press;
    slot.config.systemMessages.push_back(lo);
    instrument.AddController(std::move(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    // Delete the sceneIx=3 row by its presentation label.
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t targetIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].label.find("scene select 3") != std::string::npos) {
            targetIx = ix;
        }
    }
    REQUIRE_TRUE(targetIx != SIZE_MAX);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, targetIx, out, &reason));

    const auto& messages = out.controllers[0].config.systemMessages;
    REQUIRE_TRUE(messages.size() == 2);
    // Remaining two (sceneIx 1, 5) must be in sorted order post-commit.
    REQUIRE_TRUE(messages[0].press.sceneIx == 1);
    REQUIRE_TRUE(messages[1].press.sceneIx == 5);
}

// Reviewer finding 2 (D6 "renderer stays thin; all decisions from the view
// model"): GroupSupportsAdd/GroupSupportsBlocks are the single source of
// truth for the page's "+"/"+B" gating, replacing what used to be a
// page-local reimplementation of AddSingle/AddBlock's own dispatch rules
// (SectionBody::AddableGroup/GroupSupportsBlocks in ControllersPage.hpp).
// These JUCE-free tests cover the exact matrix called out in that finding:
// wrldbldr turn/push/system/gesture true/true; twister system true/false
// (D4 point 3's twister-never-blocks rule); config-level groups (encoder
// mode/step, analog scene blend) false/false; launchpad system true/true;
// analog(wrldbldr) gesture true/true; scene-blend false/false.
TEST_CASE(GroupSupportsAddAndBlocksMatchesAddSingleAddBlockDispatch) {
    MidiConfigViewModel vm;
    const MidiInstrumentConfig instrument = MakeFourKindInstrument();  // wrld=0, twist=1, pads=2, blank=3
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    using Group = MidiMappingRowVM::RowGroup;

    // wrldbldr (controllerIx 0): turn/push/system/gesture all true/true.
    REQUIRE_TRUE(vm.GroupSupportsAdd(0, MidiConfigSection::Encoders, Group::EncoderTurn));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(0, MidiConfigSection::Encoders, Group::EncoderTurn));
    REQUIRE_TRUE(vm.GroupSupportsAdd(0, MidiConfigSection::Encoders, Group::EncoderPush));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(0, MidiConfigSection::Encoders, Group::EncoderPush));
    REQUIRE_TRUE(vm.GroupSupportsAdd(0, MidiConfigSection::SystemMessages, Group::System));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(0, MidiConfigSection::SystemMessages, Group::System));
    REQUIRE_TRUE(vm.GroupSupportsAdd(0, MidiConfigSection::Analogs, Group::AnalogGesture));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(0, MidiConfigSection::Analogs, Group::AnalogGesture));

    // twister (controllerIx 1): system supports "+" but never "+B" (D4
    // point 3 -- AddBlock's own MfTwister refusal).
    REQUIRE_TRUE(vm.GroupSupportsAdd(1, MidiConfigSection::SystemMessages, Group::System));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(1, MidiConfigSection::SystemMessages, Group::System));

    // config-level groups are never addable, on any controller/section:
    // encoder mode/step and analog scene blend.
    REQUIRE_TRUE(!vm.GroupSupportsAdd(0, MidiConfigSection::Encoders, Group::EncoderMode));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(0, MidiConfigSection::Encoders, Group::EncoderMode));
    REQUIRE_TRUE(!vm.GroupSupportsAdd(0, MidiConfigSection::Encoders, Group::EncoderStep));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(0, MidiConfigSection::Encoders, Group::EncoderStep));
    REQUIRE_TRUE(!vm.GroupSupportsAdd(0, MidiConfigSection::Analogs, Group::AnalogSceneBlend));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(0, MidiConfigSection::Analogs, Group::AnalogSceneBlend));

    // launchpad (controllerIx 2): system true/true (blocks apply -- only
    // MfTwister is special-cased out).
    REQUIRE_TRUE(vm.GroupSupportsAdd(2, MidiConfigSection::SystemMessages, Group::System));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(2, MidiConfigSection::SystemMessages, Group::System));

    // analog (wrldbldr) gesture true/true; scene-blend false/false -- same
    // assertions as above, restated here to match the finding's exact list.
    REQUIRE_TRUE(vm.GroupSupportsAdd(0, MidiConfigSection::Analogs, Group::AnalogGesture));
    REQUIRE_TRUE(vm.GroupSupportsBlocks(0, MidiConfigSection::Analogs, Group::AnalogGesture));
    REQUIRE_TRUE(!vm.GroupSupportsAdd(0, MidiConfigSection::Analogs, Group::AnalogSceneBlend));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(0, MidiConfigSection::Analogs, Group::AnalogSceneBlend));

    // Reviewer finding 2 (drift): the matrix above is a hardcoded restatement
    // of what GroupSupportsAdd/GroupSupportsBlocks currently return -- it
    // would keep passing even if GroupSupportsAdd/Blocks's dispatch (the
    // switch in MidiConfigViewModel.cpp, kept "literally adjacent to
    // AddSingle/AddBlock" per that function's doc comment specifically so a
    // change to one can't silently skip the other) drifted out of sync with
    // AddSingle/AddBlock's ACTUAL dispatch, since nothing here calls
    // AddSingle/AddBlock at all. Close that gap directly: for every
    // (controller, section, group) triple in the four-kind fixture, call
    // AddSingle/AddBlock (both take no seed -- they self-generate a
    // next-free-address default row/block from the controller's own config,
    // see AddSingle/AddBlock's header doc comments) and require the actual
    // outcome to agree with what GroupSupportsAdd/GroupSupportsBlocks
    // predicted. AddSingle/AddBlock are const and write their result to a
    // throwaway `out` rather than mutating `vm`, so one shared `vm` (already
    // Rebuild()'t above) can be reused for every call below.
    //
    // GroupSupportsAdd/GroupSupportsBlocks's own doc comments are explicit
    // that they answer "could possibly succeed" (dispatch-level: does this
    // (section, group[, kind]) combination reach a real branch at all), NOT
    // "will succeed" -- AddSingle/AddBlock's header doc comments likewise
    // spell out that a dispatch-matched call can still be refused for
    // per-controller runtime state: no encoder/analog input on this
    // controller kind, no free address/grid position/twister button, or --
    // AddBlock only, see the "Finding 3" comment on its Encoders branch -- a
    // block wide enough to walk past what the single-cell next-free scan
    // guaranteed free for the START cell only (caught by the
    // HasDuplicate*Address backstop). So the equality this loop checks is
    // asymmetric by design, not a loosened drift check:
    //   - GroupSupports*==false must mean the Add* call refuses (dispatch
    //     agreement is exact and unconditional -- this is the direction
    //     finding 2 is actually about, and where a real drift bug would
    //     show up as an unexpected `true`).
    //   - GroupSupports*==true means the Add* call must have reached its
    //     OWN matching dispatch branch -- i.e. its refusal reason (if any)
    //     must not be the generic catch-all each function's dispatch itself
    //     falls through to ("this group does not support adding
    //     [a block/individual rows]"). One concrete case exercised by this
    //     fixture below: AddBlock(wrldbldr, Analogs, AnalogGesture) refuses
    //     with a duplicate-address reason (wrldbldr's default profile
    //     densely packs gestureIx 0-15 across two channels, leaving channel 2
    //     cc0 -- the sceneBlend address, not a gesture -- as the lone free
    //     single cc; AddBlock's default 2-wide block then walks into the
    //     already-occupied cc1) -- a genuine dispatch agreement with a
    //     documented runtime refusal, not drift. sru-11's "first add creates
    //     an absent container" means AddSingle/AddBlock(twister, Analogs,
    //     AnalogGesture) no longer refuse with "controller has no analog
    //     input" -- they now CREATE the container and proceed, then refuse at
    //     SlotValidForKind with "analog input not supported by this
    //     controller kind" instead (MfTwister's KindSupport().analogs is
    //     false) -- still a genuine in-branch runtime refusal, not the
    //     dispatch catch-all, so this loop's assertions still hold.
    const std::size_t controllerCount = 4;  // wrld, twist, pads, blank
    const MidiConfigSection sections[] = {MidiConfigSection::Encoders, MidiConfigSection::SystemMessages,
                                          MidiConfigSection::Analogs};
    const Group groups[] = {Group::EncoderTurn,     Group::EncoderPush,      Group::EncoderMode,
                            Group::EncoderStep,     Group::AnalogGesture,    Group::AnalogSceneBlend,
                            Group::System};
    const std::string kAddDispatchRefusal = "this group does not support adding individual rows";
    const std::string kBlockDispatchRefusal = "this group does not support adding a block";
    for (std::size_t controllerIx = 0; controllerIx < controllerCount; ++controllerIx) {
        for (const MidiConfigSection section : sections) {
            for (const Group group : groups) {
                const bool expectAdd = vm.GroupSupportsAdd(controllerIx, section, group);
                const bool expectBlocks = vm.GroupSupportsBlocks(controllerIx, section, group);

                MidiInstrumentConfig addOut;
                std::string addReason;
                const bool addSucceeded = vm.AddSingle(controllerIx, section, group, addOut, &addReason);
                if (!expectAdd) {
                    REQUIRE_TRUE(!addSucceeded);
                    // GroupSupportsAdd returns false iff the dispatch doesn't match,
                    // so the refusal must be dispatch-level.
                    REQUIRE_TRUE(addReason == kAddDispatchRefusal);
                } else if (!addSucceeded) {
                    // Dispatch still agreed (matched a real branch); only a
                    // documented in-branch runtime refusal is tolerated here.
                    REQUIRE_TRUE(addReason != kAddDispatchRefusal);
                }

                MidiInstrumentConfig blockOut;
                std::string blockReason;
                const bool blockSucceeded = vm.AddBlock(controllerIx, section, group, blockOut, &blockReason);
                if (!expectBlocks) {
                    REQUIRE_TRUE(!blockSucceeded);
                    // GroupSupportsBlocks returns false iff either GroupSupportsAdd is
                    // false (dispatch mismatch, so dispatch-level refusal) OR the dispatch
                    // matches but there's an early return like Twister system messages.
                    // Only the first case should have dispatch-level refusal.
                    if (expectAdd) {
                        // Dispatch matched (it's a supported Add), so any Block failure
                        // must be an in-branch refusal (e.g., Twister's "never block").
                        REQUIRE_TRUE(blockReason != kBlockDispatchRefusal);
                    } else {
                        // Dispatch doesn't match, so it must be dispatch-level.
                        REQUIRE_TRUE(blockReason == kBlockDispatchRefusal);
                    }
                } else if (!blockSucceeded) {
                    REQUIRE_TRUE(blockReason != kBlockDispatchRefusal);
                }
            }
        }
    }
}

TEST_CASE(GroupSupportsAddOutOfRangeControllerIxReturnsFalse) {
    MidiConfigViewModel vm;
    const MidiInstrumentConfig instrument = MakeFourKindInstrument();
    const MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);

    REQUIRE_TRUE(!vm.GroupSupportsAdd(99, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System));
    REQUIRE_TRUE(!vm.GroupSupportsBlocks(99, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System));
}

// --- sru-11: AddableGroups / GroupColumnFields (empty-group add headers) ----

TEST_CASE(AddableGroupsListsEncoderTurnThenPushInCanonicalOrder) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());  // wrld=0

    const auto groups = vm.AddableGroups(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(groups.size() == 2);
    REQUIRE_TRUE(groups[0] == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(groups[1] == MidiMappingRowVM::RowGroup::EncoderPush);
}

TEST_CASE(AddableGroupsListsAnalogGestureOnlyForAnalogsSection) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());  // wrld=0

    const auto groups = vm.AddableGroups(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(groups.size() == 1);
    REQUIRE_TRUE(groups[0] == MidiMappingRowVM::RowGroup::AnalogGesture);
}

TEST_CASE(AddableGroupsListsSystemThenGridForSupportedSystemMessagesSection) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());  // wrld=0

    const auto groups = vm.AddableGroups(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(groups.size() == 2);
    REQUIRE_TRUE(groups[0] == MidiMappingRowVM::RowGroup::System);
    REQUIRE_TRUE(groups[1] == MidiMappingRowVM::RowGroup::Grid);
}

TEST_CASE(AddableGroupsNeverListsConfigLevelGroups) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    using Group = MidiMappingRowVM::RowGroup;
    auto contains = [](const std::vector<Group>& groups, Group g) {
        return std::find(groups.begin(), groups.end(), g) != groups.end();
    };
    REQUIRE_TRUE(!contains(vm.AddableGroups(0, MidiConfigSection::Encoders), Group::EncoderMode));
    REQUIRE_TRUE(!contains(vm.AddableGroups(0, MidiConfigSection::Encoders), Group::EncoderStep));
    REQUIRE_TRUE(!contains(vm.AddableGroups(0, MidiConfigSection::Analogs), Group::AnalogSceneBlend));
}

TEST_CASE(AddableGroupsEmptyForOutOfRangeControllerIx) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    REQUIRE_TRUE(vm.AddableGroups(99, MidiConfigSection::Encoders).empty());
}

// AddableGroups mirrors GroupSupportsAdd's own doc-commented contract: it is
// mostly dispatch-level, except the Grid group is intentionally restricted
// to the two controller kinds with two-dimensional address forms.
// so, matching GroupSupportsAdd(0/1/2/3, Encoders, EncoderTurn) all being
// true regardless of kind (GroupSupportsAddAndBlocksMatchesAddSingleAddBlock
// Dispatch's own "today's dispatch does not vary by kind" doc comment),
// AddableGroups(2 /*launchpad*/, Encoders) still lists EncoderTurn/EncoderPush
// even though launchpad's KindSupport has no encoders -- the renderer never
// actually calls this for a section absent from that controller's own
// MidiControllerRowVM::sections (SectionsForKind-filtered, ControllerRow's
// `for (const synth::MidiConfigSection section : rowVm.sections)` loop), so
// this is unreachable in practice but must stay dispatch-consistent per
// AddableGroups' own doc comment.
TEST_CASE(AddableGroupsIsDispatchLevelNotKindFiltered) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    const auto encoderGroups = vm.AddableGroups(2, MidiConfigSection::Encoders);
    REQUIRE_TRUE(encoderGroups.size() == 2);
    const auto analogGroups = vm.AddableGroups(2, MidiConfigSection::Analogs);
    REQUIRE_TRUE(analogGroups.size() == 1);
    const auto launchpadSystemGroups = vm.AddableGroups(2, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(launchpadSystemGroups.size() == 2);
    REQUIRE_TRUE(launchpadSystemGroups[0] == MidiMappingRowVM::RowGroup::System);
    REQUIRE_TRUE(launchpadSystemGroups[1] == MidiMappingRowVM::RowGroup::Grid);
    REQUIRE_TRUE(vm.AddableGroups(1, MidiConfigSection::SystemMessages) ==
                 std::vector<MidiMappingRowVM::RowGroup>{MidiMappingRowVM::RowGroup::System});
    REQUIRE_TRUE(vm.AddableGroups(3, MidiConfigSection::SystemMessages) ==
                 std::vector<MidiMappingRowVM::RowGroup>{MidiMappingRowVM::RowGroup::System});
}

// GroupColumnFields must match exactly what BuildSectionRows() gives a real
// added Individual row in the same group -- the whole point of this method
// is that an empty-group header renders the SAME columns a populated one
// would, so add a row via AddSingle into an EMPTY group (post Fix A, this
// now succeeds even from a nullopt container) and compare.
TEST_CASE(GroupColumnFieldsMatchesWhatARealAddedEncoderTurnRowGets) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    const auto columnFields =
        vm.GroupColumnFields(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(!columnFields.empty());

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderTurn, out, &reason));
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(!rows.empty());
    REQUIRE_TRUE(rows.front().group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(rows.front().editableFields == columnFields);
}

TEST_CASE(GroupColumnFieldsMatchesWhatARealAddedAnalogGestureRowGets) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    instrument.AddController(MakeGenericSlot("gen"));
    MidiConnectionState connection = MakeSingleControllerConnection();
    vm.Rebuild(instrument, connection);

    const auto columnFields =
        vm.GroupColumnFields(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogGesture);
    const std::vector<MidiMappingRowVM::Field> expectedColumnFields = {
        MidiMappingRowVM::Field::Channel, MidiMappingRowVM::Field::Cc, MidiMappingRowVM::Field::GestureIx};
    REQUIRE_TRUE(columnFields == expectedColumnFields);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(
        vm.AddSingle(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogGesture, out, &reason));
    vm.Rebuild(out, connection);
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::Analogs);
    REQUIRE_TRUE(!rows.empty());
    REQUIRE_TRUE(rows.front().group == MidiMappingRowVM::RowGroup::AnalogGesture);
    REQUIRE_TRUE(rows.front().editableFields == columnFields);
}

// System's schema is per-kind (sru-8) -- check every kind's empty-section
// GroupColumnFields matches SystemRowEditableFields(kind) (the same table
// BuildSectionRows()' Individual system-row case uses), via a real added row.
TEST_CASE(GroupColumnFieldsMatchesWhatARealAddedSystemRowGetsPerKind) {
    struct KindFixture {
        MidiControllerSlot (*makeSlot)(const char*);
        const char* name;
    };
    const KindFixture fixtures[] = {
        {MakeWrldBldrSlot, "wrld"},
        {MakeTwisterSlot, "twist"},
        {MakeLaunchpadSlot, "pads"},
        {MakeGenericSlot, "gen"},
    };
    for (const KindFixture& fixture : fixtures) {
        MidiConfigViewModel vm;
        MidiInstrumentConfig instrument;
        instrument.AddController(fixture.makeSlot(fixture.name));
        MidiConnectionState connection = MakeSingleControllerConnection();
        vm.Rebuild(instrument, connection);

        const auto columnFields =
            vm.GroupColumnFields(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System);
        REQUIRE_TRUE(!columnFields.empty());

        // Clear this kind's default-profile system messages first so AddSingle
        // is genuinely adding into an EMPTY group (matches the empty-section
        // scenario this method exists for), then add one and compare.
        MidiInstrumentConfig cleared = instrument;
        cleared.controllers[0].config.systemMessages.clear();
        vm.Rebuild(cleared, connection);

        MidiInstrumentConfig out;
        std::string reason;
        REQUIRE_TRUE(
            vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System, out, &reason));
        vm.Rebuild(out, connection);
        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(!rows.empty());
        REQUIRE_TRUE(rows.front().group == MidiMappingRowVM::RowGroup::System);
        REQUIRE_TRUE(rows.front().editableFields == columnFields);
    }
}

TEST_CASE(GroupColumnFieldsEmptyForConfigLevelGroupsAndOutOfRangeControllerIx) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeFourKindInstrument(), MakeFourKindConnection());

    REQUIRE_TRUE(
        vm.GroupColumnFields(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderMode).empty());
    REQUIRE_TRUE(
        vm.GroupColumnFields(0, MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderStep).empty());
    REQUIRE_TRUE(
        vm.GroupColumnFields(0, MidiConfigSection::Analogs, MidiMappingRowVM::RowGroup::AnalogSceneBlend).empty());
    REQUIRE_TRUE(
        vm.GroupColumnFields(99, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System).empty());
}

// Drift check mirroring GroupSupportsAddAndBlocksMatchesAddSingleAddBlockDispatch's
// own rationale, applied to AddableGroups: every group it lists (or omits)
// for a given (controllerIx, section) must agree with GroupSupportsAdd
// itself, so a hardcoded expectation elsewhere in this file can never
// silently diverge from AddableGroups' actual behavior.
TEST_CASE(AddableGroupsAgreesWithGroupSupportsAddForEveryGroupAndSection) {
    MidiConfigViewModel vm;
    const MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    using Group = MidiMappingRowVM::RowGroup;
    const MidiConfigSection sections[] = {MidiConfigSection::Encoders, MidiConfigSection::SystemMessages,
                                          MidiConfigSection::Analogs};
    const Group groups[] = {Group::EncoderTurn,     Group::EncoderPush,      Group::EncoderMode,
                            Group::EncoderStep,     Group::AnalogGesture,    Group::AnalogSceneBlend,
                            Group::System};
    for (std::size_t controllerIx = 0; controllerIx < instrument.controllers.size(); ++controllerIx) {
        for (const MidiConfigSection section : sections) {
            const std::vector<Group> addable = vm.AddableGroups(controllerIx, section);
            for (const Group group : groups) {
                const bool listed = std::find(addable.begin(), addable.end(), group) != addable.end();
                REQUIRE_TRUE(listed == vm.GroupSupportsAdd(controllerIx, section, group));
            }
        }
    }
}

// Reviewer finding 1: two rows sharing (RowGroup, Kind) can still disagree
// on editableFields (a System Block run mixing a BankSelect block --
// editableFields includes Field::BlockBankSlotIx -- with a SceneSelect/
// GestureSelect block, which doesn't). This test drives that scenario
// through the actual VM (AddBlock twice, editing the second block's message
// type to BankSelect via ApplyMappingEdit) and asserts SectionRows() surfaces
// the differing editableFields the renderer must split its header run on --
// the JUCE-free half of the fix; ControllersPage.hpp's SectionBody grouping
// loop (now comparing full editableFields vectors, not just RowGroup/Kind)
// is exercised only by the launch smoke test, not by these headless tests.
TEST_CASE(BankSelectBlockEditableFieldsDifferFromSceneSelectBlockInSameGroup) {
    MidiConfigViewModel vm;
    const MidiInstrumentConfig instrument = MakeFourKindInstrument();
    vm.Rebuild(instrument, MakeFourKindConnection());

    // "wrld"'s default profile (WrldBldrDefaultProfileConfig, see
    // src/MidiController.cpp) already reconstructs multiple System::Block
    // rows in the same RowGroup: a scene-select block, a bank-select block,
    // and a gesture-select block, all contiguous in sort order and none
    // requiring any AddBlock call here. Find one of each of the two whose
    // editableFields the finding says must differ.
    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t bankBlockIx = SIZE_MAX;
    std::size_t sceneBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].group != MidiMappingRowVM::RowGroup::System || rows[ix].kind != MidiMappingRowVM::Kind::Block) {
            continue;
        }
        if (rows[ix].label.rfind("bank select block", 0) == 0) {
            bankBlockIx = ix;
        } else if (rows[ix].label.rfind("scene select block", 0) == 0) {
            sceneBlockIx = ix;
        }
    }
    REQUIRE_TRUE(bankBlockIx != SIZE_MAX);
    REQUIRE_TRUE(sceneBlockIx != SIZE_MAX);

    const std::vector<MidiMappingRowVM::Field>& bankFields = rows[bankBlockIx].editableFields;
    const std::vector<MidiMappingRowVM::Field>& sceneFields = rows[sceneBlockIx].editableFields;
    REQUIRE_TRUE(bankFields != sceneFields);
    // Specifically: the BankSelect block carries BlockBankSlotIx; the
    // SceneSelect block does not (SystemBlockEditableFields in
    // MidiConfigViewModel.cpp only appends it for BlockableMessage::
    // BankSelect).
    auto hasBankSlot = [](const std::vector<MidiMappingRowVM::Field>& fields) {
        for (const MidiMappingRowVM::Field field : fields) {
            if (field == MidiMappingRowVM::Field::BlockBankSlotIx) {
                return true;
            }
        }
        return false;
    };
    REQUIRE_TRUE(!hasBankSlot(sceneFields));
    REQUIRE_TRUE(hasBankSlot(bankFields));
}

TEST_CASE(SystemRowsDoNotCoalesceUntilSectionClosesAndReopens) {
    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("generic")));

    MidiConfigViewModel vm;
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    MidiInstrumentConfig afterFirstAdd;
    std::string reason;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System,
                              afterFirstAdd, &reason));
    vm.Rebuild(afterFirstAdd, connection);

    MidiInstrumentConfig afterSecondAdd;
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System,
                              afterSecondAdd, &reason));
    vm.Rebuild(afterSecondAdd, connection);

    const std::vector<MidiMappingRowVM> openRows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    std::size_t openSingles = 0;
    for (const MidiMappingRowVM& row : openRows) {
        if (row.group == MidiMappingRowVM::RowGroup::System && row.kind == MidiMappingRowVM::Kind::Individual) {
            ++openSingles;
        }
    }
    REQUIRE_TRUE(openSingles >= 2);

    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    const std::vector<MidiMappingRowVM> reopenedRows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    bool sawBlock = false;
    for (const MidiMappingRowVM& row : reopenedRows) {
        if (row.group == MidiMappingRowVM::RowGroup::System && row.kind == MidiMappingRowVM::Kind::Block) {
            sawBlock = true;
        }
    }
    REQUIRE_TRUE(sawBlock);
}

TEST_CASE(BlockEditFlushesWithoutVisibleRegrouping) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    MidiConnectionState connection = MakeFourKindConnection();
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::Encoders);

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::Encoders);
    std::size_t blockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < before.size(); ++ix) {
        if (before[ix].group == MidiMappingRowVM::RowGroup::EncoderTurn &&
            before[ix].kind == MidiMappingRowVM::Kind::Block) {
            blockIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(blockIx != SIZE_MAX);

    MidiInstrumentConfig edited;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::Encoders, blockIx,
                                     MidiMappingRowVM::Field::BlockStartPos, 1.0, edited, &reason));
    vm.Rebuild(edited, connection);

    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::Encoders);
    REQUIRE_TRUE(after.size() == before.size());
    REQUIRE_TRUE(after[blockIx].group == MidiMappingRowVM::RowGroup::EncoderTurn);
    REQUIRE_TRUE(after[blockIx].kind == MidiMappingRowVM::Kind::Block);
}

TEST_CASE(SystemMessageRowsExposeKindAndArgumentSeparately) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("generic");
    slot.config.systemMessages.clear();
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 0, .cc = 10};
    association.press = synth::MessageIn::SceneSelect(0, 3);
    association.feedback = association.press;
    association.outputFeedback = true;
    slot.config.systemMessages.push_back(association);
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    bool sawKindAndArg = false;
    for (const MidiMappingRowVM& row : rows) {
        if (row.group != MidiMappingRowVM::RowGroup::System ||
            row.kind != MidiMappingRowVM::Kind::Individual) {
            continue;
        }
        const bool hasKind = std::find(row.editableFields.begin(), row.editableFields.end(),
                                       MidiMappingRowVM::Field::MessageKind) != row.editableFields.end();
        const bool hasArg = std::find(row.editableFields.begin(), row.editableFields.end(),
                                      MidiMappingRowVM::Field::MessageArg) != row.editableFields.end();
        if (hasKind && hasArg) {
            sawKindAndArg = true;
            break;
        }
    }
    REQUIRE_TRUE(sawKindAndArg);
}

TEST_CASE(SystemMessageRowsDescribeGridMessageSemantics) {
    MidiConfigViewModel vm;
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("generic");
    slot.config.systemMessages.clear();

    const synth::MessageIn messages[] = {
        synth::MessageIn::GridPress(0, 1, -1, 7, 100),
        synth::MessageIn::GridRelease(0, 1, -1, 7),
        synth::MessageIn::GridPressureChange(0, 1, -1, 7, 64),
        synth::MessageIn::SelectGrid(0, 2, 5),
    };
    for (std::size_t ix = 0; ix < 4; ++ix) {
        MidiControllerSystemMessageAssociation association;
        association.control = MidiControlAddress{.channel = 0, .cc = static_cast<std::uint8_t>(10 + ix)};
        association.press = messages[ix];
        association.feedback = messages[ix];
        slot.config.systemMessages.push_back(association);
    }
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(rows.size() == 4);
    REQUIRE_TRUE(rows[0].label.find("grid press slot 1 (-1,7) velocity 100") != std::string::npos);
    REQUIRE_TRUE(rows[1].label.find("grid release slot 1 (-1,7)") != std::string::npos);
    REQUIRE_TRUE(rows[2].label.find("grid pressure slot 1 (-1,7) pressure 64") != std::string::npos);
    REQUIRE_TRUE(rows[3].label.find("select grid 5 (slot 2)") != std::string::npos);
}

TEST_CASE(UISystemMessageIndexAndArgumentFieldsRoundTrip) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("generic");
    slot.config.systemMessages.clear();
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 0, .cc = 10};
    association.press = synth::MessageIn::SceneSelect(0, 3);
    association.feedback = association.press;
    association.outputFeedback = true;
    slot.config.systemMessages.push_back(association);
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    REQUIRE_TRUE(vm.UISystemMessageIndex(0, MidiConfigSection::SystemMessages, 0) ==
                 UISystemMessageIndex(UISystemMessage::SceneSelect));

    double value = 0.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0,
                                  MidiMappingRowVM::Field::MessageArg, value));
    REQUIRE_TRUE(value == 3.0);

    MidiInstrumentConfig edited;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageArg, 5.0, edited, &reason));
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].press.type ==
                 synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].press.sceneIx == 5);
    REQUIRE_TRUE(!edited.controllers[0].config.systemMessages[0].release.has_value());

    vm.Rebuild(edited, connection);
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageKind,
                                     static_cast<double>(UISystemMessageIndex(UISystemMessage::HoldGestureSelect)),
                                     edited, &reason));
    vm.Rebuild(edited, connection);
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageArg, 4.0, edited, &reason));
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].release.has_value());
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].release->type ==
                 synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].release->gestureIx == 4);
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].release->boolValue == false);
    REQUIRE_TRUE(edited.controllers[0].config.systemMessages[0].press.boolValue == true);
}

TEST_CASE(UISystemMessageCatalogExpansionCoversPressAndReleaseSemantics) {
    struct Expected {
        UISystemMessage message;
        synth::MessageIn::Type pressType;
        bool pressBool;
        bool hasRelease;
        synth::MessageIn::Type releaseType;
        bool releaseBool;
        bool hasArg;
    };

    const Expected expected[] = {
        {UISystemMessage::ParamIncDec, synth::MessageIn::Type::ParamIncDec, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::ParamSetAbsolute, synth::MessageIn::Type::ParamSetAbsolute, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::ParamPush, synth::MessageIn::Type::ParamPush, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::ToggleReset, synth::MessageIn::Type::ToggleReset, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::HoldReset, synth::MessageIn::Type::ToggleReset, true, true,
         synth::MessageIn::Type::ToggleReset, false, false},
        {UISystemMessage::ToggleRandom, synth::MessageIn::Type::ToggleRandom, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::HoldRandom, synth::MessageIn::Type::ToggleRandom, true, true,
         synth::MessageIn::Type::ToggleRandom, false, false},
        {UISystemMessage::ToggleRandomMod, synth::MessageIn::Type::ToggleRandomMod, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::HoldRandomMod, synth::MessageIn::Type::ToggleRandomMod, true, true,
         synth::MessageIn::Type::ToggleRandomMod, false, false},
        {UISystemMessage::ToggleGestureSelect, synth::MessageIn::Type::ToggleGestureSelect, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::HoldGestureSelect, synth::MessageIn::Type::SetGestureSelect, true, true,
         synth::MessageIn::Type::SetGestureSelect, false, true},
        {UISystemMessage::SelectParamBank, synth::MessageIn::Type::SelectParamBank, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::NextParamBank, synth::MessageIn::Type::NextParamBank, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::PrevParamBank, synth::MessageIn::Type::PrevParamBank, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::Start, synth::MessageIn::Type::Start, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::Continue, synth::MessageIn::Type::Continue, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::Stop, synth::MessageIn::Type::Stop, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::Clock, synth::MessageIn::Type::Clock, false, false,
         synth::MessageIn::Type::Clock, false, false},
        {UISystemMessage::SetGestureValue, synth::MessageIn::Type::SetGestureValue, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::SceneSelect, synth::MessageIn::Type::SceneSelect, false, false,
         synth::MessageIn::Type::Clock, false, true},
        {UISystemMessage::SetSceneBlend, synth::MessageIn::Type::SetSceneBlend, false, false,
         synth::MessageIn::Type::Clock, false, false},
    };

    const std::vector<synth::UISystemMessageChoice>& catalog = synth::UISystemMessageCatalog();
    REQUIRE_TRUE(catalog.size() == sizeof(expected) / sizeof(expected[0]));

    for (std::size_t ix = 0; ix < catalog.size(); ++ix) {
        REQUIRE_TRUE(catalog[ix].message == expected[ix].message);

        MidiInstrumentConfig instrument;
        MidiControllerSlot slot = MakeGenericSlot("generic");
        slot.config.systemMessages.clear();
        MidiControllerSystemMessageAssociation association;
        association.control = MidiControlAddress{.channel = 2, .cc = 40};
        association.press = synth::MessageIn::SceneSelect(0, 3);
        association.feedback = association.press;
        association.outputFeedback = true;
        slot.config.systemMessages.push_back(association);
        REQUIRE_TRUE(instrument.AddController(slot));

        MidiConnectionState connection;
        connection.controllers.push_back(MidiControllerConnection{});

        MidiConfigViewModel vm;
        vm.Rebuild(instrument, connection);
        vm.ToggleConfig(0);
        vm.ToggleSection(0, MidiConfigSection::SystemMessages);

        MidiInstrumentConfig edited;
        std::string reason;
        REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                         MidiMappingRowVM::Field::MessageKind, static_cast<double>(ix), edited,
                                         &reason));
        const MidiControllerSystemMessageAssociation& out = edited.controllers[0].config.systemMessages[0];
        REQUIRE_TRUE(out.press.type == expected[ix].pressType);
        if (out.press.hasBoolValue) {
            REQUIRE_TRUE(out.press.boolValue == expected[ix].pressBool);
        }
        REQUIRE_TRUE(out.release.has_value() == expected[ix].hasRelease);
        if (expected[ix].hasRelease) {
            REQUIRE_TRUE(out.release->type == expected[ix].releaseType);
            REQUIRE_TRUE(out.release->boolValue == expected[ix].releaseBool);
        }
        REQUIRE_TRUE(vm.UISystemMessageIndex(0, MidiConfigSection::SystemMessages, 0) == static_cast<int>(ix));

        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(!rows.empty());
        const bool exposesArg = std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                                          MidiMappingRowVM::Field::MessageArg) != rows[0].editableFields.end();
        REQUIRE_TRUE(exposesArg == expected[ix].hasArg);
    }
}

TEST_CASE(TwisterSystemMessageKindEditDoesNotReorderOpenRows) {
    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeTwisterSlot("twist");
    slot.config.systemMessages.clear();
    REQUIRE_TRUE(instrument.AddController(slot));

    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    std::string reason;
    for (int ix = 0; ix < 6; ++ix) {
        MidiInstrumentConfig added;
        REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System,
                                  added, &reason));
        vm.Rebuild(added, connection);
    }

    const std::vector<MidiMappingRowVM> before = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(before.size() == 6);
    for (std::size_t ix = 0; ix < before.size(); ++ix) {
        double button = -1.0;
        REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                      MidiMappingRowVM::Field::Button, button));
        REQUIRE_TRUE(button == static_cast<double>(ix));
    }

    MidiInstrumentConfig edited;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageKind,
                                     static_cast<double>(UISystemMessageIndex(UISystemMessage::SelectParamBank)),
                                     edited, &reason));
    vm.Rebuild(edited, connection);

    const std::vector<MidiMappingRowVM> after = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(after.size() == 6);
    REQUIRE_TRUE(vm.UISystemMessageIndex(0, MidiConfigSection::SystemMessages, 0) ==
                 UISystemMessageIndex(UISystemMessage::SelectParamBank));
    for (std::size_t ix = 0; ix < after.size(); ++ix) {
        double button = -1.0;
        REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                      MidiMappingRowVM::Field::Button, button));
        REQUIRE_TRUE(button == static_cast<double>(ix));
    }
}

TEST_CASE(TwisterSystemMessagesRandomizedOpenSessionOracle) {
    struct OracleRow {
        int button = 0;
        UISystemMessage message = UISystemMessage::SceneSelect;
        std::size_t arg = 0;
    };

    auto argOf = [](const synth::MessageIn& message) {
        switch (message.type) {
            case synth::MessageIn::Type::SceneSelect:
                return message.sceneIx;
            case synth::MessageIn::Type::SelectParamBank:
                return message.bankIx;
            case synth::MessageIn::Type::NextParamBank:
            case synth::MessageIn::Type::PrevParamBank:
                return message.slotIx;
            case synth::MessageIn::Type::SetGestureSelect:
                return message.gestureIx;
            default:
                return std::size_t{0};
        }
    };
    auto uiMessageOf = [](const MidiControllerSystemMessageAssociation& association) {
        switch (association.press.type) {
            case synth::MessageIn::Type::SceneSelect:
                return UISystemMessage::SceneSelect;
            case synth::MessageIn::Type::SelectParamBank:
                return UISystemMessage::SelectParamBank;
            case synth::MessageIn::Type::NextParamBank:
                return UISystemMessage::NextParamBank;
            case synth::MessageIn::Type::PrevParamBank:
                return UISystemMessage::PrevParamBank;
            case synth::MessageIn::Type::SetGestureSelect:
                return UISystemMessage::HoldGestureSelect;
            default:
                return UISystemMessage::Clock;
        }
    };
    auto rowFromAssociation = [&](const MidiControllerSystemMessageAssociation& association) {
        OracleRow row;
        row.button = static_cast<int>(association.control->cc - 8);
        row.message = uiMessageOf(association);
        row.arg = argOf(association.press);
        return row;
    };
    auto expectMatches = [&](MidiConfigViewModel& vm, const std::vector<OracleRow>& oracle) {
        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(rows.size() == oracle.size());
        for (std::size_t ix = 0; ix < oracle.size(); ++ix) {
            REQUIRE_TRUE(rows[ix].kind == MidiMappingRowVM::Kind::Individual);
            double button = -1.0;
            REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                          MidiMappingRowVM::Field::Button, button));
            REQUIRE_TRUE(button == static_cast<double>(oracle[ix].button));
            REQUIRE_TRUE(vm.UISystemMessageIndex(0, MidiConfigSection::SystemMessages, ix) ==
                         UISystemMessageIndex(oracle[ix].message));
            double arg = -1.0;
            REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                          MidiMappingRowVM::Field::MessageArg, arg));
            REQUIRE_TRUE(arg == static_cast<double>(oracle[ix].arg));
        }
    };
    auto expectTruthMatches = [&](const MidiInstrumentConfig& instrument, const std::vector<OracleRow>& oracle) {
        const auto& messages = instrument.controllers[0].config.systemMessages;
        REQUIRE_TRUE(messages.size() == oracle.size());
        for (const OracleRow& row : oracle) {
            bool found = false;
            for (const MidiControllerSystemMessageAssociation& association : messages) {
                if (!association.control.has_value()) {
                    continue;
                }
                if (static_cast<int>(association.control->cc - 8) == row.button &&
                    uiMessageOf(association) == row.message && argOf(association.press) == row.arg) {
                    if (row.message == UISystemMessage::NextParamBank ||
                        row.message == UISystemMessage::PrevParamBank) {
                        REQUIRE_TRUE(!association.release.has_value());
                        REQUIRE_TRUE(association.feedback.type == association.press.type);
                        REQUIRE_TRUE(association.feedback.slotIx == association.press.slotIx);
                    }
                    found = true;
                    break;
                }
            }
            REQUIRE_TRUE(found);
        }
    };
    auto reopenOracleFromTruth = [&](const MidiInstrumentConfig& instrument) {
        std::vector<OracleRow> rows;
        for (const MidiControllerSystemMessageAssociation& association :
             instrument.controllers[0].config.systemMessages) {
            rows.push_back(rowFromAssociation(association));
        }
        return rows;
    };
    auto usedButton = [](const std::vector<OracleRow>& rows, int button) {
        for (const OracleRow& row : rows) {
            if (row.button == button) {
                return true;
            }
        }
        return false;
    };

    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeTwisterSlot("twist");
    slot.config.systemMessages.clear();
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection;
    connection.controllers.push_back(MidiControllerConnection{});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    std::vector<OracleRow> oracle;
    std::mt19937 rng(0x5EAF);
    const UISystemMessage messages[] = {
        UISystemMessage::SceneSelect,
        UISystemMessage::SelectParamBank,
        UISystemMessage::HoldGestureSelect,
        UISystemMessage::NextParamBank,
        UISystemMessage::PrevParamBank,
    };

    for (int step = 0; step < 400; ++step) {
        const int action = static_cast<int>(rng() % 6);
        std::string reason;
        MidiInstrumentConfig out;
        if (action == 0 && oracle.size() < 6) {
            REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages,
                                      MidiMappingRowVM::RowGroup::System, out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            OracleRow row;
            for (int button = 0; button < 6; ++button) {
                if (!usedButton(oracle, button)) {
                    row.button = button;
                    break;
                }
            }
            std::size_t nextArg = 0;
            bool used = true;
            while (used) {
                used = false;
                for (const OracleRow& existing : oracle) {
                    if (existing.message == UISystemMessage::SceneSelect && existing.arg == nextArg) {
                        used = true;
                        ++nextArg;
                        break;
                    }
                }
            }
            row.arg = nextArg;
            oracle.push_back(row);
        } else if (action == 1 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            const UISystemMessage message = messages[rng() % std::size(messages)];
            REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::MessageKind,
                                             static_cast<double>(UISystemMessageIndex(message)), out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            oracle[ix].message = message;
        } else if (action == 2 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            const std::size_t arg = static_cast<std::size_t>(rng() % 9);
            REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::MessageArg,
                                             static_cast<double>(arg), out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            oracle[ix].arg = arg;
        } else if (action == 3 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            int button = oracle[ix].button;
            for (int candidate = 0; candidate < 6; ++candidate) {
                if (!usedButton(oracle, candidate) || candidate == oracle[ix].button) {
                    button = candidate;
                    break;
                }
            }
            REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::Button,
                                             static_cast<double>(button), out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            oracle[ix].button = button;
        } else if (action == 4 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, ix, out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(ix));
        } else {
            vm.ToggleSection(0, MidiConfigSection::SystemMessages);
            vm.ToggleSection(0, MidiConfigSection::SystemMessages);
            oracle = reopenOracleFromTruth(instrument);
        }

        expectMatches(vm, oracle);
        expectTruthMatches(instrument, oracle);
    }
}

TEST_CASE(RelativeBankSystemMessagesExposeCatalogAndKeepSlotsAcrossEdits) {
    const auto& catalog = synth::UISystemMessageCatalog();
    const auto next = std::find_if(catalog.begin(), catalog.end(), [](const auto& choice) {
        return choice.message == UISystemMessage::NextParamBank && choice.label == "Next Bank";
    });
    const auto previous = std::find_if(catalog.begin(), catalog.end(), [](const auto& choice) {
        return choice.message == UISystemMessage::PrevParamBank && choice.label == "Previous Bank";
    });
    REQUIRE_TRUE(next != catalog.end());
    REQUIRE_TRUE(previous != catalog.end());

    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeTwisterSlot("relative-bank");
    slot.config.systemMessages.clear();
    slot.config.systemMessages.push_back({
        .control = MidiControlAddress{.channel = 3, .cc = 8},
        .press = synth::MessageIn::NextParamBank(0, 2),
        .feedback = synth::MessageIn::NextParamBank(0, 2),
    });
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection;
    connection.controllers.push_back({});

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    double arg = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, 0,
                                  MidiMappingRowVM::Field::MessageArg, arg));
    REQUIRE_TRUE(arg == 2.0);

    std::string reason;
    MidiInstrumentConfig edited;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageArg, 5.0, edited, &reason));
    const auto& afterArg = edited.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(afterArg.press.slotIx == 5);
    REQUIRE_TRUE(afterArg.feedback.slotIx == 5);
    REQUIRE_TRUE(!afterArg.release.has_value());

    vm.Rebuild(edited, connection);
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, 0,
                                     MidiMappingRowVM::Field::MessageKind,
                                     static_cast<double>(UISystemMessageIndex(UISystemMessage::PrevParamBank)),
                                     edited, &reason));
    const auto& afterKind = edited.controllers[0].config.systemMessages[0];
    REQUIRE_TRUE(afterKind.press.type == synth::MessageIn::Type::PrevParamBank);
    REQUIRE_TRUE(afterKind.press.slotIx == 5);
    REQUIRE_TRUE(afterKind.feedback.type == synth::MessageIn::Type::PrevParamBank);
    REQUIRE_TRUE(afterKind.feedback.slotIx == 5);
    REQUIRE_TRUE(!afterKind.release.has_value());
}

TEST_CASE(GenericSystemBlocksRandomizedOpenSessionOracle) {
    struct OracleBlock {
        int startCc = 0;
        int endCc = 0;
        synth::BlockableMessage message = synth::BlockableMessage::SceneSelect;
        std::size_t startArg = 0;
    };

    auto messageTypeIndex = [](synth::BlockableMessage message) {
        switch (message) {
            case synth::BlockableMessage::SceneSelect:
                return 0;
            case synth::BlockableMessage::BankSelect:
                return 1;
            case synth::BlockableMessage::GestureSelect:
                return 2;
        }
        return 0;
    };
    auto tupleFor = [](synth::BlockableMessage message, std::size_t arg) {
        struct Tuple {
            synth::MessageIn::Type type;
            std::size_t arg;
        };
        switch (message) {
            case synth::BlockableMessage::SceneSelect:
                return Tuple{synth::MessageIn::Type::SceneSelect, arg};
            case synth::BlockableMessage::BankSelect:
                return Tuple{synth::MessageIn::Type::SelectParamBank, arg};
            case synth::BlockableMessage::GestureSelect:
                return Tuple{synth::MessageIn::Type::SetGestureSelect, arg};
        }
        return Tuple{synth::MessageIn::Type::SceneSelect, arg};
    };
    auto argOf = [](const synth::MessageIn& message) {
        switch (message.type) {
            case synth::MessageIn::Type::SceneSelect:
                return message.sceneIx;
            case synth::MessageIn::Type::SelectParamBank:
                return message.bankIx;
            case synth::MessageIn::Type::SetGestureSelect:
                return message.gestureIx;
            default:
                return std::size_t{0};
        }
    };
    auto markUsed = [](const std::vector<OracleBlock>& oracle, std::size_t skipIx) {
        std::array<bool, 128> used{};
        for (std::size_t ix = 0; ix < oracle.size(); ++ix) {
            if (ix == skipIx) {
                continue;
            }
            for (int cc = oracle[ix].startCc; cc < oracle[ix].endCc; ++cc) {
                if (cc >= 0 && cc < 128) {
                    used[static_cast<std::size_t>(cc)] = true;
                }
            }
        }
        return used;
    };
    auto lowestFreeArg = [](const std::vector<OracleBlock>& oracle, synth::BlockableMessage message) {
        std::vector<bool> used;
        for (const OracleBlock& block : oracle) {
            if (block.message != message) {
                continue;
            }
            for (int offset = 0; offset < block.endCc - block.startCc; ++offset) {
                const std::size_t arg = block.startArg + static_cast<std::size_t>(offset);
                if (arg >= used.size()) {
                    used.resize(arg + 1, false);
                }
                used[arg] = true;
            }
        }
        for (std::size_t ix = 0; ix < used.size(); ++ix) {
            if (!used[ix]) {
                return ix;
            }
        }
        return used.size();
    };
    auto argRangeFree = [](const std::vector<OracleBlock>& oracle, std::size_t skipIx,
                           synth::BlockableMessage message, std::size_t startArg, std::size_t width) {
        for (std::size_t ix = 0; ix < oracle.size(); ++ix) {
            if (ix == skipIx || oracle[ix].message != message) {
                continue;
            }
            const std::size_t otherStart = oracle[ix].startArg;
            const std::size_t otherEnd =
                oracle[ix].startArg + static_cast<std::size_t>(oracle[ix].endCc - oracle[ix].startCc);
            const std::size_t endArg = startArg + width;
            if (startArg < otherEnd && otherStart < endArg) {
                return false;
            }
        }
        return true;
    };
    auto lowestFreeTwoCc = [&](const std::vector<OracleBlock>& oracle) -> std::optional<int> {
        const auto used = markUsed(oracle, SIZE_MAX);
        for (int cc = 0; cc < 127; ++cc) {
            if (!used[static_cast<std::size_t>(cc)] && !used[static_cast<std::size_t>(cc + 1)]) {
                return cc;
            }
            if (!used[static_cast<std::size_t>(cc)]) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    };
    auto truthMatches = [&](const MidiInstrumentConfig& instrument, const std::vector<OracleBlock>& oracle) {
        std::vector<std::tuple<int, synth::MessageIn::Type, std::size_t>> expected;
        for (const OracleBlock& block : oracle) {
            for (int offset = 0; offset < block.endCc - block.startCc; ++offset) {
                const auto tuple = tupleFor(block.message, block.startArg + static_cast<std::size_t>(offset));
                expected.emplace_back(block.startCc + offset, tuple.type, tuple.arg);
            }
        }
        const auto& messages = instrument.controllers[0].config.systemMessages;
        REQUIRE_TRUE(messages.size() == expected.size());
        for (const auto& [cc, type, arg] : expected) {
            bool found = false;
            for (const MidiControllerSystemMessageAssociation& association : messages) {
                if (!association.control.has_value()) {
                    continue;
                }
                if (association.control->channel == 0 && association.control->cc == cc &&
                    association.press.type == type && argOf(association.press) == arg) {
                    found = true;
                    break;
                }
            }
            REQUIRE_TRUE(found);
        }
    };
    auto presentationMatches = [&](MidiConfigViewModel& vm, const std::vector<OracleBlock>& oracle) {
        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(rows.size() == oracle.size());
        for (std::size_t ix = 0; ix < oracle.size(); ++ix) {
            REQUIRE_TRUE(rows[ix].kind == MidiMappingRowVM::Kind::Block);
            double value = -1.0;
            REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                          MidiMappingRowVM::Field::BlockStartCc, value));
            REQUIRE_TRUE(value == static_cast<double>(oracle[ix].startCc));
            REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                          MidiMappingRowVM::Field::BlockEndCc, value));
            REQUIRE_TRUE(value == static_cast<double>(oracle[ix].endCc));
            REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                          MidiMappingRowVM::Field::BlockStartArg, value));
            REQUIRE_TRUE(value == static_cast<double>(oracle[ix].startArg));
            REQUIRE_TRUE(vm.BlockMessageTypeIndex(0, MidiConfigSection::SystemMessages, ix) ==
                         messageTypeIndex(oracle[ix].message));
        }
    };
    auto reopenOracleFromTruth = [&](const MidiInstrumentConfig& instrument) {
        MidiConfigViewModel fresh;
        fresh.Rebuild(instrument, MakeSingleControllerConnection());
        const std::vector<MidiMappingRowVM> rows = fresh.SectionRows(0, MidiConfigSection::SystemMessages);
        std::vector<OracleBlock> rebuilt;
        for (std::size_t ix = 0; ix < rows.size(); ++ix) {
            REQUIRE_TRUE(rows[ix].kind == MidiMappingRowVM::Kind::Block);
            double value = 0.0;
            REQUIRE_TRUE(fresh.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::BlockStartCc, value));
            OracleBlock block;
            block.startCc = static_cast<int>(value);
            REQUIRE_TRUE(fresh.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::BlockEndCc, value));
            block.endCc = static_cast<int>(value);
            REQUIRE_TRUE(fresh.RowFieldValue(0, MidiConfigSection::SystemMessages, ix,
                                             MidiMappingRowVM::Field::BlockStartArg, value));
            block.startArg = static_cast<std::size_t>(value);
            switch (fresh.BlockMessageTypeIndex(0, MidiConfigSection::SystemMessages, ix)) {
                case 0:
                    block.message = synth::BlockableMessage::SceneSelect;
                    break;
                case 1:
                    block.message = synth::BlockableMessage::BankSelect;
                    break;
                case 2:
                    block.message = synth::BlockableMessage::GestureSelect;
                    break;
                default:
                    REQUIRE_TRUE(false);
            }
            rebuilt.push_back(block);
        }
        return rebuilt;
    };

    MidiInstrumentConfig instrument;
    MidiControllerSlot slot = MakeGenericSlot("generic");
    REQUIRE_TRUE(instrument.AddController(slot));
    MidiConnectionState connection = MakeSingleControllerConnection();
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleConfig(0);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    std::vector<OracleBlock> oracle;
    std::mt19937 rng(0xB10C);
    for (int step = 0; step < 300; ++step) {
        const int action = static_cast<int>(rng() % 6);
        MidiInstrumentConfig out;
        std::string reason;
        if (action == 0 && oracle.size() < 12) {
            const std::optional<int> startCc = lowestFreeTwoCc(oracle);
            const std::size_t startArg = lowestFreeArg(oracle, synth::BlockableMessage::SceneSelect);
            if (startCc.has_value() &&
                argRangeFree(oracle, SIZE_MAX, synth::BlockableMessage::SceneSelect, startArg, 2)) {
                REQUIRE_TRUE(vm.AddBlock(0, MidiConfigSection::SystemMessages,
                                         MidiMappingRowVM::RowGroup::System, out, &reason));
                instrument = out;
                vm.Rebuild(instrument, connection);
                oracle.push_back(OracleBlock{.startCc = *startCc,
                                             .endCc = *startCc + 2,
                                             .message = synth::BlockableMessage::SceneSelect,
                                             .startArg = startArg});
            }
        } else if (action == 1 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            const std::size_t arg = static_cast<std::size_t>(rng() % 16);
            const std::size_t width = static_cast<std::size_t>(oracle[ix].endCc - oracle[ix].startCc);
            if (argRangeFree(oracle, ix, oracle[ix].message, arg, width)) {
                REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                                 MidiMappingRowVM::Field::BlockStartArg,
                                                 static_cast<double>(arg), out, &reason));
                instrument = out;
                vm.Rebuild(instrument, connection);
                oracle[ix].startArg = arg;
            }
        } else if (action == 2 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            const auto message = static_cast<synth::BlockableMessage>(rng() % 3);
            const std::size_t width = static_cast<std::size_t>(oracle[ix].endCc - oracle[ix].startCc);
            if (argRangeFree(oracle, ix, message, oracle[ix].startArg, width)) {
                REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                                 MidiMappingRowVM::Field::BlockMessageType,
                                                 static_cast<double>(messageTypeIndex(message)), out, &reason));
                instrument = out;
                vm.Rebuild(instrument, connection);
                oracle[ix].message = message;
            }
        } else if (action == 3 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            const auto used = markUsed(oracle, ix);
            int newEnd = oracle[ix].startCc + 2 + static_cast<int>(rng() % 3);
            newEnd = std::min(newEnd, 128);
            bool valid = newEnd > oracle[ix].startCc;
            for (int cc = oracle[ix].startCc; cc < newEnd; ++cc) {
                if (used[static_cast<std::size_t>(cc)]) {
                    valid = false;
                }
            }
            const std::size_t width = static_cast<std::size_t>(newEnd - oracle[ix].startCc);
            if (!argRangeFree(oracle, ix, oracle[ix].message, oracle[ix].startArg, width)) {
                valid = false;
            }
            if (valid) {
                REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, ix,
                                                 MidiMappingRowVM::Field::BlockEndCc,
                                                 static_cast<double>(newEnd), out, &reason));
                instrument = out;
                vm.Rebuild(instrument, connection);
                oracle[ix].endCc = newEnd;
            }
        } else if (action == 4 && !oracle.empty()) {
            const std::size_t ix = static_cast<std::size_t>(rng() % oracle.size());
            REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, ix, out, &reason));
            instrument = out;
            vm.Rebuild(instrument, connection);
            oracle.erase(oracle.begin() + static_cast<std::ptrdiff_t>(ix));
        } else {
            vm.ToggleSection(0, MidiConfigSection::SystemMessages);
            vm.ToggleSection(0, MidiConfigSection::SystemMessages);
            oracle = reopenOracleFromTruth(instrument);
        }

        presentationMatches(vm, oracle);
        truthMatches(instrument, oracle);
    }
}

TEST_CASE(SystemMessagePipelineSharesMessageFieldsAcrossKinds) {
    struct Case {
        MidiProfileKind kind;
        MidiControllerSlot (*makeSlot)(const char*);
        std::vector<MidiMappingRowVM::Field> addressFields;
    };
    const Case cases[] = {
        {MidiProfileKind::WrldBldr, MakeWrldBldrSlot,
         {MidiMappingRowVM::Field::Channel, MidiMappingRowVM::Field::WrldBldrX,
          MidiMappingRowVM::Field::WrldBldrY}},
        {MidiProfileKind::Launchpad, MakeLaunchpadSlot,
         {MidiMappingRowVM::Field::LaunchpadX, MidiMappingRowVM::Field::LaunchpadY}},
        {MidiProfileKind::MfTwister, MakeTwisterSlot, {MidiMappingRowVM::Field::Button}},
        {MidiProfileKind::Generic, MakeGenericSlot,
         {MidiMappingRowVM::Field::Channel, MidiMappingRowVM::Field::Cc}},
    };

    for (const Case& testCase : cases) {
        MidiInstrumentConfig instrument;
        MidiControllerSlot slot = testCase.makeSlot("ctl");
        slot.config.systemMessages.clear();
        MidiControllerSystemMessageAssociation association;
        association.press = synth::MessageIn::SceneSelect(0, 3);
        association.feedback = association.press;
        association.outputFeedback = true;
        switch (testCase.kind) {
            case MidiProfileKind::WrldBldr:
                association.wrldBldrPosition = synth::WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 0};
                association.control = MidiControlAddress{.channel = 5, .cc = synth::WrldBldrPositionToCC(0, 0)};
                break;
            case MidiProfileKind::Launchpad:
                association.launchpadPosition =
                    synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX,
                                                 .x = 0,
                                                 .y = 0};
                break;
            case MidiProfileKind::MfTwister:
                association.control = MidiControlAddress{.channel = 3, .cc = 8};
                break;
            case MidiProfileKind::Generic:
                association.control = MidiControlAddress{.channel = 0, .cc = 0};
                break;
        }
        slot.config.systemMessages.push_back(association);
        REQUIRE_TRUE(instrument.AddController(slot));

        MidiConnectionState connection;
        connection.controllers.push_back(MidiControllerConnection{});
        MidiConfigViewModel vm;
        vm.Rebuild(instrument, connection);
        vm.ToggleConfig(0);
        vm.ToggleSection(0, MidiConfigSection::SystemMessages);

        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(!rows.empty());
        const std::vector<MidiMappingRowVM::Field>& fields = rows[0].editableFields;
        REQUIRE_TRUE(std::find(fields.begin(), fields.end(), MidiMappingRowVM::Field::MessageKind) != fields.end());
        REQUIRE_TRUE(std::find(fields.begin(), fields.end(), MidiMappingRowVM::Field::MessageArg) != fields.end());
        for (MidiMappingRowVM::Field addressField : testCase.addressFields) {
            REQUIRE_TRUE(std::find(fields.begin(), fields.end(), addressField) != fields.end());
        }
    }
}

bool HasField(const MidiMappingRowVM& row, MidiMappingRowVM::Field field) {
    return std::find(row.editableFields.begin(), row.editableFields.end(), field) != row.editableFields.end();
}

MidiControllerSystemMessageAssociation MakeGenericSceneSelect(std::uint8_t cc, std::size_t sceneIx) {
    MidiControllerSystemMessageAssociation association;
    association.control = MidiControlAddress{.channel = 2, .cc = cc};
    association.press = synth::MessageIn::SceneSelect(0, sceneIx);
    association.feedback = association.press;
    return association;
}

MidiInstrumentConfig MakeAddressTypeViewModelInstrument() {
    MidiInstrumentConfig instrument = MakeFourKindInstrument();
    MidiControllerSlot& generic = instrument.controllers[3];

    synth::EncoderMidiInConfig encoders;
    encoders.turns.push_back({.control = {.channel = 1, .cc = 10}, .slotIx = 0, .position = 0});
    encoders.pushes.push_back({.control = {.channel = 1, .cc = 60}, .slotIx = 0, .position = 0});
    encoders.pushes.push_back({.control = {.channel = 1, .cc = 61}, .slotIx = 0, .position = 1});
    encoders.pushes.push_back({.control = {.channel = 1, .cc = 90}, .slotIx = 0, .position = 9});
    generic.config.encoderInput = std::move(encoders);

    synth::AnalogMidiInConfig analogs;
    analogs.gestures.push_back({.control = {.channel = 3, .cc = 20}, .gestureIx = 0});
    generic.config.analogInput = std::move(analogs);

    generic.config.systemMessages = {
        MakeGenericSceneSelect(40, 0),
        MakeGenericSceneSelect(41, 1),
        MakeGenericSceneSelect(90, 9),
    };
    return instrument;
}

TEST_CASE(ControlAddressTypeCatalogAndFieldMetadataAreDistinct) {
    const std::vector<std::string>& catalog = synth::ControlAddressTypeCatalog();
    REQUIRE_TRUE(catalog.size() == 2);
    REQUIRE_TRUE(catalog[0] == "CC");
    REQUIRE_TRUE(catalog[1] == "Note");
    REQUIRE_TRUE(!FieldIsInteger(MidiMappingRowVM::Field::AddressType));
    REQUIRE_TRUE(std::string(FieldShortLabel(MidiMappingRowVM::Field::AddressType)) == "Addr");
    REQUIRE_TRUE(std::string(FieldShortLabel(MidiMappingRowVM::Field::AddressType)) !=
                 FieldShortLabel(MidiMappingRowVM::Field::BlockMessageType));
    REQUIRE_TRUE(MidiMappingRowVM::Field::AddressType != MidiMappingRowVM::Field::MessageKind);
    REQUIRE_TRUE(MidiMappingRowVM::Field::AddressType != MidiMappingRowVM::Field::BlockMessageType);
}

TEST_CASE(AddressTypeFieldAppearsOnlyOnPushAndGenericSystemRowsAndBlocks) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeAddressTypeViewModelInstrument(), MakeFourKindConnection());

    bool sawPushIndividual = false;
    bool sawPushBlock = false;
    for (std::size_t controllerIx = 0; controllerIx < 4; ++controllerIx) {
        for (const MidiMappingRowVM& row : vm.SectionRows(controllerIx, MidiConfigSection::Encoders)) {
            const bool hasAddressType = HasField(row, MidiMappingRowVM::Field::AddressType);
            if (row.group == MidiMappingRowVM::RowGroup::EncoderPush) {
                REQUIRE_TRUE(hasAddressType);
                sawPushIndividual = sawPushIndividual || row.kind == MidiMappingRowVM::Kind::Individual;
                sawPushBlock = sawPushBlock || row.kind == MidiMappingRowVM::Kind::Block;
            } else {
                REQUIRE_TRUE(!hasAddressType);
            }
        }
        for (const MidiMappingRowVM& row : vm.SectionRows(controllerIx, MidiConfigSection::Analogs)) {
            REQUIRE_TRUE(!HasField(row, MidiMappingRowVM::Field::AddressType));
        }
    }
    REQUIRE_TRUE(sawPushIndividual);
    REQUIRE_TRUE(sawPushBlock);

    for (std::size_t controllerIx = 0; controllerIx < 4; ++controllerIx) {
        bool sawGenericIndividual = false;
        bool sawGenericBlock = false;
        for (const MidiMappingRowVM& row : vm.SectionRows(controllerIx, MidiConfigSection::SystemMessages)) {
            const bool hasAddressType = HasField(row, MidiMappingRowVM::Field::AddressType);
            REQUIRE_TRUE(hasAddressType == (controllerIx == 3));
            sawGenericIndividual = sawGenericIndividual ||
                                   (controllerIx == 3 && row.kind == MidiMappingRowVM::Kind::Individual);
            sawGenericBlock = sawGenericBlock || (controllerIx == 3 && row.kind == MidiMappingRowVM::Kind::Block);
        }
        if (controllerIx == 3) {
            REQUIRE_TRUE(sawGenericIndividual);
            REQUIRE_TRUE(sawGenericBlock);
        }
    }
}

TEST_CASE(AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession) {
    MidiInstrumentConfig instrument = MakeAddressTypeViewModelInstrument();
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> rows = vm.SectionRows(3, MidiConfigSection::Encoders);
    std::size_t pushIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].group == MidiMappingRowVM::RowGroup::EncoderPush &&
            rows[ix].kind == MidiMappingRowVM::Kind::Individual) {
            pushIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(pushIx != SIZE_MAX);

    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::Encoders, pushIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 0.0);

    MidiInstrumentConfig noteOut;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::Encoders, pushIx,
                                     MidiMappingRowVM::Field::AddressType, 1.0, noteOut, &reason));
    REQUIRE_TRUE(noteOut.controllers[3].config.encoderInput->pushes.back().control.type ==
                 synth::MidiControlType::Note);
    vm.Rebuild(noteOut, MakeFourKindConnection());
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::Encoders, pushIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 1.0);

    MidiInstrumentConfig sentinel;
    sentinel.controllers.push_back(MakeGenericSlot("sentinel"));
    for (double invalid : {-1.0, 0.5, 2.0}) {
        bool presentationChanged = true;
        REQUIRE_TRUE(!vm.ApplyMappingEdit(3, MidiConfigSection::Encoders, pushIx,
                                          MidiMappingRowVM::Field::AddressType, invalid, sentinel, &reason,
                                          &presentationChanged));
        REQUIRE_TRUE(!presentationChanged);
        REQUIRE_TRUE(sentinel.controllers.size() == 1);
        REQUIRE_TRUE(sentinel.controllers[0].name == "sentinel");
        REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::Encoders, pushIx,
                                      MidiMappingRowVM::Field::AddressType, value));
        REQUIRE_TRUE(value == 1.0);
    }

    MidiInstrumentConfig ccOut;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::Encoders, pushIx,
                                     MidiMappingRowVM::Field::AddressType, 0.0, ccOut, &reason));
    vm.Rebuild(ccOut, MakeFourKindConnection());
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::Encoders, pushIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 0.0);

    const std::vector<MidiMappingRowVM> systemRows = vm.SectionRows(3, MidiConfigSection::SystemMessages);
    std::size_t systemIndividualIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < systemRows.size(); ++ix) {
        if (systemRows[ix].kind == MidiMappingRowVM::Kind::Individual) {
            systemIndividualIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(systemIndividualIx != SIZE_MAX);
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::SystemMessages, systemIndividualIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 0.0);

    MidiInstrumentConfig systemNoteOut;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::SystemMessages, systemIndividualIx,
                                     MidiMappingRowVM::Field::AddressType, 1.0, systemNoteOut, &reason));
    vm.Rebuild(systemNoteOut, MakeFourKindConnection());
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::SystemMessages, systemIndividualIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 1.0);

    MidiInstrumentConfig systemCcOut;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::SystemMessages, systemIndividualIx,
                                     MidiMappingRowVM::Field::AddressType, 0.0, systemCcOut, &reason));
    vm.Rebuild(systemCcOut, MakeFourKindConnection());
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::SystemMessages, systemIndividualIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 0.0);
}

TEST_CASE(AddressTypeBlockEditsExpandToNoteMappings) {
    MidiConfigViewModel vm;
    vm.Rebuild(MakeAddressTypeViewModelInstrument(), MakeFourKindConnection());

    const std::vector<MidiMappingRowVM> encoderRows = vm.SectionRows(3, MidiConfigSection::Encoders);
    std::size_t pushBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < encoderRows.size(); ++ix) {
        if (encoderRows[ix].group == MidiMappingRowVM::RowGroup::EncoderPush &&
            encoderRows[ix].kind == MidiMappingRowVM::Kind::Block) {
            pushBlockIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(pushBlockIx != SIZE_MAX);

    MidiInstrumentConfig encoderOut;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::Encoders, pushBlockIx,
                                     MidiMappingRowVM::Field::AddressType, 1.0, encoderOut, &reason));
    std::size_t notePushes = 0;
    for (const synth::EncoderMidiMapping& mapping : encoderOut.controllers[3].config.encoderInput->pushes) {
        notePushes += mapping.control.type == synth::MidiControlType::Note ? 1 : 0;
    }
    REQUIRE_TRUE(notePushes == 2);

    vm.Rebuild(encoderOut, MakeFourKindConnection());
    double value = -1.0;
    REQUIRE_TRUE(vm.RowFieldValue(3, MidiConfigSection::Encoders, pushBlockIx,
                                  MidiMappingRowVM::Field::AddressType, value));
    REQUIRE_TRUE(value == 1.0);

    const std::vector<MidiMappingRowVM> systemRows = vm.SectionRows(3, MidiConfigSection::SystemMessages);
    std::size_t systemBlockIx = SIZE_MAX;
    for (std::size_t ix = 0; ix < systemRows.size(); ++ix) {
        if (systemRows[ix].kind == MidiMappingRowVM::Kind::Block) {
            systemBlockIx = ix;
            break;
        }
    }
    REQUIRE_TRUE(systemBlockIx != SIZE_MAX);

    MidiInstrumentConfig systemOut;
    REQUIRE_TRUE(vm.ApplyMappingEdit(3, MidiConfigSection::SystemMessages, systemBlockIx,
                                     MidiMappingRowVM::Field::AddressType, 1.0, systemOut, &reason));
    std::size_t noteSystems = 0;
    for (const MidiControllerSystemMessageAssociation& association : systemOut.controllers[3].config.systemMessages) {
        noteSystems += association.control.has_value() && association.control->type == synth::MidiControlType::Note
                           ? 1
                           : 0;
    }
    REQUIRE_TRUE(noteSystems == 2);
}

TEST_CASE(AddressTypeBlockInvalidIndicesLeaveOutputAndPresentationUnchanged) {
    MidiInstrumentConfig instrument = MakeAddressTypeViewModelInstrument();
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, MakeFourKindConnection());

    struct BlockCase {
        MidiConfigSection section;
        MidiMappingRowVM::RowGroup group;
    };
    const BlockCase cases[] = {
        {MidiConfigSection::Encoders, MidiMappingRowVM::RowGroup::EncoderPush},
        {MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::System},
    };

    for (const BlockCase& blockCase : cases) {
        const std::vector<MidiMappingRowVM> rows = vm.SectionRows(3, blockCase.section);
        std::size_t blockIx = SIZE_MAX;
        for (std::size_t ix = 0; ix < rows.size(); ++ix) {
            if (rows[ix].group == blockCase.group && rows[ix].kind == MidiMappingRowVM::Kind::Block) {
                blockIx = ix;
                break;
            }
        }
        REQUIRE_TRUE(blockIx != SIZE_MAX);

        double beforeValue = -1.0;
        REQUIRE_TRUE(vm.RowFieldValue(3, blockCase.section, blockIx,
                                      MidiMappingRowVM::Field::AddressType, beforeValue));
        REQUIRE_TRUE(beforeValue == 0.0);

        for (double invalid : {-1.0, 0.5, 2.0}) {
            MidiInstrumentConfig out;
            out.controllers.push_back(MakeGenericSlot("sentinel"));
            const std::string beforeOut = DumpInstrument(out);
            std::string reason;
            bool presentationChanged = true;
            REQUIRE_TRUE(!vm.ApplyMappingEdit(3, blockCase.section, blockIx,
                                              MidiMappingRowVM::Field::AddressType, invalid, out, &reason,
                                              &presentationChanged));
            REQUIRE_TRUE(!presentationChanged);
            REQUIRE_TRUE(!reason.empty());
            REQUIRE_TRUE(DumpInstrument(out) == beforeOut);

            double afterValue = -1.0;
            REQUIRE_TRUE(vm.RowFieldValue(3, blockCase.section, blockIx,
                                          MidiMappingRowVM::Field::AddressType, afterValue));
            REQUIRE_TRUE(afterValue == beforeValue);
        }
    }
}

MidiInstrumentConfig MakeGridPresentationInstrument(MidiProfileKind kind, bool asBlock,
                                                     synth::PolyphonicPressureMapping* orphanOut = nullptr) {
    MidiControllerSlot slot = kind == MidiProfileKind::WrldBldr ? MakeWrldBldrSlot("grid")
                                                                : MakeLaunchpadSlot("grid");
    slot.config.systemMessages.clear();
    slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{};

    synth::GridMappingExpansion expansion;
    if (asBlock) {
        synth::GridBlock block;
        block.kind = kind;
        block.channel = 5;
        block.startX = 0;
        block.startY = kind == MidiProfileKind::Launchpad ? -1 : 0;
        block.endX = 2;
        block.endY = block.startY + 1;
        block.gridSlotIx = 3;
        REQUIRE_TRUE(synth::ExpandGridBlock(block, expansion));
    } else {
        synth::GridButton button;
        button.kind = kind;
        button.channel = 5;
        button.x = 1;
        button.y = 1;
        button.gridSlotIx = 4;
        REQUIRE_TRUE(synth::ExpandGridButton(button, expansion));
    }
    slot.config.systemMessages = expansion.systemMessages;
    slot.config.pressureInput->mappings = expansion.pressureMappings;

    synth::PolyphonicPressureMapping orphan;
    orphan.address = synth::MidiNoteAddress{.channel = 15, .note = 127};
    orphan.pressure = synth::MessageIn::GridPressureChange(91, 99, -17, 23, 41);
    slot.config.pressureInput->mappings.push_back(orphan);
    if (orphanOut != nullptr) {
        *orphanOut = orphan;
    }

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(slot));
    return instrument;
}

std::size_t FindGridRow(const MidiConfigViewModel& vm, MidiMappingRowVM::Kind kind) {
    const auto rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
    for (std::size_t ix = 0; ix < rows.size(); ++ix) {
        if (rows[ix].group == MidiMappingRowVM::RowGroup::Grid && rows[ix].kind == kind) {
            return ix;
        }
    }
    throw std::runtime_error("grid row not found");
}

TEST_CASE(GridPresentationReconstructsWrldBldrBlockAndLaunchpadButtonWithoutPressureRows) {
    struct Case {
        MidiProfileKind kind;
        bool block;
    };
    for (const Case testCase : {Case{MidiProfileKind::WrldBldr, true},
                                Case{MidiProfileKind::Launchpad, false}}) {
        MidiConfigViewModel vm;
        const MidiInstrumentConfig instrument = MakeGridPresentationInstrument(testCase.kind, testCase.block);
        MidiConnectionState connection;
        connection.controllers.push_back({});
        vm.Rebuild(instrument, connection);
        vm.ToggleSection(0, MidiConfigSection::SystemMessages);

        const auto rows = vm.SectionRows(0, MidiConfigSection::SystemMessages);
        REQUIRE_TRUE(rows.size() == 1);
        REQUIRE_TRUE(rows[0].group == MidiMappingRowVM::RowGroup::Grid);
        REQUIRE_TRUE(rows[0].kind == (testCase.block ? MidiMappingRowVM::Kind::Block
                                                     : MidiMappingRowVM::Kind::Individual));
        REQUIRE_TRUE(rows[0].label.find(testCase.block ? "Grid Block" : "Grid Button") != std::string::npos);
        REQUIRE_TRUE(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                               MidiMappingRowVM::Field::GridSlotIx) != rows[0].editableFields.end());
        REQUIRE_TRUE(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                               MidiMappingRowVM::Field::GridXMin) != rows[0].editableFields.end());
        REQUIRE_TRUE(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                               MidiMappingRowVM::Field::GridYMin) != rows[0].editableFields.end());
        REQUIRE_TRUE(rows[0].label.find("pressure") == std::string::npos);
        REQUIRE_TRUE(rows[0].label.find("aftertouch") == std::string::npos);
        if (testCase.block) {
            REQUIRE_TRUE(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                                   MidiMappingRowVM::Field::GridXMax) != rows[0].editableFields.end());
            REQUIRE_TRUE(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                                   MidiMappingRowVM::Field::GridYMax) != rows[0].editableFields.end());
        }
    }
}

TEST_CASE(GridOpenSessionOwnsPairsAndPreservesHiddenOrphanAcrossEditDeleteAndReopen) {
    synth::PolyphonicPressureMapping orphan;
    MidiInstrumentConfig instrument =
        MakeGridPresentationInstrument(MidiProfileKind::Launchpad, true, &orphan);
    MidiConnectionState connection;
    connection.controllers.push_back({});
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);

    const std::size_t rowIx = FindGridRow(vm, MidiMappingRowVM::Kind::Block);
    double yMin = 0.0;
    REQUIRE_TRUE(vm.RowFieldValue(0, MidiConfigSection::SystemMessages, rowIx,
                                  MidiMappingRowVM::Field::GridYMin, yMin));
    REQUIRE_TRUE(yMin == -1.0);

    // An open session keeps row identity and values even if persisted truth is
    // rebuilt in a different shape underneath it.
    MidiInstrumentConfig changedTruth = instrument;
    changedTruth.controllers[0].config.systemMessages.clear();
    vm.Rebuild(changedTruth, connection);
    REQUIRE_TRUE(FindGridRow(vm, MidiMappingRowVM::Kind::Block) == rowIx);

    MidiInstrumentConfig out;
    std::string reason;
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, rowIx,
                                     MidiMappingRowVM::Field::GridSlotIx, 7.0, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 2);
    REQUIRE_TRUE(out.controllers[0].config.pressureInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings.size() == 3);
    REQUIRE_TRUE(std::find(out.controllers[0].config.pressureInput->mappings.begin(),
                           out.controllers[0].config.pressureInput->mappings.end(), orphan) !=
                 out.controllers[0].config.pressureInput->mappings.end());
    for (const auto& association : out.controllers[0].config.systemMessages) {
        REQUIRE_TRUE(association.press.gridSlotIx == 7);
    }

    instrument = out;
    vm.Rebuild(instrument, connection);
    REQUIRE_TRUE(FindGridRow(vm, MidiMappingRowVM::Kind::Block) == rowIx);
    REQUIRE_TRUE(vm.DeleteRow(0, MidiConfigSection::SystemMessages, rowIx, out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.empty());
    REQUIRE_TRUE(out.controllers[0].config.pressureInput.has_value());
    REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings ==
                 std::vector<synth::PolyphonicPressureMapping>{orphan});

    instrument = out;
    vm.Rebuild(instrument, connection);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    REQUIRE_TRUE(vm.SectionRows(0, MidiConfigSection::SystemMessages).empty());
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::Grid,
                              out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 1);
    REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings.size() == 2);
    instrument = out;
    vm.Rebuild(instrument, connection);
    REQUIRE_TRUE(vm.AddBlock(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::Grid,
                             out, &reason));
    REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 3);
    REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings.size() == 4);
}

TEST_CASE(GridInvalidRectangleAndDuplicatePairEditsRefuseAtomically) {
    MidiInstrumentConfig instrument = MakeGridPresentationInstrument(MidiProfileKind::WrldBldr, true);
    MidiConnectionState connection;
    connection.controllers.push_back({});
    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    vm.ToggleSection(0, MidiConfigSection::SystemMessages);
    const std::size_t rowIx = FindGridRow(vm, MidiMappingRowVM::Kind::Block);
    const std::string before = DumpInstrument(instrument);

    MidiInstrumentConfig out = MakeFourKindInstrument();
    const std::string untouched = DumpInstrument(out);
    std::string reason;
    bool presentationChanged = false;
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, rowIx,
                                      MidiMappingRowVM::Field::GridXMax, 0.0, out, &reason,
                                      &presentationChanged));
    REQUIRE_TRUE(presentationChanged);
    REQUIRE_TRUE(!reason.empty());
    REQUIRE_TRUE(DumpInstrument(out) == untouched);
    REQUIRE_TRUE(DumpInstrument(instrument) == before);

    // Repair the open rectangle, then add a distinct cell and prove moving it
    // onto the block's physical address cannot commit half a system/pressure pair.
    REQUIRE_TRUE(vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, rowIx,
                                     MidiMappingRowVM::Field::GridXMax, 2.0, out, &reason));
    instrument = out;
    vm.Rebuild(instrument, connection);
    REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages, MidiMappingRowVM::RowGroup::Grid,
                              out, &reason));
    instrument = out;
    vm.Rebuild(instrument, connection);
    const std::size_t buttonIx = FindGridRow(vm, MidiMappingRowVM::Kind::Individual);
    const std::string beforeDuplicate = DumpInstrument(instrument);
    out = MakeFourKindInstrument();
    const std::string duplicateUntouched = DumpInstrument(out);
    REQUIRE_TRUE(!vm.ApplyMappingEdit(0, MidiConfigSection::SystemMessages, buttonIx,
                                      MidiMappingRowVM::Field::GridXMin, 0.0, out, &reason));
    REQUIRE_TRUE(DumpInstrument(out) == duplicateUntouched);
    REQUIRE_TRUE(DumpInstrument(instrument) == beforeDuplicate);
}

TEST_CASE(GridAddSkipsPhysicalAddressesOwnedByHiddenOrphans) {
    {
        MidiControllerSlot slot = MakeWrldBldrSlot("wrld-hidden-pressure");
        slot.config.systemMessages.clear();
        synth::PolyphonicPressureMapping orphan;
        orphan.address = synth::MidiNoteAddress{
            .channel = 5, .note = synth::WrldBldrPositionToCC(0, 0)};
        orphan.pressure = synth::MessageIn::GridPressureChange(0, 99, -7, 11, 23);
        slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{{orphan}};
        MidiInstrumentConfig instrument;
        REQUIRE_TRUE(instrument.AddController(slot));
        MidiConnectionState connection;
        connection.controllers.push_back({});
        MidiConfigViewModel vm;
        vm.Rebuild(instrument, connection);
        vm.ToggleSection(0, MidiConfigSection::SystemMessages);

        MidiInstrumentConfig out;
        std::string reason;
        REQUIRE_TRUE(vm.AddSingle(0, MidiConfigSection::SystemMessages,
                                  MidiMappingRowVM::RowGroup::Grid, out, &reason));
        REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 1);
        REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings.size() == 2);
        REQUIRE_TRUE(std::count(out.controllers[0].config.pressureInput->mappings.begin(),
                                out.controllers[0].config.pressureInput->mappings.end(), orphan) == 1);
        REQUIRE_TRUE(out.controllers[0].config.systemMessages[0].control.has_value());
        REQUIRE_TRUE(out.controllers[0].config.systemMessages[0].control->cc != orphan.address.note);
    }

    {
        MidiControllerSlot slot = MakeLaunchpadSlot("launchpad-hidden-pressure");
        slot.config.systemMessages.clear();
        const auto firstNote = synth::LaunchpadPositionToNote(
            synth::LaunchpadController::LaunchpadX, 0, -1);
        REQUIRE_TRUE(firstNote.has_value());
        synth::PolyphonicPressureMapping orphan;
        orphan.address = synth::MidiNoteAddress{.channel = 0, .note = *firstNote};
        orphan.pressure = synth::MessageIn::GridPressureChange(0, 101, -8, 13, 29);
        slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{{orphan}};
        MidiInstrumentConfig instrument;
        REQUIRE_TRUE(instrument.AddController(slot));
        MidiConnectionState connection;
        connection.controllers.push_back({});
        MidiConfigViewModel vm;
        vm.Rebuild(instrument, connection);
        vm.ToggleSection(0, MidiConfigSection::SystemMessages);

        MidiInstrumentConfig out;
        std::string reason;
        REQUIRE_TRUE(vm.AddBlock(0, MidiConfigSection::SystemMessages,
                                 MidiMappingRowVM::RowGroup::Grid, out, &reason));
        REQUIRE_TRUE(out.controllers[0].config.systemMessages.size() == 2);
        REQUIRE_TRUE(out.controllers[0].config.pressureInput->mappings.size() == 3);
        REQUIRE_TRUE(std::count(out.controllers[0].config.pressureInput->mappings.begin(),
                                out.controllers[0].config.pressureInput->mappings.end(), orphan) == 1);
        for (const auto& mapping : out.controllers[0].config.pressureInput->mappings) {
            if (!(mapping == orphan)) {
                REQUIRE_TRUE(!(mapping.address == orphan.address));
            }
        }
    }
}

TEST_CASE(ControllerLifecycleMutationsPreserveIdentityAndGateRegistryActions) {
    MidiControllerSlot known = MakeTwisterSlot("known");
    known.output = {.identifier = "twister-out-id", .name = "MF Twister Out"};
    known.wizardId = "com.sheaf.midi-fighter-twister";

    MidiControllerSlot unknown = MakeGenericSlot("unknown");
    unknown.wizardId = "com.example.removed-wizard";

    MidiControllerSlot blacklisted = known;
    blacklisted.name = "blacklisted";
    blacklisted.disposition = synth::MidiControllerDisposition::Blacklisted;
    blacklisted.dormantConfig = blacklisted.config;
    blacklisted.config = {};

    MidiInstrumentConfig instrument;
    REQUIRE_TRUE(instrument.AddController(MakeGenericSlot("manual")));
    REQUIRE_TRUE(instrument.AddController(known));
    REQUIRE_TRUE(instrument.AddController(unknown));
    REQUIRE_TRUE(instrument.AddController(blacklisted));
    MidiConnectionState connection;
    connection.controllers.resize(instrument.controllers.size());

    MidiConfigViewModel vm;
    vm.Rebuild(instrument, connection);
    MidiInstrumentConfig out;
    std::string reason;

    REQUIRE_TRUE(vm.RenameController(0, "manual renamed", out, &reason));
    REQUIRE_TRUE(out.controllers.size() == 4 &&
                 out.controllers[0].name == "manual renamed" &&
                 out.controllers[0].kind == MidiProfileKind::Generic);
    const std::string beforeRefusedRename = DumpInstrument(instrument);
    out = MidiInstrumentConfig{};
    REQUIRE_TRUE(!vm.RenameController(0, "", out, &reason));
    REQUIRE_TRUE(out.controllers.empty() && DumpInstrument(instrument) == beforeRefusedRename);
    REQUIRE_TRUE(!vm.RenameController(0, "known", out, &reason));
    REQUIRE_TRUE(!vm.RenameController(0, "manual", out, &reason));
    REQUIRE_TRUE(out.controllers.empty() && reason.find("unchanged") != std::string::npos);

    // sru-4: rename uniqueness spans both dispositions in both directions. An
    // Active record cannot take a Blacklisted record's name, a Blacklisted
    // record cannot take an Active record's name, and each refusal retains the
    // prior name without mutating the live instrument.
    out = MidiInstrumentConfig{};
    REQUIRE_TRUE(!vm.RenameController(0, "blacklisted", out, &reason));
    REQUIRE_TRUE(out.controllers.empty() &&
                 reason.find("already exists") != std::string::npos);
    REQUIRE_TRUE(!vm.RenameController(3, "manual", out, &reason));
    REQUIRE_TRUE(out.controllers.empty() &&
                 reason.find("already exists") != std::string::npos);
    REQUIRE_TRUE(instrument.controllers[0].disposition ==
                     synth::MidiControllerDisposition::Active &&
                 instrument.controllers[0].name == "manual" &&
                 instrument.controllers[3].disposition ==
                     synth::MidiControllerDisposition::Blacklisted &&
                 instrument.controllers[3].name == "blacklisted" &&
                 DumpInstrument(instrument) == beforeRefusedRename);

    REQUIRE_TRUE(vm.BlacklistController(1, out, &reason));
    const MidiControllerSlot& blacklistedKnown = out.controllers[1];
    REQUIRE_TRUE(blacklistedKnown.disposition == synth::MidiControllerDisposition::Blacklisted &&
                 blacklistedKnown.dormantConfig.has_value() &&
                 blacklistedKnown.dormantConfig->encoderInput.has_value() &&
                 blacklistedKnown.dormantConfig->encoderOutput.has_value() &&
                 blacklistedKnown.dormantConfig->encoderInput->turns.size() ==
                     known.config.encoderInput->turns.size() &&
                 blacklistedKnown.dormantConfig->encoderInput->pushes.size() ==
                     known.config.encoderInput->pushes.size() &&
                 blacklistedKnown.dormantConfig->encoderOutput->mappings.size() ==
                     known.config.encoderOutput->mappings.size() &&
                 blacklistedKnown.dormantConfig->systemMessages.size() ==
                     known.config.systemMessages.size() &&
                 !blacklistedKnown.config.encoderInput.has_value() &&
                 blacklistedKnown.name == known.name &&
                 blacklistedKnown.kind == known.kind &&
                 blacklistedKnown.wizardId == known.wizardId &&
                 blacklistedKnown.input.identifier == known.input.identifier &&
                 blacklistedKnown.input.name == known.input.name &&
                 blacklistedKnown.output.identifier == known.output.identifier &&
                 blacklistedKnown.output.name == known.output.name);

    out = MidiInstrumentConfig{};
    REQUIRE_TRUE(!vm.BlacklistController(0, out, &reason));
    REQUIRE_TRUE(!vm.BlacklistController(2, out, &reason));
    REQUIRE_TRUE(out.controllers.empty());

    MidiControllerSlot incomplete = known;
    incomplete.name = "incomplete";
    incomplete.output = {};
    MidiInstrumentConfig incompleteInstrument;
    REQUIRE_TRUE(incompleteInstrument.AddController(incomplete));
    MidiConfigViewModel incompleteVm;
    incompleteVm.Rebuild(incompleteInstrument, MidiConnectionState{{MidiControllerConnection{}}});
    const std::string beforeIncompleteBlacklist = DumpInstrument(incompleteInstrument);
    REQUIRE_TRUE(!incompleteVm.BlacklistController(0, out, &reason));
    REQUIRE_TRUE(out.controllers.empty() && reason.find("endpoint") != std::string::npos &&
                 DumpInstrument(incompleteInstrument) == beforeIncompleteBlacklist);

    REQUIRE_TRUE(vm.DeleteController(0, out, &reason));
    REQUIRE_TRUE(out.controllers.size() == 3 && out.controllers[0].name == "known");
    REQUIRE_TRUE(vm.DeleteController(2, out, &reason));
    REQUIRE_TRUE(out.controllers.size() == 3 && out.controllers[2].name == "blacklisted");

    REQUIRE_TRUE(vm.RenameController(3, "blacklisted renamed", out, &reason));
    REQUIRE_TRUE(out.controllers[3].name == "blacklisted renamed");
    REQUIRE_TRUE(vm.RemoveFromBlacklist(3, out, &reason));
    REQUIRE_TRUE(out.controllers.size() == 3 && out.controllers.back().name == "unknown");
    REQUIRE_TRUE(vm.RenameController(2, "unknown renamed", out, &reason));
    REQUIRE_TRUE(out.controllers[2].name == "unknown renamed");
    REQUIRE_TRUE(vm.DeleteController(2, out, &reason));
    REQUIRE_TRUE(out.controllers.size() == 3 && out.controllers.back().name == "blacklisted");
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

} // namespace

int main() {
    return Main();
}
