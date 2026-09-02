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

// A resolved tree that renders is one where every node has an extent. A tree
// whose children all collapsed to nothing at the parent origin satisfies every
// presence and action assertion in this file, so these three look at geometry.
bool EveryNodeHasExtent(const synth::ui::NodeTree& tree)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.bounds.width <= 0.0f || node.bounds.height <= 0.0f)
        {
            return false;
        }
    }
    return true;
}

bool BoundsAre(const synth::ui::Node* node, float x, float y, float width, float height)
{
    return node != nullptr && node->bounds.x == x && node->bounds.y == y &&
           node->bounds.width == width && node->bounds.height == height;
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

bool StacksInDeclarationOrder(const synth::ui::NodeTree& tree, const std::string& parentId)
{
    const synth::ui::Node* parent = FindNodeById(tree, parentId);
    if (parent == nullptr || parent->children.empty())
    {
        return false;
    }
    float bottom = 0.0f;
    for (const synth::ui::NodeId& childId : parent->children)
    {
        const synth::ui::Node* child = FindNodeById(tree, childId.value);
        if (child == nullptr || child->bounds.y < bottom)
        {
            return false;
        }
        bottom = child->bounds.y + child->bounds.height;
    }
    return true;
}

bool ChildrenFitParent(const synth::ui::NodeTree& tree, const std::string& parentId)
{
    const synth::ui::Node* parent = FindNodeById(tree, parentId);
    if (parent == nullptr || parent->children.empty())
    {
        return false;
    }
    for (const synth::ui::NodeId& childId : parent->children)
    {
        const synth::ui::Node* child = FindNodeById(tree, childId.value);
        if (child == nullptr || child->bounds.x < 0.0f || child->bounds.y < 0.0f ||
            child->bounds.x + child->bounds.width > parent->bounds.width ||
            child->bounds.y + child->bounds.height > parent->bounds.height)
        {
            return false;
        }
    }
    return true;
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
        return synth::runtime_ui::ControllersPageSurface(std::move(callbacks));
    }
};

synth::WizardCandidate MakeTwisterCandidate(const char* suffix = "")
{
    return {.wizardId = "com.sheaf.midi-fighter-twister",
            .displayName = "MIDI Fighter Twister",
            .kind = synth::MidiProfileKind::MfTwister,
            .input = {std::string("twister-in") + suffix, "Midi Fighter Twister"},
            .output = {std::string("twister-out") + suffix, "Midi Fighter Twister"}};
}

void AttachCandidate(TestHarness& harness, const synth::WizardCandidate& candidate)
{
    harness.devices.outputs.erase(
        std::remove_if(
            harness.devices.outputs.begin(), harness.devices.outputs.end(),
            [](const synth::MidiDeviceInfoRef& device) {
                return device.name == "Midi Fighter Twister";
            }),
        harness.devices.outputs.end());
    harness.devices.inputs.push_back(candidate.input);
    harness.devices.outputs.push_back(candidate.output);
}

void RefreshWizardDiscovery(synth::runtime_ui::ControllersPageSurface& surface,
                            const TestHarness& harness)
{
    surface.SetDiscovery(synth::DiscoverControllerWizards(
        harness.devices, harness.instrument,
        synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{})));
}

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
    Require(row != nullptr, "available controller row exists");
    // D8: no backend paints a container's own label, so the area heading and the
    // recognized controller's descriptor name must be rendered child nodes. The
    // descriptor name is not derivable from the endpoint device names beside it.
    Require(row->label.empty(), "available controller row carries no unrendered label");
    const synth::ui::Node* availableSection = FindNodeById(tree, "runtime.controllers.available");
    Require(availableSection != nullptr && availableSection->label.empty(),
            "available controllers section carries no unrendered label");
    const synth::ui::Node* heading = FindNodeById(tree, "runtime.controllers.available.heading");
    Require(heading != nullptr && heading->kind == synth::ui::NodeKind::Label &&
                heading->text == "Available controllers",
            "available controllers area renders its heading");
    const synth::ui::Node* name = FindNodeById(tree, "runtime.controllers.available.0.name");
    Require(name != nullptr && name->kind == synth::ui::NodeKind::Label &&
                name->text == "MIDI Fighter Twister",
            "available controller row renders its recognized controller name");
    const synth::ui::Node* endpoints =
        FindNodeById(tree, "runtime.controllers.available.0.endpoints");
    Require(endpoints != nullptr &&
                endpoints->text == "Midi Fighter Twister / Midi Fighter Twister",
            "available controller row keeps its paired endpoint labels as their own node");
    Require(name->bounds.x + name->bounds.width <= endpoints->bounds.x,
            "the recognized controller name does not overlap its endpoint labels");
    Require(FindNodeById(tree, "runtime.controllers.available.0.configure") != nullptr,
            "available controller row exposes portable Configure action");
    Require(FindNodeById(tree, "runtime.controllers.available.0.ignore") != nullptr,
            "available controller row exposes portable Ignore action");
    Require(FindNodeById(tree, "runtime.controllers.available.unmatched_inputs") != nullptr,
            "unmatched input diagnostics are portable data");
    Require(FindNodeById(tree, "runtime.controllers.available.unmatched_outputs") != nullptr,
            "unmatched output diagnostics are portable data");

    discovery.available.clear();
    surface.SetDiscovery(std::move(discovery));
    Require(surface.TreeRevision() == discoveryRevision + 1,
            "changed discovery snapshot revises the portable tree exactly once");
}

std::string VisibleTextLower(const synth::ui::NodeTree& tree);

void TestWizardSessionRoutesPortableChooserAndForm()
{
    const synth::WizardCandidate first{
        .wizardId = "com.sheaf.midi-fighter-twister",
        .displayName = "MIDI Fighter Twister",
        .kind = synth::MidiProfileKind::MfTwister,
        .input = {"twister-in-a", "Midi Fighter Twister"},
        .output = {"twister-out-a", "Midi Fighter Twister"}};
    const synth::WizardCandidate second{
        .wizardId = "com.sheaf.midi-fighter-twister",
        .displayName = "MIDI Fighter Twister",
        .kind = synth::MidiProfileKind::MfTwister,
        .input = {"twister-in-b", "Midi Fighter Twister"},
        .output = {"twister-out-b", "Midi Fighter Twister"}};

    TestHarness emptyHarness;
    auto emptySurface = emptyHarness.MakeSurface();
    const synth::ui::NodeTree emptyTree = emptySurface.BuildTree();
    const synth::ui::Node* emptyLaunch =
        FindNodeById(emptyTree, "runtime.controllers.wizard.launch");
    Require(emptyLaunch != nullptr && !emptyLaunch->enabled,
            "zero candidates leave Configuration Wizard visibly disabled");
    Require(VisibleTextLower(emptyTree).find("no recognized unconfigured controller pair") !=
                std::string::npos,
            "zero candidates explain why the wizard is disabled");

    TestHarness uniqueHarness;
    auto uniqueSurface = uniqueHarness.MakeSurface();
    uniqueSurface.SetDiscovery({.available = {first}});
    uniqueSurface.DispatchAction(
        synth::ui::Action::Named("runtime.controllers.wizard.open"));
    const synth::ui::NodeTree formTree = uniqueSurface.BuildTree();
    Require(FindNodeById(formTree, "runtime.controllers.wizard.form") != nullptr,
            "unique candidate opens its form directly");
    Require(FindNodeById(formTree, "runtime.controllers.wizard.launch") == nullptr,
            "open form exposes no second launch action");
    Require(FindNodeById(formTree, "runtime.controllers.wizard.submit") != nullptr,
            "form exposes portable Submit action");
    Require(FindNodeById(formTree, "runtime.controllers.wizard.ignore") != nullptr,
            "new candidate form exposes portable Ignore action");
    Require(FindNodeById(formTree, "controller-wizard.twister.encoder-slot") != nullptr,
            "session dispatches the Twister form with its one Encoder Slot");
    Require(FindNodeById(formTree, "controller-wizard.twister.column.0") != nullptr &&
                FindNodeById(formTree, "controller-wizard.twister.column.1") != nullptr,
            "Twister form retains its two portable columns");
    std::size_t twisterRows = 0;
    for (const synth::ui::Node& node : formTree.nodes)
    {
        constexpr std::string_view prefix = "controller-wizard.twister.button.";
        if (node.kind == synth::ui::NodeKind::Row &&
            node.id.value.rfind(prefix, 0) == 0 &&
            std::all_of(node.id.value.begin() + static_cast<std::string::difference_type>(prefix.size()),
                        node.id.value.end(),
                        [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); }))
        {
            ++twisterRows;
        }
    }
    Require(twisterRows == 6, "Twister form renders exactly six button rows through the session");
    uniqueSurface.DispatchAction(
        synth::ui::Action::WithValue("controller-wizard.twister.encoder-slot", "5"));
    const synth::ui::Node* editedSlot =
        FindNodeById(uniqueSurface.BuildTree(), "controller-wizard.twister.encoder-slot");
    Require(editedSlot != nullptr && editedSlot->text == "5",
            "page routing dispatches edits into the form-owned state");
    uniqueSurface.SetDiscovery({});
    const synth::ui::Node* preservedSlot =
        FindNodeById(uniqueSurface.BuildTree(), "controller-wizard.twister.encoder-slot");
    Require(preservedSlot != nullptr && preservedSlot->text == "5",
            "discovery refresh does not replace an open form or its entered state");
    const std::size_t uniqueControllerCount = uniqueHarness.instrument.controllers.size();
    uniqueSurface.DispatchAction(
        synth::ui::Action::Named("runtime.controllers.wizard.cancel"));
    Require(FindNodeById(uniqueSurface.BuildTree(), "runtime.controllers.wizard.launch") != nullptr,
            "Cancel closes the session back to the Controllers list");
    Require(uniqueHarness.instrument.controllers.size() == uniqueControllerCount && uniqueHarness.commits == 0,
            "Cancel preserves the instrument without a commit");

    TestHarness chooserHarness;
    auto chooserSurface = chooserHarness.MakeSurface();
    chooserSurface.SetDiscovery({.available = {first, second}});
    chooserSurface.DispatchAction(
        synth::ui::Action::Named("runtime.controllers.wizard.open"));
    const synth::ui::NodeTree chooserTree = chooserSurface.BuildTree();
    Require(FindNodeById(chooserTree,
                         synth::runtime_ui::NodeIds::WizardChooserCandidate(first)) != nullptr &&
                FindNodeById(chooserTree,
                             synth::runtime_ui::NodeIds::WizardChooserCandidate(second)) != nullptr,
            "multiple candidates open a deterministic portable chooser");
    Require(VisibleTextLower(chooserTree).find("twister-in-a") != std::string::npos &&
                VisibleTextLower(chooserTree).find("twister-out-b") != std::string::npos,
            "chooser labels expose paired endpoint identifiers");

    // The failure mode this closes: every child resolves to zero extent at the
    // parent origin and every presence-and-actions assertion above still
    // passes. That is exactly what the chooser did between the auto-flow
    // deletion and its conversion onto the library, in both backends, invisibly.
    Require(EveryNodeHasExtent(chooserTree), "every chooser node resolves to a non-zero extent");
    const std::string chooserBody =
        std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".body";
    Require(FindNodeById(chooserTree, chooserBody) != nullptr,
            "the chooser stacks its rows in a container rather than under the root");
    Require(StacksInDeclarationOrder(chooserTree, chooserBody),
            "chooser rows stack in declaration order without overlapping");
    Require(ChildrenFitParent(chooserTree, chooserBody),
            "chooser rows resolve inside the page they were given");
    const synth::ui::Node* firstChoice =
        FindNodeById(chooserTree, synth::runtime_ui::NodeIds::WizardChooserCandidate(first));
    const synth::ui::Node* secondChoice =
        FindNodeById(chooserTree, synth::runtime_ui::NodeIds::WizardChooserCandidate(second));
    Require(firstChoice->bounds.y + firstChoice->bounds.height <= secondChoice->bounds.y,
            "the first candidate resolves above the second, in discovery order");
    // Exact geometry the resolver derives from the default 640x480 content
    // rectangle: page margin 4 and row gap 6 around a 32-high action row, a
    // 24-high heading, then one full-width 32-high button per candidate. Every
    // number here comes from a declared extent, not from a producer's arithmetic.
    Require(BoundsAre(FindNodeById(chooserTree, chooserBody), 0.0f, 0.0f, 640.0f, 480.0f),
            "the chooser body fills the content rectangle");
    Require(BoundsAre(FindNodeById(chooserTree, std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".actions"),
                      4.0f, 4.0f, 632.0f, 32.0f),
            "the action row spans the page inside its margin");
    Require(BoundsAre(FindNodeById(chooserTree, synth::runtime_ui::NodeIds::kWizardBack),
                      0.0f, 0.0f, 80.0f, 32.0f),
            "Back keeps its own width at the action row's origin");
    Require(BoundsAre(FindNodeById(chooserTree, std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".heading"),
                      4.0f, 42.0f, 632.0f, 24.0f),
            "the heading follows the action row by one row gap");
    Require(BoundsAre(firstChoice, 4.0f, 72.0f, 632.0f, 32.0f) &&
                BoundsAre(secondChoice, 4.0f, 110.0f, 632.0f, 32.0f),
            "candidate buttons take the page width and stack one row gap apart");

    const synth::ui::Action staleFirstChoice =
        *FindNodeById(chooserTree, synth::runtime_ui::NodeIds::WizardChooserCandidate(first))->action;
    const synth::ui::Action staleSecondChoice =
        *FindNodeById(chooserTree, synth::runtime_ui::NodeIds::WizardChooserCandidate(second))->action;
    chooserSurface.SetDiscovery({.available = {second}});
    const synth::ui::NodeTree refreshedChooser = chooserSurface.BuildTree();
    Require(FindNodeById(refreshedChooser,
                         synth::runtime_ui::NodeIds::WizardChooserCandidate(second)) != nullptr &&
                FindNodeById(refreshedChooser,
                             synth::runtime_ui::NodeIds::WizardChooserCandidate(first)) == nullptr,
            "chooser refresh drops disappeared candidates");
    chooserSurface.DispatchAction(staleFirstChoice);
    const synth::ui::NodeTree staleFirstTree = chooserSurface.BuildTree();
    Require(FindNodeById(staleFirstTree, "runtime.controllers.wizard.form") == nullptr &&
                FindNodeById(staleFirstTree, "runtime.controllers.wizard.chooser.status") != nullptr,
            "stale chooser action does not silently open a different candidate");
    chooserSurface.DispatchAction(staleSecondChoice);
    Require(FindNodeById(chooserSurface.BuildTree(), "runtime.controllers.wizard.form") != nullptr,
            "stable chooser action opens its original candidate after refresh");

    TestHarness emptyChooserHarness;
    auto emptyChooserSurface = emptyChooserHarness.MakeSurface();
    emptyChooserSurface.SetDiscovery({.available = {first, second}});
    emptyChooserSurface.DispatchAction(
        synth::ui::Action::Named("runtime.controllers.wizard.open"));
    emptyChooserSurface.SetDiscovery({});
    Require(FindNodeById(emptyChooserSurface.BuildTree(), "runtime.controllers.wizard.chooser.empty") != nullptr,
            "empty refreshed chooser explains that no candidates remain");

    TestHarness deferredHarness;
    auto deferredSurface = deferredHarness.MakeSurface();
    for (const char* actionName : {synth::runtime_ui::Actions::kBack,
                                   synth::runtime_ui::Actions::kAvailableConfigure,
                                   synth::runtime_ui::Actions::kWizardOpen,
                                   synth::runtime_ui::Actions::kWizardChoose,
                                   synth::runtime_ui::Actions::kWizardBack,
                                   synth::runtime_ui::Actions::kWizardCancel})
    {
        Require(deferredSurface.NeedsDeferredDispatch(synth::ui::Action::Named(actionName)),
                "wizard navigation action requires deferred dispatch");
    }

    TestHarness existingHarness;
    synth::MidiControllerSlot existing;
    existing.name = "existing twister";
    existing.kind = synth::MidiProfileKind::MfTwister;
    existing.config = synth::MfTwisterDefaultProfileConfig();
    existing.wizardId = "com.sheaf.midi-fighter-twister";
    existing.input = {.identifier = first.input.identifier, .name = first.input.name};
    existing.output = {.identifier = first.output.identifier, .name = first.output.name};
    Require(existingHarness.instrument.AddController(std::move(existing)), "add existing Twister record");
    auto existingSurface = existingHarness.MakeSurface();
    Require(existingSurface.OpenExisting(3), "existing wizard record opens a portable session");
    const synth::ui::NodeTree existingTree = existingSurface.BuildTree();
    Require(FindNodeById(existingTree, "runtime.controllers.wizard.ignore") == nullptr,
            "existing-record session does not expose Ignore");
    const std::size_t existingControllerCount = existingHarness.instrument.controllers.size();
    existingSurface.DispatchAction(
        synth::ui::Action::Named("runtime.controllers.wizard.back"));
    Require(existingHarness.instrument.controllers.size() == existingControllerCount && existingHarness.commits == 0,
            "Back closes an existing-record session without changing the instrument");
}

void TestWizardSubmitCommitsCompleteProfileThenSaves()
{
    TestHarness harness;
    Require(harness.instrument.AddController(MakeGenericSlot("MIDI Fighter Twister")),
            "occupy base Twister display name");
    Require(harness.instrument.AddController(MakeGenericSlot("MIDI Fighter Twister 3")),
            "leave the smallest suffix gap at 2");
    harness.connection.controllers.resize(harness.instrument.controllers.size());
    const synth::WizardCandidate candidate = MakeTwisterCandidate();
    AttachCandidate(harness, candidate);

    auto surface = harness.MakeSurface();
    RefreshWizardDiscovery(surface, harness);
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    surface.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.encoder-slot", "5"));
    surface.DispatchAction(synth::ui::Action::Named(
        synth::runtime_ui::Actions::kWizardSubmit));

    Require(harness.commitAttempts == 1 && harness.commits == 1,
            "Submit requests exactly one accepted instrument commit");
    Require(harness.saves == 1 &&
                harness.persistenceEvents == std::vector<std::string>({"commit", "save"}),
            "Submit requests save exactly once after the accepted commit");
    Require(harness.instrument.controllers.size() == 6,
            "Submit appends exactly one controller record");
    const synth::MidiControllerSlot& installed = harness.instrument.controllers.back();
    Require(installed.name == "MIDI Fighter Twister 2",
            "Submit chooses the smallest unused numeric display-name suffix");
    Require(installed.kind == synth::MidiProfileKind::MfTwister &&
                installed.disposition == synth::MidiControllerDisposition::Active,
            "Submit installs one complete Active Twister record");
    Require(installed.wizardId == candidate.wizardId,
            "Submit persists the descriptor's stable opaque wizard id");
    Require(installed.input.identifier == candidate.input.identifier &&
                installed.input.name == candidate.input.name &&
                installed.output.identifier == candidate.output.identifier &&
                installed.output.name == candidate.output.name,
            "Submit persists both concrete endpoint identities");
    Require(installed.config.encoderInput.has_value() &&
                installed.config.encoderInput->turns.size() == 16 &&
                installed.config.encoderInput->pushes.size() == 16 &&
                installed.config.encoderOutput.has_value() &&
                installed.config.encoderOutput->mappings.size() == 16 &&
                installed.config.systemMessages.size() == 6,
            "Submit commits the complete generated Twister profile");
    for (const synth::EncoderMidiMapping& turn : installed.config.encoderInput->turns)
    {
        Require(turn.slotIx == 5, "submitted encoder turns retain the entered form slot");
    }
    Require(surface.Discovery().available.empty(),
            "successful Submit refreshes candidate classification immediately");
    Require(surface.ActiveWizardSession() == nullptr,
            "successful commit and save close the new-candidate form");
}

void TestWizardSubmitRefusalsRetainFormAndPersistence()
{
    const synth::WizardCandidate candidate = MakeTwisterCandidate();

    TestHarness disconnectedHarness;
    AttachCandidate(disconnectedHarness, candidate);
    auto disconnectedSurface = disconnectedHarness.MakeSurface();
    RefreshWizardDiscovery(disconnectedSurface, disconnectedHarness);
    disconnectedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    disconnectedSurface.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.encoder-slot", "7"));
    disconnectedHarness.devices.outputs.clear();
    disconnectedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardSubmit));
    const synth::ui::NodeTree disconnectedTree = disconnectedSurface.BuildTree();
    Require(disconnectedHarness.commitAttempts == 0 && disconnectedHarness.saves == 0,
            "disappeared candidate refuses without commit or save");
    Require(FindNodeById(disconnectedTree, "controller-wizard.twister.encoder-slot")->text == "7",
            "disappeared candidate retains every entered form value");
    Require(FindNodeById(disconnectedTree, synth::runtime_ui::NodeIds::kWizardStatus) != nullptr &&
                VisibleTextLower(disconnectedTree).find("reconnect") != std::string::npos,
            "disappeared candidate keeps the form open with an inline reconnect status");

    TestHarness contendedHarness;
    AttachCandidate(contendedHarness, candidate);
    auto contendedSurface = contendedHarness.MakeSurface();
    RefreshWizardDiscovery(contendedSurface, contendedHarness);
    contendedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    synth::MidiControllerSlot claimant = MakeGenericSlot("out-of-band claimant");
    claimant.input = {.identifier = candidate.input.identifier, .name = candidate.input.name};
    Require(contendedHarness.instrument.AddController(std::move(claimant)),
            "out-of-band record claims one candidate endpoint");
    contendedHarness.connection.controllers.resize(contendedHarness.instrument.controllers.size());
    contendedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardSubmit));
    Require(contendedHarness.commitAttempts == 0 && contendedHarness.saves == 0,
            "contended candidate refuses without commit or save");
    Require(contendedSurface.ActiveWizardSession() != nullptr &&
                VisibleTextLower(contendedSurface.BuildTree()).find("no longer available") !=
                    std::string::npos,
            "contention retains the open form with an inline stale-candidate status");

    TestHarness invalidHarness;
    AttachCandidate(invalidHarness, candidate);
    auto invalidSurface = invalidHarness.MakeSurface();
    RefreshWizardDiscovery(invalidSurface, invalidHarness);
    invalidSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    invalidSurface.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.encoder-slot", "-1"));
    invalidSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardSubmit));
    const synth::ui::NodeTree invalidTree = invalidSurface.BuildTree();
    Require(invalidHarness.commitAttempts == 0 && invalidHarness.saves == 0,
            "invalid form refuses without commit or save");
    Require(FindNodeById(invalidTree, "controller-wizard.twister.encoder-slot")->text == "-1" &&
                VisibleTextLower(invalidTree).find("encoder slot") != std::string::npos,
            "validation refusal retains the invalid value and reports its field inline");

    TestHarness rejectedHarness;
    AttachCandidate(rejectedHarness, candidate);
    rejectedHarness.commitSucceeds = false;
    const synth::MidiInstrumentConfig beforeRejectedCommit = rejectedHarness.instrument;
    auto rejectedSurface = rejectedHarness.MakeSurface();
    RefreshWizardDiscovery(rejectedSurface, rejectedHarness);
    rejectedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    rejectedSurface.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.encoder-slot", "9"));
    rejectedSurface.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardSubmit));
    Require(rejectedHarness.commitAttempts == 1 && rejectedHarness.commits == 0 &&
                rejectedHarness.saves == 0,
            "host commit refusal never requests persistence save");
    Require(rejectedHarness.instrument.controllers.size() ==
                beforeRejectedCommit.controllers.size() &&
                rejectedSurface.ActiveWizardSession() != nullptr,
            "host commit refusal changes no instrument state and keeps the session");
    Require(FindNodeById(rejectedSurface.BuildTree(),
                         "controller-wizard.twister.encoder-slot")->text == "9",
            "host commit refusal retains entered form values");
}

void TestWizardSaveFailureDoesNotRollbackCommittedInstrument()
{
    TestHarness harness;
    const synth::WizardCandidate candidate = MakeTwisterCandidate();
    AttachCandidate(harness, candidate);
    harness.saveSucceeds = false;
    auto surface = harness.MakeSurface();
    RefreshWizardDiscovery(surface, harness);
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardSubmit));

    Require(harness.commits == 1 && harness.saves == 1 &&
                harness.instrument.FindController("MIDI Fighter Twister") != nullptr,
            "save failure leaves the accepted instrument commit installed");
    Require(surface.Discovery().available.empty(),
            "save failure still refreshes discovery from committed state");
    Require(surface.ActiveWizardSession() != nullptr &&
                VisibleTextLower(surface.BuildTree()).find("save") != std::string::npos,
            "save failure remains visible on the still-open workflow");
}

void TestConfigureSeedsFromDormantDataForBlacklistedRecords()
{
    synth::MfTwisterControllerWizard wizard;
    synth::MfTwisterConfigForm form;
    form.encoderSlotText = "4";
    const synth::WizardGenerationResult generated = wizard.GenerateProfile(
        form, {.name = "twister", .input = {"offline-in", "Offline In"},
               .output = {"offline-out", "Offline Out"}});
    Require(static_cast<bool>(generated), "generate compatible offline Twister record");

    TestHarness blacklistedHarness;
    blacklistedHarness.instrument.controllers.clear();
    synth::MidiControllerSlot ignored = *generated.controller;
    ignored.disposition = synth::MidiControllerDisposition::Blacklisted;
    ignored.config = {};
    ignored.dormantConfig.reset();
    Require(blacklistedHarness.instrument.AddController(std::move(ignored)),
            "add ignored record with absent dormant seed data");
    blacklistedHarness.connection.controllers.resize(1);
    auto blacklistedSurface = blacklistedHarness.MakeSurface();
    blacklistedSurface.MarkDirty();
    blacklistedSurface.RefreshOnTick();
    blacklistedSurface.DispatchAction(*FindNodeById(
        blacklistedSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerConfigure(0))->action);
    const synth::ui::NodeTree blacklistedTree = blacklistedSurface.BuildTree();
    Require(FindNodeById(blacklistedTree, "controller-wizard.twister.encoder-slot")->text == "0" &&
                FindNodeById(blacklistedTree, synth::runtime_ui::NodeIds::kWizardIgnore) == nullptr &&
                VisibleTextLower(blacklistedTree).find("replaces") != std::string::npos,
            "blacklisted records without dormant data open destructive defaults without Ignore");

    TestHarness dormantCompatibleHarness;
    dormantCompatibleHarness.instrument.controllers.clear();
    synth::MidiControllerSlot dormantCompatible = *generated.controller;
    dormantCompatible.disposition = synth::MidiControllerDisposition::Blacklisted;
    dormantCompatible.dormantConfig = dormantCompatible.config;
    dormantCompatible.config = {};
    Require(dormantCompatibleHarness.instrument.AddController(std::move(dormantCompatible)),
            "add blacklisted record with compatible dormant Twister profile");
    dormantCompatibleHarness.connection.controllers.resize(1);
    auto dormantCompatibleSurface = dormantCompatibleHarness.MakeSurface();
    dormantCompatibleSurface.MarkDirty();
    dormantCompatibleSurface.RefreshOnTick();
    dormantCompatibleSurface.DispatchAction(*FindNodeById(
        dormantCompatibleSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerConfigure(0))->action);
    const synth::ui::NodeTree dormantCompatibleTree = dormantCompatibleSurface.BuildTree();
    Require(FindNodeById(dormantCompatibleTree, "controller-wizard.twister.encoder-slot")->text == "4" &&
                FindNodeById(dormantCompatibleTree, synth::runtime_ui::NodeIds::kWizardWarning) == nullptr,
            "compatible dormant Twister data seeds Configure without a destructive warning");

    TestHarness dormantIncompatibleHarness;
    dormantIncompatibleHarness.instrument.controllers.clear();
    synth::MidiControllerSlot dormantIncompatible = *generated.controller;
    dormantIncompatible.disposition = synth::MidiControllerDisposition::Blacklisted;
    dormantIncompatible.dormantConfig = dormantIncompatible.config;
    dormantIncompatible.dormantConfig->systemMessages.push_back(
        dormantIncompatible.dormantConfig->systemMessages.front());
    dormantIncompatible.config = {};
    Require(dormantIncompatibleHarness.instrument.AddController(std::move(dormantIncompatible)),
            "add blacklisted record with incompatible dormant Twister profile");
    dormantIncompatibleHarness.connection.controllers.resize(1);
    auto dormantIncompatibleSurface = dormantIncompatibleHarness.MakeSurface();
    dormantIncompatibleSurface.MarkDirty();
    dormantIncompatibleSurface.RefreshOnTick();
    dormantIncompatibleSurface.DispatchAction(*FindNodeById(
        dormantIncompatibleSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerConfigure(0))->action);
    const synth::ui::NodeTree dormantIncompatibleTree = dormantIncompatibleSurface.BuildTree();
    Require(FindNodeById(dormantIncompatibleTree, "controller-wizard.twister.encoder-slot")->text == "0" &&
                VisibleTextLower(dormantIncompatibleTree).find("replaces") != std::string::npos,
            "incompatible dormant Twister data opens destructive defaults");
}

std::vector<synth::ControllerWizardDescriptor> TwoAppDefaultLayouts()
{
    synth::MidiAppCatalog catalog;

    synth::MidiAppDeviceDefault twister;
    twister.id = "froggers.twister";
    twister.displayName = "MIDI Fighter Twister";
    twister.kind = synth::MidiProfileKind::MfTwister;
    twister.inputAliases = {"Midi Fighter Twister"};
    twister.outputAliases = {"Midi Fighter Twister"};
    twister.config = synth::MfTwisterDefaultProfileConfig(
        synth::MfTwisterDefaultProfileOptions{.slotIx = 0});
    catalog.deviceDefaults.push_back(std::move(twister));

    synth::MidiAppDeviceDefault apc40;
    apc40.id = "froggers.apc40.generic";
    apc40.displayName = "Akai APC40 mkII (Generic)";
    apc40.kind = synth::MidiProfileKind::Generic;
    apc40.inputAliases = {"APC40 mkII"};
    apc40.outputAliases = {"APC40 mkII"};
    apc40.config.analogInput = synth::AnalogMidiInConfig{
        .sceneBlend = synth::MidiControlAddress{.channel = 0, .cc = 15}};
    catalog.deviceDefaults.push_back(std::move(apc40));

    return synth::MakeControllerWizardRegistry(catalog);
}

void TestLayoutComboOffersLayoutNamesThenCustom()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    Require(harness.instrument.AddController(MakeGenericSlot("controller")), "add plain controller");
    harness.connection.controllers.resize(1);
    harness.layouts = TwoAppDefaultLayouts();

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* combo =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerLayout(0));
    Require(combo != nullptr && combo->options.size() == 3 &&
                combo->options[0].label == "MIDI Fighter Twister" &&
                combo->options[1].label == "Akai APC40 mkII (Generic)" &&
                combo->options[2].label == "Custom" &&
                combo->selectedOption == "2",
            "Layout combo offers each layout's name then Custom, defaulting to Custom");
}

void TestLayoutComboReadsCustomForUnresolvedWizardId()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    synth::MidiControllerSlot unresolved = MakeGenericSlot("controller");
    unresolved.wizardId = "com.example.missing-wizard";
    Require(harness.instrument.AddController(unresolved), "add controller with unresolved wizard id");
    harness.connection.controllers.resize(1);
    harness.layouts = TwoAppDefaultLayouts();

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();
    const synth::ui::Node* combo =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerLayout(0));
    Require(combo != nullptr && combo->options.size() == 3 &&
                combo->options[2].label == "Custom" &&
                combo->selectedOption == "2",
            "Layout combo reads Custom when the stored wizard id resolves against no registry descriptor");
}

void TestChoosingALayoutInstallsItsConfigAndSetsWizardId()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    Require(harness.instrument.AddController(MakeGenericSlot("controller")), "add plain controller");
    harness.connection.controllers.resize(1);
    harness.layouts = TwoAppDefaultLayouts();

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerLayout, "0:0"));

    Require(harness.commits == 1 && harness.saves == 1,
            "choosing a layout commits once and saves once");
    const synth::MidiControllerSlot& installed = harness.instrument.controllers[0];
    Require(installed.wizardId == "froggers.twister" &&
                installed.kind == synth::MidiProfileKind::MfTwister &&
                installed.config.encoderInput.has_value() &&
                installed.config.encoderInput->turns.size() == 16,
            "choosing a layout installs its generated config and wizard id");

    surface.MarkDirty();
    surface.RefreshOnTick();
    if (!surface.ViewModel().SectionExpanded(0, synth::MidiConfigSection::Encoders))
    {
        surface.ViewModel().ToggleSection(0, synth::MidiConfigSection::Encoders);
    }
    const std::vector<synth::MidiMappingRowVM> rows =
        surface.ViewModel().SectionRows(0, synth::MidiConfigSection::Encoders);
    std::optional<std::size_t> modeRowIx;
    for (std::size_t ix = 0; ix < rows.size(); ++ix)
    {
        if (rows[ix].group == synth::MidiMappingRowVM::RowGroup::EncoderMode)
        {
            modeRowIx = ix;
        }
    }
    Require(modeRowIx.has_value(), "installed Twister profile exposes an encoder mode row");

    const std::string editValue =
        "0:encoders:" + std::to_string(*modeRowIx) + ":" +
        std::to_string(static_cast<int>(synth::MidiMappingRowVM::Field::EncoderMode)) + ":0";
    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kMappingFieldCommit, editValue));
    Require(harness.instrument.controllers[0].wizardId == std::nullopt,
            "a committed mapping edit clears the installed wizard id");

    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree treeAfterEdit = surface.BuildTree();
    const synth::ui::Node* comboAfterEdit =
        FindNodeById(treeAfterEdit, synth::runtime_ui::NodeIds::ControllerLayout(0));
    Require(comboAfterEdit != nullptr && comboAfterEdit->selectedOption == "2",
            "the Layout combo reads Custom once the installed layout no longer matches");
}

void TestChoosingCustomClearsWizardIdWithoutTouchingConfig()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    synth::MidiControllerSlot slot = MakeGenericSlot("controller");
    slot.wizardId = "froggers.twister";
    slot.kind = synth::MidiProfileKind::MfTwister;
    slot.config = synth::MfTwisterDefaultProfileConfig(
        synth::MfTwisterDefaultProfileOptions{.slotIx = 2});
    Require(harness.instrument.AddController(slot), "add a layout-installed controller");
    harness.connection.controllers.resize(1);
    harness.layouts = TwoAppDefaultLayouts();
    const synth::MidiControllerProfileConfig configBefore = harness.instrument.controllers[0].config;

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerLayout, "0:2"));

    Require(harness.commits == 1 && harness.saves == 1,
            "choosing Custom commits once and saves once");
    const synth::MidiControllerSlot& afterCustom = harness.instrument.controllers[0];
    Require(afterCustom.wizardId == std::nullopt, "choosing Custom clears the wizard id");
    Require(afterCustom.config.encoderInput->turns.front().slotIx ==
                configBefore.encoderInput->turns.front().slotIx,
            "choosing Custom leaves the stored config byte-for-byte unchanged");
}

void TestWizardIgnoreCommitsOneInertBlacklistedRecord()
{
    TestHarness harness;
    Require(harness.instrument.AddController(MakeGenericSlot("MIDI Fighter Twister")),
            "occupy ignored candidate base name");
    Require(harness.instrument.AddController(MakeGenericSlot("MIDI Fighter Twister 2")),
            "occupy ignored candidate first suffix");
    harness.connection.controllers.resize(harness.instrument.controllers.size());
    const synth::WizardCandidate candidate = MakeTwisterCandidate("-ignored");
    AttachCandidate(harness, candidate);
    auto surface = harness.MakeSurface();
    RefreshWizardDiscovery(surface, harness);
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    surface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardIgnore));

    Require(harness.commitAttempts == 1 && harness.commits == 1 && harness.saves == 1 &&
                harness.persistenceEvents == std::vector<std::string>({"commit", "save"}),
            "Ignore performs one commit followed by one save");
    const synth::MidiControllerSlot& ignored = harness.instrument.controllers.back();
    Require(ignored.name == "MIDI Fighter Twister 3" &&
                ignored.kind == synth::MidiProfileKind::MfTwister &&
                ignored.disposition == synth::MidiControllerDisposition::Blacklisted,
            "Ignore uses deterministic naming and persists a Blacklisted Twister record");
    Require(ignored.wizardId == candidate.wizardId &&
                ignored.input.identifier == candidate.input.identifier &&
                ignored.input.name == candidate.input.name &&
                ignored.output.identifier == candidate.output.identifier &&
                ignored.output.name == candidate.output.name,
            "Ignore retains the stable opaque id and exact endpoint references");
    Require(!ignored.dormantConfig.has_value() &&
                !ignored.config.encoderInput.has_value() &&
                !ignored.config.encoderOutput.has_value() &&
                !ignored.config.analogInput.has_value() &&
                !ignored.config.pressureInput.has_value() &&
                ignored.config.systemMessages.empty(),
            "newly ignored candidate carries neither active nor dormant profile data");
    Require(surface.Discovery().available.empty(),
            "Ignore immediately refreshes classification so the pair is no longer available");

    TestHarness rowHarness;
    const synth::WizardCandidate rowCandidate = MakeTwisterCandidate("-row");
    AttachCandidate(rowHarness, rowCandidate);
    auto rowSurface = rowHarness.MakeSurface();
    RefreshWizardDiscovery(rowSurface, rowHarness);
    const synth::ui::NodeTree rowTree = rowSurface.BuildTree();
    const synth::ui::Node* rowIgnore = FindNodeById(
        rowTree, synth::runtime_ui::NodeIds::AvailableIgnore(0));
    Require(rowIgnore != nullptr && rowIgnore->action.has_value(),
            "available row exposes a dispatchable Ignore action");
    rowSurface.DispatchAction(*rowIgnore->action);
    Require(rowHarness.commits == 1 && rowHarness.saves == 1 &&
                rowHarness.instrument.controllers.back().disposition ==
                    synth::MidiControllerDisposition::Blacklisted,
            "available-row Ignore uses the same atomic blacklist commit path");

    TestHarness staleHarness;
    staleHarness.devices.outputs.erase(
        std::remove_if(
            staleHarness.devices.outputs.begin(),
            staleHarness.devices.outputs.end(),
            [](const synth::MidiDeviceInfoRef& device) {
                return device.name == "Midi Fighter Twister";
            }),
        staleHarness.devices.outputs.end());
    const synth::WizardCandidate first = MakeTwisterCandidate("-first");
    const synth::WizardCandidate second = MakeTwisterCandidate("-second");
    staleHarness.devices.inputs.push_back(first.input);
    staleHarness.devices.inputs.push_back(second.input);
    staleHarness.devices.outputs.push_back(first.output);
    staleHarness.devices.outputs.push_back(second.output);
    auto staleSurface = staleHarness.MakeSurface();
    staleSurface.RefreshOnTick();
    RefreshWizardDiscovery(staleSurface, staleHarness);
    const synth::ui::NodeTree staleTree = staleSurface.BuildTree();
    const synth::ui::Node* firstIgnore = FindNodeById(
        staleTree, synth::runtime_ui::NodeIds::AvailableIgnore(0));
    Require(firstIgnore != nullptr && firstIgnore->action.has_value(),
            "first available Ignore action can be retained across a refresh");
    const synth::ui::Action staleFirstIgnore = *firstIgnore->action;
    staleHarness.devices.inputs.erase(
        std::remove(staleHarness.devices.inputs.begin(),
                    staleHarness.devices.inputs.end(), first.input),
        staleHarness.devices.inputs.end());
    staleHarness.devices.outputs.erase(
        std::remove(staleHarness.devices.outputs.begin(),
                    staleHarness.devices.outputs.end(), first.output),
        staleHarness.devices.outputs.end());
    RefreshWizardDiscovery(staleSurface, staleHarness);
    const int instrumentSnapshotsBeforeStaleIgnore =
        staleHarness.instrumentSnapshots;
    const int snapshotsBeforeStaleIgnore = staleHarness.deviceSnapshots;
    staleSurface.DispatchAction(staleFirstIgnore);
    Require(staleHarness.commitAttempts == 0 && staleHarness.saves == 0,
            "stale available-row Ignore cannot retarget a different candidate");
    Require(staleHarness.deviceSnapshots == snapshotsBeforeStaleIgnore + 1,
            "stale available-row Ignore still takes a current device snapshot before refusal");
    Require(staleHarness.instrumentSnapshots ==
                instrumentSnapshotsBeforeStaleIgnore + 1,
            "stale available-row Ignore still takes a current instrument snapshot before refusal");
    Require(staleSurface.Discovery().available.size() == 1 &&
                staleSurface.Discovery().available.front().input.identifier ==
                    second.input.identifier &&
                VisibleTextLower(staleSurface.BuildTree()).find("reconnect") !=
                    std::string::npos,
            "stale available-row Ignore preserves the remaining candidate and reports refusal");

    TestHarness changedIdentityHarness;
    const synth::WizardCandidate originalIdentity =
        MakeTwisterCandidate("-same-identifiers");
    AttachCandidate(changedIdentityHarness, originalIdentity);
    auto changedIdentitySurface = changedIdentityHarness.MakeSurface();
    RefreshWizardDiscovery(changedIdentitySurface, changedIdentityHarness);
    const synth::ui::NodeTree originalIdentityTree =
        changedIdentitySurface.BuildTree();
    const synth::ui::Action originalIdentityIgnore =
        *FindNodeById(originalIdentityTree,
                      synth::runtime_ui::NodeIds::AvailableIgnore(0))->action;
    changedIdentityHarness.devices.inputs.back().name =
        "MIDI FIGHTER TWISTER";
    changedIdentityHarness.devices.outputs.back().name =
        "MIDI FIGHTER TWISTER";
    RefreshWizardDiscovery(changedIdentitySurface, changedIdentityHarness);
    changedIdentitySurface.DispatchAction(originalIdentityIgnore);
    Require(changedIdentityHarness.commitAttempts == 0 &&
                changedIdentityHarness.saves == 0,
            "Ignore refuses changed endpoint content even when identifiers are unchanged");
    Require(changedIdentitySurface.Discovery().available.size() == 1,
            "exact-identity refusal leaves the changed candidate available for a fresh action");
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
        // is (task 7.1). This pins the case that used to make its old name a
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

    // sru-43's inspection over every runtime producer source, not just the two
    // this suite grew up with. `RuntimePages.hpp` joined the set in task 7.1
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
    // place that legitimately hand-places already-resolved subtree roots (D6,
    // and confirmed in 7.1's review), so it is the fixture that proves the
    // predicate still fires on real repository source.
    Require(synth::test::SourceAssemblesUiNodeByHand(
                "projects/synth/include/synth/RuntimeMainComponent.hpp"),
            "the shell's deliberate composition root keeps the sru-43 scan honest");
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

void TestControllerKindLabelsShowTheCombinedDisplayNames()
{
    TestHarness harness;
    harness.instrument.controllers.clear();
    harness.connection.controllers.clear();
    synth::MidiControllerSlot twister;
    twister.name = "twister";
    twister.kind = synth::MidiProfileKind::MfTwister;
    twister.config = synth::MfTwisterDefaultProfileConfig();
    Require(harness.instrument.AddController(std::move(twister)), "add Twister slot");
    harness.connection.controllers.push_back({});
    Require(harness.instrument.AddController(MakeGenericSlot("blank")), "add Generic slot");
    harness.connection.controllers.push_back({});

    auto surface = harness.MakeSurface();
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree tree = surface.BuildTree();

    const synth::ui::Node* twisterKind =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerKind(0));
    const synth::ui::Node* genericKind =
        FindNodeById(tree, synth::runtime_ui::NodeIds::ControllerKind(1));
    Require(twisterKind != nullptr && twisterKind->text == "MF Twister",
            "the Active row's kind label reads MF Twister for a Twister slot");
    Require(genericKind != nullptr && genericKind->text == "Generic",
            "the Active row's kind label reads Generic for a Generic slot");
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
    const synth::ui::Node* renameDraft = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0));
    const synth::ui::Node* renameButton = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerRename(0));
    Require(renameDraft != nullptr && renameDraft->action.has_value() &&
                renameDraft->action->value.back() != ':' &&
                renameButton != nullptr && renameButton->action.has_value(),
            "Rename exposes a draft field with an unambiguous renderer prefix and an explicit commit button");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(0) + ".caption")
                    ->text == "Rename to",
            "active row rename draft has a visible caption");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRenameDraft(3) + ".caption")
                    ->text == "Rename to",
            "blacklisted row rename draft has a visible caption");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDelete(0)) != nullptr,
            "manual active row exposes Delete");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerBlacklist(1)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerLayout(1)) != nullptr,
            "resolved active row exposes Blacklist and the Layout combo");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerBlacklist(2)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerLayout(2)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDelete(2)) != nullptr,
            "unknown active id gates Blacklist but preserves recovery Delete and the Layout combo");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerBadge(3)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRemoveBlacklist(3)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerConfigure(3)) != nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerDisclosure(3)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(3)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(3)) == nullptr,
            "resolved blacklisted row has its lifecycle controls but no live editor controls");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerConfigure(4)) == nullptr &&
                FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerRemoveBlacklist(4)) != nullptr,
            "unknown blacklisted id preserves Remove but gates Configure");
    // sru-4: a blacklisted row shows its stored endpoint labels. Its endpoints
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
    const synth::ui::Node* incompleteBlacklist = FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerBlacklist(5));
    Require(incompleteBlacklist != nullptr && !incompleteBlacklist->enabled,
            "resolved Active records with incomplete endpoints keep a disabled Blacklist affordance");
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
    Require(FindNodeById(staleRefusalTree, synth::runtime_ui::NodeIds::ControllerDelete(1)) != nullptr &&
                FindNodeById(staleRefusalTree, synth::runtime_ui::NodeIds::ControllerDelete(2)) == nullptr,
            "a refusal publishes the current controller structure without retaining a stale lifecycle row");
    staleSurface.DispatchAction(staleDelete);
    Require(staleHarness.commits == 0 && staleHarness.saves == 0 &&
                FindNodeById(staleSurface.BuildTree(), synth::runtime_ui::NodeIds::ControllerDelete(2)) == nullptr,
            "repeated stale lifecycle refusals leave the published controller tree consistent");

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerDelete,
        synth::runtime_ui::NodeIds::ControllerActionToken(3, "blacklisted known")));
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerBlacklist,
        synth::runtime_ui::NodeIds::ControllerActionToken(0, "manual")));
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kControllerBlacklist,
        synth::runtime_ui::NodeIds::ControllerActionToken(5, "incomplete")));
    Require(harness.commits == 0 && harness.saves == 0,
            "refused stale lifecycle actions perform neither a commit nor a save");
    Require(surface.StatusText().find("endpoint") != std::string::npos &&
                harness.instrument.FindController("incomplete")->disposition ==
                    synth::MidiControllerDisposition::Active,
            "incomplete endpoint pairs are refused by the view model without mutating the active record");

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
    surface.DispatchAction(*FindNodeById(
        surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerRename(0))->action);
    Require(harness.commits == 1 && harness.saves == 1,
            "unchanged rename is refused without a second commit or save");
    harness.connection.controllers[1].input = {
        .status = synth::MidiEndpointStatus::Online, .openIdentifier = "known-in"};
    harness.connection.controllers[1].output = {
        .status = synth::MidiEndpointStatus::Online, .openIdentifier = "known-out"};
    surface.DispatchAction(*FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerBlacklist(1))->action);
    const synth::MidiControllerSlot& transitioned = harness.instrument.controllers[1];
    Require(harness.commits == 2 && harness.saves == 2 &&
                transitioned.disposition == synth::MidiControllerDisposition::Blacklisted &&
                transitioned.dormantConfig.has_value() && !transitioned.config.encoderInput.has_value(),
            "Blacklist commits through normal reconciliation and retains dormant profile data");
    synth::MidiDeviceList knownDevices;
    knownDevices.inputs.push_back({"known-in", "Known Input"});
    knownDevices.outputs.push_back({"known-out", "Known Output"});
    const synth::ReconcilePlan teardown = synth::PlanMidiReconciliation(
        harness.instrument, knownDevices, harness.connection);
    int closedInputs = 0;
    int closedOutputs = 0;
    synth::MidiEndpointOps endpointOps;
    endpointOps.closeInput = [&](std::size_t controllerIx) {
        closedInputs += controllerIx == 1 ? 1 : 0;
    };
    endpointOps.closeOutput = [&](std::size_t controllerIx) {
        closedOutputs += controllerIx == 1 ? 1 : 0;
    };
    const synth::MidiConnectionState reconciled = synth::ExecuteReconcilePlan(
        teardown, harness.connection, endpointOps);
    Require(closedInputs == 1 && closedOutputs == 1 &&
                reconciled.controllers[1].input.status == synth::MidiEndpointStatus::Unconfigured &&
                reconciled.controllers[1].output.status == synth::MidiEndpointStatus::Unconfigured,
            "the committed blacklist transition reaches normal reconcile execution and closes both endpoints");
    surface.DispatchAction(*FindNodeById(
        initialTree, synth::runtime_ui::NodeIds::ControllerDelete(2))->action);
    Require(harness.commits == 3 && harness.saves == 3 &&
                harness.instrument.FindController("unknown") == nullptr,
            "Delete remains available for an unknown persisted id and commits through the normal path");
    surface.DispatchAction(*FindNodeById(
        surface.BuildTree(), synth::runtime_ui::NodeIds::ControllerRemoveBlacklist(3))->action);
    Require(harness.commits == 4 && harness.saves == 4 &&
                harness.instrument.FindController("blacklisted unknown") == nullptr,
            "Remove from blacklist deletes unknown-id inert records through one commit and save");
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

}  // namespace

int main()
{
    TestNoHandRolledControllerNodesSurvive();
    TestControllersSectionsNestThroughLibraryContainers();
    TestControllerRowsStayReadableWithLargeLists();
    TestControllerKindLabelsShowTheCombinedDisplayNames();
    TestDiscoveryRendersPortableAvailableRowsAndDiagnostics();
    TestWizardSessionRoutesPortableChooserAndForm();
    TestWizardSubmitCommitsCompleteProfileThenSaves();
    TestWizardSubmitRefusalsRetainFormAndPersistence();
    TestWizardSaveFailureDoesNotRollbackCommittedInstrument();
    TestConfigureSeedsFromDormantDataForBlacklistedRecords();
    TestLayoutComboOffersLayoutNamesThenCustom();
    TestLayoutComboReadsCustomForUnresolvedWizardId();
    TestChoosingALayoutInstallsItsConfigAndSetsWizardId();
    TestChoosingCustomClearsWizardIdWithoutTouchingConfig();
    TestWizardIgnoreCommitsOneInertBlacklistedRecord();
    TestEndpointSelectorsPreferTheExactStoredIdentifier();
    TestControllerLifecycleActionsUseTheNormalCommitAndSavePath();

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
    const synth::ui::Node* addName =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::kAddName);
    const synth::ui::Node* addKind =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::kAddKind);
    const synth::ui::Node* addKindCaption =
        FindNodeById(initialTree, std::string(synth::runtime_ui::NodeIds::kAddKind) + ".caption");
    Require(addName != nullptr && addName->action.has_value() &&
                addName->action->name == "runtime.controllers.add_name_draft",
            "add controller name edits dispatch a portable draft action");
    Require(addKind != nullptr && addKind->action.has_value() &&
                addKind->action->name == "runtime.controllers.add_kind_draft",
            "add controller kind edits dispatch a portable draft action");
    Require(addKindCaption != nullptr && addKindCaption->text == "Device",
            "add controller kind selector has a visible caption");
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
    const synth::ui::Node* wrldDots =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerStatusDots(0));
    const synth::ui::Node* padsOutput =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(1));
    const synth::ui::Node* padsOutputRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(1) + ".row");
    const synth::ui::Node* padsVariant =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerVariant(1));
    const synth::ui::Node* padsVariantRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerVariant(1) + ".row");
    const synth::ui::Node* padsLayoutRow =
        FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerLayout(1) + ".row");
    const synth::ui::Node* scrollNode = FindNodeById(initialTree, synth::runtime_ui::NodeIds::kScroll);
    Require(wrldInput != nullptr && padsInput != nullptr && wrldOutput != nullptr && padsOutput != nullptr,
            "controller device controls render");
    Require(wrldInputRow != nullptr && padsInputRow != nullptr && wrldOutputRow != nullptr &&
                padsOutputRow != nullptr && padsVariantRow != nullptr && padsLayoutRow != nullptr,
            "captioned endpoint selectors render their flow-slot rows");
    Require(wrldDots != nullptr, "controller status dots render");
    Require(padsVariant != nullptr, "launchpad variant selector renders");
    Require(scrollNode != nullptr, "scroll area node still present");
    Require(wrldInput->bounds.x == padsInput->bounds.x, "launchpad input aligns with other controller inputs");
    Require(wrldOutput->bounds.x == padsOutput->bounds.x, "launchpad output aligns with other controller outputs");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerInput(0) + ".caption")->text ==
                "MIDI in",
            "input selector caption is visible text");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerOutput(0) + ".caption")->text ==
                "MIDI out",
            "output selector caption is visible text");
    Require(FindNodeById(initialTree, synth::runtime_ui::NodeIds::ControllerVariant(1) + ".caption")->text ==
                "Variant",
            "variant selector caption is visible text");
    Require(wrldInputRow->bounds.x == padsInputRow->bounds.x,
            "captioned launchpad input aligns with other controller input flow slots");
    Require(wrldOutputRow->bounds.x == padsOutputRow->bounds.x,
            "captioned launchpad output aligns with other controller output flow slots");
    Require(wrldOutputRow->bounds.x == wrldInputRow->bounds.x + wrldInputRow->bounds.width +
                                         synth::runtime_ui::ControllersLayout::kEndpointBoxGap,
            "input and output selector flow slots keep endpoint spacing");
    Require(padsVariantRow->bounds.x > padsLayoutRow->bounds.x + padsLayoutRow->bounds.width,
            "launchpad variant flow slot sits to the right of the Layout combo on the identity line");
    Require(padsVariantRow->bounds.x == padsLayoutRow->bounds.x + padsLayoutRow->bounds.width +
                                        synth::runtime_ui::ControllersLayout::kLifecycleControlGap,
            "Layout and variant selector flow slots keep lifecycle spacing");
    Require(padsVariantRow->bounds.y == padsLayoutRow->bounds.y,
            "the variant selector shares the identity line with the Layout combo");
    // Bounds are parent-relative (design.md), so a variant/output line check
    // has to walk parentage rather than compare raw y across different
    // immediate parents.
    const synth::ui::Node* padsVariantLine = FindParentOf(initialTree, padsVariantRow->id.value);
    const synth::ui::Node* padsOutputEndpoints = FindParentOf(initialTree, padsOutputRow->id.value);
    const synth::ui::Node* padsOutputLine =
        padsOutputEndpoints != nullptr ? FindParentOf(initialTree, padsOutputEndpoints->id.value)
                                       : nullptr;
    Require(padsVariantLine != nullptr &&
                padsVariantLine->id.value ==
                    synth::runtime_ui::NodeIds::ControllerRow(1) + ".line1",
            "the variant selector is on the identity line");
    Require(padsOutputLine != nullptr &&
                padsOutputLine->id.value == synth::runtime_ui::NodeIds::ControllerRow(1) + ".line2",
            "the output selector is on the ports line");
    Require(wrldDots->bounds.y ==
                (synth::runtime_ui::ControllersLayout::kControllerHeaderLineHeight -
                 wrldDots->bounds.height) /
                    2.0f,
            "status dots are vertically centered in their header line");
    Require(scrollNode->scrollContentWidth >= padsVariantRow->bounds.x + padsVariantRow->bounds.width,
            "scroll content reserves launchpad variant width");

    surface.DispatchAction(
        synth::ui::Action::WithValue("runtime.controllers.add_name_draft", "newctl"));
    surface.DispatchAction(
        synth::ui::Action::WithValue("runtime.controllers.add_kind_draft", "generic"));
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

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kVariantSelect, "1:1"));
    Require(harness.commits == 9, "launchpad variant selection commits");

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
    Require(harness.commits == 10, "absolute mode edit commits through Controllers surface");
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
    Require(harness.commits == 11, "switching back to signed mode commits");
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
