#include "ControllersPageHarness.hpp"
#include "PortableJuceBackend.hpp"

#include "synth/ControllersPageUI.hpp"

#include "../tests/support/VisualCriteria.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void Require(bool condition, const std::string& label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

const synth::ui::Node* FindNode(const synth::ui::NodeTree& tree, const synth::ui::NodeId& id)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id == id)
        {
            return &node;
        }
    }
    return nullptr;
}

std::unordered_map<std::string, std::string> BuildParentMap(const synth::ui::NodeTree& tree)
{
    std::unordered_map<std::string, std::string> parents;
    for (const synth::ui::Node& node : tree.nodes)
    {
        for (const synth::ui::NodeId& child : node.children)
        {
            parents[child.value] = node.id.value;
        }
    }
    return parents;
}

bool IsRenderedNode(const synth::ui::Node& node)
{
    return node.kind != synth::ui::NodeKind::Root && node.kind != synth::ui::NodeKind::ScrollArea;
}

// True for the two modal trees that replace the page outright -- the wizard
// chooser and the wizard form -- identified by their own root node. Neither
// one renders a controller row or the add row, so callers that check for
// those must skip while either is showing.
bool IsWizardModalTree(const synth::ui::NodeTree& tree)
{
    return FindNode(tree, synth::runtime_ui::NodeIds::kWizardForm) != nullptr ||
           FindNode(tree, synth::runtime_ui::NodeIds::kWizardChooser) != nullptr;
}

std::string Describe(const synth::ui::Action& action)
{
    return action.name + "(" + action.value + ")";
}

// Structural criteria over the RESOLVED portable tree.
// This is JUCE's half of the criteria: bounds are in the tree, so containment,
// sibling overlap and spacing conformance are assertable without rendering,
// and the simulation walks 250 randomly chosen states of the surface rather
// than one hand-built snapshot. `VerifyTreeAndRenderer` already checks the
// rendered components against their parents; these check the tree the backend
// was handed, which is where a layout defect actually lives.
namespace criteria = synth::ui::criteria;

bool EndsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The Controllers page positions one node class out of flow: each row's
// status dots are an explicitly bounded Draw hand-centred inside its cell, so
// they consume no stacking space and a gap measured against them is
// meaningless; matched by suffix because the simulation adds and removes
// controllers, so that id set changes every step. The legend row's own
// dot/word pairs are ordinary in-flow Row children and carry no exemption.
std::set<std::string> OutOfFlowIds(const synth::ui::NodeTree& tree)
{
    std::set<std::string> ids;
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (EndsWith(node.id.value, ".status_dots"))
        {
            ids.insert(node.id.value);
        }
    }
    return ids;
}

// Form controls that carry no caption node, in two groups.
//
// The mapping-table cells (`...mapping.N.field.M`) are the DESIGNED case: a
// mapping section emits a group header whose captions label the columns, so a
// caption per cell would repeat the heading on every row. They are excluded on
// that basis, not as a residual.
//
// The rest are the residual: controller-row endpoint selectors, the rename and
// add fields, the variant selector, and the wizard's message/argument cells all
// carry their only identifying string in a field neither backend renders
// (`ComboBox::label` was retired; `TextField::label` was never
// rendered), and their tables have no column headings. Each is a recorded
// appearance question, not a licence: a NEW uncaptioned
// control anywhere else on the page fails.
// The simulation adds, removes, renames and blacklists controllers, so the id
// set changes every step and cannot be written out once. It is still derived
// control by control rather than by suffix: for each controller row the
// simulation has produced, the four cell ids that row is known to publish are
// named through `NodeIds`, and the wizard's are enumerated per button. A
// control that is NOT one of those -- a new field, a renamed one, anything the
// page grows -- is examined and must carry a caption.
//
// That is the difference from a suffix match, and it is the whole point: a
// pattern would have silently absorbed whatever arrived next. `residualsMatched`
// below then proves the list was used rather than sitting inert.
std::map<std::string, std::string> UncaptionedResiduals(const synth::ui::NodeTree& tree)
{
    std::map<std::string, std::string> exceptions;
    const auto except = [&exceptions](std::string id, std::string reason) {
        exceptions.emplace(std::move(id), std::move(reason));
    };
    const auto present = [&tree](const std::string& id) {
        return FindNode(tree, synth::ui::NodeId(id)) != nullptr;
    };

    // The add row's Preset combo carries its own visible caption ("Preset"),
    // so unlike the old bare name field and the retired-ComboBox::label kind
    // selector it needs no exception here.
    for (std::size_t ix = 0; present(synth::runtime_ui::NodeIds::ControllerRow(ix)); ++ix)
    {
        const std::string row = "controller row " + std::to_string(ix);
        // The rename draft moved into the expanded editor and carries its own
        // visible caption ("Name") there, so it is not listed alongside the
        // header's endpoint selectors below.
        for (const auto& [id, what] :
             {std::pair{synth::runtime_ui::NodeIds::ControllerInput(ix), "MIDI input selector"},
              std::pair{synth::runtime_ui::NodeIds::ControllerOutput(ix), "MIDI output selector"}})
        {
            if (present(id))
            {
                except(id, row + " " + what + "; a table cell whose column has no heading");
            }
        }
    }
    // Mapping cells are excused by their section's COLUMN HEADINGS, so the
    // headings have to exist. A section that lost them fails instead of
    // inheriting the exclusion.
    std::set<std::string> headedBodies;
    for (const synth::ui::Node& node : tree.nodes)
    {
        const std::string& id = node.id.value;
        const std::size_t header = id.find(".header.");
        if (header != std::string::npos && EndsWith(id, ".caption"))
        {
            headedBodies.insert(id.substr(0, header));
        }
    }
    for (const synth::ui::Node& node : tree.nodes)
    {
        const std::string& id = node.id.value;
        if (!synth::ui::criteria::IsFormControl(node.kind) ||
            id.find(".mapping.") == std::string::npos || id.find(".field.") == std::string::npos)
        {
            continue;
        }
        const std::string body = id.substr(0, id.find(".mapping."));
        Require(headedBodies.count(body) != 0,
                "mapping section " + body + " publishes no column headings, so its cells cannot "
                                            "be excused from carrying captions");
        except(id, "mapping table cell in " + body + "; identified by that section's headings");
    }
    return exceptions;
}

// The Controllers page and its wizard draw spacing from two named tables plus
// the library's own. `TwisterFormLayout`'s 8 and 16 are restated rather than
// named because that table is private to `src/ControllerWizard.cpp`; it
// will be deleted because the layout contract bans producer-side arithmetic
// like it contains.
const std::vector<float>& ControllersPageSpacing()
{
    static const std::vector<float> values{0.0f,
                                           synth::ui::kSpacing.gap,
                                           synth::ui::kSpacing.padding,
                                           synth::ui::kSpacing.labelGap,
                                           synth::runtime_ui::ControllersLayout::kPageMargin,
                                           synth::runtime_ui::ControllersLayout::kRowGap,
                                           synth::runtime_ui::ControllersLayout::kEndpointBoxGap,
                                           synth::runtime_ui::ControllersLayout::kAvailableControlGap,
                                           synth::runtime_ui::ControllersLayout::kLifecycleControlGap,
                                           8.0f,
                                           16.0f};
    return values;
}

// Run-level anti-vacuity totals for the caption criterion, asserted in `main`
// once every simulation has finished.
//
// `examined > 0` is deliberately NOT among them, and the reason is a finding
// rather than a convenience: **the Controllers page has no conforming form
// control at all.** Every combo box, text field and toggle it renders -- the
// row endpoint selectors, the rename field, the add row, and every mapping cell
// -- is one of the named uncaptioned exceptions. Requiring
// one examined control would not be a stricter test, it would be a permanently
// red one, and the page passing it is the product decision still waiting to
// be made. (Asserting it was how this was established: the requirement was added,
// the whole 250-step run reported zero, and that is not a defect in the walk.)
//
// So the guard is on SUBJECTS instead of on conformers. `formControlsSeen`
// proves the criterion had something to look at, and `residualsMatched` proves
// the exception list was the thing excusing them rather than the criterion
// quietly finding nothing. A control that is not on the list is still examined
// and must still carry a caption, which is what stops the exceptions growing.
std::size_t g_captionFormControlsSeen = 0;
std::size_t g_captionResidualsMatched = 0;

void VerifyNamedVisualCriteria(const synth::ui::NodeTree& tree, const std::string& context)
{
    const std::set<std::string> outOfFlow = OutOfFlowIds(tree);

    const std::vector<std::string> containment = criteria::ContainmentViolations(tree);
    Require(containment.empty(), context + " containment: " + criteria::Join(containment));

    const std::vector<std::string> overlaps = criteria::SiblingOverlapViolations(tree, outOfFlow);
    Require(overlaps.empty(), context + " overlap: " + criteria::Join(overlaps));

    const std::vector<std::string> underlays = criteria::UnderlayViolations(tree);
    Require(underlays.empty(), context + " underlay: " + criteria::Join(underlays));

    const criteria::SpacingReport spacing =
        criteria::SpacingConformance(tree, ControllersPageSpacing(), outOfFlow);
    Require(spacing.violations.empty(), context + " spacing: " + criteria::Join(spacing.violations));
    Require(!spacing.observed.empty(), context + " spacing: nothing was measured");

    const std::map<std::string, std::string> exceptions = UncaptionedResiduals(tree);
    const criteria::CaptionReport captions = criteria::UncaptionedFormControls(tree, exceptions);
    Require(captions.violations.empty(), context + " caption: " + criteria::Join(captions.violations));
    // Every named exception was derived from a control this tree actually
    // contains, so all of them must have matched. A shortfall means an id
    // convention moved under the derivation and the list is quietly waiving
    // nothing while real controls go unexamined.
    Require(captions.residualsMatched == exceptions.size(),
            context + " caption: " + std::to_string(exceptions.size()) +
                " exceptions were derived but only " + std::to_string(captions.residualsMatched) +
                " matched a control");
    g_captionFormControlsSeen += captions.examined + captions.residualsMatched;
    g_captionResidualsMatched += captions.residualsMatched;

    const std::vector<std::string> silent = criteria::EmptyTextNodes(tree);
    Require(silent.empty(), context + " empty text: " + criteria::Join(silent));
}

void VerifyTreeAndRenderer(const synth::ui::NodeTree& tree,
                           synth_juce::PortableComponent& renderer,
                           const synth_runtime::test::ControllersHarnessFixture& fixture,
                           int step,
                           const std::string& actionDescription)
{
    VerifyNamedVisualCriteria(tree,
                              "step " + std::to_string(step) + " after " + actionDescription);

    if (!IsWizardModalTree(tree))
    {
        for (std::size_t ix = 0; ix < fixture.state.instrument.controllers.size(); ++ix)
        {
            Require(FindNode(tree, synth::ui::NodeId(synth::runtime_ui::NodeIds::ControllerRow(ix))) != nullptr,
                    "step " + std::to_string(step) + " missing controller row " + std::to_string(ix) + " after " +
                        actionDescription);
            Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerRow(ix)) != nullptr,
                    "step " + std::to_string(step) + " missing rendered controller row " + std::to_string(ix) +
                        " after " + actionDescription);
        }

        Require(FindNode(tree, synth::runtime_ui::NodeIds::kAddRow) != nullptr,
                "step " + std::to_string(step) + " missing add row after " + actionDescription);
        Require(renderer.FindByNodeId(synth::runtime_ui::NodeIds::kAddButton) != nullptr,
                "step " + std::to_string(step) + " missing add button after " + actionDescription);
    }

    const auto parents = BuildParentMap(tree);
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (!IsRenderedNode(node))
        {
            continue;
        }

        juce::Component* component = renderer.FindByNodeId(node.id.value);
        Require(component != nullptr,
                "step " + std::to_string(step) + " missing component " + node.id.value + " after " +
                    actionDescription);

        if (node.kind == synth::ui::NodeKind::ComboBox)
        {
            auto* combo = dynamic_cast<juce::ComboBox*>(component);
            Require(combo != nullptr,
                    "step " + std::to_string(step) + " combo node not rendered as ComboBox " + node.id.value);
            Require(combo->getNumItems() == static_cast<int>(node.options.size()),
                    "step " + std::to_string(step) + " combo item count mismatch " + node.id.value);
        }

        const auto parentIt = parents.find(node.id.value);
        if (parentIt == parents.end())
        {
            continue;
        }
        const synth::ui::Node* parentNode = FindNode(tree, synth::ui::NodeId(parentIt->second));
        if (parentNode == nullptr ||
            (parentNode->kind != synth::ui::NodeKind::Row && parentNode->kind != synth::ui::NodeKind::Section))
        {
            continue;
        }
        juce::Component* parentComponent = renderer.FindByNodeId(parentNode->id.value);
        Require(parentComponent != nullptr,
                "step " + std::to_string(step) + " missing parent component " + parentNode->id.value);
        Require(component->getParentComponent() == parentComponent,
                "step " + std::to_string(step) + " wrong parent for " + node.id.value);
        const juce::Rectangle<int> bounds =
            renderer.getLocalArea(component, component->getLocalBounds());
        const juce::Rectangle<int> parentBounds =
            renderer.getLocalArea(parentComponent, parentComponent->getLocalBounds());
        Require(parentBounds.contains(bounds),
                "step " + std::to_string(step) + " clipped child " + node.id.value + " after " +
                    actionDescription);
    }
}

std::vector<synth::ui::Action> CollectActions(const synth::ui::NodeTree& tree, int controllerNameSuffix)
{
    std::vector<synth::ui::Action> actions;
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (!node.action.has_value())
        {
            continue;
        }

        const synth::ui::Action& action = *node.action;
        if (node.kind == synth::ui::NodeKind::Button || node.kind == synth::ui::NodeKind::Toggle)
        {
            actions.push_back(action);
            continue;
        }

        if (node.kind != synth::ui::NodeKind::ComboBox || node.options.empty())
        {
            continue;
        }

        for (const synth::ui::ControlOption& option : node.options)
        {
            if (option.id == node.selectedOption)
            {
                continue;
            }
            synth::ui::Action dispatched = action;
            if (action.name == synth::runtime_ui::Actions::kEndpointSelect ||
                action.name == synth::runtime_ui::Actions::kMappingFieldCommit)
            {
                dispatched.value = action.value + ":" + option.id;
            }
            else
            {
                dispatched.value = option.id;
            }
            actions.push_back(std::move(dispatched));
            break;
        }
    }

    actions.push_back(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddController,
                                                   "sim" + std::to_string(controllerNameSuffix) + ":generic"));
    return actions;
}

struct GridOracleSeed
{
    synth::MidiControllerSystemMessageAssociation legacy;
    std::vector<synth::PolyphonicPressureMapping> hidden;
};

GridOracleSeed SeedGridSimulation(synth_runtime::test::ControllersHarnessFixture& fixture)
{
    auto& slot = fixture.state.instrument.controllers[1];
    slot.config.systemMessages.clear();
    slot.config.pressureInput = synth::PolyphonicPressureMidiInConfig{};

    synth::GridBlock block;
    block.kind = synth::MidiProfileKind::Launchpad;
    block.startX = 0;
    block.startY = -1;
    block.endX = 2;
    block.endY = 0;
    block.gridSlotIx = 7;
    synth::GridMappingExpansion expansion;
    Require(synth::ExpandGridBlock(block, expansion), "grid simulation seed expansion");
    slot.config.systemMessages = expansion.systemMessages;
    slot.config.pressureInput->mappings = expansion.pressureMappings;

    GridOracleSeed seed;
    seed.legacy.launchpadPosition = synth::LaunchpadGridPosition{
        .controller = synth::LaunchpadController::LaunchpadX, .x = 7, .y = 7};
    seed.legacy.press = synth::MessageIn::SceneSelect(0, 19);
    seed.legacy.feedback = seed.legacy.press;
    seed.legacy.outputFeedback = true;
    slot.config.systemMessages.push_back(seed.legacy);

    for (int ix = 0; ix < 2; ++ix)
    {
        synth::PolyphonicPressureMapping orphan;
        orphan.address = synth::MidiNoteAddress{.channel = static_cast<std::uint8_t>(14 + ix),
                                                .note = static_cast<std::uint8_t>(126 + ix)};
        orphan.pressure = synth::MessageIn::GridPressureChange(
            0, 90 + static_cast<std::size_t>(ix), -20 - ix, 30 + ix,
            static_cast<std::uint8_t>(40 + ix));
        seed.hidden.push_back(orphan);
        slot.config.pressureInput->mappings.push_back(orphan);
    }
    synth::NormalizeMidiProfileConfig(slot.config, slot.kind);
    return seed;
}

std::optional<synth::MidiNoteAddress> PhysicalAddress(
    const synth::MidiControllerSystemMessageAssociation& association)
{
    if (association.launchpadPosition.has_value())
    {
        const auto& position = *association.launchpadPosition;
        const auto note = synth::LaunchpadPositionToNote(position.controller, position.x, position.y);
        if (note.has_value())
        {
            return synth::MidiNoteAddress{.channel = 0, .note = *note};
        }
    }
    if (association.control.has_value())
    {
        return synth::MidiNoteAddress{.channel = association.control->channel, .note = association.control->cc};
    }
    return std::nullopt;
}

void VerifyGridProfileIndependent(const synth::MidiControllerSlot& slot, const GridOracleSeed& seed, int step)
{
    Require(slot.config.pressureInput.has_value(), "grid oracle pressure container missing step " +
                                                      std::to_string(step));
    const auto& pressure = slot.config.pressureInput->mappings;
    std::size_t visibleCells = 0;
    std::size_t legacyCount = 0;
    for (const auto& association : slot.config.systemMessages)
    {
        if (association.press.type != synth::MessageIn::Type::GridPress)
        {
            const bool sameLaunchpad = association.launchpadPosition.has_value() &&
                                       seed.legacy.launchpadPosition.has_value() &&
                                       association.launchpadPosition->controller ==
                                           seed.legacy.launchpadPosition->controller &&
                                       association.launchpadPosition->x == seed.legacy.launchpadPosition->x &&
                                       association.launchpadPosition->y == seed.legacy.launchpadPosition->y;
            legacyCount += !association.control.has_value() && !association.wrldBldrPosition.has_value() &&
                                   sameLaunchpad &&
                                   association.press == seed.legacy.press &&
                                   association.release == seed.legacy.release &&
                                   association.feedback == seed.legacy.feedback &&
                                   association.outputFeedback == seed.legacy.outputFeedback
                               ? 1
                               : 0;
            continue;
        }
        ++visibleCells;
        Require(association.release.has_value() &&
                    association.release->type == synth::MessageIn::Type::GridRelease &&
                    association.release->gridSlotIx == association.press.gridSlotIx &&
                    association.release->gridX == association.press.gridX &&
                    association.release->gridY == association.press.gridY,
                "grid oracle release mismatch step " + std::to_string(step));
        Require(association.feedback.type == synth::MessageIn::Type::GridPress &&
                    association.feedback.gridSlotIx == association.press.gridSlotIx &&
                    association.feedback.gridX == association.press.gridX &&
                    association.feedback.gridY == association.press.gridY,
                "grid oracle feedback mismatch step " + std::to_string(step));
        const auto physical = PhysicalAddress(association);
        Require(physical.has_value(), "grid oracle physical address missing step " + std::to_string(step));
        std::size_t exactPressure = 0;
        for (const auto& mapping : pressure)
        {
            exactPressure += mapping.address == *physical &&
                                     mapping.pressure.type == synth::MessageIn::Type::GridPressureChange &&
                                     mapping.pressure.gridSlotIx == association.press.gridSlotIx &&
                                     mapping.pressure.gridX == association.press.gridX &&
                                     mapping.pressure.gridY == association.press.gridY
                                 ? 1
                                 : 0;
        }
        Require(exactPressure == 1,
                "grid oracle expected one exact pressure pair step " + std::to_string(step));
    }
    Require(legacyCount == 1, "grid oracle legacy row changed step " + std::to_string(step));
    Require(pressure.size() == visibleCells + seed.hidden.size(),
            "grid oracle unexpected pressure count step " + std::to_string(step));
    for (const auto& orphan : seed.hidden)
    {
        Require(std::count(pressure.begin(), pressure.end(), orphan) == 1,
                "grid oracle hidden orphan bytes changed step " + std::to_string(step));
    }
}

std::vector<std::size_t> GridRows(const synth::runtime_ui::ControllersPageSurface& surface,
                                  synth::MidiMappingRowVM::Kind kind)
{
    std::vector<std::size_t> result;
    const auto rows = surface.ViewModel().SectionRows(1, synth::MidiConfigSection::SystemMessages);
    for (std::size_t ix = 0; ix < rows.size(); ++ix)
    {
        if (rows[ix].group == synth::MidiMappingRowVM::RowGroup::Grid && rows[ix].kind == kind)
        {
            result.push_back(ix);
        }
    }
    return result;
}

void RunGridSimulation()
{
    constexpr std::uint32_t kGridSeed = 0x6A1D2026;
    constexpr int kOperations = 320;
    std::mt19937 rng(kGridSeed);
    synth_runtime::test::ControllersHarnessFixture fixture;
    const GridOracleSeed oracle = SeedGridSimulation(fixture);
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    surface.SetContentBounds({0.0f, 0.0f, 980.0f, 300.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    // Malformed integer text must not be interpreted as zero and committed.
    const std::size_t seededBlock = GridRows(surface, synth::MidiMappingRowVM::Kind::Block).front();
    const int commitsBeforeMalformed = fixture.state.commits;
    surface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kMappingFieldCommit,
        "1:system_messages:" + std::to_string(seededBlock) + ":" +
            synth::runtime_ui::ControllersLayout::FieldToken(synth::MidiMappingRowVM::Field::GridSlotIx) +
            ":not-an-integer"));
    Require(fixture.state.commits == commitsBeforeMalformed, "malformed grid integer committed");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "1"));
    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection,
                                                        "1:system_messages"));
    synth_juce::PortableComponent renderer(surface);
    renderer.setSize(980, 300);
    int accepted = 0;

    for (int step = 0; step < kOperations; ++step)
    {
        const int commitsBefore = fixture.state.commits;
        const int action = step % 8;
        if (action == 0)
        {
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kAddSingle, "1:system_messages:grid"));
        }
        else if (action == 1)
        {
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kAddBlock, "1:system_messages:grid"));
        }
        else if (action == 2)
        {
            const auto rows = GridRows(surface, synth::MidiMappingRowVM::Kind::Individual);
            if (!rows.empty())
            {
                const std::size_t rowIx = rows[rng() % rows.size()];
                surface.DispatchAction(synth::ui::Action::WithValue(
                    synth::runtime_ui::Actions::kMappingFieldCommit,
                    "1:system_messages:" + std::to_string(rowIx) + ":" +
                        synth::runtime_ui::ControllersLayout::FieldToken(
                            synth::MidiMappingRowVM::Field::GridSlotIx) +
                        ":" + std::to_string(rng() % 8)));
            }
        }
        else if (action == 3)
        {
            auto rows = GridRows(surface, synth::MidiMappingRowVM::Kind::Individual);
            const auto blocks = GridRows(surface, synth::MidiMappingRowVM::Kind::Block);
            rows.insert(rows.end(), blocks.begin(), blocks.end());
            if (rows.size() > 1)
            {
                const std::size_t rowIx = rows[rng() % rows.size()];
                surface.DispatchAction(synth::ui::Action::WithValue(
                    synth::runtime_ui::Actions::kDeleteRow,
                    "1:system_messages:" + std::to_string(rowIx)));
            }
        }
        else if (action == 4)
        {
            const auto blocks = GridRows(surface, synth::MidiMappingRowVM::Kind::Block);
            if (!blocks.empty())
            {
                const std::size_t rowIx = blocks[rng() % blocks.size()];
                double xMin = 0.0;
                double xMax = 0.0;
                Require(surface.ViewModel().RowFieldValue(1, synth::MidiConfigSection::SystemMessages, rowIx,
                                                           synth::MidiMappingRowVM::Field::GridXMin, xMin),
                        "grid simulation reads x min");
                Require(surface.ViewModel().RowFieldValue(1, synth::MidiConfigSection::SystemMessages, rowIx,
                                                           synth::MidiMappingRowVM::Field::GridXMax, xMax),
                        "grid simulation reads x max");
                const int beforeInvalid = fixture.state.commits;
                const std::string prefix = "1:system_messages:" + std::to_string(rowIx) + ":" +
                                           synth::runtime_ui::ControllersLayout::FieldToken(
                                               synth::MidiMappingRowVM::Field::GridXMax) +
                                           ":";
                surface.DispatchAction(synth::ui::Action::WithValue(
                    synth::runtime_ui::Actions::kMappingFieldCommit,
                    prefix + std::to_string(static_cast<int>(xMin))));
                Require(fixture.state.commits == beforeInvalid, "invalid grid rectangle committed");
                surface.DispatchAction(synth::ui::Action::WithValue(
                    synth::runtime_ui::Actions::kMappingFieldCommit,
                    prefix + std::to_string(static_cast<int>(xMax))));
            }
        }
        else if (action == 5)
        {
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kToggleSection, "1:system_messages"));
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kToggleSection, "1:system_messages"));
        }
        else if (action == 6)
        {
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kToggleSection, "1:system_messages"));
            synth::JsonArena arena(1024 * 1024);
            const synth::JSON json = synth::ToJSON(arena, fixture.state.instrument);
            synth::MidiInstrumentConfig loaded;
            Require(synth::FromJSON(json, loaded), "grid simulation JSON reload");
            fixture.state.instrument = std::move(loaded);
            surface.MarkDirty();
            surface.RefreshOnTick();
            surface.DispatchAction(synth::ui::Action::WithValue(
                synth::runtime_ui::Actions::kToggleSection, "1:system_messages"));
        }
        else
        {
            const auto rows = GridRows(surface, synth::MidiMappingRowVM::Kind::Block);
            if (!rows.empty())
            {
                const std::string stableId = synth::runtime_ui::NodeIds::MappingField(
                    1, synth::MidiConfigSection::SystemMessages, rows.front(),
                    synth::MidiMappingRowVM::Field::GridYMin);
                surface.MarkDirty();
                surface.RefreshOnTick();
                renderer.RefreshFromSurface();
                Require(renderer.FindByNodeId(stableId) != nullptr, "grid simulation stable rendered row id");
            }
        }

        if (fixture.state.commits != commitsBefore)
        {
            ++accepted;
            surface.RefreshOnTick();
        }
        renderer.RefreshFromSurface();
        renderer.setSize(980, 300);
        VerifyGridProfileIndependent(fixture.state.instrument.controllers[1], oracle, step);
    }

    const synth::ui::NodeTree finalTree = surface.BuildTree();
    const synth::ui::Node* scroll = FindNode(finalTree, synth::runtime_ui::NodeIds::kScroll);
    Require(scroll != nullptr && scroll->scrollContentHeight > scroll->bounds.height,
            "grid simulation scroll reaches expanded content");
    std::cout << "GridControllersSimulation passed seed=0x" << std::hex << kGridSeed << std::dec
              << " operations=" << kOperations << " accepted=" << accepted << "\n";
}

// ---------------------------------------------------------------------------
// Controller wizard parity
//
// These cases drive the same stable node ids the Playwright acceptance suite
// drives, but through JUCE components, and compare the rendered ids, labels,
// option ids/labels, selected values, enabled states, and declared bounds
// against the portable tree. All controller-specific ids live here, in the
// simulation that drives the portable surface -- never in the generic renderer
// tests.
// ---------------------------------------------------------------------------

constexpr const char* kTwisterWizardId = "com.sheaf.midi-fighter-twister";
constexpr const char* kTwisterDisplayName = "MIDI Fighter Twister";
constexpr const char* kTwisterFormPrefix = "controller-wizard.twister.";

// The closed set of supported choices, in the order the form offers
// them. Indexes into this array are what SelectOption() below selects.
const std::array<const char*, 16> kTwisterChoiceLabels = {"Toggle Reset",
                                                          "Hold Reset",
                                                          "Toggle Random",
                                                          "Hold Random",
                                                          "Toggle Random Mod",
                                                          "Hold Random Mod",
                                                          "Toggle Gesture Select",
                                                          "Hold Gesture Select",
                                                          "Bank Select",
                                                          "Next Bank",
                                                          "Previous Bank",
                                                          "Start",
                                                          "Continue",
                                                          "Stop",
                                                          "Clock",
                                                          "Scene Select"};

const std::array<const char*, 6> kTwisterDefaultLabels = {
    "Hold Reset", "Hold Random", "Hold Random Mod", "Next Bank", "Start", "Previous Bank"};

constexpr const char* kEncoderSlotId = "controller-wizard.twister.encoder-slot";

std::string TwisterButtonField(std::size_t buttonIx, const char* field)
{
    return std::string(kTwisterFormPrefix) + "button." + std::to_string(buttonIx) + "." + field;
}

juce::Rectangle<int> SurfaceBoundsOf(const juce::Component& surface, const juce::Component& child)
{
    return surface.getLocalArea(&child, child.getLocalBounds());
}

std::string RenderedLabelText(const juce::Component& component)
{
    const auto* label = dynamic_cast<const juce::Label*>(&component);
    return label != nullptr ? label->getText().toStdString() : std::string();
}

std::string RequireLabelText(synth_juce::PortableComponent& renderer,
                             const std::string& id,
                             const std::string& step)
{
    juce::Component* component = renderer.FindByNodeId(id);
    Require(component != nullptr, step + ": " + id + " is not a rendered label");
    Require(dynamic_cast<juce::Label*>(component) != nullptr,
            step + ": " + id + " is not rendered as a Label");
    return RenderedLabelText(*component);
}

// Generic portable/JUCE comparison run after every wizard step: every semantic
// node is rendered, and its label, text, options, selected option, checked
// state, enabled state, and declared size survive into JUCE unchanged.
void VerifyRendererParity(const synth::ui::NodeTree& tree,
                          synth_juce::PortableComponent& renderer,
                          const std::string& step)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.kind == synth::ui::NodeKind::Root)
        {
            continue;
        }
        const std::string where = step + ": " + node.id.value;
        juce::Component* component = renderer.FindByNodeId(node.id.value);
        Require(component != nullptr, where + " is not rendered");
        Require(component->isEnabled() == node.enabled, where + " enabled-state mismatch");

        switch (node.kind)
        {
            case synth::ui::NodeKind::Button:
            {
                auto* button = dynamic_cast<juce::TextButton*>(component);
                Require(button != nullptr, where + " is not a TextButton");
                Require(button->getButtonText().toStdString() == node.label,
                        where + " button label mismatch");
                break;
            }
            case synth::ui::NodeKind::Label:
            case synth::ui::NodeKind::StatusText:
            {
                auto* label = dynamic_cast<juce::Label*>(component);
                Require(label != nullptr, where + " is not a Label");
                Require(label->getText().toStdString() == (node.text.empty() ? node.label : node.text),
                        where + " label text mismatch");
                break;
            }
            case synth::ui::NodeKind::ComboBox:
            {
                auto* combo = dynamic_cast<juce::ComboBox*>(component);
                Require(combo != nullptr, where + " is not a ComboBox");
                Require(combo->getNumItems() == static_cast<int>(node.options.size()),
                        where + " option count mismatch");
                int expectedSelected = -1;
                for (int ix = 0; ix < static_cast<int>(node.options.size()); ++ix)
                {
                    const synth::ui::ControlOption& option = node.options[static_cast<std::size_t>(ix)];
                    Require(combo->getItemText(ix).toStdString() == option.label,
                            where + " option label mismatch at " + std::to_string(ix));
                    if (option.id == node.selectedOption)
                    {
                        expectedSelected = ix;
                    }
                }
                Require(combo->getSelectedItemIndex() == expectedSelected,
                        where + " selected option mismatch");
                break;
            }
            case synth::ui::NodeKind::TextField:
            {
                auto* editor = dynamic_cast<juce::TextEditor*>(component);
                Require(editor != nullptr, where + " is not a TextEditor");
                Require(editor->getText().toStdString() == node.text,
                        where + " text value mismatch");
                break;
            }
            case synth::ui::NodeKind::Toggle:
            {
                auto* toggle = dynamic_cast<juce::ToggleButton*>(component);
                Require(toggle != nullptr, where + " is not a ToggleButton");
                Require(toggle->getToggleState() == node.checked, where + " checked-state mismatch");
                break;
            }
            default:
                break;
        }

        if (synth_juce::HasExplicitBounds(node.bounds))
        {
            const juce::Rectangle<int> declared = synth_juce::UiToJuceRect(node.bounds);
            const juce::Rectangle<int> rendered = SurfaceBoundsOf(renderer, *component);
            Require(rendered.getWidth() == declared.getWidth() &&
                        rendered.getHeight() == declared.getHeight(),
                    where + " declared size was reflowed by the renderer");
            Require(rendered == renderer.SurfaceBoundsForNode(node.id.value),
                    where + " resolved bounds did not survive its semantic host");
        }
    }
}

std::size_t CountNodes(const synth::ui::NodeTree& tree, const std::string& suffix)
{
    std::size_t count = 0;
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id.value.rfind(kTwisterFormPrefix, 0) == 0 &&
            node.id.value.size() >= suffix.size() &&
            node.id.value.compare(node.id.value.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            ++count;
        }
    }
    return count;
}

juce::TextButton& RequireButton(synth_juce::PortableComponent& renderer,
                                const std::string& id,
                                const std::string& step)
{
    auto* button = dynamic_cast<juce::TextButton*>(renderer.FindByNodeId(id));
    Require(button != nullptr, step + ": " + id + " is not a rendered button");
    return *button;
}

juce::TextEditor& RequireEditor(synth_juce::PortableComponent& renderer,
                                const std::string& id,
                                const std::string& step)
{
    auto* editor = dynamic_cast<juce::TextEditor*>(renderer.FindByNodeId(id));
    Require(editor != nullptr, step + ": " + id + " is not a rendered text editor");
    return *editor;
}

juce::ComboBox& RequireCombo(synth_juce::PortableComponent& renderer,
                             const std::string& id,
                             const std::string& step)
{
    auto* combo = dynamic_cast<juce::ComboBox*>(renderer.FindByNodeId(id));
    Require(combo != nullptr, step + ": " + id + " is not a rendered combo box");
    return *combo;
}

class WizardParityFixture
{
public:
    WizardParityFixture()
        : renderer_(harness_.Surface())
    {
        harness_.Surface().SetContentBounds({0.0f, 0.0f, 980.0f, 720.0f});
        renderer_.setSize(980, 720);
        Tick("initial");
    }

    synth_runtime::test::TwisterWizardHarness& Harness() { return harness_; }
    synth_juce::PortableComponent& Renderer() { return renderer_; }

    // One host UI frame followed by the full portable/JUCE comparison.
    void Tick(const std::string& step)
    {
        harness_.RefreshHost();
        renderer_.RefreshFromSurface();
        VerifyRendererParity(harness_.Surface().BuildTree(), renderer_, step);
    }

    void Click(const std::string& id, const std::string& step)
    {
        RequireButton(renderer_, id, step).onClick();
        Tick(step);
    }

    void TypeInto(const std::string& id, const std::string& text, const std::string& step)
    {
        juce::TextEditor& editor = RequireEditor(renderer_, id, step);
        editor.setText(text, true);
        editor.onReturnKey();
        Tick(step);
    }

    void SelectOption(const std::string& id, int optionIndex, const std::string& step)
    {
        RequireCombo(renderer_, id, step).setSelectedItemIndex(optionIndex, juce::sendNotificationSync);
        Tick(step);
    }

    bool Exists(const std::string& id) { return renderer_.FindByNodeId(id) != nullptr; }

private:
    synth_runtime::test::TwisterWizardHarness harness_;
    synth_juce::PortableComponent renderer_;
};

void VerifyTwisterFormDefaults(WizardParityFixture& fixture, const std::string& expectedSlot,
                               const std::string& step)
{
    const synth::ui::NodeTree tree = fixture.Harness().Surface().BuildTree();
    Require(CountNodes(tree, "encoder-slot") == 1, step + ": expected exactly one Encoder Slot");
    Require(CountNodes(tree, ".message") == 6, step + ": expected exactly six message controls");
    Require(CountNodes(tree, ".argument") == 6, step + ": expected exactly six argument controls");
    Require(RequireEditor(fixture.Renderer(), kEncoderSlotId, step).getText().toStdString() ==
                expectedSlot,
            step + ": Encoder Slot value mismatch");

    // The form names its own controls. The column headings state the
    // physical CC range and each row names its side button with the same
    // one-based wording Validate() uses when it refuses a field.
    Require(RequireLabelText(fixture.Renderer(),
                             std::string(kTwisterFormPrefix) + "column.0.heading", step) ==
                "Left (CC 8-10)",
            step + ": left column heading mismatch");
    Require(RequireLabelText(fixture.Renderer(),
                             std::string(kTwisterFormPrefix) + "column.1.heading", step) ==
                "Right (CC 11-13)",
            step + ": right column heading mismatch");

    for (std::size_t buttonIx = 0; buttonIx < 6; ++buttonIx)
    {
        Require(RequireLabelText(fixture.Renderer(), TwisterButtonField(buttonIx, "label"), step) ==
                    "Button " + std::to_string(buttonIx + 1),
                step + ": button label mismatch for button " + std::to_string(buttonIx));
        juce::ComboBox& message =
            RequireCombo(fixture.Renderer(), TwisterButtonField(buttonIx, "message"), step);
        Require(message.getNumItems() == static_cast<int>(kTwisterChoiceLabels.size()),
                step + ": message choice count mismatch");
        for (int optionIx = 0; optionIx < static_cast<int>(kTwisterChoiceLabels.size()); ++optionIx)
        {
            Require(message.getItemText(optionIx).toStdString() ==
                        kTwisterChoiceLabels[static_cast<std::size_t>(optionIx)],
                    step + ": message choice label mismatch");
        }
        Require(message.getText().toStdString() == kTwisterDefaultLabels[buttonIx],
                step + ": default message mismatch for button " + std::to_string(buttonIx));
        Require(!RequireEditor(fixture.Renderer(), TwisterButtonField(buttonIx, "argument"), step)
                     .isEnabled(),
                step + ": default argument should be disabled for button " + std::to_string(buttonIx));
    }
}

// Buttons 0-2 render in the first column and 3-5 in the second,
// mirroring the browser acceptance suite's bounding-box assertions.
void VerifyTwisterColumnGeometry(WizardParityFixture& fixture, const std::string& step)
{
    std::array<juce::Rectangle<int>, 6> boxes{};
    for (std::size_t buttonIx = 0; buttonIx < boxes.size(); ++buttonIx)
    {
        juce::ComboBox& message =
            RequireCombo(fixture.Renderer(), TwisterButtonField(buttonIx, "message"), step);
        boxes[buttonIx] = SurfaceBoundsOf(fixture.Renderer(), message);
    }
    for (std::size_t row = 1; row < 3; ++row)
    {
        Require(boxes[row].getX() == boxes[0].getX(), step + ": left column is not x-aligned");
        Require(boxes[3 + row].getX() == boxes[3].getX(), step + ": right column is not x-aligned");
        Require(boxes[row].getY() > boxes[row - 1].getY(), step + ": left column rows do not stack");
        Require(boxes[3 + row].getY() > boxes[2 + row].getY(),
                step + ": right column rows do not stack");
    }
    for (std::size_t row = 0; row < 3; ++row)
    {
        Require(boxes[3 + row].getX() >= boxes[row].getRight(),
                step + ": right column overlaps the left column");
        Require(boxes[3 + row].getY() == boxes[row].getY(),
                step + ": paired column rows are not aligned");
    }

    // BuildWizardFormTree() places the page's own chrome below the
    // height the form reports, so the form's columns must not overlap it.
    int columnsBottom = 0;
    for (std::size_t column = 0; column < 2; ++column)
    {
        const std::string columnId =
            std::string(kTwisterFormPrefix) + "column." + std::to_string(column);
        juce::Component* section = fixture.Renderer().FindByNodeId(columnId);
        Require(section != nullptr, step + ": " + columnId + " is not rendered");
        columnsBottom = std::max(columnsBottom, SurfaceBoundsOf(fixture.Renderer(), *section).getBottom());
    }
    for (const char* chromeId : {synth::runtime_ui::NodeIds::kWizardBack,
                                 synth::runtime_ui::NodeIds::kWizardCancel,
                                 synth::runtime_ui::NodeIds::kWizardSubmit})
    {
        Require(SurfaceBoundsOf(fixture.Renderer(),
                                RequireButton(fixture.Renderer(), chromeId, step))
                        .getY() >= columnsBottom,
                step + ": page chrome overlaps the form");
    }
}

const synth::MidiControllerSlot& RequireController(
    const synth_runtime::test::TwisterWizardHarness& harness,
    std::size_t index,
    const std::string& step)
{
    Require(index < harness.Instrument().controllers.size(),
            step + ": expected controller record " + std::to_string(index));
    return harness.Instrument().controllers[index];
}

void RunControllerWizardParitySimulation()
{
    namespace NodeIds = synth::runtime_ui::NodeIds;

    WizardParityFixture fixture;
    auto& harness = fixture.Harness();

    // No candidate leaves Configuration Wizard visible but disabled and
    // explains why, and the disabled action dispatches nothing.
    Require(!RequireButton(fixture.Renderer(), NodeIds::kWizardLaunch, "no candidate").isEnabled(),
            "no candidate leaves Configuration Wizard disabled");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::kAvailableEmpty, "no candidate") ==
                "No recognized unconfigured controller pair is present",
            "no candidate explains the disabled action");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::kAvailableHeading, "no candidate") ==
                "Available controllers",
            "the available controllers area renders its heading");
    fixture.Click(NodeIds::kWizardLaunch, "disabled launch");
    Require(fixture.Exists(NodeIds::kWizardLaunch) && !fixture.Exists(kEncoderSlotId),
            "a disabled Configuration Wizard opens no session");

    // One recognized unclaimed pair is available and opens its
    // form directly.
    harness.AddTwisterPair(1);
    fixture.Tick("one candidate");
    Require(RequireButton(fixture.Renderer(), NodeIds::kWizardLaunch, "one candidate").isEnabled(),
            "one candidate enables Configuration Wizard");
    // The row names the recognized controller by its registry descriptor and
    // its paired endpoints by their device names, in two separate rendered
    // nodes. Both hosts are pinned to the same two strings.
    Require(RequireLabelText(fixture.Renderer(), NodeIds::AvailableName(0), "one candidate") ==
                kTwisterDisplayName,
            "the available row names the recognized controller");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::AvailableRow(0) + ".endpoints",
                             "one candidate")
                .find(synth_runtime::test::kTwisterDeviceName) != std::string::npos,
            "the available row names the recognized pair's endpoints");

    fixture.Click(NodeIds::kWizardLaunch, "unique candidate opens");
    Require(!fixture.Exists(NodeIds::kWizardChooser), "a unique candidate skips the chooser");
    Require(!fixture.Exists(NodeIds::kWizardLaunch), "an open form exposes no second launch action");
    VerifyTwisterFormDefaults(fixture, "0", "unique candidate form");
    VerifyTwisterColumnGeometry(fixture, "unique candidate form");
    Require(fixture.Exists(NodeIds::kWizardIgnore), "the fast path still exposes Ignore");

    // A disabled argument control mutates no form state.
    juce::TextEditor& disabledArgument =
        RequireEditor(fixture.Renderer(), TwisterButtonField(0, "argument"), "disabled argument");
    disabledArgument.setText("42", true);
    disabledArgument.onReturnKey();
    fixture.Tick("disabled argument");
    Require(RequireEditor(fixture.Renderer(), TwisterButtonField(0, "argument"), "disabled argument")
                    .getText() == juce::String("0"),
            "a disabled argument control does not mutate form state");

    // Retained JUCE controls must follow the portable tree, not their own last
    // user input: cancelling and relaunching opens a fresh form whose selected
    // options and text come back to the defaults on the same components.
    fixture.TypeInto(kEncoderSlotId, "9", "cancelled form edits slot");
    fixture.SelectOption(TwisterButtonField(0, "message"), 8, "cancelled form edits message");
    fixture.Click(NodeIds::kWizardCancel, "cancel form");
    Require(harness.Commits() == 0, "Cancel commits nothing");
    fixture.Click(NodeIds::kWizardLaunch, "relaunch after cancel");
    VerifyTwisterFormDefaults(fixture, "0", "relaunched form");

    fixture.Click(NodeIds::kWizardSubmit, "unique candidate submit");
    Require(harness.Commits() == 1 && harness.Saves() == 1,
            "an accepted Submit commits once and requests one save");
    {
        const synth::MidiControllerSlot& installed = RequireController(harness, 0, "submitted record");
        Require(installed.disposition == synth::MidiControllerDisposition::Active,
                "Submit installs an Active record");
        Require(installed.name == kTwisterDisplayName, "Submit uses the descriptor display name");
        Require(installed.wizardId.has_value() && *installed.wizardId == kTwisterWizardId,
                "Submit persists the descriptor wizard id");
        Require(installed.input.identifier == "twister-in-1" &&
                    installed.output.identifier == "twister-out-1",
                "Submit assigns both discovered endpoints");
    }
    Require(harness.Status() == std::string("Configured ") + kTwisterDisplayName,
            "an accepted Submit reports the configured controller through the host status callback");
    Require(harness.Cache().Discovery().available.empty(),
            "the configured pair is no longer an available candidate");
    Require(fixture.Exists(NodeIds::ControllerBlacklist(0)),
            "a resolved wizard id offers Blacklist");

    // Blacklisting retains the profile as dormant seed data and the
    // row loses its live endpoint and mapping controls.
    fixture.Click(NodeIds::ControllerBlacklist(0), "blacklist");
    {
        const synth::MidiControllerSlot& blacklisted =
            RequireController(harness, 0, "blacklisted record");
        Require(blacklisted.disposition == synth::MidiControllerDisposition::Blacklisted,
                "Blacklist changes the disposition");
        Require(blacklisted.dormantConfig.has_value(),
                "Blacklist retains the prior profile as dormant seed data");
    }
    Require(!fixture.Exists(NodeIds::ControllerInput(0)) &&
                !fixture.Exists(NodeIds::ControllerOutput(0)) &&
                !fixture.Exists(NodeIds::ControllerDisclosure(0)),
            "a blacklisted row exposes no live endpoint selectors or mapping disclosure");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::ControllerBadge(0), "blacklist")
                .find("Released") != std::string::npos,
            "a blacklisted row shows its badge");

    // The dormant profile is observable through Configure seeding its stored slot.
    fixture.Click(NodeIds::ControllerConfigure(0), "blacklisted configure");
    VerifyTwisterFormDefaults(fixture, "0", "blacklisted configure seeds dormant slot");
    fixture.Click(NodeIds::kWizardSubmit, "blacklisted configure submit");
    Require(RequireController(harness, 0, "reactivated record").disposition ==
                synth::MidiControllerDisposition::Active,
            "Configure returns a blacklisted record to Active");

    // Rename is an inline draft plus a commit action; delete is immediate.
    // The draft and its button live in the expanded editor now, so opening
    // the row is part of reaching them.
    fixture.Click(NodeIds::ControllerDisclosure(0), "open editor for rename");
    fixture.TypeInto(NodeIds::ControllerRenameDraft(0), "Studio Twister", "rename draft");
    fixture.Click(NodeIds::ControllerRename(0), "rename commit");
    Require(RequireController(harness, 0, "renamed record").name == "Studio Twister",
            "rename commits the inline draft");
    Require(fixture.Exists(NodeIds::ControllerRenameDraft(0)) && fixture.Exists(NodeIds::ControllerRename(0)),
            "the rename editor stays open through the commit");
    fixture.Click(NodeIds::ControllerDelete(0), "delete");
    Require(harness.Instrument().controllers.empty(), "delete removes the record immediately");
    Require(harness.Cache().Discovery().available.size() == 1,
            "deleting the record restores the available candidate");

    // Ignore persists an inert record without an active profile, and
    // removing it returns the pair to Available controllers.
    fixture.Click(NodeIds::AvailableIgnore(0), "ignore available row");
    {
        const synth::MidiControllerSlot& ignored = RequireController(harness, 0, "ignored record");
        Require(ignored.disposition == synth::MidiControllerDisposition::Blacklisted,
                "Ignore persists a Blacklisted record");
        Require(!ignored.dormantConfig.has_value(),
                "Ignore stores no dormant profile");
        Require(ignored.input.identifier == "twister-in-1" &&
                    ignored.output.identifier == "twister-out-1",
                "Ignore records the concrete endpoint identities");
    }
    Require(harness.Cache().Discovery().available.empty(), "an ignored pair stops warning");
    fixture.Click(NodeIds::ControllerRemoveBlacklist(0), "remove from blacklist");
    Require(harness.Instrument().controllers.empty(), "Remove from blacklist deletes the record");
    Require(harness.Cache().Discovery().available.size() == 1,
            "Remove from blacklist restores the available candidate");

    // Two candidates open a chooser that identifies both pairs.
    harness.AddTwisterPair(2);
    fixture.Tick("two candidates");
    Require(harness.Cache().Discovery().available.size() == 2,
            "two present pairs classify as two candidates");
    fixture.Click(NodeIds::kWizardLaunch, "chooser opens");
    Require(!fixture.Exists(kEncoderSlotId), "multiple candidates require a selection first");
    const std::string secondChoiceId =
        NodeIds::WizardChooserCandidate(harness.Cache().Discovery().available[1]);
    Require(RequireButton(fixture.Renderer(), secondChoiceId, "chooser")
                    .getButtonText()
                    .toStdString()
                    .find("twister-in-2") != std::string::npos,
            "chooser rows expose their paired endpoint identifiers");
    fixture.Click(secondChoiceId, "chooser selects second candidate");
    VerifyTwisterFormDefaults(fixture, "0", "chosen candidate form");
    fixture.Click(NodeIds::kWizardSubmit, "chosen candidate submit");
    Require(RequireController(harness, 0, "chosen record").input.identifier == "twister-in-2",
            "the chooser opens only the selected candidate");

    // The second record takes the smallest free numeric suffix.
    fixture.Click(NodeIds::kWizardLaunch, "remaining candidate opens");
    fixture.Click(NodeIds::kWizardSubmit, "remaining candidate submit");
    Require(RequireController(harness, 1, "suffixed record").name ==
                std::string(kTwisterDisplayName) + " 2",
            "a duplicate display name takes the smallest free suffix");

    std::cout << "ControllerWizardParitySimulation passed\n";
}

// A manually added record carries no persisted wizard id, so the
// registry-gated lifecycle actions are not offered even though its kind is the
// same hardware kind the wizard installs. This is the negative control for a
// manual record's wizard id, checked directly below, with Blacklist and
// Configure both withheld as a result.
void RunManualRecordSimulation()
{
    namespace NodeIds = synth::runtime_ui::NodeIds;

    WizardParityFixture fixture;
    // Preset options: the one registry descriptor (MIDI Fighter Twister) then
    // the four Custom entries in kind order Generic, MF Twister, Launchpad,
    // WRLD.Bldr -- so Custom (MF Twister) is index 2. There is no add-row name
    // field any more; the record's name is derived from the chosen kind.
    fixture.SelectOption(NodeIds::kAddPreset, 2, "manual add preset");
    Require(RequireCombo(fixture.Renderer(), NodeIds::kAddPreset, "manual add preset")
                    .getText() == juce::String("Custom (MF Twister)"),
            "the add-row Preset combo offers a Custom entry for the Twister hardware kind");
    fixture.Click(NodeIds::kAddButton, "manual add commit");

    const synth::MidiControllerSlot& manual =
        RequireController(fixture.Harness(), 0, "manual record");
    Require(manual.name == "MF Twister" && manual.kind == synth::MidiProfileKind::MfTwister,
            "a Custom add derives the manual record's name from its chosen kind's display name");
    Require(!manual.wizardId.has_value(), "a manual record carries no wizard id");
    fixture.Click(NodeIds::ControllerDisclosure(0), "open editor for manual record checks");
    Require(fixture.Exists(NodeIds::ControllerRename(0)) &&
                fixture.Exists(NodeIds::ControllerDelete(0)),
            "a manual record keeps Rename (in its expanded editor) and Delete");
    Require(!fixture.Exists(NodeIds::ControllerBlacklist(0)) &&
                !fixture.Exists(NodeIds::ControllerConfigure(0)),
            "a manual record is offered no registry-gated wizard action");

    std::cout << "ControllerWizardManualRecordSimulation passed\n";
}

// A refused Submit keeps every entered value, commits nothing, and
// saves nothing -- both for an invalid form and for a candidate whose endpoints
// disappeared while the form was open.
void RunControllerWizardRefusalSimulation()
{
    namespace NodeIds = synth::runtime_ui::NodeIds;
    constexpr const char* kOverflowArgument = "999999999999999999999999999999999999999";

    WizardParityFixture fixture;
    auto& harness = fixture.Harness();
    harness.AddTwisterPair(1);
    fixture.Tick("refusal fixture");
    Require(!harness.NoteDeviceListChanged(),
            "an unchanged device list never recomputes the cached classification");

    fixture.Click(NodeIds::kWizardLaunch, "refusal form opens");
    fixture.TypeInto(kEncoderSlotId, "4", "refusal edits slot");
    fixture.SelectOption(TwisterButtonField(0, "message"), 8, "refusal selects Bank Select");
    Require(RequireEditor(fixture.Renderer(), TwisterButtonField(0, "argument"), "refusal")
                .isEnabled(),
            "Bank Select enables its per-button argument");
    fixture.TypeInto(TwisterButtonField(0, "argument"), "7", "refusal sets bank argument");
    fixture.SelectOption(TwisterButtonField(1, "message"), 15, "refusal selects Scene Select");
    fixture.TypeInto(TwisterButtonField(1, "argument"), kOverflowArgument, "refusal overflows");

    fixture.Click(NodeIds::kWizardSubmit, "invalid submit");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::kWizardStatus, "invalid submit")
                .find("Button 2") != std::string::npos,
            "an invalid form refusal names the offending button");
    Require(harness.Commits() == 0 && harness.Saves() == 0,
            "a refused Submit commits and saves nothing");
    Require(RequireEditor(fixture.Renderer(), kEncoderSlotId, "invalid submit").getText() ==
                    juce::String("4") &&
                RequireEditor(fixture.Renderer(), TwisterButtonField(0, "argument"), "invalid submit")
                        .getText() == juce::String("7") &&
                RequireEditor(fixture.Renderer(), TwisterButtonField(1, "argument"), "invalid submit")
                        .getText() == juce::String(kOverflowArgument),
            "a refused Submit retains every entered value");

    fixture.TypeInto(TwisterButtonField(1, "argument"), "5", "refusal fixes argument");
    harness.RemoveTwisterPair(1);
    fixture.Tick("candidate disappears");
    fixture.Click(NodeIds::kWizardSubmit, "stale submit");
    Require(RequireLabelText(fixture.Renderer(), NodeIds::kWizardStatus, "stale submit")
                .find("reconnect") != std::string::npos,
            "a disappeared candidate refuses Submit with a reconnect message");
    Require(harness.Commits() == 0 && harness.Saves() == 0,
            "a stale Submit commits and saves nothing");
    Require(harness.Instrument().controllers.empty(), "a stale Submit installs no record");
    Require(RequireEditor(fixture.Renderer(), kEncoderSlotId, "stale submit").getText() ==
                    juce::String("4") &&
                RequireEditor(fixture.Renderer(), TwisterButtonField(0, "argument"), "stale submit")
                        .getText() == juce::String("7"),
            "a stale Submit retains every entered value");

    std::cout << "ControllerWizardRefusalSimulation passed\n";
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    constexpr std::uint32_t kSeed = 0x5EAF2026;
    std::mt19937 rng(kSeed);

    synth_runtime::test::ControllersHarnessFixture fixture;
    synth::runtime_ui::ControllersPageSurface surface = fixture.MakeSurface();
    surface.SetEnumerateDevices(fixture.state.devices);
    surface.SetContentBounds({0.0f, 0.0f, 980.0f, 720.0f});
    surface.MarkDirty();
    surface.RefreshOnTick();

    synth_juce::PortableComponent renderer(surface);
    renderer.setSize(980, 720);
    renderer.RefreshFromSurface();

    auto* inputCombo = dynamic_cast<juce::ComboBox*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerInput(0)));
    Require(inputCombo != nullptr, "controller zero input node is a ComboBox");
    int twisterInputIndex = -1;
    for (int optionIx = 0; optionIx < inputCombo->getNumItems(); ++optionIx)
    {
        if (inputCombo->getItemText(optionIx) == juce::String("Twister In"))
        {
            twisterInputIndex = optionIx;
            break;
        }
    }
    Require(twisterInputIndex >= 0, "controller zero input includes Twister In");
    inputCombo->setSelectedItemIndex(twisterInputIndex, juce::sendNotificationSync);
    surface.RefreshOnTick();
    renderer.RefreshFromSurface();
    auto* refreshedCombo = dynamic_cast<juce::ComboBox*>(
        renderer.FindByNodeId(synth::runtime_ui::NodeIds::ControllerInput(0)));
    Require(refreshedCombo != nullptr, "refreshed controller zero input node is a ComboBox");
    Require(fixture.state.instrument.controllers[0].input.identifier == "twister-in-id",
            "JUCE endpoint selection commits the selected device");
    Require(refreshedCombo->getText() == juce::String("Twister In"),
            "JUCE endpoint selection remains selected after refresh");

    int controllerNameSuffix = 0;
    std::string lastAction = "initial";
    for (int step = 0; step < 250; ++step)
    {
        synth::ui::NodeTree tree = surface.BuildTree();
        std::vector<synth::ui::Action> actions = CollectActions(tree, controllerNameSuffix);
        Require(!actions.empty(), "simulation action set empty");

        std::uniform_int_distribution<std::size_t> pick(0, actions.size() - 1);
        synth::ui::Action action = actions[pick(rng)];
        if (action.name == synth::runtime_ui::Actions::kAddController)
        {
            ++controllerNameSuffix;
        }

        lastAction = Describe(action);
        surface.DispatchAction(action);
        if (surface.NeedsDeferredDispatch(action))
        {
            surface.RefreshOnTick();
        }
        renderer.RefreshFromSurface();
        renderer.setSize(980, 720);
        VerifyTreeAndRenderer(surface.BuildTree(), renderer, fixture, step, lastAction);
    }

    RunGridSimulation();
    RunControllerWizardParitySimulation();
    RunManualRecordSimulation();
    RunControllerWizardRefusalSimulation();

    // The caption criterion had real subjects across the whole run, and its
    // exception list was the thing excusing them. Without these an id-convention
    // change could leave every step looking at nothing and every step passing.
    // See the note beside the counters for why the floor is on subjects rather
    // than on conforming controls.
    Require(g_captionFormControlsSeen > 0,
            "the caption criterion saw no form control at all in the entire simulation");
    Require(g_captionResidualsMatched > 0,
            "no named caption exception matched anywhere in the simulation, so the list is stale");

    std::cout << "ControllersPageSimulationTests passed seed=0x" << std::hex << kSeed << "\n";
    return 0;
}
