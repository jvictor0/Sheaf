#include "synth/ControllersPageUI.hpp"
#include "synth/ControllerWizard.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "support/SourceScan.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "controllers page UI tests must not see JUCE"
#endif

#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const std::string& id)
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

const synth::ui::Node* FindParentOf(const synth::ui::NodeTree& tree, const std::string& childId)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        for (const synth::ui::NodeId& child : node.children)
        {
            if (child.value == childId)
            {
                return &node;
            }
        }
    }
    return nullptr;
}

synth::MidiControllerSlot MakeWrldBldrSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::WrldBldr;
    slot.config = synth::WrldBldrDefaultProfileConfig();
    slot.input.identifier = "wrldbldr-in-id";
    slot.input.name = "WRLD.Bldr In";
    slot.output.identifier = "wrldbldr-out-id";
    slot.output.name = "WRLD.Bldr Out";
    return slot;
}

synth::MidiControllerSlot MakeLaunchpadSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig();
    slot.input.name = "Launchpad X";
    return slot;
}

synth::MidiControllerSlot MakeGenericSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::Generic;
    return slot;
}

synth::MidiInstrumentConfig MakeInstrument()
{
    synth::MidiInstrumentConfig instrument;
    Require(instrument.AddController(MakeWrldBldrSlot("wrld")), "add wrld");
    Require(instrument.AddController(MakeLaunchpadSlot("pads")), "add pads");
    Require(instrument.AddController(MakeGenericSlot("blank")), "add blank");
    return instrument;
}

synth::MidiConnectionState MakeConnectionState()
{
    synth::MidiConnectionState state;
    state.controllers.push_back({});
    state.controllers.push_back({});
    state.controllers.push_back({});
    state.controllers[0].input.status = synth::MidiEndpointStatus::Online;
    state.controllers[0].output.status = synth::MidiEndpointStatus::Online;
    state.controllers[1].input.status = synth::MidiEndpointStatus::Offline;
    state.controllers[1].output.status = synth::MidiEndpointStatus::Unconfigured;
    state.controllers[2].input.status = synth::MidiEndpointStatus::Unconfigured;
    state.controllers[2].output.status = synth::MidiEndpointStatus::Unconfigured;
    return state;
}

struct TestHarness
{
    synth::MidiInstrumentConfig instrument = MakeInstrument();
    synth::MidiConnectionState connection = MakeConnectionState();
    synth::MidiDeviceList devices;
    std::string status;
    std::vector<std::string> persistenceEvents;
    bool commitSucceeds = true;
    bool saveSucceeds = true;
    int instrumentSnapshots = 0;
    int deviceSnapshots = 0;
    int commitAttempts = 0;
    int commits = 0;
    int saves = 0;
    std::vector<synth::ControllerWizardDescriptor> layouts;
    std::vector<synth::UISystemMessageChoice> messageCatalog;

    TestHarness()
    {
        devices.inputs.push_back({"wrldbldr-in-id", "WRLD.Bldr In"});
        devices.outputs.push_back({"wrldbldr-out-id", "WRLD.Bldr Out"});
        devices.outputs.push_back({"uid:782494201", "Midi Fighter Twister"});
    }

    synth::runtime_ui::ControllersPageSurface MakeSurface()
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] {
            ++instrumentSnapshots;
            return instrument;
        };
        callbacks.connectionState = [this] { return connection; };
        callbacks.enumerateDevices = [this] {
            ++deviceSnapshots;
            return devices;
        };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig out) {
            ++commitAttempts;
            persistenceEvents.push_back("commit");
            if (!commitSucceeds)
            {
                return false;
            }
            instrument = std::move(out);
            connection.controllers.resize(instrument.controllers.size());
            ++commits;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [this] {
            ++saves;
            persistenceEvents.push_back("save");
            return saveSucceeds;
        };
        callbacks.setStatus = [this](std::string text) { status = std::move(text); };
        callbacks.layouts = layouts;
        callbacks.messageCatalog = messageCatalog;
        return synth::runtime_ui::ControllersPageSurface(std::move(callbacks));
    }
};

void SeedGridPresentation(TestHarness& harness)
{
    for (std::size_t controllerIx : {std::size_t{0}, std::size_t{1}})
    {
        auto& slot = harness.instrument.controllers[controllerIx];
        slot.config.systemMessages.clear();
        slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{};
    }

    synth::GridMappingExpansion wrld;
    synth::GridBlock wrldBlock;
    wrldBlock.kind = synth::MidiProfileKind::WrldBldr;
    wrldBlock.channel = 5;
    wrldBlock.startX = 0;
    wrldBlock.startY = 0;
    wrldBlock.endX = 2;
    wrldBlock.endY = 1;
    wrldBlock.gridSlotIx = 3;
    Require(synth::ExpandGridBlock(wrldBlock, wrld), "expand wrld grid block");
    synth::GridButton wrldButton;
    wrldButton.kind = synth::MidiProfileKind::WrldBldr;
    wrldButton.channel = 5;
    wrldButton.x = 3;
    wrldButton.y = 3;
    wrldButton.gridSlotIx = 4;
    Require(synth::ExpandGridButton(wrldButton, wrld), "expand wrld grid button");
    harness.instrument.controllers[0].config.systemMessages = wrld.systemMessages;
    harness.instrument.controllers[0].config.pressureInput->mappings = wrld.pressureMappings;

    synth::GridMappingExpansion launchpad;
    synth::GridBlock launchpadBlock;
    launchpadBlock.kind = synth::MidiProfileKind::Launchpad;
    launchpadBlock.startX = 0;
    launchpadBlock.startY = -1;
    launchpadBlock.endX = 2;
    launchpadBlock.endY = 0;
    launchpadBlock.gridSlotIx = 7;
    Require(synth::ExpandGridBlock(launchpadBlock, launchpad), "expand launchpad grid block");
    harness.instrument.controllers[1].config.systemMessages = launchpad.systemMessages;
    harness.instrument.controllers[1].config.pressureInput->mappings = launchpad.pressureMappings;
    synth::PolyphonicPressureMapping orphan;
    orphan.address = synth::MidiNoteAddress{.channel = 15, .note = 127};
    orphan.pressure = synth::MessageIn::GridPressureChange(17, 88, -9, 12, 33);
    harness.instrument.controllers[1].config.pressureInput->mappings.push_back(orphan);
}

void TestDiscoveryRendersPortableAvailableRowsAndDiagnostics()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    synth::WizardDiscovery discovery;
    discovery.available.push_back({.wizardId = "com.sheaf.midi-fighter-twister",
                                   .displayName = "MIDI Fighter Twister",
                                   .kind = synth::MidiProfileKind::MfTwister,
                                   .input = {"twister-in", "Midi Fighter Twister"},
                                   .output = {"twister-out", "Midi Fighter Twister"}});
    discovery.unmatchedInputs.push_back({"unknown-in", "Unknown Input"});
    discovery.unmatchedOutputs.push_back({"unknown-out", "Unknown Output"});

    surface.SetDiscovery(discovery);
    const std::uint64_t discoveryRevision = surface.TreeRevision();
    surface.SetDiscovery(discovery);
    Require(surface.TreeRevision() == discoveryRevision,
            "identical discovery snapshot does not revise the portable tree");
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* row = FindNodeById(tree, "runtime.controllers.available.0");
    Require(row != nullptr, "connected-not-set-up row exists");
    // no backend paints a container's own label, so the area heading and each
    // waiting device's status text must be rendered child nodes.
    Require(row->label.empty(), "connected-not-set-up row carries no unrendered label");
    const synth::ui::Node* availableSection = FindNodeById(tree, "runtime.controllers.available");
    Require(availableSection != nullptr && availableSection->label.empty(),
            "connected-not-set-up section carries no unrendered label");
    const synth::ui::Node* heading = FindNodeById(tree, "runtime.controllers.available.heading");
    Require(heading != nullptr && heading->kind == synth::ui::NodeKind::Label &&
                heading->text == "Available controllers",
            "the block is headed Available controllers");
    Require(row->kind == synth::ui::NodeKind::StatusText,
            "a waiting device is rendered as status, not a button row");
    Require(row->text == "Midi Fighter Twister / Midi Fighter Twister: MIDI Fighter Twister",
            "the row names the waiting pair's ports and the preset that matches them");
    Require(FindNodeById(tree, "runtime.controllers.available.0.configure") == nullptr &&
                FindNodeById(tree, "runtime.controllers.available.0.ignore") == nullptr,
            "the waiting-device row offers no Configure or Ignore action");
    Require(!row->action.has_value(), "the waiting-device row dispatches no action");
    const synth::ui::Node* otherInputs =
        FindNodeById(tree, "runtime.controllers.available.unmatched_inputs");
    Require(otherInputs != nullptr && otherInputs->text == "Other inputs: Unknown Input",
            "every other connected input is listed as Other inputs");
    const synth::ui::Node* otherOutputs =
        FindNodeById(tree, "runtime.controllers.available.unmatched_outputs");
    Require(otherOutputs != nullptr && otherOutputs->text == "Other outputs: Unknown Output",
            "every other connected output is listed as Other outputs");

    discovery.available.clear();
    surface.SetDiscovery(std::move(discovery));
    Require(surface.TreeRevision() == discoveryRevision + 1,
            "changed discovery snapshot revises the portable tree exactly once");
    const synth::ui::NodeTree emptyTree = surface.BuildTree();
    const synth::ui::Node* empty = FindNodeById(emptyTree, "runtime.controllers.available.empty");
    Require(empty != nullptr && empty->text == "No connected controller is waiting to be set up",
            "with nothing waiting the block explains there is nothing to set up");
}

void TestConnectedNotSetUpListsDevicesWithoutActions()
{
    // Two presets that share the exact same aliases, the way frogg3rs's two
    // APC40 mkII presets (Generic and Ableton) do, plus a third preset whose
    // device is only half connected.
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back({.id = "shared.device.generic",
                                      .displayName = "Shared Device (Generic)",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Shared Device"},
                                      .outputAliases = {"Shared Device"},
                                      .config = {}});
    catalog.deviceDefaults.push_back({.id = "shared.device.ableton",
                                      .displayName = "Shared Device (Ableton)",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Shared Device"},
                                      .outputAliases = {"Shared Device"},
                                      .config = {}});
    catalog.deviceDefaults.push_back({.id = "solo.device",
                                      .displayName = "Solo Device",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Solo Device"},
                                      .outputAliases = {"Solo Device"},
                                      .config = {}});
    const std::vector<synth::ControllerWizardDescriptor> layouts =
        synth::MakeControllerWizardRegistry(catalog);

    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    harness.layouts = layouts;
    harness.devices.inputs.clear();
    harness.devices.outputs.clear();
    harness.devices.inputs.push_back({"shared-in", "Shared Device"});
    harness.devices.outputs.push_back({"shared-out", "Shared Device"});
    // Solo Device is a recognized preset's device, but only its input is
    // connected: half a pair, so it cannot be a waiting device.
    harness.devices.inputs.push_back({"solo-in", "Solo Device"});
    // Random Input matches no preset's aliases at all.
    harness.devices.inputs.push_back({"random-in", "Random Input"});
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetDiscovery(
        synth::DiscoverControllerWizards(harness.devices, harness.instrument, layouts));
    surface.MarkDirty();
    surface.RefreshOnTick();

    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* heading =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kAvailableHeading);
    Require(heading != nullptr && heading->text == "Available controllers",
            "the block is headed Available controllers");

    const synth::ui::Node* row =
        FindNodeById(tree, synth::runtime_ui::NodeIds::AvailableRow(0));
    Require(row != nullptr && row->text.find("Shared Device / Shared Device") != std::string::npos,
            "the device matching two presets is listed once, by its port names");
    Require(row->text.find("Shared Device (Generic)") != std::string::npos &&
                row->text.find("Shared Device (Ableton)") != std::string::npos,
            "the row names both presets that match it");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::AvailableRow(1)) == nullptr,
            "the half-connected and unmatched ports are not waiting devices");

    const synth::ui::Node* otherInputs =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kAvailableUnmatchedInputs);
    Require(otherInputs != nullptr && otherInputs->text.rfind("Other inputs: ", 0) == 0,
            "unclaimed inputs are headed Other inputs");
    Require(otherInputs->text.find("Solo Device") != std::string::npos,
            "a recognized device with only one port present is listed under Other");
    Require(otherInputs->text.find("Random Input") != std::string::npos,
            "a port no preset matches is listed under Other");

    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id.value.rfind(synth::runtime_ui::NodeIds::kAvailable, 0) == 0)
        {
            Require(!node.action.has_value(),
                    ("no action anywhere in the block: " + node.id.value).c_str());
        }
    }

    // With nothing waiting, the block explains there is nothing to set up.
    synth::MidiDeviceList noDevices;
    surface.SetEnumerateDevices(noDevices);
    surface.SetDiscovery(synth::DiscoverControllerWizards(noDevices, harness.instrument, layouts));
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree emptyTree = surface.BuildTree();
    const synth::ui::Node* empty = FindNodeById(emptyTree, synth::runtime_ui::NodeIds::kAvailableEmpty);
    Require(empty != nullptr && empty->text == "No connected controller is waiting to be set up",
            "with nothing waiting the block explains there is nothing to set up");
}

std::string VisibleTextLower(const synth::ui::NodeTree& tree);


void TestSaveFailureKeepsTheCommittedEditAndReportsIt()
{
    TestHarness harness;
    harness.saveSucceeds = false;
    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);
    surface.ViewModel().ToggleSection(0, synth::MidiConfigSection::Encoders);
    surface.MarkDirty();
    surface.RefreshOnTick();

    const std::vector<synth::MidiMappingRowVM> encoderRows =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> turnStepRowIx;
    for (std::size_t ix = 0; ix < encoderRows.size(); ++ix)
    {
        for (synth::MidiMappingRowVM::Field field : encoderRows[ix].editableFields)
        {
            if (field == synth::MidiMappingRowVM::Field::TurnStep)
            {
                turnStepRowIx = ix;
                break;
            }
        }
        if (turnStepRowIx.has_value())
        {
            break;
        }
    }
    Require(turnStepRowIx.has_value(), "find an editable field to commit");

    const std::string value = "0:encoders:" + std::to_string(*turnStepRowIx) + ":" +
                              std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::TurnStep)) +
                              ":0.25";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, value));

    Require(harness.commitAttempts == 1 && harness.commits == 1 && harness.saves == 1,
            "a mapping field edit commits and attempts a save that fails");
    Require(harness.instrument.controllers[0].config.encoderInput.has_value() &&
                harness.instrument.controllers[0].config.encoderInput->turnStep == 0.25f,
            "the edit stays committed even though the save failed");
    Require(surface.StatusText() ==
                "The controller was committed, but runtime configuration save failed",
            "the save failure status reaches the player");
}

void TestAddFromPresetWithNoDeviceInstallsTheDefaultPresetWithNoneEndpoints()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    // No preset-draft action dispatched: this proves the row's displayed
    // default (the Preset combo's first option -- here the library's only
    // descriptor, the Twister) is what Add actually installs, not merely
    // what the combo happens to show.
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));

    Require(harness.commits == 1, "add-from-preset with the untouched default commits");
    Require(harness.instrument.controllers.size() == 4, "add-from-preset appends one controller");
    const synth::MidiControllerSlot& added = harness.instrument.controllers[3];
    Require(added.name == "MIDI Fighter Twister", "the installed record takes the preset's display name");
    Require(added.kind == synth::MidiProfileKind::MfTwister &&
                added.wizardId == "com.sheaf.midi-fighter-twister",
            "the installed record carries the preset's kind and wizard id");
    Require(added.config.encoderInput.has_value() && added.config.encoderInput->turns.size() == 16,
            "the installed record carries the preset's generated config");
    Require(!added.input.IsConfigured() && !added.output.IsConfigured(),
            "with no matching device pair both ports are left unset, reading (none)");
}

void TestAddFromPresetWithMatchingOnlinePairBindsBothEndpoints()
{
    TestHarness harness;
    // The harness already carries an unclaimed "Midi Fighter Twister" output
    // (uid:782494201); adding the matching input completes the pair the
    // library Twister descriptor's aliases need.
    harness.devices.inputs.push_back({"twister-in-id", "Midi Fighter Twister"});
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "com.sheaf.midi-fighter-twister"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));

    Require(harness.commits == 1, "add-from-preset with a matching pair commits");
    Require(harness.instrument.controllers.size() == 4, "add-from-preset appends one controller");
    const synth::MidiControllerSlot& added = harness.instrument.controllers[3];
    Require(added.input.identifier == "twister-in-id" && added.input.name == "Midi Fighter Twister",
            "an unclaimed matching input is bound");
    Require(added.output.identifier == "uid:782494201" && added.output.name == "Midi Fighter Twister",
            "an unclaimed matching output is bound");
    Require(added.kind == synth::MidiProfileKind::MfTwister &&
                added.wizardId == "com.sheaf.midi-fighter-twister",
            "the installed record still carries the preset's kind and wizard id");
}

void TestAddCustomGenericYieldsAnEmptyGenericRecord()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "custom"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));

    Require(harness.commits == 1, "add Custom commits");
    Require(harness.instrument.controllers.size() == 4, "add Custom appends one controller");
    const synth::MidiControllerSlot& added = harness.instrument.controllers[3];
    Require(added.name == "Custom", "a Custom add is named Custom");
    Require(added.kind == synth::MidiProfileKind::Generic && added.wizardId == std::nullopt,
            "a Custom add is always the Generic kind and carries no wizard id");
    Require(!added.config.encoderInput.has_value() && !added.input.IsConfigured() &&
                !added.output.IsConfigured(),
            "a Custom add seeds an empty record: no encoder block, no endpoints");
}

void TestAddedRowOpensWithEverySectionOpen()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    // An added preset shows its mappings: expanded, with every section open.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "com.sheaf.midi-fighter-twister"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.instrument.controllers.size() == 1, "preset add installs one row");
    Require(surface.ViewModel().Controllers()[0].configExpanded, "an added preset row starts expanded");
    for (synth::MidiConfigSection section : surface.ViewModel().Controllers()[0].sections)
    {
        Require(surface.ViewModel().SectionExpanded(0, section), "every section the row lists starts open");
    }
    const std::vector<synth::MidiMappingRowVM> encoderRows =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    Require(!encoderRows.empty(), "the preset's mapping entries are in the page's tree");

    // The player can still collapse it: the disclosure, then a section toggle.
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    Require(!surface.ViewModel().Controllers()[0].configExpanded, "pressing the disclosure collapses the row");
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    Require(surface.ViewModel().Controllers()[0].configExpanded, "re-expanding for the section-toggle check");
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "0:encoders"));
    Require(!surface.ViewModel().SectionExpanded(0, synth::MidiConfigSection::Encoders),
            "pressing a section toggle closes that section");

    // An added Custom row opens too.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "custom"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.instrument.controllers.size() == 2, "custom add installs a second row");
    Require(surface.ViewModel().Controllers()[1].configExpanded, "an added Custom row starts expanded");
    for (synth::MidiConfigSection section : surface.ViewModel().Controllers()[1].sections)
    {
        Require(surface.ViewModel().SectionExpanded(1, section), "the Custom row's own sections start open too");
    }

    // A row that appears any other way -- here, loaded out of band rather
    // than added through the page -- still starts collapsed.
    harness.instrument.controllers.push_back(MakeGenericSlot("out-of-band"));
    harness.connection.controllers.resize(harness.instrument.controllers.size());
    surface.MarkDirty();
    surface.RefreshOnTick();
    Require(!surface.ViewModel().Controllers()[2].configExpanded,
            "a row that appears any other way starts collapsed");
}

void TestAddPresetDropdownListsRegistryDescriptorsThenOneCustomEntry()
{
    // Without a catalog: the library fallback registry (Twister, Launchpad,
    // WRLD.Bldr), then exactly one Custom entry, not one Custom(<kind>)
    // entry per registry descriptor.
    const std::vector<synth::ControllerWizardDescriptor> libraryRegistry =
        synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{});
    const std::vector<synth::ui::ControlOption> libraryOptions =
        synth::runtime_ui::ControllersLayout::BuildAddPresetOptions(libraryRegistry);
    Require(libraryOptions.size() == libraryRegistry.size() + 1,
            "the library-registry dropdown is the registry plus exactly one Custom entry");
    for (std::size_t ix = 0; ix < libraryRegistry.size(); ++ix)
    {
        Require(libraryOptions[ix].id == libraryRegistry[ix].id &&
                    libraryOptions[ix].label == libraryRegistry[ix].displayName,
                "each registry descriptor keeps its own id and display name in the dropdown");
    }
    Require(libraryOptions.back().id == "custom" && libraryOptions.back().label == "Custom",
            "the dropdown's last entry is the plain Custom option");

    // With a catalog: an app-supplied registry of arbitrary size gets the
    // same treatment -- its descriptors, then one Custom entry.
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back({.id = "app.device.one",
                                      .displayName = "App Device One",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"App Device One"},
                                      .outputAliases = {"App Device One"},
                                      .config = {}});
    catalog.deviceDefaults.push_back({.id = "app.device.two",
                                      .displayName = "App Device Two",
                                      .kind = synth::MidiProfileKind::MfTwister,
                                      .inputAliases = {"App Device Two"},
                                      .outputAliases = {"App Device Two"},
                                      .config = {}});
    const std::vector<synth::ControllerWizardDescriptor> appRegistry =
        synth::MakeControllerWizardRegistry(catalog);
    // This catalog covers Generic and MfTwister, so a device must still stay
    // reachable for Launchpad and WRLD.Bldr: the registry appends a library
    // descriptor for each, before the dropdown's one Custom entry (frogg3rs'
    // real shape: its six devices cover MfTwister/Generic/Launchpad, so only
    // WRLD.Bldr gets appended there).
    Require(appRegistry.size() == 4, "the app registry is its 2 catalog devices plus library Launchpad and"
                                     " WRLD.Bldr, the 2 kinds this catalog does not cover");
    const std::vector<synth::ui::ControlOption> appOptions =
        synth::runtime_ui::ControllersLayout::BuildAddPresetOptions(appRegistry);
    Require(appOptions.size() == 5, "an app catalog's dropdown is its 2 devices, the 2 appended library"
                                    " descriptors, and one Custom entry");
    Require(appOptions[0].id == "app.device.one" && appOptions[1].id == "app.device.two",
            "an app catalog's descriptors keep their own order and ids first");
    Require(appOptions[2].id == "library.launchpad" && appOptions[3].id == "library.wrldbldr",
            "library descriptors for uncovered kinds follow the catalog's own devices");
    Require(appOptions.back().id == "custom" && appOptions.back().label == "Custom",
            "an app catalog's dropdown also ends in exactly one Custom entry");
}

void TestAddLibraryLaunchpadAndWrldBldrGiveDefaultConfigAndDeviceLabel()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "library.launchpad"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 1, "add library Launchpad commits");
    Require(harness.instrument.controllers.size() == 1, "add library Launchpad appends one controller");
    const synth::MidiControllerSlot& launchpad = harness.instrument.controllers[0];
    Require(launchpad.kind == synth::MidiProfileKind::Launchpad, "the Launchpad row carries the Launchpad kind");
    Require(launchpad.wizardId == "library.launchpad", "the Launchpad row carries the library descriptor's id");
    const synth::MidiControllerProfileConfig expectedLaunchpad = synth::LaunchpadDefaultProfileConfig();
    Require(launchpad.config.systemMessages.size() == expectedLaunchpad.systemMessages.size(),
            "the Launchpad row's config matches the library default's system messages");

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "library.wrldbldr"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 2, "add library WRLD.Bldr commits");
    Require(harness.instrument.controllers.size() == 2, "add library WRLD.Bldr appends one controller");
    const synth::MidiControllerSlot& wrldbldr = harness.instrument.controllers[1];
    Require(wrldbldr.kind == synth::MidiProfileKind::WrldBldr, "the WRLD.Bldr row carries the WrldBldr kind");
    Require(wrldbldr.wizardId == "library.wrldbldr", "the WRLD.Bldr row carries the library descriptor's id");
    const synth::MidiControllerProfileConfig expectedWrldBldr = synth::WrldBldrDefaultProfileConfig();
    Require(wrldbldr.config.systemMessages.size() == expectedWrldBldr.systemMessages.size(),
            "the WRLD.Bldr row's config matches the library default's system messages");

    harness.connection.controllers.resize(harness.instrument.controllers.size());
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* launchpadDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(0));
    const synth::ui::Node* wrldbldrDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(1));
    Require(launchpadDevice != nullptr && launchpadDevice->text == "Launchpad",
            "the Launchpad row's device label reads the library descriptor's display name");
    Require(wrldbldrDevice != nullptr && wrldbldrDevice->text == "WRLD.Bldr",
            "the WRLD.Bldr row's device label reads the library descriptor's display name");
}

void TestAddRowStartsOnTheFirstWaitingDevicesPreset()
{
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back({.id = "device.unconnected",
                                      .displayName = "Unconnected Device",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Unconnected Device"},
                                      .outputAliases = {"Unconnected Device"},
                                      .config = {}});
    catalog.deviceDefaults.push_back({.id = "device.waiting",
                                      .displayName = "Waiting Device",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Waiting Device"},
                                      .outputAliases = {"Waiting Device"},
                                      .config = {}});
    const std::vector<synth::ControllerWizardDescriptor> layouts =
        synth::MakeControllerWizardRegistry(catalog);

    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    harness.layouts = layouts;
    harness.devices.inputs.clear();
    harness.devices.outputs.clear();
    harness.devices.inputs.push_back({"waiting-in", "Waiting Device"});
    harness.devices.outputs.push_back({"waiting-out", "Waiting Device"});
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetDiscovery(
        synth::DiscoverControllerWizards(harness.devices, harness.instrument, layouts));
    surface.MarkDirty();
    surface.RefreshOnTick();

    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* addPreset = FindNodeById(tree, synth::runtime_ui::NodeIds::kAddPreset);
    Require(addPreset != nullptr && addPreset->selectedOption == "device.waiting",
            "the add row defaults to the waiting device's own preset, not the registry's first descriptor");

    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.instrument.controllers.size() == 1 &&
                harness.instrument.controllers[0].input.identifier == "waiting-in" &&
                harness.instrument.controllers[0].output.identifier == "waiting-out",
            "pressing Add without touching the combo installs the waiting device's preset, bound to it");

    // The pair is now claimed and no longer waiting, so with still no chosen
    // draft the add row falls back to the registry's first descriptor.
    surface.SetDiscovery(
        synth::DiscoverControllerWizards(harness.devices, harness.instrument, layouts));
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree treeAfterAdd = surface.BuildTree();
    const synth::ui::Node* addPresetAfterAdd =
        FindNodeById(treeAfterAdd, synth::runtime_ui::NodeIds::kAddPreset);
    Require(addPresetAfterAdd != nullptr && addPresetAfterAdd->selectedOption == "device.unconnected",
            "once the device is claimed, the add row falls back to the registry's first descriptor");
}

void TestAddBindsAConnectedDeviceForEveryPresetItMatches()
{
    // Two presets sharing the same aliases, the way frogg3rs's two APC40
    // mkII presets (Generic and Ableton) do. Discovery binds a connected
    // pair to whichever of the two comes first in the registry.
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back({.id = "shared.device.generic",
                                      .displayName = "Shared Device (Generic)",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Shared Device"},
                                      .outputAliases = {"Shared Device"},
                                      .config = {}});
    catalog.deviceDefaults.push_back({.id = "shared.device.ableton",
                                      .displayName = "Shared Device (Ableton)",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {"Shared Device"},
                                      .outputAliases = {"Shared Device"},
                                      .config = {}});
    const std::vector<synth::ControllerWizardDescriptor> layouts =
        synth::MakeControllerWizardRegistry(catalog);

    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    harness.layouts = layouts;
    harness.devices.inputs.clear();
    harness.devices.outputs.clear();
    harness.devices.inputs.push_back({"shared-in", "Shared Device"});
    harness.devices.outputs.push_back({"shared-out", "Shared Device"});
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Adding the SECOND preset first still binds both ports, even though
    // discovery assigned this pair to the first descriptor in the registry.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "shared.device.ableton"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 1, "add binds a device for the second matching preset");
    Require(harness.instrument.controllers.size() == 1, "the Ableton preset installs one row");
    Require(harness.instrument.controllers[0].input.identifier == "shared-in" &&
                harness.instrument.controllers[0].output.identifier == "shared-out",
            "the second preset binds both ports of the shared device");

    // The pair is now claimed by that row, so a later add of the first
    // preset leaves both ports unbound.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "shared.device.generic"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 2, "add still commits the generic preset");
    Require(harness.instrument.controllers.size() == 2, "the generic preset installs a second row");
    Require(!harness.instrument.controllers[1].input.IsConfigured() &&
                !harness.instrument.controllers[1].output.IsConfigured(),
            "a later add of the first preset leaves both ports (none) because the pair is in use");
}

void TestSystemMessageRowShowsAStoredKindTheCatalogLacks()
{
    // A catalog that carries only Param Inc/Dec, as frogg3rs's own catalog
    // lacks the library's Hold kinds.
    synth::MidiAppCatalog catalog;
    catalog.libraryKinds = {synth::UISystemMessage::ParamIncDec};
    const std::vector<synth::UISystemMessageChoice> messageCatalog =
        synth::MakeUISystemMessageChoices(catalog);
    Require(messageCatalog.size() == 1 && messageCatalog.front().label == "Param Inc/Dec",
            "the test catalog carries only Param Inc/Dec, lacking the Hold kinds");

    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    harness.messageCatalog = messageCatalog;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "library.wrldbldr"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.instrument.controllers.size() == 1, "the WRLD.Bldr preset installs one row");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    const std::size_t rowCount =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::SystemMessages).size();

    std::vector<std::string> shownKindNames;
    for (std::size_t rowIx = 0; rowIx < rowCount; ++rowIx)
    {
        const synth::ui::Node* combo = FindNodeById(
            tree, synth::runtime_ui::NodeIds::MappingField(0, synth::MidiConfigSection::SystemMessages, rowIx,
                                                            synth::MidiMappingRowVM::Field::MessageKind));
        if (combo == nullptr)
        {
            continue;
        }
        for (const synth::ui::ControlOption& option : combo->options)
        {
            if (option.id == combo->selectedOption)
            {
                shownKindNames.push_back(option.label);
                break;
            }
        }
    }

    const auto shows = [&](const char* name) {
        return std::find(shownKindNames.begin(), shownKindNames.end(), name) != shownKindNames.end();
    };
    Require(shows("Hold Reset") && shows("Hold Random") && shows("Hold Random Mod") &&
                shows("Hold Gesture Select"),
            "each System Messages row shows its own stored kind, which the catalog lacks");
    Require(!shows("Param Inc/Dec"),
            "no row silently falls back to the combo's unrelated first option");
}

void TestEndpointSelectorsPreferTheExactStoredIdentifier()
{
    TestHarness harness;
    harness.devices.inputs.clear();
    harness.devices.outputs.clear();
    harness.devices.inputs.push_back({"twister-in-1", "Midi Fighter Twister"});
    harness.devices.inputs.push_back({"twister-in-2", "Midi Fighter Twister"});
    harness.devices.outputs.push_back({"twister-out-1", "Midi Fighter Twister"});
    harness.devices.outputs.push_back({"twister-out-2", "Midi Fighter Twister"});
    harness.instrument.controllers.clear();
    for (const char* ordinal : {"1", "2"})
    {
        synth::MidiControllerSlot slot;
        slot.name = std::string("twister ") + ordinal;
        slot.kind = synth::MidiProfileKind::MfTwister;
        slot.config = synth::MfTwisterDefaultProfileConfig();
        slot.wizardId = "com.sheaf.midi-fighter-twister";
        slot.input = {.identifier = std::string("twister-in-") + ordinal, .name = "Midi Fighter Twister"};
        slot.output = {.identifier = std::string("twister-out-") + ordinal, .name = "Midi Fighter Twister"};
        Require(harness.instrument.AddController(slot), "add duplicate-name twister");
    }
    harness.connection.controllers.assign(harness.instrument.controllers.size(), {});
    for (auto& controller : harness.connection.controllers)
    {
        controller.input.status = synth::MidiEndpointStatus::Online;
        controller.output.status = synth::MidiEndpointStatus::Online;
    }

    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 900.0f, 700.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    // Duplicate units share a device name, so a name-only match would select
    // the same endpoint for both rows. Reconciliation identity semantics put
    // the exact identifier first.
    for (std::size_t controllerIx = 0; controllerIx < 2; ++controllerIx)
    {
        const std::string ordinal = std::to_string(controllerIx + 1);
        const synth::ui::Node* input =
            FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerInput(controllerIx));
        const synth::ui::Node* output =
            FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerOutput(controllerIx));
        Require(input != nullptr && input->selectedOption == "twister-in-" + ordinal,
                "input selector resolves its own duplicate-name device");
        Require(output != nullptr && output->selectedOption == "twister-out-" + ordinal,
                "output selector resolves its own duplicate-name device");
    }
}

void TestNoHandRolledControllerNodesSurvive()
{
    const auto findRepoRoot = [](std::filesystem::path prefix) {
        while (!prefix.empty())
        {
            if (std::filesystem::exists(prefix / "projects/synth/include/synth/ControllersPageUI.hpp"))
            {
                return prefix;
            }
            const std::filesystem::path next = prefix.parent_path();
            if (next == prefix)
            {
                break;
            }
            prefix = next;
        }
        throw std::runtime_error("missing repo root for source scan test");
    };

    struct RestoreCurrentPath
    {
        std::filesystem::path path;
        ~RestoreCurrentPath() { std::filesystem::current_path(path); }
    } restore{std::filesystem::current_path()};

    const std::filesystem::path repoRoot = findRepoRoot(restore.path);
    std::filesystem::current_path(repoRoot / "projects/synth");
    Require(!synth::test::ReadSourceFile("projects/synth/include/synth/ControllersPageUI.hpp").empty(),
            "source scan resolves repo-relative paths from a nested working directory");
    std::filesystem::current_path(restore.path);

    const std::filesystem::path temp =
        std::filesystem::temp_directory_path() / "sheaf-source-scan-round1.cpp";
    {
        std::ofstream out(temp);
        out << "// ui::Node commented; commented.kind = synth::ui::NodeKind::Root;\n"
               "void clean() {}\n";
    }
    Require(!synth::test::SourceAssemblesUiNodeByHand(temp),
            "source scan ignores commented node examples");
    {
        std::ofstream out(temp);
        out << "void brace() { ui::Node node{}; }\n";
    }
    Require(synth::test::SourceAssemblesUiNodeByHand(temp),
            "source scan catches brace-initialized ui::Node construction");
    {
        std::ofstream out(temp);
        out << "void copy() { synth::ui::Node node = synth::ui::Node{}; }\n";
    }
    Require(synth::test::SourceAssemblesUiNodeByHand(temp),
            "source scan catches copy-initialized ui::Node construction");
    {
        // A plain copy out of a tree is hand assembly too, and the predicate is
        // named for that breadth rather than pretending to be narrower than it
        // is. This pins the case that used to make its old name a
        // lie.
        std::ofstream out(temp);
        out << "void alias(const std::vector<ui::Node>& nodes) { ui::Node node = nodes.front(); }\n";
    }
    Require(synth::test::SourceAssemblesUiNodeByHand(temp),
            "source scan catches a ui::Node copied out of a tree into a local");
    {
        // The other half of the contract, and the one that keeps the scan from
        // condemning every consumer: taking a node by parameter or reference,
        // or holding a container of them, is not assembling one.
        std::ofstream out(temp);
        out << "void consume(const ui::Node& node, std::vector<synth::ui::Node>& out) {\n"
               "    out.push_back(node);\n"
               "}\n"
               "float widthOf(synth::ui::Node node) { return node.bounds.width; }\n";
    }
    Require(!synth::test::SourceAssemblesUiNodeByHand(temp),
            "source scan does not flag ui::Node parameters, references, or containers");

    // Inspection over every runtime producer source, not just the two
    // this suite grew up with. `RuntimePages.hpp` joined the set
    // when `BuildSidebarTree` moved onto the library; it was the last runtime
    // page code hand-rolling nodes.
    for (const char* file : {"projects/synth/include/synth/ControllersPageUI.hpp",
                             "projects/synth/include/synth/RuntimePages.hpp",
                             "projects/synth/src/ControllerWizard.cpp"})
    {
        Require(!synth::test::SourceAssemblesUiNodeByHand(file),
                (std::string(file) + " no longer assembles ui::Node values by hand").c_str());
    }
    // Anti-vacuity for the sweep above: a predicate that had quietly started
    // returning false for everything would pass all three. The shell is the one
    // place that legitimately hand-places already-resolved subtree roots, so it
    // is the fixture that proves the predicate still fires on real repository
    // source.
    Require(synth::test::SourceAssemblesUiNodeByHand(
                "projects/synth/include/synth/RuntimeMainComponent.hpp"),
            "the shell's deliberate composition root keeps the scan honest");
}

void TestControllersSectionsNestThroughLibraryContainers()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetContentBounds({0.0f, 0.0f, 360.0f, 560.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection,
                                     "0:system_messages"));

    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* scroll =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kScroll);
    Require(scroll != nullptr && scroll->kind == synth::ui::NodeKind::ScrollArea,
            "the mapping list lives in a real scroll area");
    Require(!scroll->children.empty(), "the Controllers scroll area has nested children");
    const synth::ui::Node* row =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRow(0));
    Require(row != nullptr && row->kind == synth::ui::NodeKind::Section,
            "controller list entries are Section containers stacking their two header lines");
    Require(FindParentOf(tree, row->id.value) == scroll,
            "controller list rows are nested under the scroll area");
    const synth::ui::Node* section =
        FindNodeById(tree,
                     synth::runtime_ui::NodeIds::SectionBody(
                         0, synth::MidiConfigSection::SystemMessages));
    Require(section != nullptr && section->kind == synth::ui::NodeKind::Section,
            "expanded mapping groups are section containers");
    Require(FindParentOf(tree, section->id.value) == scroll,
            "expanded mapping sections remain scroll-content children");
    Require(section->children.size() > 1, "mapping sections carry real nested row children");
    Require(section->bounds.width > scroll->bounds.width,
            "wide mapping sections keep their natural width for horizontal scrolling");
    Require(scroll->scrollContentWidth >= section->bounds.x + section->bounds.width,
            "expanded mapping section is inside the horizontal scroll content width");
    const synth::ui::Node* toggle =
        FindNodeById(tree,
                     synth::runtime_ui::NodeIds::SectionToggle(
                         0, synth::MidiConfigSection::SystemMessages));
    Require(toggle != nullptr &&
                toggle->bounds.width == 220.0f &&
                toggle->bounds.height == synth::runtime_ui::ControllersLayout::kSectionHeaderHeight,
            "section toggles keep column-oriented width and height");
}

void TestControllerRowsStayReadableWithLargeLists()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    for (int ix = 0; ix < 60; ++ix)
    {
        std::ostringstream name;
        name << "controller " << ix;
        Require(harness.instrument.AddController(MakeGenericSlot(name.str().c_str())),
                "add large-list controller");
        harness.connection.controllers.push_back({});
    }

    auto surface = harness.MakeSurface();
    surface.SetContentBounds({0.0f, 0.0f, 360.0f, 360.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* scroll =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kScroll);
    const synth::ui::Node* first =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRow(0));
    const synth::ui::Node* tail =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRow(59));
    Require(scroll != nullptr && first != nullptr && tail != nullptr,
            "large controller list exposes first and tail rows");
    Require(first->bounds.height == synth::runtime_ui::ControllersLayout::kControllerHeaderHeight &&
                tail->bounds.height == synth::runtime_ui::ControllersLayout::kControllerHeaderHeight,
            "large controller list rows keep the recovered readable height");
    Require(scroll->scrollContentHeight > scroll->bounds.height,
            "large controller list publishes a larger scroll content extent");
    Require(tail->bounds.y + tail->bounds.height <= scroll->scrollContentHeight + 0.001f,
            "large controller list tail stays inside scroll content");
}

void TestControllerDeviceLabelsIdentifyThePresetOrBoundInputDevice()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();

    // (a) a row added from a device preset: the resolved wizard id's
    // descriptor names the row, not its internal profile kind.
    synth::MidiControllerSlot fromPreset;
    fromPreset.name = "from preset";
    fromPreset.kind = synth::MidiProfileKind::MfTwister;
    fromPreset.config = synth::MfTwisterDefaultProfileConfig();
    fromPreset.wizardId = "com.sheaf.midi-fighter-twister";
    fromPreset.input = {.identifier = "twister-in", .name = "Midi Fighter Twister"};
    fromPreset.output = {.identifier = "twister-out", .name = "Midi Fighter Twister"};
    Require(harness.instrument.AddController(fromPreset), "add preset-created controller");
    harness.connection.controllers.push_back({});

    // (b) a Custom row: no wizard id resolves, so the label falls back to
    // the MIDI input device the row is bound to, the same identity the
    // "MIDI in:" label already shows on a blacklisted row.
    synth::MidiControllerSlot custom = MakeGenericSlot("custom row");
    custom.input = {.identifier = "custom-in", .name = "Custom Input"};
    Require(harness.instrument.AddController(custom), "add Custom controller");
    harness.connection.controllers.push_back({});

    // A Custom row bound to no device: the fallback still resolves, to the
    // same "(none)" the stored-endpoint label uses.
    Require(harness.instrument.AddController(MakeGenericSlot("unbound custom")),
            "add unbound Custom controller");
    harness.connection.controllers.push_back({});

    // (c) a blacklisted row from a preset: the same descriptor lookup
    // applies once the row is Released.
    synth::MidiControllerSlot blacklisted = fromPreset;
    blacklisted.name = "blacklisted preset";
    blacklisted.disposition = synth::MidiControllerDisposition::Blacklisted;
    blacklisted.dormantConfig = blacklisted.config;
    blacklisted.config = {};
    Require(harness.instrument.AddController(blacklisted), "add blacklisted preset controller");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();

    const synth::ui::Node* presetDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(0));
    const synth::ui::Node* customDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(1));
    const synth::ui::Node* unboundCustomDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(2));
    const synth::ui::Node* blacklistedDevice =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerDevice(3));

    Require(presetDevice != nullptr && presetDevice->text == "MIDI Fighter Twister",
            "a row created from a device preset shows the preset's descriptor name");
    Require(customDevice != nullptr && customDevice->text == "Custom Input (custom-in)",
            "a Custom row with no resolved preset shows the MIDI input device it is bound to");
    Require(unboundCustomDevice != nullptr && unboundCustomDevice->text == "(none)",
            "a Custom row bound to no device shows the same (none) the MIDI in: label uses");
    Require(blacklistedDevice != nullptr && blacklistedDevice->text == "MIDI Fighter Twister",
            "a blacklisted row created from a device preset still shows the preset's descriptor name");
}

void TestControllerLifecycleActionsUseTheNormalCommitAndSavePath()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    synth::MidiControllerSlot known;
    known.name = "known";
    known.kind = synth::MidiProfileKind::MfTwister;
    known.config = synth::MfTwisterDefaultProfileConfig();
    known.wizardId = "com.sheaf.midi-fighter-twister";
    known.input = {.identifier = "known-in", .name = "Known Input"};
    known.output = {.identifier = "known-out", .name = "Known Output"};
    synth::MidiControllerSlot unknown = MakeGenericSlot("unknown");
    unknown.wizardId = "com.example.missing-wizard";
    synth::MidiControllerSlot blacklistedKnown = known;
    blacklistedKnown.name = "blacklisted known";
    blacklistedKnown.disposition = synth::MidiControllerDisposition::Blacklisted;
    blacklistedKnown.dormantConfig = blacklistedKnown.config;
    blacklistedKnown.config = {};
    synth::MidiControllerSlot blacklistedUnknown = blacklistedKnown;
    blacklistedUnknown.name = "blacklisted unknown";
    blacklistedUnknown.wizardId = "com.example.missing-wizard";
    synth::MidiControllerSlot incomplete = known;
    incomplete.name = "incomplete";
    incomplete.output = {};
    Require(harness.instrument.AddController(MakeGenericSlot("manual")), "add manual controller");
    Require(harness.instrument.AddController(known), "add resolved controller");
    Require(harness.instrument.AddController(unknown), "add unknown active controller");
    Require(harness.instrument.AddController(blacklistedKnown), "add resolved blacklisted controller");
    Require(harness.instrument.AddController(blacklistedUnknown), "add unknown blacklisted controller");
    Require(harness.instrument.AddController(incomplete), "add incomplete resolved controller");
    harness.connection.controllers.resize(harness.instrument.controllers.size());

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree initialTree = surface.BuildTree();
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRename(0)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(3)) == nullptr,
            "the Name row is absent from the header -- a collapsed active row and a blacklisted row (which"
            " has no editor to move it into) both show neither the draft nor its button");
    surface.ViewModel().ToggleConfig(0);
    const synth::ui::NodeTree expandedTree = surface.BuildTree();
    const synth::ui::Node* renameDraft = FindNodeById(
        expandedTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0));
    const synth::ui::Node* renameButton = FindNodeById(
        expandedTree, synth::runtime_ui::NodeIds::ControllerRename(0));
    Require(renameDraft != nullptr && renameDraft->action.has_value() &&
                renameDraft->action->value.back() != ':' &&
                renameButton != nullptr && renameButton->action.has_value(),
            "the expanded editor exposes a Name draft field with an unambiguous renderer prefix and an"
            " explicit commit button");
    Require(FindNodeById(expandedTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0) + ".caption")
                    ->text == "Name",
            "the editor's Name draft has a visible caption");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDelete(0)) != nullptr,
            "manual active row exposes Delete");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerBadge(3)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDelete(3)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDisclosure(3)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(3)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(3)) == nullptr,
            "a blacklisted row has its Released badge and Delete but no live editor controls");
    // A blacklisted row shows its stored endpoint labels. Its endpoints
    // stay deliberately Unconfigured, so the label cannot come from connection
    // status, and the identifier must survive so duplicate same-name devices
    // remain distinguishable.
    const synth::ui::Node* blacklistedInputLabel = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerInputLabel(3));
    const synth::ui::Node* blacklistedOutputLabel = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerOutputLabel(3));
    Require(blacklistedInputLabel != nullptr &&
                blacklistedInputLabel->text.find("Known Input") != std::string::npos &&
                blacklistedInputLabel->text.find("known-in") != std::string::npos,
            "blacklisted row shows its stored input name and identifier");
    Require(blacklistedOutputLabel != nullptr &&
                blacklistedOutputLabel->text.find("Known Output") != std::string::npos &&
                blacklistedOutputLabel->text.find("known-out") != std::string::npos,
            "blacklisted row shows its stored output name and identifier");
    const synth::ui::Node* lifecycleScroll = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::kScroll);
    Require(lifecycleScroll != nullptr, "lifecycle tree includes its scroll container");
    const std::string legendId = synth::runtime_ui::NodeIds::kStatusLegend;
    const synth::ui::Node* onlineDot = FindNodeById(initialTree, legendId + ".online");
    const synth::ui::Node* onlineLabel = FindNodeById(initialTree, legendId + ".online.label");
    const synth::ui::Node* offlineDot = FindNodeById(initialTree, legendId + ".offline");
    const synth::ui::Node* offlineLabel = FindNodeById(initialTree, legendId + ".offline.label");
    const synth::ui::Node* notSetDot = FindNodeById(initialTree, legendId + ".not_set");
    const synth::ui::Node* notSetLabel = FindNodeById(initialTree, legendId + ".not_set.label");
    Require(onlineDot != nullptr && onlineLabel != nullptr && offlineDot != nullptr &&
                offlineLabel != nullptr && notSetDot != nullptr && notSetLabel != nullptr,
            "the status dot legend carries all three dot/label pairs");
    Require(onlineLabel->text == "online" && offlineLabel->text == "offline" &&
                notSetLabel->text == "not set",
            "the status dot legend names the three endpoint statuses in order");
    Require(onlineDot->bounds.x < onlineLabel->bounds.x &&
                offlineDot->bounds.x < offlineLabel->bounds.x &&
                notSetDot->bounds.x < notSetLabel->bounds.x,
            "each status dot precedes its own label on x");
    for (const synth::ui::Node& node : initialTree.nodes)
    {
        if (node.id.value.starts_with("runtime.controllers.row.") &&
            node.bounds.x + node.bounds.width > lifecycleScroll->scrollContentWidth)
        {
            Require(false, "controller lifecycle control exceeds the horizontal scroll content width");
        }
    }

    TestHarness staleHarness;
    staleHarness.instrument = harness.instrument;
    staleHarness.connection = harness.connection;
    auto staleSurface = staleHarness.MakeSurface();
    staleSurface.MarkDirty();
    staleSurface.RefreshOnTick();
    const synth::ui::Action staleDelete = *FindNodeById(
        staleSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDelete(2))->action;
    staleHarness.instrument.RemoveController(1);
    staleHarness.connection.controllers.resize(staleHarness.instrument.controllers.size());
    staleSurface.DispatchAction(staleDelete);
    Require(staleHarness.commits == 0 && staleHarness.saves == 0 &&
                staleHarness.instrument.controllers[2].name == "blacklisted known",
            "stale row action cannot retarget the record now occupying its old index");
    const synth::ui::NodeTree staleRefusalTree = staleSurface.BuildTree();
    Require(FindNodeById(staleRefusalTree, synth::runtime_ui::NodeIds::ControllerDisclosure(1)) != nullptr &&
                FindNodeById(staleRefusalTree, synth::runtime_ui::NodeIds::ControllerDisclosure(2)) == nullptr,
            "a refusal publishes the current controller structure without retaining a stale lifecycle row");
    staleSurface.DispatchAction(staleDelete);
    Require(staleHarness.commits == 0 && staleHarness.saves == 0 &&
                FindNodeById(staleSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDisclosure(2)) == nullptr,
            "repeated stale lifecycle refusals leave the published controller tree consistent");

    synth::ui::Action rename = *renameDraft->action;
    rename.value += ":manual:renamed";
    surface.DispatchAction(rename);
    Require(harness.commits == 0 && harness.saves == 0 &&
                FindNodeById(surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerRenameDraft(0))->text ==
                    "manual:renamed",
            "rename typing updates only the portable draft without committing or saving");
    surface.DispatchAction(*renameButton->action);
    Require(harness.commits == 1 && harness.saves == 1 &&
                harness.instrument.controllers[0].name == "manual:renamed",
            "Rename preserves a colon-containing valid name through the lifecycle callback path");
    const synth::ui::NodeTree renamedTree = surface.BuildTree();
    const synth::ui::Node* renameDraftAgain = FindNodeById(
        renamedTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0));
    const synth::ui::Node* renameAgain = FindNodeById(
        renamedTree, synth::runtime_ui::NodeIds::ControllerRename(0));
    Require(renameDraftAgain != nullptr && renameAgain != nullptr && renameAgain->action.has_value(),
            "the rename editor stays open under the new name after the commit");
    surface.DispatchAction(*renameAgain->action);
    Require(harness.commits == 1 && harness.saves == 1,
            "unchanged rename is refused without a second commit or save");
    surface.DispatchAction(*FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerDelete(2))->action);
    Require(harness.commits == 2 && harness.saves == 2 &&
                harness.instrument.FindController("unknown") == nullptr,
            "Delete remains available for an unknown persisted id and commits through the normal path");
    // Deleting "unknown" (index 2) shifted the remaining rows down by one, so
    // the blacklisted record from the initial fixture ("blacklisted known",
    // index 3) is now at index 2.
    surface.DispatchAction(*FindNodeById(
        surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDelete(2))->action);
    Require(harness.commits == 3 && harness.saves == 3 &&
                harness.instrument.FindController("blacklisted known") == nullptr,
            "Delete removes an inert blacklisted record through one commit and save");
}

std::string VisibleTextLower(const synth::ui::NodeTree& tree)
{
    std::string text;
    for (const synth::ui::Node& node : tree.nodes)
    {
        text += node.label;
        text += ' ';
        text += node.text;
        text += ' ';
        for (const synth::ui::ControlOption& option : node.options)
        {
            text += option.label;
            text += ' ';
        }
    }
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void TestBlacklistedRecordPersistsAndRoundTrips()
{
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot released;
    released.name = "released twister";
    released.kind = synth::MidiProfileKind::MfTwister;
    released.disposition = synth::MidiControllerDisposition::Blacklisted;
    released.wizardId = "com.sheaf.midi-fighter-twister";
    released.input = {.identifier = "rt-in", .name = "Released Twister In"};
    released.output = {.identifier = "rt-out", .name = "Released Twister Out"};
    released.dormantConfig = synth::MfTwisterDefaultProfileConfig();
    Require(instrument.AddController(released), "add a released record");

    synth::JsonArena arena(256 * 1024);
    const synth::JSON json = synth::ToJSON(arena, instrument);
    const synth::JSON controller = json.Get("controllers").GetAt(0);
    Require(controller.Get("disposition").StringValue() == std::string_view("blacklisted"),
            "the persisted disposition token is unchanged by relabelling");

    synth::MidiInstrumentConfig loaded;
    Require(synth::FromJSON(json, loaded), "the released record round-trips");
    Require(loaded.controllers.size() == 1 &&
                loaded.controllers[0].disposition == synth::MidiControllerDisposition::Blacklisted &&
                loaded.controllers[0].wizardId == released.wizardId,
            "the round trip preserves the released disposition and identity");
}

void TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence()
{
    // Seed a genuinely installed Twister row through the real add-from-preset
    // path (the same InstallDescriptorProfile call Restore itself uses), so
    // the config copied from it below is guaranteed to be what
    // SlotMatchesWizardProfile will regenerate and compare against -- not a
    // hand-guessed approximation of it.
    TestHarness seedHarness;
    seedHarness.devices.inputs.push_back({"twister-in-id", "Midi Fighter Twister"});
    auto seedSurface = seedHarness.MakeSurface();
    seedSurface.SetEnumerateDevices(seedHarness.devices);
    seedSurface.MarkDirty();
    seedSurface.RefreshOnTick();
    seedSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(seedHarness.instrument.controllers.size() == 4, "seed harness installs one preset row");
    const synth::MidiControllerSlot installed = seedHarness.instrument.controllers[3];
    Require(installed.wizardId == "com.sheaf.midi-fighter-twister" &&
                installed.input.identifier == "twister-in-id" &&
                installed.output.identifier == "uid:782494201",
            "the seed row installed from the library preset with both ports bound");

    TestHarness harness;
    harness.instrument.controllers.clear();

    synth::MidiControllerSlot pristine = installed;
    pristine.name = "pristine";
    pristine.input = {.identifier = "pristine-in", .name = "Pristine Input"};
    pristine.output = {.identifier = "pristine-out", .name = "Pristine Output"};

    synth::MidiControllerSlot manual = MakeGenericSlot("manual restore check");

    synth::MidiControllerSlot edited = installed;
    edited.name = "edited";
    edited.input = {.identifier = "edited-in", .name = "Edited Input"};
    edited.output = {.identifier = "edited-out", .name = "Edited Output"};

    Require(harness.instrument.AddController(pristine), "add pristine preset row");
    Require(harness.instrument.AddController(manual), "add manual row");
    Require(harness.instrument.AddController(edited), "add row to diverge next");
    harness.connection.controllers.resize(harness.instrument.controllers.size());

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Diverge row 2's mapping through the real per-field commit path, exactly
    // as a user editing a mapping would, rather than hand-mutating its config.
    surface.ViewModel().ToggleConfig(2);
    surface.ViewModel().ToggleSection(2, synth::MidiConfigSection::Encoders);
    surface.MarkDirty();
    surface.RefreshOnTick();
    const std::vector<synth::MidiMappingRowVM> encoderRows =
        surface.ViewModel().SectionRows(2, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> turnStepRowIx;
    for (std::size_t ix = 0; ix < encoderRows.size(); ++ix)
    {
        for (synth::MidiMappingRowVM::Field field : encoderRows[ix].editableFields)
        {
            if (field == synth::MidiMappingRowVM::Field::TurnStep)
            {
                turnStepRowIx = ix;
                break;
            }
        }
        if (turnStepRowIx.has_value())
        {
            break;
        }
    }
    Require(turnStepRowIx.has_value(), "find an editable turn-step row to diverge");
    const std::string editValue = "2:encoders:" + std::to_string(*turnStepRowIx) + ":" +
                                  std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::TurnStep)) +
                                  ":0.25";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, editValue));
    Require(harness.commits == 1, "the mapping edit commits");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();

    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRestore(0)) == nullptr,
            "Restore is absent from an untouched preset row");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRestore(1)) == nullptr,
            "Restore is absent from a row never created from a preset");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerRestore(2)) != nullptr,
            "Restore is present on a preset row whose config has diverged from its preset");

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerRestore,
        synth::runtime_ui::NodeIds::ControllerActionToken(2, "edited")));

    const synth::MidiControllerSlot& restored = harness.instrument.controllers[2];
    Require(restored.name == "edited", "Restore preserves the row's name");
    Require(restored.input.identifier == "edited-in" && restored.output.identifier == "edited-out",
            "Restore preserves both endpoint refs");
    Require(restored.disposition == synth::MidiControllerDisposition::Active,
            "Restore preserves the row's disposition");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree afterRestoreTree = surface.BuildTree();
    Require(FindNodeById(afterRestoreTree, synth::runtime_ui::NodeIds::ControllerRestore(2)) == nullptr,
            "Restore disappears once the row matches its preset again");
}

// The Encoders section's Turn and Push group headers lay their column labels
// and Add/Block buttons out in one Row sharing ControllersLayout::
// kEditorColumnGap (ControllersPageUI.hpp's emitGroupHeader), the same gap
// the mapping rows below use between fields. Pin the gap by geometry, not by
// control count, so a regression that collapsed it back to the old literal
// 0.0f (welding the last column to the Add button) fails here.
void TestEncoderGroupHeaderSeparatesLastColumnFromAddButton()
{
    using RowGroup = synth::MidiMappingRowVM::RowGroup;

    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 1000.0f, 800.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Controller 2 ("blank") is MakeGenericSlot()'s untouched default: no
    // encoderInput at all, so SectionRows() below is empty and both group
    // headers come from AddableGroups()' header-only affordance rather than
    // from any actual mapping row.
    constexpr std::size_t controllerIx = 2;
    surface.ViewModel().ToggleConfig(controllerIx);
    surface.ViewModel().ToggleSection(controllerIx, synth::MidiConfigSection::Encoders);
    surface.MarkDirty();
    surface.RefreshOnTick();

    Require(surface.ViewModel().SectionRows(controllerIx, synth::MidiConfigSection::Encoders).empty(),
            "blank generic controller starts with no encoder mapping rows");

    const std::vector<synth::MidiMappingRowVM::Field> turnFields = surface.ViewModel().GroupColumnFields(
        controllerIx, synth::MidiConfigSection::Encoders, RowGroup::EncoderTurn);
    const std::vector<synth::MidiMappingRowVM::Field> pushFields = surface.ViewModel().GroupColumnFields(
        controllerIx, synth::MidiConfigSection::Encoders, RowGroup::EncoderPush);
    Require(turnFields.size() > 1, "Turn header shows more than one column label");
    Require(pushFields.size() > 1, "Push header shows more than one column label");

    const synth::ui::NodeTree tree = surface.BuildTree();

    auto requireHeaderGaps = [&](std::size_t headerIx, std::size_t lastFieldIx, const char* presenceLabel,
                                 const char* columnGapLabel, const char* addGapLabel) {
        const synth::ui::Node* lastColumn = FindNodeById(
            tree, synth::runtime_ui::NodeIds::GroupColumnLabel(
                      controllerIx, synth::MidiConfigSection::Encoders, headerIx, lastFieldIx));
        const synth::ui::Node* addSingle = FindNodeById(
            tree, synth::runtime_ui::NodeIds::GroupAddSingle(
                      controllerIx, synth::MidiConfigSection::Encoders, headerIx));
        const synth::ui::Node* addBlock = FindNodeById(
            tree, synth::runtime_ui::NodeIds::GroupAddBlock(
                      controllerIx, synth::MidiConfigSection::Encoders, headerIx));
        Require(lastColumn != nullptr && addSingle != nullptr && addBlock != nullptr, presenceLabel);

        Require(addSingle->bounds.x - (lastColumn->bounds.x + lastColumn->bounds.width) ==
                    synth::runtime_ui::ControllersLayout::kEditorColumnGap,
                columnGapLabel);
        Require(addBlock->bounds.x - (addSingle->bounds.x + addSingle->bounds.width) ==
                    synth::runtime_ui::ControllersLayout::kEditorColumnGap,
                addGapLabel);
    };

    requireHeaderGaps(0, turnFields.size() - 1,
                      "Turn header's last column, add_single, and add_block nodes all render",
                      "Turn header: last column to add_single keeps kEditorColumnGap",
                      "Turn header: add_single to add_block keeps kEditorColumnGap");
    requireHeaderGaps(1, pushFields.size() - 1,
                      "Push header's last column, add_single, and add_block nodes all render",
                      "Push header: last column to add_single keeps kEditorColumnGap",
                      "Push header: add_single to add_block keeps kEditorColumnGap");
}

void TestSystemMessageShiftFieldRendersAndCommits()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 1000.0f, 800.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Controller 2 ("blank") is MakeGenericSlot()'s untouched default -- no
    // system messages at all, so AddSingle seeds the row this test edits.
    constexpr std::size_t controllerIx = 2;
    surface.ViewModel().ToggleConfig(controllerIx);
    surface.ViewModel().ToggleSection(controllerIx, synth::MidiConfigSection::SystemMessages);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                        "2:system_messages:system"));
    surface.MarkDirty();
    surface.RefreshOnTick();

    const std::vector<synth::MidiMappingRowVM> rows =
        surface.ViewModel().SectionRows(controllerIx, synth::MidiConfigSection::SystemMessages);
    Require(!rows.empty(), "add single creates a system row");
    Require(std::find(rows[0].editableFields.begin(), rows[0].editableFields.end(),
                      synth::MidiMappingRowVM::Field::ShiftAction) != rows[0].editableFields.end(),
            "fresh system row exposes the Shift field");

    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* shiftCombo = FindNodeById(
        tree, synth::runtime_ui::NodeIds::MappingField(controllerIx, synth::MidiConfigSection::SystemMessages, 0,
                                                        synth::MidiMappingRowVM::Field::ShiftAction));
    Require(shiftCombo != nullptr, "system row's Shift combo renders");
    Require(shiftCombo->kind == synth::ui::NodeKind::ComboBox, "Shift field renders as a combo box");
    Require(shiftCombo->options.size() == surface.ViewModel().ShiftCatalog().size(),
            "Shift combo offers every ShiftCatalog() choice");
    Require(shiftCombo->selectedOption == "0", "a fresh row's Shift combo starts at (none)");

    const std::vector<synth::UISystemMessageChoice>& shiftCatalog = surface.ViewModel().ShiftCatalog();
    Require(shiftCatalog.size() > 1, "fixture's ShiftCatalog offers at least one real choice besides (none)");
    constexpr int kShiftChoiceIx = 1;

    const std::string commitValue =
        std::to_string(controllerIx) + ":system_messages:0:" +
        std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::ShiftAction)) + ":" +
        std::to_string(kShiftChoiceIx);
    const int commitsBefore = harness.commits;
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, commitValue));
    Require(harness.commits == commitsBefore + 1, "Shift field commit persists through the normal commit path");

    const synth::MidiControllerSystemMessageAssociation& committed =
        harness.instrument.controllers[controllerIx].config.systemMessages[0];
    Require(committed.shiftedPress.has_value(), "committed association carries a shifted press");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::Node* shiftComboAfter = FindNodeById(
        surface.BuildTree(), synth::runtime_ui::NodeIds::MappingField(
                                 controllerIx, synth::MidiConfigSection::SystemMessages, 0,
                                 synth::MidiMappingRowVM::Field::ShiftAction));
    Require(shiftComboAfter != nullptr && shiftComboAfter->selectedOption == std::to_string(kShiftChoiceIx),
            "Shift combo reflects the committed choice after rebuild");
}

}  // namespace

void TestLaunchpadRowOffersVariantAndRetargetsItsPads()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 1000.0f, 800.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // MakeInstrument()'s three rows: 0 wrldbldr, 1 launchpad, 2 generic.
    constexpr std::size_t launchpadIx = 1;
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* variant =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerVariant(launchpadIx));
    Require(variant != nullptr, "the launchpad row offers a model selector");
    Require(variant->kind == synth::ui::NodeKind::ComboBox, "Variant is a combo box");
    Require(variant->options.size() == 3, "Variant offers every Launchpad model");
    Require(variant->options[0].label == std::string("Launchpad X"), "first option is Launchpad X");
    Require(variant->options[2].label == std::string("Launchpad Mini MK3"),
            "last option is the Mini MK3");
    Require(variant->selectedOption == "0", "a default-profile row shows Launchpad X");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerVariant(0)) == nullptr &&
                FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerVariant(2)) == nullptr,
            "no other kind offers a model selector");

    // The backend appends the chosen option's id to the action's value.
    const int commitsBefore = harness.commits;
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kVariantSelect, "1:2"));
    surface.MarkDirty();
    surface.RefreshOnTick();

    Require(harness.commits == commitsBefore + 1, "choosing a model commits once");
    const synth::MidiControllerSlot& pads = harness.instrument.controllers[launchpadIx];
    Require(pads.config.launchpadModel == synth::LaunchpadController::LaunchpadMiniMk3,
            "the row records the chosen model");
    std::size_t retargeted = 0;
    for (const auto& association : pads.config.systemMessages)
    {
        if (association.launchpadPosition.has_value())
        {
            Require(association.launchpadPosition->controller ==
                        synth::LaunchpadController::LaunchpadMiniMk3,
                    "every pad follows the chosen model");
            ++retargeted;
        }
    }
    Require(retargeted > 0, "the default profile had pads to retarget");

    const synth::ui::NodeTree after = surface.BuildTree();
    const synth::ui::Node* afterVariant =
        FindNodeById(after, synth::runtime_ui::NodeIds::ControllerVariant(launchpadIx));
    Require(afterVariant != nullptr && afterVariant->selectedOption == "2",
            "the selector shows what the row now records");
}

void TestLaunchpadRowModelSelectorIsCaptionedModel()
{
    TestHarness harness;
    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 1000.0f, 800.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // MakeInstrument()'s three rows: 0 wrldbldr, 1 launchpad, 2 generic.
    constexpr std::size_t launchpadIx = 1;
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* caption = FindNodeById(
        tree, synth::runtime_ui::NodeIds::ControllerVariant(launchpadIx) + ".caption");
    Require(caption != nullptr && caption->text == "Model",
            "the launchpad row's model selector is captioned Model");
}

void TestConnectMessageShowsOnAnAbletonStyleRowsExpandedConfiguration()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    // The real Akai APC40 mkII Ableton-mode connect message
    // (app/FroggersMidiCatalog.hpp's Apc40AbletonDeviceDefault), not a
    // placeholder byte string.
    synth::MidiControllerSlot ableton = MakeGenericSlot("Ableton APC40");
    ableton.config.openSysEx.push_back({0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x41, 0x09, 0x07, 0x01, 0xF7});
    Require(harness.instrument.AddController(ableton), "add Ableton-style controller");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);
    const synth::ui::NodeTree tree = surface.BuildTree();

    const synth::ui::Node* field =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ConnectMessageField(0, 0));
    Require(field != nullptr && field->text == "F0 47 7F 29 60 00 04 41 09 07 01 F7",
            "an Ableton-style row's connect message shows as hex bytes in its expanded configuration");
    const synth::ui::Node* heading =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ConnectMessagesHeading(0));
    Require(heading != nullptr && heading->text == "Connect messages",
            "the connect-messages area has a visible heading");
    const synth::ui::Node* addButton =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ConnectMessageAdd(0));
    Require(addButton != nullptr, "the connect-messages area offers an Add button");
    // Measured bounds, not appearance: the button must be emitted inside a
    // Row so its width/height style arguments land on the Row's horizontal
    // and vertical axes respectively, not swapped by a Column parent.
    Require(addButton->bounds.width == synth::runtime_ui::ControllersLayout::kAddButtonWidth &&
                addButton->bounds.height == 28.0f,
            "the Add button's measured width and height land on their own axes");
    const synth::ui::Node* deleteButton =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ConnectMessageDelete(0, 0));
    Require(deleteButton != nullptr, "the stored connect message has a delete button");
}

void TestConnectMessageEditCommitsValidAndRefusesInvalidUnchanged()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    // Starts with a placeholder message; the edit below replaces it with the
    // real Akai APC40 mkII Ableton-mode message
    // (app/FroggersMidiCatalog.hpp's Apc40AbletonDeviceDefault).
    synth::MidiControllerSlot ableton = MakeGenericSlot("Ableton APC40");
    ableton.config.openSysEx.push_back({0xF0, 0x7E, 0x00, 0xF7});
    Require(harness.instrument.AddController(ableton), "add Ableton-style controller");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);

    // A valid edit (F0 ... F7, data bytes in 00-7F) commits and updates the
    // field text.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit,
        "0:0:F0 47 7F 29 60 00 04 41 09 07 01 F7"));
    Require(harness.commits == 1 && surface.StatusText() == "OK",
            "a valid connect-message edit commits");
    Require(harness.instrument.controllers[0].config.openSysEx[0] ==
                (std::vector<std::uint8_t>{0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x41, 0x09, 0x07, 0x01,
                                          0xF7}),
            "the committed instrument carries the parsed bytes");
    const synth::ui::NodeTree afterValid = surface.BuildTree();
    Require(FindNodeById(afterValid, synth::runtime_ui::NodeIds::ConnectMessageField(0, 0))->text ==
                "F0 47 7F 29 60 00 04 41 09 07 01 F7",
            "the field shows the newly committed message");

    // An edit that is not a single SysEx message refuses and changes
    // nothing: not F0-led, not F7-tailed, and a data byte over 0x7F.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:0:00 F7"));
    Require(harness.commits == 1, "a non-F0-led edit is refused, not committed");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:0:F0 00"));
    Require(harness.commits == 1, "a non-F7-tailed edit is refused, not committed");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:0:F0 80 F7"));
    Require(harness.commits == 1, "a data byte over 0x7F is refused, not committed");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:0:not hex"));
    Require(harness.commits == 1, "malformed hex is refused, not committed");
    Require(surface.StatusText().starts_with("Refused"), "the refusal sets a status message");
    // Re-commit the same valid message to move the status off "Refused"
    // before testing the empty commit below. Without this, a silently
    // dropped empty commit (the arity guard treating "0:0" as too few parts
    // and returning before ever touching SetStatus) would leave the prior
    // refusal's status in place, and the starts_with("Refused") check below
    // would pass whether or not the drop was silent.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit,
        "0:0:F0 47 7F 29 60 00 04 41 09 07 01 F7"));
    Require(harness.commits == 2 && surface.StatusText() == "OK",
            "re-committing the same valid message resets status to OK");
    // An empty commit tokenises to just "0:0" (the trailing empty field after
    // the last ':' is dropped by Split's getline loop), so the arity guard
    // must accept that shape and let SetConnectMessage's own emptiness check
    // refuse it -- not treat it as too few parts and silently return.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:0:"));
    Require(harness.commits == 2, "an empty commit is refused, not committed");
    Require(surface.StatusText().starts_with("Refused"), "an empty commit sets a refusal status");
    Require(harness.instrument.controllers[0].config.openSysEx[0] ==
                (std::vector<std::uint8_t>{0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x41, 0x09, 0x07, 0x01,
                                          0xF7}),
            "every refused edit leaves the stored message exactly as the last valid commit left it");
    const synth::ui::NodeTree afterRefusals = surface.BuildTree();
    Require(FindNodeById(afterRefusals, synth::runtime_ui::NodeIds::ConnectMessageField(0, 0))->text ==
                "F0 47 7F 29 60 00 04 41 09 07 01 F7",
            "the displayed field is unchanged by every refused edit");
}

void TestConnectMessageAddAndDelete()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    Require(harness.instrument.AddController(MakeGenericSlot("Custom row")), "add a Custom row");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);

    Require(FindNodeById(surface.BuildTree(), synth::runtime_ui::NodeIds::ConnectMessageField(0, 0)) ==
                nullptr,
            "a Custom row starts with no connect messages");
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kConnectMessageAdd, "0"));
    Require(harness.commits == 1 && harness.instrument.controllers[0].config.openSysEx.size() == 1,
            "Add appends one connect message");
    Require(harness.instrument.controllers[0].config.openSysEx[0] ==
                (std::vector<std::uint8_t>{0xF0, 0xF7}),
            "a newly added connect message starts as an already-valid empty message");

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageDelete, "0:0"));
    Require(harness.commits == 2 && harness.instrument.controllers[0].config.openSysEx.empty(),
            "Delete removes the connect message");
    Require(FindNodeById(surface.BuildTree(), synth::runtime_ui::NodeIds::ConnectMessageField(0, 0)) ==
                nullptr,
            "the deleted connect message's field is gone from the tree");
}

// Shared by the two-message index tests below: a row holding messages
// F0 01 F7 (index 0) and F0 02 F7 (index 1), so a later commit or delete at
// index 1 has a distinct index 0 to prove untouched.
void SetUpTwoConnectMessageRow(TestHarness& harness)
{
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    synth::MidiControllerSlot slot = MakeGenericSlot("two messages");
    slot.config.openSysEx.push_back({0xF0, 0x01, 0xF7});
    slot.config.openSysEx.push_back({0xF0, 0x02, 0xF7});
    Require(harness.instrument.AddController(slot), "add a row with two connect messages");
    harness.connection.controllers.push_back({});
}

void TestConnectMessageDeleteRemovesTheGivenIndexNotAlwaysFirst()
{
    namespace NodeIds = synth::runtime_ui::NodeIds;

    TestHarness harness;
    SetUpTwoConnectMessageRow(harness);

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);

    // Delete index 1, not index 0: a "delete always removes index 0" bug
    // would instead remove message F0 01 F7 and still leave one message
    // behind, so the surviving message is the only thing that tells them
    // apart.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageDelete, "0:1"));
    Require(harness.commits == 1 && harness.instrument.controllers[0].config.openSysEx.size() == 1,
            "Delete removes exactly one connect message");
    Require(harness.instrument.controllers[0].config.openSysEx[0] ==
                (std::vector<std::uint8_t>{0xF0, 0x01, 0xF7}),
            "Delete removes the message at the given index, leaving the other one, not always index 0");
    const synth::ui::Node* survivorField =
        FindNodeById(surface.BuildTree(), NodeIds::ConnectMessageField(0, 0));
    Require(survivorField != nullptr && survivorField->text == "F0 01 F7",
            "the surviving message is what the page shows at index 0");
}

void TestConnectMessageCommitAtIndexOneLeavesIndexZeroUnchanged()
{
    namespace NodeIds = synth::runtime_ui::NodeIds;

    TestHarness harness;
    SetUpTwoConnectMessageRow(harness);

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);

    // Commit to index 1, not index 0: a "commit always writes index 0" bug
    // would instead overwrite F0 01 F7 and leave message 1 untouched, so
    // checking both indexes' final bytes is what tells the two apart.
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kConnectMessageCommit, "0:1:F0 55 F7"));
    Require(harness.commits == 1 && surface.StatusText() == "OK",
            "a valid edit at index 1 commits");
    Require(harness.instrument.controllers[0].config.openSysEx[0] ==
                (std::vector<std::uint8_t>{0xF0, 0x01, 0xF7}),
            "the edit at index 1 leaves index 0's stored bytes unchanged");
    Require(harness.instrument.controllers[0].config.openSysEx[1] ==
                (std::vector<std::uint8_t>{0xF0, 0x55, 0xF7}),
            "the edit at index 1 updates index 1 to the edited bytes");
    const synth::ui::NodeTree afterCommit = surface.BuildTree();
    Require(FindNodeById(afterCommit, NodeIds::ConnectMessageField(0, 0))->text == "F0 01 F7",
            "the displayed field at index 0 is unchanged");
    Require(FindNodeById(afterCommit, NodeIds::ConnectMessageField(0, 1))->text == "F0 55 F7",
            "the displayed field at index 1 shows the newly committed message");
}

void TestConnectMessageHandlersRefuseVisiblyWhenHostRejectsCommit()
{
    const std::string kHostRejectedText = "Refused: host rejected the instrument commit";

    // HandleConnectMessageCommit: a valid edit that the host's commit
    // callback refuses.
    {
        TestHarness harness;
        SetUpTwoConnectMessageRow(harness);
        harness.commitSucceeds = false;
        auto surface = harness.MakeSurface();
        surface.MarkDirty();
        surface.RefreshOnTick();
        surface.ViewModel().ToggleConfig(0);

        surface.DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kConnectMessageCommit, "0:1:F0 55 F7"));
        Require(harness.commitAttempts == 1 && harness.commits == 0,
                "a host-rejected connect-message commit attempts but does not complete a commit");
        Require(surface.StatusText() == kHostRejectedText,
                "a host-rejected connect-message commit reports the shared refusal status");
        Require(harness.instrument.controllers[0].config.openSysEx[1] ==
                    (std::vector<std::uint8_t>{0xF0, 0x02, 0xF7}),
                "a host-rejected connect-message commit leaves the stored message unchanged");
    }

    // HandleConnectMessageDelete: a valid delete that the host's commit
    // callback refuses.
    {
        TestHarness harness;
        SetUpTwoConnectMessageRow(harness);
        harness.commitSucceeds = false;
        auto surface = harness.MakeSurface();
        surface.MarkDirty();
        surface.RefreshOnTick();
        surface.ViewModel().ToggleConfig(0);

        surface.DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kConnectMessageDelete, "0:1"));
        Require(harness.commitAttempts == 1 && harness.commits == 0,
                "a host-rejected connect-message delete attempts but does not complete a commit");
        Require(surface.StatusText() == kHostRejectedText,
                "a host-rejected connect-message delete reports the shared refusal status");
        Require(harness.instrument.controllers[0].config.openSysEx.size() == 2,
                "a host-rejected connect-message delete leaves both stored messages in place");
    }

    // HandleConnectMessageAdd: an add that the host's commit callback
    // refuses.
    {
        TestHarness harness;
        harness.instrument.controllers.clear();
        harness.connection.controllers.clear();
        Require(harness.instrument.AddController(MakeGenericSlot("Custom row")), "add a Custom row");
        harness.connection.controllers.push_back({});
        harness.commitSucceeds = false;
        auto surface = harness.MakeSurface();
        surface.MarkDirty();
        surface.RefreshOnTick();
        surface.ViewModel().ToggleConfig(0);

        surface.DispatchAction(
            synth::ui::Action::WithValue(synth::runtime_ui::Actions::kConnectMessageAdd, "0"));
        Require(harness.commitAttempts == 1 && harness.commits == 0,
                "a host-rejected connect-message add attempts but does not complete a commit");
        Require(surface.StatusText() == kHostRejectedText,
                "a host-rejected connect-message add reports the shared refusal status");
        Require(harness.instrument.controllers[0].config.openSysEx.empty(),
                "a host-rejected connect-message add leaves the row with no stored messages");
    }
}

void TestConnectMessageShownOnEveryKindNotJustGeneric()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    synth::MidiControllerSlot wrld = MakeWrldBldrSlot("wrld with connect message");
    wrld.config.openSysEx.push_back({0xF0, 0x7E, 0x00, 0xF7});
    Require(harness.instrument.AddController(wrld), "add a WRLD.Bldr row with a connect message");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);
    const synth::ui::Node* field =
        FindNodeById(surface.BuildTree(), synth::runtime_ui::NodeIds::ConnectMessageField(0, 0));
    Require(field != nullptr && field->text == "F0 7E 00 F7",
            "the connect-messages list shows on a WRLD.Bldr row too, not only a Generic row");
}

void TestControllerDeviceLabelForUnresolvedWizardIdShowsBoundDevice()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    synth::MidiControllerSlot slot = MakeGenericSlot("stale preset");
    slot.wizardId = "some.descriptor.no.longer.in.the.registry";
    slot.input = {.identifier = "lp-in", .name = "LPX MIDI In"};
    Require(harness.instrument.AddController(slot), "add a row whose wizard id resolves against nothing");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::Node* device =
        FindNodeById(surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDevice(0));
    Require(device != nullptr && device->text == "LPX MIDI In (lp-in)",
            "a row whose wizard id no longer resolves shows its bound input device, not a resolution failure");
}

void TestControllerDeviceLabelForAppCatalogDeviceShowsItsName()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    // A real app-catalog device (AppDefaultControllerWizard, the same
    // construction MakeControllerWizardRegistry gives every frogg3rs
    // device default), not a library descriptor -- the dropdown test with
    // a catalog only ever checked option ids, never this label.
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back({.id = "app.device.one",
                                      .displayName = "App Device One",
                                      .kind = synth::MidiProfileKind::Generic,
                                      .inputAliases = {},
                                      .outputAliases = {},
                                      .config = {}});
    harness.layouts = synth::MakeControllerWizardRegistry(catalog);

    auto surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.MarkDirty();
    surface.RefreshOnTick();

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "app.device.one"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 1, "add from the app catalog device commits");
    const std::size_t addedIx = harness.instrument.controllers.size() - 1;
    Require(harness.instrument.controllers[addedIx].wizardId == "app.device.one",
            "the added row carries the app catalog device's id");

    harness.connection.controllers.resize(harness.instrument.controllers.size());
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::Node* device = FindNodeById(
        surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDevice(addedIx));
    Require(device != nullptr && device->text == "App Device One",
            "a row added from an app catalog device shows that device's own display name");
}

int main()
{
    TestNoHandRolledControllerNodesSurvive();
    TestControllersSectionsNestThroughLibraryContainers();
    TestControllerRowsStayReadableWithLargeLists();
    TestControllerDeviceLabelsIdentifyThePresetOrBoundInputDevice();
    TestDiscoveryRendersPortableAvailableRowsAndDiagnostics();
    TestConnectedNotSetUpListsDevicesWithoutActions();
    TestSaveFailureKeepsTheCommittedEditAndReportsIt();
    TestAddFromPresetWithNoDeviceInstallsTheDefaultPresetWithNoneEndpoints();
    TestAddFromPresetWithMatchingOnlinePairBindsBothEndpoints();
    TestAddCustomGenericYieldsAnEmptyGenericRecord();
    TestAddedRowOpensWithEverySectionOpen();
    TestAddPresetDropdownListsRegistryDescriptorsThenOneCustomEntry();
    TestAddLibraryLaunchpadAndWrldBldrGiveDefaultConfigAndDeviceLabel();
    TestAddRowStartsOnTheFirstWaitingDevicesPreset();
    TestAddBindsAConnectedDeviceForEveryPresetItMatches();
    TestSystemMessageRowShowsAStoredKindTheCatalogLacks();
    TestEndpointSelectorsPreferTheExactStoredIdentifier();
    TestControllerLifecycleActionsUseTheNormalCommitAndSavePath();
    TestBlacklistedRecordPersistsAndRoundTrips();
    TestRestoreReinstallsADivergedPresetAndIsGatedByDivergence();
    TestEncoderGroupHeaderSeparatesLastColumnFromAddButton();
    TestSystemMessageShiftFieldRendersAndCommits();
    TestLaunchpadRowOffersVariantAndRetargetsItsPads();
    TestLaunchpadRowModelSelectorIsCaptionedModel();
    TestConnectMessageShowsOnAnAbletonStyleRowsExpandedConfiguration();
    TestConnectMessageEditCommitsValidAndRefusesInvalidUnchanged();
    TestConnectMessageAddAndDelete();
    TestConnectMessageDeleteRemovesTheGivenIndexNotAlwaysFirst();
    TestConnectMessageCommitAtIndexOneLeavesIndexZeroUnchanged();
    TestConnectMessageHandlersRefuseVisiblyWhenHostRejectsCommit();
    TestConnectMessageShownOnEveryKindNotJustGeneric();
    TestControllerDeviceLabelForUnresolvedWizardIdShowsBoundDevice();
    TestControllerDeviceLabelForAppCatalogDeviceShowsItsName();

    TestHarness harness;
    synth::runtime_ui::ControllersPageSurface surface = harness.MakeSurface();
    surface.SetEnumerateDevices(harness.devices);
    surface.SetContentBounds({0.0f, 0.0f, 900.0f, 700.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();
    Require(surface.ViewModel().Controllers().size() == 3, "initial controller rows rebuild");

    const synth::ui::NodeTree initialTree = surface.BuildTree();
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::kBack) != nullptr, "back button node");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::kScroll) != nullptr, "scroll area node");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::kAddButton) != nullptr, "add controller button");
    const synth::ui::Node* addPreset =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::kAddPreset);
    const synth::ui::Node* addPresetCaption =
        FindNodeById(initialTree, std::string(synth::runtime_ui::NodeIds::kAddPreset) + ".caption");
    Require(addPreset != nullptr && addPreset->action.has_value() &&
                addPreset->action->name == "runtime.controllers.add_preset_draft",
            "add controller preset edits dispatch a portable draft action");
    Require(addPresetCaption != nullptr && addPresetCaption->text == "Preset",
            "add controller preset selector has a visible caption");
    Require(!addPreset->options.empty() && addPreset->options.back().label == "Custom" &&
                addPreset->selectedOption == addPreset->options.front().id,
            "the add row's Preset combo offers the registry then one Custom entry, defaulting to the"
            " first option");
    const synth::ui::Node* wrldInput =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(0));
    const synth::ui::Node* wrldInputRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(0) + ".row");
    const synth::ui::Node* padsInput =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(1));
    const synth::ui::Node* padsInputRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(1) + ".row");
    const synth::ui::Node* wrldOutput =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(0));
    const synth::ui::Node* wrldOutputRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(0) + ".row");
    const synth::ui::Node* wrldInputDot =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInputStatus(0));
    const synth::ui::Node* wrldOutputDot =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutputStatus(0));
    const synth::ui::Node* padsOutput =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(1));
    const synth::ui::Node* padsOutputRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(1) + ".row");
    const synth::ui::Node* scrollNode = FindNodeById(initialTree, synth::runtime_ui::NodeIds::kScroll);
    Require(wrldInput != nullptr && padsInput != nullptr && wrldOutput != nullptr && padsOutput != nullptr,
            "controller device controls render");
    Require(wrldInputRow != nullptr && padsInputRow != nullptr && wrldOutputRow != nullptr &&
                padsOutputRow != nullptr,
            "captioned endpoint selectors render their flow-slot rows");
    Require(wrldInputDot != nullptr && wrldOutputDot != nullptr, "controller status dots render");
    Require(scrollNode != nullptr, "scroll area node still present");
    Require(wrldInput->bounds.x == padsInput->bounds.x, "launchpad input aligns with other controller inputs");
    Require(wrldOutput->bounds.x == padsOutput->bounds.x, "launchpad output aligns with other controller outputs");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(0) + ".caption")->text ==
                "MIDI in",
            "input selector caption is visible text");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(0) + ".caption")->text ==
                "MIDI out",
            "output selector caption is visible text");
    Require(wrldInputRow->bounds.x == padsInputRow->bounds.x,
            "captioned launchpad input aligns with other controller input flow slots");
    Require(wrldOutputRow->bounds.x == padsOutputRow->bounds.x,
            "captioned launchpad output aligns with other controller output flow slots");
    // wrldInputRow and wrldOutputRow are no longer siblings: each now sits
    // inside its own per-port row alongside that port's status dot, so their
    // raw bounds.x are relative to different immediate parents (bounds are
    // parent-relative) and are not directly comparable. Compare
    // the port rows themselves, which are the siblings the gap sits between.
    const synth::ui::Node* wrldInputPort = FindParentOf(initialTree, wrldInputRow->id.value);
    const synth::ui::Node* wrldOutputPort = FindParentOf(initialTree, wrldOutputRow->id.value);
    Require(wrldInputPort != nullptr && wrldOutputPort != nullptr,
            "each port's captioned combo sits inside a per-port row alongside its status dot");
    Require(wrldOutputPort->bounds.x == wrldInputPort->bounds.x + wrldInputPort->bounds.width +
                                        synth::runtime_ui::ControllersLayout::kEndpointBoxGap,
            "the input and output ports keep endpoint spacing between them");
    // Bounds are parent-relative, so the output line check has to walk
    // parentage rather than compare raw y across different immediate
    // parents. The output selector's row now nests one level deeper than
    // before: its own captioned row sits inside a per-port row (paired
    // with that port's status dot), which sits inside the shared endpoints
    // cluster, which sits on line two.
    const synth::ui::Node* padsOutputPort = FindParentOf(initialTree, padsOutputRow->id.value);
    const synth::ui::Node* padsOutputEndpoints =
        padsOutputPort != nullptr ? FindParentOf(initialTree, padsOutputPort->id.value) : nullptr;
    const synth::ui::Node* padsOutputLine =
        padsOutputEndpoints != nullptr ? FindParentOf(initialTree, padsOutputEndpoints->id.value)
                                       : nullptr;
    Require(padsOutputLine != nullptr &&
                padsOutputLine->id.value == synth::runtime_ui::NodeIds::ControllerRow(1) + ".line2",
            "the output selector is on the ports line");
    Require(wrldInputDot->bounds.x < wrldInputRow->bounds.x &&
                wrldOutputDot->bounds.x < wrldOutputRow->bounds.x,
            "each port's status dot precedes its own combo on line two");

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "custom"));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAddController));
    Require(harness.commits == 1, "add controller commits");
    Require(harness.instrument.controllers.size() == 4, "add controller increases count");
    Require(harness.status.find("Added") != std::string::npos, "add controller status");

    surface.MarkDirty();
    surface.RefreshOnTick();
    Require(surface.ViewModel().Controllers().size() == 4, "controller rows rebuild after add");
    const synth::ui::NodeTree afterAddTree = surface.BuildTree();
    const synth::ui::Node* inputCombo =
        FindNodeById(afterAddTree, synth::runtime_ui::NodeIds::ControllerInput(0));
    Require(inputCombo != nullptr && !inputCombo->options.empty(), "endpoint selector renders");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kEndpointSelect, "0:output:none"));
    Require(harness.commits == 2, "endpoint clear commits");
    Require(harness.instrument.controllers[0].output.identifier.empty(), "endpoint cleared");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kEndpointSelect, "0:output:uid:782494201"));
    Require(harness.commits == 3, "endpoint device selection commits");
    Require(harness.instrument.controllers[0].output.identifier == "uid:782494201", "endpoint device selected");
    Require(harness.status == "Selected Midi Fighter Twister", "endpoint device selection status");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kEndpointSelect, "1:input:keep_offline"));
    Require(harness.commits == 3, "offline endpoint keep is a no-op");

    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.ViewModel().ToggleConfig(0);
    surface.ViewModel().ToggleSection(0, synth::MidiConfigSection::Encoders);
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree expandedTree = surface.BuildTree();
    const synth::ui::Node* encoderSection =
        FindNodeById(expandedTree,
                     synth::runtime_ui::NodeIds::SectionBody(0, synth::MidiConfigSection::Encoders));
    Require(encoderSection != nullptr &&
                encoderSection->bounds.height > synth::runtime_ui::ControllersLayout::kSectionMaxHeight,
            "expanded encoder section is not capped");

    const std::vector<synth::MidiMappingRowVM> editableEncoderRows =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> turnStepRowIx;
    for (std::size_t ix = 0; ix < editableEncoderRows.size(); ++ix)
    {
        for (synth::MidiMappingRowVM::Field field : editableEncoderRows[ix].editableFields)
        {
            if (field == synth::MidiMappingRowVM::Field::TurnStep)
            {
                turnStepRowIx = ix;
                break;
            }
        }
        if (turnStepRowIx.has_value())
        {
            break;
        }
    }
    Require(turnStepRowIx.has_value(), "find editable turn-step row");

    const std::string acceptValue = "0:encoders:" + std::to_string(*turnStepRowIx) + ":" +
                                    std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::TurnStep)) +
                                    ":0.25";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, acceptValue));
    Require(harness.commits == 4, "mapping edit acceptance commits");
    Require(harness.status == "OK", "mapping edit accepted status");

    const std::string refuseValue = "0:encoders:" + std::to_string(*turnStepRowIx) + ":" +
                                    std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::TurnStep)) +
                                    ":-1";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, refuseValue));
    Require(harness.commits == 4, "mapping edit refusal does not commit");
    Require(harness.status.find("Refused") != std::string::npos, "mapping edit refusal status");

    const std::size_t rowCountBeforeAdd =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders).size();
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddSingle, "0:encoders:encoder_turn"));
    Require(harness.commits == 5, "add row commits");
    surface.MarkDirty();
    surface.RefreshOnTick();
    Require(surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders).size() == rowCountBeforeAdd + 1,
            "add row increases section rows");

    const std::vector<synth::MidiMappingRowVM> rowsBeforeDelete =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> deleteRowIx;
    for (std::size_t ix = 0; ix < rowsBeforeDelete.size(); ++ix)
    {
        if (rowsBeforeDelete[ix].deletable && rowsBeforeDelete[ix].kind == synth::MidiMappingRowVM::Kind::Individual)
        {
            deleteRowIx = ix;
            break;
        }
    }
    Require(deleteRowIx.has_value(), "find deletable individual row");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kDeleteRow, "0:encoders:" + std::to_string(*deleteRowIx)));
    Require(harness.commits == 6, "delete row commits");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddBlock, "0:encoders:encoder_turn"));
    Require(harness.commits == 7, "add block commits");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const std::vector<synth::MidiMappingRowVM> encoderRows =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> blockRowIx;
    for (std::size_t ix = 0; ix < encoderRows.size(); ++ix)
    {
        if (encoderRows[ix].kind == synth::MidiMappingRowVM::Kind::Block)
        {
            blockRowIx = ix;
            break;
        }
    }
    Require(blockRowIx.has_value(), "add block creates block row");
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kDeleteRow, "0:encoders:" + std::to_string(*blockRowIx)));
    Require(harness.commits == 8, "delete block commits");

    surface.ViewModel().ToggleConfig(2);
    surface.ViewModel().ToggleSection(2, synth::MidiConfigSection::Encoders);
    surface.MarkDirty();
    surface.RefreshOnTick();
    bool foundEmptyGroupAdd = false;
    for (const synth::ui::Node& node : surface.BuildTree().nodes)
    {
        if (node.action.has_value() && node.action->name == synth::runtime_ui::Actions::kAddSingle &&
            node.action->value.find("2:encoders:") == 0)
        {
            foundEmptyGroupAdd = true;
        }
    }
    Require(foundEmptyGroupAdd, "empty-group add affordance");

    harness.instrument.controllers[0].name = "renamed_out_of_band";
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree renamedTree = surface.BuildTree();
    const synth::ui::Node* renamed =
        FindNodeById(renamedTree, synth::runtime_ui::NodeIds::ControllerName(0));
    Require(renamed != nullptr && renamed->text == "renamed_out_of_band", "out-of-band refresh updates tree");

    surface.SetFocusGuard([] { return true; });
    harness.connection.controllers[0].input.status = synth::MidiEndpointStatus::Offline;
    surface.MarkDirty();
    surface.RefreshOnTick();
    Require(surface.ViewModel().Controllers()[0].inputStatus == synth::MidiEndpointStatus::Online,
            "focus guard blocks refresh while editing");
    surface.SetFocusGuard({});
    surface.RefreshOnTick();
    Require(surface.ViewModel().Controllers()[0].inputStatus == synth::MidiEndpointStatus::Offline,
            "deferred refresh after focus released");

    surface.SetContentBounds({0.0f, 0.0f, 900.0f, 260.0f});
    if (!surface.ViewModel().Controllers()[0].configExpanded)
    {
        surface.ViewModel().ToggleConfig(0);
    }
    if (!surface.ViewModel().SectionExpanded(0, synth::MidiConfigSection::SystemMessages))
    {
        surface.ViewModel().ToggleSection(0, synth::MidiConfigSection::SystemMessages);
    }
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree scrolledTree = surface.BuildTree();
    const synth::ui::Node* scroll = FindNodeById(scrolledTree, synth::runtime_ui::NodeIds::kScroll);
    Require(scroll != nullptr, "scroll node exists for small viewport");
    Require(scroll->scrollContentHeight > scroll->bounds.height, "scroll content extent exceeds viewport height");

    bool sawSystemMessageKindCombo = false;
    for (const synth::ui::Node& node : scrolledTree.nodes)
    {
        if (node.kind != synth::ui::NodeKind::ComboBox)
        {
            continue;
        }
        bool hasSceneSelect = false;
        for (const synth::ui::ControlOption& option : node.options)
        {
            Require(option.label.find("Scene Select ") == std::string::npos,
                    "scene select combo label does not bake argument");
            Require(option.label.find("Bank Select ") == std::string::npos,
                    "bank select combo label does not bake argument");
            Require(option.label.find("Gesture Select ") == std::string::npos,
                    "gesture select combo label does not bake argument");
            if (option.label == "Scene Select")
            {
                hasSceneSelect = true;
            }
        }
        sawSystemMessageKindCombo = sawSystemMessageKindCombo || hasSceneSelect;
    }
    Require(sawSystemMessageKindCombo, "system message kind combo uses argument-free labels");

    if (!surface.ViewModel().SectionExpanded(0, synth::MidiConfigSection::Encoders))
    {
        surface.ViewModel().ToggleSection(0, synth::MidiConfigSection::Encoders);
    }
    surface.MarkDirty();
    surface.RefreshOnTick();
    const std::vector<synth::MidiMappingRowVM> rowsBeforeAbsolute =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> modeRowIx;
    std::optional<std::size_t> retainedStepRowIx;
    for (std::size_t ix = 0; ix < rowsBeforeAbsolute.size(); ++ix)
    {
        if (rowsBeforeAbsolute[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderMode)
        {
            modeRowIx = ix;
        }
        if (rowsBeforeAbsolute[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderStep)
        {
            retainedStepRowIx = ix;
        }
    }
    Require(modeRowIx.has_value() && retainedStepRowIx.has_value(), "mode and step rows remain present");
    Require(!rowsBeforeAbsolute[*modeRowIx].deletable && !rowsBeforeAbsolute[*retainedStepRowIx].deletable,
            "mode and step rows remain non-deletable");

    const synth::ui::NodeTree beforeAbsoluteTree = surface.BuildTree();
    const synth::ui::Node* modeFieldBefore = FindNodeById(
        beforeAbsoluteTree,
        synth::runtime_ui::NodeIds::MappingField(
            0, synth::MidiConfigSection::Encoders, *modeRowIx, synth::MidiMappingRowVM::Field::EncoderMode));
    Require(modeFieldBefore != nullptr && modeFieldBefore->options.size() == 3,
            "portable mode combo exposes three declaration-order choices");
    Require(modeFieldBefore->options[0].label == "Signed 7-bit" &&
                modeFieldBefore->options[1].label == "Direction only" &&
                modeFieldBefore->options[2].label == "Absolute",
            "portable mode combo labels all modes in declaration order");
    Require(modeFieldBefore->selectedOption == "0", "portable mode combo starts signed 7-bit");
    const bool sawRelativeOnlyCue =
        VisibleTextLower(beforeAbsoluteTree).find("relative modes only") != std::string::npos;
    Require(sawRelativeOnlyCue, "portable turn-step row identifies relative-only behavior");

    const std::string absoluteValue =
        "0:encoders:" + std::to_string(*modeRowIx) + ":" +
        std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::EncoderMode)) + ":2";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, absoluteValue));
    Require(harness.commits == 9, "absolute mode edit commits through Controllers surface");
    Require(harness.instrument.controllers[0].config.encoderInput->mode == synth::EncoderMode::Absolute,
            "Controllers commit persists absolute mode");
    Require(harness.instrument.controllers[0].config.encoderInput->turnStep == 0.25f,
            "absolute mode commit retains stored relative turn step");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const std::vector<synth::MidiMappingRowVM> rowsAfterAbsolute =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    Require(rowsAfterAbsolute.size() == rowsBeforeAbsolute.size(), "absolute rebuild keeps open row count");
    for (std::size_t ix = 0; ix < rowsBeforeAbsolute.size(); ++ix)
    {
        Require(rowsAfterAbsolute[ix].kind == rowsBeforeAbsolute[ix].kind &&
                    rowsAfterAbsolute[ix].group == rowsBeforeAbsolute[ix].group,
                "absolute rebuild keeps open row identity and order");
    }
    const synth::ui::NodeTree afterAbsoluteTree = surface.BuildTree();
    const synth::ui::Node* modeFieldAfter = FindNodeById(
        afterAbsoluteTree,
        synth::runtime_ui::NodeIds::MappingField(
            0, synth::MidiConfigSection::Encoders, *modeRowIx, synth::MidiMappingRowVM::Field::EncoderMode));
    Require(modeFieldAfter != nullptr && modeFieldAfter->selectedOption == "2",
            "open portable mode row survives rebuild with absolute selected");

    synth::MessageInBus absoluteBus(nullptr, 16);
    auto absoluteProfile = synth::CreateMidiControllerProfile(
        harness.instrument.controllers[0].config, &absoluteBus, nullptr, nullptr, [] { return 701; });
    Require(absoluteProfile.input != nullptr, "committed config rebuilds live input processor");
    const auto turn = harness.instrument.controllers[0].config.encoderInput->turns.front();
    absoluteProfile.input->Process(synth::BasicMidi::CC(1, turn.control.channel, turn.control.cc, 127));
    synth::MessageIn message;
    Require(absoluteBus.Pop(message, 701) && message.type == synth::MessageIn::Type::ParamSetAbsolute &&
                message.value == 1.0f,
            "rebuilt processor applies committed absolute mode");

    const std::string relativeValue =
        "0:encoders:" + std::to_string(*modeRowIx) + ":" +
        std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::EncoderMode)) + ":0";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, relativeValue));
    Require(harness.commits == 10, "switching back to signed mode commits");
    Require(harness.instrument.controllers[0].config.encoderInput->turnStep == 0.25f,
            "switching back restores stored relative step");
    synth::MessageInBus relativeBus(nullptr, 16);
    auto relativeProfile = synth::CreateMidiControllerProfile(
        harness.instrument.controllers[0].config, &relativeBus, nullptr, nullptr, [] { return 702; });
    relativeProfile.input->Process(synth::BasicMidi::CC(1, turn.control.channel, turn.control.cc, 65));
    Require(relativeBus.Pop(message, 702) && message.type == synth::MessageIn::Type::ParamIncDec &&
                message.delta == 0.25f,
            "rebuilt relative processor uses the retained step");

    TestHarness typedHarness;
    synth::MidiControllerSlot& generic = typedHarness.instrument.controllers[2];
    synth::EncoderMidiInConfig typedEncoders;
    typedEncoders.turns.push_back({.control = {.channel = 1, .cc = 10}, .slotIx = 0, .position = 0});
    typedEncoders.pushes.push_back(
        {.control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note}, .slotIx = 0, .position = 0});
    typedEncoders.pushes.push_back(
        {.control = {.channel = 1, .cc = 61, .type = synth::MidiControlType::Note}, .slotIx = 0, .position = 1});
    typedEncoders.pushes.push_back({.control = {.channel = 1, .cc = 90}, .slotIx = 0, .position = 9});
    generic.config.encoderInput = std::move(typedEncoders);

    auto makeGenericScene = [](std::uint8_t cc, std::size_t sceneIx, synth::MidiControlType type) {
        synth::MidiControllerSystemMessageAssociation association;
        association.control = synth::MidiControlAddress{.channel = 2, .cc = cc, .type = type};
        association.press = synth::MessageIn::SceneSelect(0, sceneIx);
        association.feedback = association.press;
        return association;
    };
    generic.config.systemMessages = {
        makeGenericScene(40, 0, synth::MidiControlType::Note),
        makeGenericScene(41, 1, synth::MidiControlType::Note),
        makeGenericScene(90, 9, synth::MidiControlType::Cc),
    };

    synth::runtime_ui::ControllersPageSurface typedSurface = typedHarness.MakeSurface();
    typedSurface.SetEnumerateDevices(typedHarness.devices);
    typedSurface.SetContentBounds({0.0f, 0.0f, 1000.0f, 800.0f});
    typedSurface.MarkDirty();
    typedSurface.RefreshOnTick();
    typedSurface.ViewModel().ToggleConfig(2);
    typedSurface.ViewModel().ToggleSection(2, synth::MidiConfigSection::Encoders);
    typedSurface.ViewModel().ToggleSection(2, synth::MidiConfigSection::SystemMessages);
    typedSurface.ViewModel().ToggleConfig(0);
    typedSurface.ViewModel().ToggleSection(0, synth::MidiConfigSection::SystemMessages);
    typedSurface.MarkDirty();
    typedSurface.RefreshOnTick();

    const std::vector<synth::MidiMappingRowVM> typedEncoderRows =
        typedSurface.ViewModel().SectionRows(2, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> pushBlockIx;
    std::optional<std::size_t> pushIndividualIx;
    std::optional<std::size_t> turnIx;
    for (std::size_t ix = 0; ix < typedEncoderRows.size(); ++ix)
    {
        if (typedEncoderRows[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderTurn)
        {
            turnIx = ix;
        }
        else if (typedEncoderRows[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderPush &&
                 typedEncoderRows[ix].kind == synth::MidiMappingRowVM::Kind::Block)
        {
            pushBlockIx = ix;
        }
        else if (typedEncoderRows[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderPush &&
                 typedEncoderRows[ix].kind == synth::MidiMappingRowVM::Kind::Individual)
        {
            pushIndividualIx = ix;
        }
    }
    Require(pushBlockIx.has_value() && pushIndividualIx.has_value() && turnIx.has_value(),
            "typed encoder fixture has turn, push block, and push individual rows");

    const std::vector<synth::MidiMappingRowVM> typedSystemRows =
        typedSurface.ViewModel().SectionRows(2, synth::MidiConfigSection::SystemMessages);
    std::optional<std::size_t> systemBlockIx;
    std::optional<std::size_t> systemIndividualIx;
    for (std::size_t ix = 0; ix < typedSystemRows.size(); ++ix)
    {
        if (typedSystemRows[ix].kind == synth::MidiMappingRowVM::Kind::Block)
        {
            systemBlockIx = ix;
        }
        else if (typedSystemRows[ix].kind == synth::MidiMappingRowVM::Kind::Individual)
        {
            systemIndividualIx = ix;
        }
    }
    Require(systemBlockIx.has_value() && systemIndividualIx.has_value(),
            "typed system fixture has block and individual rows");

    const synth::ui::NodeTree typedTree = typedSurface.BuildTree();
    const synth::ui::Node* blockMessageTypeHeader = FindNodeById(
        typedTree,
        synth::runtime_ui::NodeIds::GroupColumnLabel(
            2, synth::MidiConfigSection::SystemMessages, 0, 0));
    const synth::ui::Node* blockAddressTypeHeader = FindNodeById(
        typedTree,
        synth::runtime_ui::NodeIds::GroupColumnLabel(
            2, synth::MidiConfigSection::SystemMessages, 0, 1));
    Require(blockMessageTypeHeader != nullptr && blockMessageTypeHeader->text == "Type",
            "Generic system block message-type header remains Type");
    Require(blockAddressTypeHeader != nullptr && blockAddressTypeHeader->text == "Addr",
            "Generic system block address-type header is distinct");

    auto requireTypeCombo = [&](synth::MidiConfigSection section, std::size_t rowIx,
                                const std::string& selected) -> const synth::ui::Node* {
        const synth::ui::Node* node = FindNodeById(
            typedTree,
            synth::runtime_ui::NodeIds::MappingField(
                2, section, rowIx, synth::MidiMappingRowVM::Field::AddressType));
        Require(node != nullptr && node->kind == synth::ui::NodeKind::ComboBox,
                "address type renders as combo box");
        Require(node->options.size() == 2 && node->options[0].id == "0" && node->options[0].label == "CC" &&
                    node->options[1].id == "1" && node->options[1].label == "Note",
                "address type combo exposes CC and Note in enum order");
        Require(node->selectedOption == selected, "address type combo selection follows model");
        return node;
    };
    requireTypeCombo(synth::MidiConfigSection::Encoders, *pushBlockIx, "1");
    const synth::ui::Node* pushIndividualType =
        requireTypeCombo(synth::MidiConfigSection::Encoders, *pushIndividualIx, "0");
    const synth::ui::Node* blockMessageTypeField = FindNodeById(
        typedTree,
        synth::runtime_ui::NodeIds::MappingField(
            2, synth::MidiConfigSection::SystemMessages, *systemBlockIx,
            synth::MidiMappingRowVM::Field::BlockMessageType));
    Require(blockMessageTypeField != nullptr,
            "Generic system block message-type field is present");
    Require(blockMessageTypeHeader->bounds.x == blockMessageTypeField->bounds.x,
            "first column header aligns with the first mapping field");
    Require(pushIndividualType->color.has_value() &&
                *pushIndividualType->color == synth::pagestyle::kDefaultPanel,
            "mapping combo boxes carry the field background rather than the button background");
    requireTypeCombo(synth::MidiConfigSection::SystemMessages, *systemBlockIx, "1");
    requireTypeCombo(synth::MidiConfigSection::SystemMessages, *systemIndividualIx, "0");

    const synth::ui::Node* numericAddress = FindNodeById(
        typedTree,
        synth::runtime_ui::NodeIds::MappingField(
            2, synth::MidiConfigSection::Encoders, *pushIndividualIx, synth::MidiMappingRowVM::Field::Cc));
    const synth::ui::Node* numericChannel = FindNodeById(
        typedTree,
        synth::runtime_ui::NodeIds::MappingField(
            2, synth::MidiConfigSection::Encoders, *pushIndividualIx, synth::MidiMappingRowVM::Field::Channel));
    Require(numericAddress != nullptr && numericAddress->kind == synth::ui::NodeKind::TextField &&
                numericAddress->text == "90",
            "note-capable row keeps numeric message number as decimal text field");
    Require(numericChannel != nullptr && numericChannel->kind == synth::ui::NodeKind::TextField &&
                numericChannel->text == "1",
            "note-capable row keeps channel as decimal text field");
    Require(numericChannel->color.has_value() &&
                *numericChannel->color == synth::pagestyle::kDefaultPanel,
            "mapping text fields carry the field background rather than the button background");
    Require(FindNodeById(
                typedTree,
                synth::runtime_ui::NodeIds::MappingField(
                    2, synth::MidiConfigSection::Encoders, *turnIx,
                    synth::MidiMappingRowVM::Field::AddressType)) == nullptr,
            "encoder turn has no address type node");
    const std::vector<synth::MidiMappingRowVM> wrldSystemRows =
        typedSurface.ViewModel().SectionRows(0, synth::MidiConfigSection::SystemMessages);
    for (std::size_t ix = 0; ix < wrldSystemRows.size(); ++ix)
    {
        Require(FindNodeById(
                    typedTree,
                    synth::runtime_ui::NodeIds::MappingField(
                        0, synth::MidiConfigSection::SystemMessages, ix,
                        synth::MidiMappingRowVM::Field::AddressType)) == nullptr,
                "controller-specific system row has no address type node");
    }

    Require(pushIndividualType->action.has_value() &&
                pushIndividualType->action->name == synth::runtime_ui::Actions::kMappingFieldCommit,
            "address type combo uses existing mapping commit action");
    typedSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kMappingFieldCommit, pushIndividualType->action->value + ":1"));
    Require(typedHarness.commits == 1, "address type combo commits through Controllers surface");
    bool committedNote90 = false;
    for (const synth::EncoderMidiMapping& mapping : typedHarness.instrument.controllers[2].config.encoderInput->pushes)
    {
        committedNote90 = committedNote90 ||
                          (mapping.control.cc == 90 && mapping.control.type == synth::MidiControlType::Note);
    }
    Require(committedNote90, "address type combo commit persists Note without changing number");

    typedSurface.MarkDirty();
    typedSurface.RefreshOnTick();
    const synth::ui::NodeTree committedTypedTree = typedSurface.BuildTree();
    const synth::ui::Node* committedType = FindNodeById(
        committedTypedTree,
        synth::runtime_ui::NodeIds::MappingField(
            2, synth::MidiConfigSection::Encoders, *pushIndividualIx,
            synth::MidiMappingRowVM::Field::AddressType));
    Require(committedType != nullptr && committedType->selectedOption == "1",
            "committed Note selection survives open-session rebuild");

    const synth::ui::Node* systemIndividualType = FindNodeById(
        committedTypedTree,
        synth::runtime_ui::NodeIds::MappingField(
            2, synth::MidiConfigSection::SystemMessages, *systemIndividualIx,
            synth::MidiMappingRowVM::Field::AddressType));
    Require(systemIndividualType != nullptr && systemIndividualType->action.has_value(),
            "Generic system address type combo remains dispatchable");
    typedSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kMappingFieldCommit, systemIndividualType->action->value + ":1"));
    Require(typedHarness.commits == 2, "Generic system address type combo commits");
    bool committedSystemNote90 = false;
    for (const synth::MidiControllerSystemMessageAssociation& association :
         typedHarness.instrument.controllers[2].config.systemMessages)
    {
        committedSystemNote90 = committedSystemNote90 ||
                                (association.control.has_value() && association.control->cc == 90 &&
                                 association.control->type == synth::MidiControlType::Note);
    }
    Require(committedSystemNote90, "Generic system combo persists Note without changing number");
    TestHarness gridHarness;
    SeedGridPresentation(gridHarness);
    synth::runtime_ui::ControllersPageSurface gridSurface = gridHarness.MakeSurface();
    gridSurface.SetContentBounds({0.0f, 0.0f, 900.0f, 260.0f});
    gridSurface.MarkDirty();
    gridSurface.RefreshOnTick();
    gridSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    gridSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "0:system_messages"));
    gridSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "1"));
    gridSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "1:system_messages"));

    const synth::ui::NodeTree gridTree = gridSurface.BuildTree();
    const std::string visible = VisibleTextLower(gridTree);
    Require(visible.find("grid button") != std::string::npos, "portable tree shows Grid Button");
    Require(visible.find("grid block") != std::string::npos, "portable tree shows Grid Block");
    for (const char* label : {"grid slot", "x min", "x max", "y min", "y max"})
    {
        Require(visible.find(label) != std::string::npos, "portable tree shows exact grid field label");
    }
    Require(visible.find("aftertouch") == std::string::npos, "portable tree hides aftertouch");
    Require(visible.find("polyphonic pressure") == std::string::npos,
            "portable tree hides polyphonic pressure");
    // The removed row editor's heading was "Pressure mappings", which neither
    // of the two greps above matches -- so this is the only assertion that
    // fails if a per-row pressure-mapping list is ever rendered again.
    Require(visible.find("pressure mapping") == std::string::npos,
            "portable tree hides the per-row pressure-mapping list");
    Require(visible.find("midi status") == std::string::npos, "portable tree hides MIDI status");
    Require(visible.find("note number") == std::string::npos, "portable tree hides standalone note field");

    bool sawNegativeSignedEditor = false;
    bool sawGridAdd = false;
    std::string stableGridFieldId;
    for (const synth::ui::Node& node : gridTree.nodes)
    {
        sawNegativeSignedEditor = sawNegativeSignedEditor ||
                                  (node.kind == synth::ui::NodeKind::TextField && node.text == "-1");
        if (node.action.has_value() && node.action->name == synth::runtime_ui::Actions::kAddSingle &&
            node.action->value == "0:system_messages:grid")
        {
            sawGridAdd = true;
        }
        if (stableGridFieldId.empty() && node.id.value.find(".mapping.") != std::string::npos &&
            node.kind == synth::ui::NodeKind::TextField && node.text == "3")
        {
            stableGridFieldId = node.id.value;
        }
    }
    Require(sawNegativeSignedEditor, "portable tree renders negative signed grid coordinate");
    Require(sawGridAdd, "portable tree routes Grid add action token");
    Require(!stableGridFieldId.empty(), "portable tree exposes stable grid field id");

    gridSurface.MarkDirty();
    gridSurface.RefreshOnTick();
    Require(FindNodeById(gridSurface.BuildTree(), stableGridFieldId) != nullptr,
            "grid field node id survives rebuild");
    const int commitsBeforeGridAdd = gridHarness.commits;
    gridSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddSingle, "0:system_messages:grid"));
    Require(gridHarness.commits == commitsBeforeGridAdd + 1, "portable Grid add action commits pair");
    Require(gridHarness.instrument.controllers[0].config.pressureInput.has_value(),
            "portable Grid add preserves pressure container");
    Require(gridHarness.instrument.controllers[0].config.pressureInput->mappings.size() ==
                gridHarness.instrument.controllers[0].config.systemMessages.size(),
            "portable Grid add commits one pressure mapping per visible cell");

    std::cout << "controllers_page_ui_tests passed\n";
    return 0;
}
