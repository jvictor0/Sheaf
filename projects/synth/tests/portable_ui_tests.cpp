#include "synth/AppConcepts.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"
#include "synth/PortableScopeVisualizer.hpp"
#include "synth/RuntimePages.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/ConstantBarVisualizer.hpp"
#include "synth/DspScope.hpp"
#include "synth/GangedRandomLfoVisualizer.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "synth/MidiController.hpp"
#include "synth/NoiseWaveformVisualizer.hpp"
#include "synth/StandardModulators.hpp"

#include "support/SourceScan.hpp"
#include "support/VisualCriteria.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "../apps/braid-4/Braid4Draw.hpp"
#include "../apps/braid-4/Braid4UI.hpp"
#include "../apps/braid-4/Braid4UiModel.hpp"
#include "../apps/miniapp/MiniAppDraw.hpp"
#include "../apps/miniapp/MiniAppUI.hpp"
#include "../apps/miniapp/MiniAppUiModel.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "portable UI tests must not see JUCE"
#endif

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

void RequireNear(float actual, float expected, float tolerance, const char* label)
{
    if (std::fabs(actual - expected) > tolerance)
    {
        throw std::runtime_error(label);
    }
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const char* id)
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

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const std::string& id)
{
    return FindNodeById(tree, id.c_str());
}

const synth::ui::Node& FindNode(const synth::ui::NodeTree& tree, const char* id)
{
    const synth::ui::Node* node = FindNodeById(tree, id);
    if (node == nullptr)
    {
        throw std::runtime_error(std::string("missing node: ") + id);
    }
    return *node;
}

const synth::ui::Node& FindNode(const synth::ui::NodeTree& tree, const std::string& id)
{
    return FindNode(tree, id.c_str());
}

bool HasNode(const synth::ui::NodeTree& tree, const char* id)
{
    return FindNodeById(tree, id) != nullptr;
}

bool FunctionBodyContains(const std::string& source,
                          const std::string& functionName,
                          const std::string& needle,
                          const std::string& returnType = "ui::NodeTree")
{
    const std::string marker = "inline " + returnType + " " + functionName + "(";
    const std::size_t signature = source.find(marker);
    if (signature == std::string::npos)
    {
        throw std::runtime_error("missing function: " + functionName);
    }
    const std::size_t bodyStart = source.find('{', signature);
    if (bodyStart == std::string::npos)
    {
        throw std::runtime_error("missing function body: " + functionName);
    }
    int depth = 0;
    for (std::size_t i = bodyStart; i < source.size(); ++i)
    {
        if (source[i] == '{')
        {
            ++depth;
        }
        else if (source[i] == '}')
        {
            --depth;
            if (depth == 0)
            {
                return synth::test::StripComments(source.substr(bodyStart, i - bodyStart + 1))
                           .find(needle) != std::string::npos;
            }
        }
    }
    throw std::runtime_error("unterminated function body: " + functionName);
}

std::vector<float> ColumnXOffsetsOf(const synth::ui::NodeTree& tree, const char* formId, std::size_t column)
{
    const synth::ui::Node& form = FindNode(tree, formId);
    std::vector<float> offsets;
    for (const synth::ui::NodeId& rowId : form.children)
    {
        const synth::ui::Node& row = FindNode(tree, rowId.value);
        if (row.kind != synth::ui::NodeKind::Row || row.children.size() <= column)
        {
            continue;
        }
        offsets.push_back(FindNode(tree, row.children[column].value).bounds.x);
    }
    return offsets;
}

std::vector<float> ColumnWidthsOf(const synth::ui::NodeTree& tree, const char* formId, std::size_t column)
{
    const synth::ui::Node& form = FindNode(tree, formId);
    std::vector<float> widths;
    for (const synth::ui::NodeId& rowId : form.children)
    {
        const synth::ui::Node& row = FindNode(tree, rowId.value);
        if (row.kind != synth::ui::NodeKind::Row || row.children.size() <= column)
        {
            continue;
        }
        widths.push_back(FindNode(tree, row.children[column].value).bounds.width);
    }
    return widths;
}

bool AllEqual(const std::vector<float>& offsets)
{
    if (offsets.empty())
    {
        return false;
    }
    return std::all_of(offsets.begin(), offsets.end(), [&](float value) {
        return std::fabs(value - offsets.front()) <= 0.0001f;
    });
}

// The x-offset shared by every cell in a form-grid column, with the emptiness
// stated rather than assumed. Callers used to take `.front()` on the vector
// above, which is safe only because a preceding `AllEqual` Require aborts first
// -- `AllEqual` returns false on an empty vector. That was undocumented,
// order-dependent, and one reordered assertion away from a crash.
float SharedColumnXOffsetOf(const synth::ui::NodeTree& tree, const char* formId, std::size_t column)
{
    const std::vector<float> offsets = ColumnXOffsetsOf(tree, formId, column);
    if (offsets.empty())
    {
        throw std::runtime_error(std::string("form '") + formId + "' has no column " +
                                 std::to_string(column) + " cells to compare");
    }
    if (!AllEqual(offsets))
    {
        throw std::runtime_error(std::string("form '") + formId + "' column " +
                                 std::to_string(column) + " cells do not share an x-offset");
    }
    return offsets.front();
}

float MaxRightEdgeOfColumn(const synth::ui::NodeTree& tree, const char* formId, std::size_t column)
{
    const synth::ui::Node& form = FindNode(tree, formId);
    float result = 0.0f;
    for (const synth::ui::NodeId& rowId : form.children)
    {
        const synth::ui::Node& row = FindNode(tree, rowId.value);
        if (row.kind != synth::ui::NodeKind::Row || row.children.size() <= column)
        {
            continue;
        }
        const synth::ui::Node& cell = FindNode(tree, row.children[column].value);
        result = std::max(result, cell.bounds.x + cell.bounds.width);
    }
    return result;
}

bool TextStyleMatches(const synth::ui::TextStyle& actual, const synth::ui::TextStyle& expected)
{
    return actual.size == expected.size && actual.color == expected.color && actual.align == expected.align;
}

int CountRootNodes(const synth::ui::NodeTree& tree)
{
    int rootCount = 0;
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.kind == synth::ui::NodeKind::Root)
        {
            ++rootCount;
        }
    }
    return rootCount;
}

bool NodeHasChild(const synth::ui::Node* parent, const synth::ui::NodeId& child)
{
    return parent != nullptr &&
           std::find(parent->children.begin(), parent->children.end(), child) != parent->children.end();
}

bool PointInside(synth::ui::Point point, synth::ui::Bounds bounds)
{
    return point.x >= bounds.x && point.x <= bounds.x + bounds.width &&
           point.y >= bounds.y && point.y <= bounds.y + bounds.height;
}

bool BoundsInside(synth::ui::Bounds inner, synth::ui::Bounds outer)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

// Bounds are parent-relative, so containment is a claim about a node against
// its own parent's box, not against the surface.
void RequireNodeContainedInParent(const synth::ui::NodeTree& tree, const std::string& id)
{
    const synth::ui::Node* node = FindNodeById(tree, id);
    Require(node != nullptr, ("missing node " + id).c_str());
    for (const synth::ui::Node& candidate : tree.nodes)
    {
        for (const synth::ui::NodeId& child : candidate.children)
        {
            if (child.value != id)
            {
                continue;
            }
            // A ScrollArea's children are placed in scroll-CONTENT space,
            // so the extent that must contain them is the content
            // extent the resolver published -- which is exactly what both
            // backends size their content surface to (`max(bounds, declared)`).
            // Checking them against the viewport box instead would assert that
            // a scrolling list never scrolls.
            const synth::ui::Bounds parentExtent =
                candidate.kind == synth::ui::NodeKind::ScrollArea
                    ? synth::ui::Bounds{0.0f,
                                        0.0f,
                                        std::max(candidate.bounds.width, candidate.scrollContentWidth),
                                        std::max(candidate.bounds.height, candidate.scrollContentHeight)}
                    : synth::ui::Bounds{0.0f, 0.0f, candidate.bounds.width, candidate.bounds.height};
            Require(BoundsInside(node->bounds, parentExtent),
                    ("node " + id + " stays inside its parent").c_str());
            return;
        }
    }
    Require(false, ("node " + id + " has no parent").c_str());
}

void RequireWaveformGeometryInside(const std::vector<synth::ui::DrawCommand>& commands,
                                   synth::ui::Bounds bounds,
                                   const char* label)
{
    std::size_t polylines = 0;
    std::size_t markers = 0;
    for (const synth::ui::DrawCommand& command : commands)
    {
        if (command.kind == synth::ui::DrawCommand::Kind::Polyline)
        {
            ++polylines;
            for (synth::ui::Point point : command.points)
            {
                Require(PointInside(point, bounds), label);
            }
        }
        if (command.kind == synth::ui::DrawCommand::Kind::FillEllipse)
        {
            ++markers;
            Require(BoundsInside(command.bounds, bounds), label);
        }
    }
    Require(polylines > 0, label);
    Require(markers > 0, label);
}

void FillScopeWriter(synth::ScopeWriter& writer, std::size_t channels)
{
    auto holder = writer.ReserveChans(channels);
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        holder.RecordStart(channel);
    }
    for (std::size_t frame = 0; frame < 64; ++frame)
    {
        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            const float normalized = static_cast<float>((frame + channel * 7) % 32) / 31.0f;
            holder.Write(channel, normalized * 2.0f - 1.0f);
        }
        writer.AdvanceIndex();
    }
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        holder.RecordEnd(channel);
    }
    writer.Publish();
}

synth::ui::EncoderDrawState RepresentativeEncoderState()
{
    synth::ui::EncoderDrawState state;
    state.connected = true;
    state.baseColor = synth::Color::Cyan;
    state.shortLabel = "tune";
    state.modulatorsAffectingMask = 1u;
    state.modulatorColors = {synth::Color::Green};
    state.gesturesAffectingMask = 1u;
    state.gestureColors = {synth::Color::Orange};
    state.voiceCount = 1;
    state.voices.push_back({.value = 0.25f,
                            .spreadValue = 0.10f,
                            .minValue = 0.1f,
                            .maxValue = 0.9f,
                            .indicatorColor = synth::Color::Yellow});
    return state;
}

bool IsDescendantOf(const synth::ui::NodeTree& tree,
                    const std::string& nodeId,
                    const std::string& ancestorId)
{
    std::string current = nodeId;
    for (std::size_t hop = 0; hop < tree.nodes.size(); ++hop)
    {
        const synth::ui::Node* parent = nullptr;
        for (const synth::ui::Node& candidate : tree.nodes)
        {
            for (const synth::ui::NodeId& child : candidate.children)
            {
                if (child.value == current)
                {
                    parent = &candidate;
                    break;
                }
            }
            if (parent != nullptr)
            {
                break;
            }
        }
        if (parent == nullptr)
        {
            return false;
        }
        if (parent->id.value == ancestorId)
        {
            return true;
        }
        current = parent->id.value;
    }
    return false;
}

void RequireBrowserIsRootlessDescendant(const synth::ui::NodeTree& tree)
{
    const synth::ui::Node* root = FindNodeById(tree, synth::runtime_ui::NodeIds::kFileRoot);
    const synth::ui::Node* browser = FindNodeById(tree, synth::runtime_ui::NodeIds::kFileBrowser);
    const synth::ui::Node* firstRow = FindNodeById(tree, synth::runtime_ui::NodeIds::FileBrowserEntry(0));
    Require(CountRootNodes(tree) == 1, "file page tree has exactly one root");
    Require(root != nullptr, "file page root exists");
    Require(browser != nullptr, "browser section exists");
    if (firstRow != nullptr)
    {
        // The row is a browser descendant through the scrolling list rather
        // than a direct child of the panel, so the check is descendancy plus
        // the exact parent -- which is strictly more than "is a child" said.
        Require(IsDescendantOf(tree, firstRow->id.value, synth::runtime_ui::NodeIds::kFileBrowser),
                "browser row is a browser descendant");
        Require(NodeHasChild(FindNodeById(tree, synth::runtime_ui::NodeIds::kFileBrowserList),
                             firstRow->id),
                "browser row is a child of the browser's scrolling list");
        Require(!NodeHasChild(root, firstRow->id), "browser row is not a direct file root child");
    }
}


bool ActionsMatch(const std::optional<synth::ui::Action>& lhs,
                  const std::optional<synth::ui::Action>& rhs)
{
    if (lhs.has_value() != rhs.has_value())
    {
        return false;
    }
    return !lhs.has_value() || (lhs->name == rhs->name && lhs->value == rhs->value);
}

// The forest roots of a rootless subtree, computed the way Splice computes
// them: the nodes no sibling names as a child.
std::vector<std::string> ForestRootsOf(const synth::ui::Subtree& subtree)
{
    std::set<std::string> namedAsChild;
    for (const synth::ui::Node& node : subtree.tree.nodes)
    {
        for (const synth::ui::NodeId& child : node.children)
        {
            namedAsChild.insert(child.value);
        }
    }
    std::vector<std::string> roots;
    for (const synth::ui::Node& node : subtree.tree.nodes)
    {
        if (namedAsChild.count(node.id.value) == 0)
        {
            roots.push_back(node.id.value);
        }
    }
    return roots;
}

void RequireSubtreeIsSplicedWhole(const synth::ui::NodeTree& page,
                                  const synth::ui::Subtree& subtree,
                                  const char* splicePointId,
                                  const char* label)
{
    for (const synth::ui::Node& node : subtree.tree.nodes)
    {
        Require(node.kind != synth::ui::NodeKind::Root, "a spliced subtree is rootless");
    }
    // The splice point's children are exactly the subtree's forest roots, in
    // the subtree's own order: nothing of the host's leaks in beside them and
    // nothing of the subtree's is left behind.
    std::vector<std::string> placed;
    for (const synth::ui::NodeId& child : FindNode(page, splicePointId).children)
    {
        placed.push_back(child.value);
    }
    Require(placed == ForestRootsOf(subtree), label);
    for (const synth::ui::Node& node : subtree.tree.nodes)
    {
        const synth::ui::Node& inPage = FindNode(page, node.id.value);
        Require(inPage.kind == node.kind && inPage.label == node.label && inPage.text == node.text &&
                    inPage.selected == node.selected && inPage.color == node.color &&
                    inPage.textStyle.has_value() == node.textStyle.has_value() &&
                    ActionsMatch(inPage.action, node.action) &&
                    ActionsMatch(inPage.doubleClickAction, node.doubleClickAction) &&
                    inPage.children.size() == node.children.size(),
                "a spliced node keeps the subtree's own identity and state");
    }
}

// The subtree's first node, whatever kind it is. Since the boundary moved
// the whole viewer into the subtree this is the title, not a row -- so it is
// named for what it returns rather than for what a row assertion would want.
// To assert about a row, find a child of `kFileBrowserList` instead.
std::string FirstSubtreeNodeId(const synth::ui::Subtree& subtree)
{
    Require(!subtree.tree.nodes.empty(), "the patch browser subtree produces nodes");
    return subtree.tree.nodes.front().id.value;
}

synth::runtime_ui::FilePageSnapshot RepresentativeBrowserState()
{
    synth::runtime_ui::FilePageSnapshot snapshot;
    snapshot.patchNameText = "PatchA";
    snapshot.hasCurrentPatch = true;
    snapshot.patchesRoot = "/patches";
    snapshot.statusText = "Choose a patch name";
    snapshot.browserOpen = true;
    snapshot.browserKind = synth::runtime_ui::FileBrowserKind::SaveAs;
    snapshot.browserSaveName = "PatchA";
    snapshot.browserEntries.push_back({"Alpha", "Alpha", false});
    snapshot.browserEntries.push_back({"Beta", "Beta", true});
    snapshot.browserEntries.push_back({"Gamma", "Gamma", false});
    return snapshot;
}

struct TestSurface final : synth::ui::Surface
{
    synth::ui::NodeTree BuildTree() override
    {
        return {};
    }
    void SetActionHandler(ActionHandler) override {}
    void DispatchAction(const synth::ui::Action&) override {}
};

struct TestVisualizer final : synth::ui::Visualizer
{
    std::vector<synth::ui::DrawCommand> DrawVisible() const override
    {
        const synth::ui::Bounds bounds = GetBounds();
        return {synth::ui::DrawCommand::Fill({0.0f, 0.0f, bounds.width, bounds.height},
                                             synth::Color::Cyan)};
    }
};

struct TestScopeLayerState
{
    std::atomic<bool> connected{false};
    std::atomic<const synth::ScopeWriter*> scope{nullptr};
    std::atomic<std::size_t> scopeChannel{0};
    synth::AtomicColor scopeColor;
};

struct TestApp
{
    static synth::RuntimeConfig Config()
    {
        return {};
    }
    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }
    TestSurface surface;
};

void TestGangedRandomLfoVisualizer()
{
    using synth::GangedRandomLfoSnapshot;
    using synth::ui::Bounds;
    using synth::ui::DrawCommand;

    GangedRandomLfoSnapshot<2> snapshot;
    snapshot.sampleRate = 8.0;
    snapshot.roundElapsedSamples = 3.0;
    snapshot.voices[0] = {
        .source = 0.0f,
        .target = 1.0f,
        .output = 0.91f,
        .shape = 0.0f,
        .waitingIncrement = 0.25,
        .movingIncrement = 0.5,
        .color = synth::Color::Cyan,
    };
    snapshot.voices[1] = {
        .source = 1.0f,
        .target = 0.25f,
        .output = 0.02f,
        .shape = 1.0f,
        .waitingIncrement = 0.125,
        .movingIncrement = 0.25,
        .color = synth::Color::Orange,
    };

    const Bounds bounds{10.0f, 20.0f, 180.0f, 90.0f};
    std::vector<DrawCommand> commands;
    synth::ui::BuildGangedRandomLfoCommands(snapshot, bounds, commands);
    Require(commands.size() > 6, "ganged visualizer emits both paths");
    Require(commands.size() <= synth::ui::GangedRandomLfoGeometry::MaximumCommandCount<2>(),
            "ganged visualizer command count is fixed and bounded");
    Require(commands[0].kind == DrawCommand::Kind::Fill && commands[1].kind == DrawCommand::Kind::Line,
            "ganged visualizer starts with background and axis");

    const Bounds nodeExtent{0.0f, 0.0f, bounds.width, bounds.height};
    const float expectedPresentX = 4.0f + (nodeExtent.width - 8.0f) * 3.0f / 12.0f;
    std::array<std::size_t, 2> dots{};
    bool sawCyanPast = false;
    bool sawOrangePast = false;
    bool sawDashedGap = false;
    synth::ui::Point cyanPastEnd{};
    synth::ui::Point cyanDotCenter{};
    float previousFutureStart = -1.0f;
    for (const DrawCommand& command : commands)
    {
        if (command.kind == DrawCommand::Kind::Polyline)
        {
            Require(command.points.size() >= 2, "ganged path polylines have drawable geometry");
            for (const auto point : command.points)
            {
                Require(PointInside(point, nodeExtent), "ganged path points are node-clipped");
            }
            if (command.points.back().x <= expectedPresentX + 0.001f)
            {
                sawCyanPast = sawCyanPast || command.color == synth::Color::Cyan;
                sawOrangePast = sawOrangePast || command.color == synth::Color::Orange;
                if (command.color == synth::Color::Cyan)
                {
                    cyanPastEnd = command.points.back();
                }
            }
            if (command.points.front().x >= expectedPresentX - 0.001f)
            {
                if (previousFutureStart >= 0.0f && command.points.front().x > previousFutureStart + 0.5f)
                {
                    sawDashedGap = true;
                }
                previousFutureStart = command.points.back().x;
            }
        }
        else if (command.kind == DrawCommand::Kind::FillEllipse)
        {
            Require(BoundsInside(command.bounds, nodeExtent), "ganged dot is node-clipped");
            const float centerX = command.bounds.x + command.bounds.width * 0.5f;
            RequireNear(centerX, expectedPresentX, 0.001f, "all ganged dots share present x");
            if (command.color == synth::Color::Cyan)
            {
                ++dots[0];
                const float centerY = command.bounds.y + command.bounds.height * 0.5f;
                cyanDotCenter = {centerX, centerY};
                RequireNear(centerY, 4.0f + (nodeExtent.height - 8.0f), 0.001f,
                            "dot reconstructs source hold instead of snapshot output");
            }
            if (command.color == synth::Color::Orange)
            {
                ++dots[1];
            }
        }
    }
    Require(dots == std::array<std::size_t, 2>{1, 1}, "one independently colored dot per voice");
    Require(sawCyanPast && sawOrangePast, "solid past uses each voice color");
    RequireNear(cyanPastEnd.x, cyanDotCenter.x, 0.001f, "solid past ends at the present dot x");
    RequireNear(cyanPastEnd.y, cyanDotCenter.y, 0.001f, "solid past ends at the reconstructed dot y");
    Require(sawDashedGap, "future path uses alternating bounded segments");

    // Voice zero lasts ceil(4) + ceil(2) = 6 samples, so its path must hold
    // target to voice one's shared ceil(8) + ceil(4) = 12-sample endpoint.
    bool cyanEndsAtTarget = false;
    const float targetY = 4.0f;
    for (const DrawCommand& command : commands)
    {
        if (command.kind == DrawCommand::Kind::Polyline && command.color == synth::Color::Cyan &&
            !command.points.empty() && command.points.back().x > nodeExtent.width - 8.0f)
        {
            cyanEndsAtTarget = std::fabs(command.points.back().y - targetY) < 0.01f;
        }
    }
    Require(cyanEndsAtTarget, "early voice holds target through shared maximum duration");

    auto movingSnapshot = snapshot;
    movingSnapshot.roundElapsedSamples = 5.0;
    movingSnapshot.voices[0].movingIncrement = 0.25;
    movingSnapshot.voices[0].shape = 1.0f;
    std::vector<DrawCommand> movingCommands;
    synth::ui::BuildGangedRandomLfoCommands(movingSnapshot, bounds, movingCommands);
    const auto movingDot = std::find_if(movingCommands.begin(), movingCommands.end(), [](const auto& command) {
        return command.kind == DrawCommand::Kind::FillEllipse && command.color == synth::Color::Cyan;
    });
    Require(movingDot != movingCommands.end(), "moving interval has a present dot");
    const float shapedQuarter = synth::ShapedInterpolate(0.0f, 1.0f, 1.0f, 0.25);
    RequireNear(movingDot->bounds.y + movingDot->bounds.height * 0.5f,
                4.0f + (nodeExtent.height - 8.0f) * (1.0f - shapedQuarter),
                0.01f,
                "moving path and dot use shared shaped interpolation");

    auto boundarySnapshot = snapshot;
    boundarySnapshot.roundElapsedSamples = 4.0;
    boundarySnapshot.voices[0].waitingIncrement = 0.3;
    boundarySnapshot.voices[0].movingIncrement = 0.6;
    std::vector<DrawCommand> boundaryCommands;
    synth::ui::BuildGangedRandomLfoCommands(boundarySnapshot, bounds, boundaryCommands);
    const auto boundaryDot = std::find_if(boundaryCommands.begin(), boundaryCommands.end(), [](const auto& command) {
        return command.kind == DrawCommand::Kind::FillEllipse && command.color == synth::Color::Cyan;
    });
    Require(boundaryDot != boundaryCommands.end(), "discarded-remainder boundary has a dot");
    RequireNear(boundaryDot->bounds.y + boundaryDot->bounds.height * 0.5f,
                4.0f + (nodeExtent.height - 8.0f),
                0.01f,
                "waiting boundary remains aligned after discarded remainder");
    boundarySnapshot.roundElapsedSamples = 5.0;
    boundaryCommands.clear();
    synth::ui::BuildGangedRandomLfoCommands(boundarySnapshot, bounds, boundaryCommands);
    const auto postBoundaryDot = std::find_if(
        boundaryCommands.begin(), boundaryCommands.end(), [](const auto& command) {
            return command.kind == DrawCommand::Kind::FillEllipse && command.color == synth::Color::Cyan;
        });
    Require(postBoundaryDot != boundaryCommands.end(), "post-boundary movement has a dot");
    RequireNear(postBoundaryDot->bounds.y + postBoundaryDot->bounds.height * 0.5f,
                4.0f + (nodeExtent.height - 8.0f) * 0.4f,
                0.01f,
                "discarded wait remainder does not shift moving interpolation");

    const Bounds resized{1.0f, 2.0f, 37.0f, 23.0f};
    const Bounds resizedExtent{0.0f, 0.0f, resized.width, resized.height};
    std::vector<DrawCommand> resizedCommands;
    auto veryLong = snapshot;
    veryLong.voices[1].waitingIncrement = 1.0 / 1000000000.0;
    synth::ui::BuildGangedRandomLfoCommands(veryLong, resized, resizedCommands);
    Require(resizedCommands.size() <= synth::ui::GangedRandomLfoGeometry::MaximumCommandCount<2>(),
            "geometry ceiling is independent of round duration");
    for (const DrawCommand& command : resizedCommands)
    {
        if (command.kind == DrawCommand::Kind::Polyline)
        {
            Require(command.points.size() <= synth::ui::GangedRandomLfoGeometry::kPathSegments + 1,
                    "ganged polyline point ceiling is fixed");
            for (const auto point : command.points)
            {
                Require(PointInside(point, resizedExtent), "resized ganged geometry remains node-clipped");
            }
        }
    }

    const Bounds tiny{2.0f, 3.0f, 7.0f, 7.0f};
    std::vector<DrawCommand> tinyCommands;
    synth::ui::BuildGangedRandomLfoCommands(snapshot, tiny, tinyCommands);
    Require(std::none_of(tinyCommands.begin(), tinyCommands.end(), [](const DrawCommand& command) {
                return command.kind == DrawCommand::Kind::FillEllipse;
            }),
            "sub-eight-pixel bounds do not emit zero-area present dots");

    auto invalid = snapshot;
    invalid.voices[0].movingIncrement = 0.0;
    std::vector<DrawCommand> invalidCommands;
    synth::ui::BuildGangedRandomLfoCommands(invalid, bounds, invalidCommands);
    Require(invalidCommands.size() == 2, "invalid increment fails closed to background and axis");
    invalid = snapshot;
    invalid.voices[0].source = std::numeric_limits<float>::quiet_NaN();
    invalidCommands.clear();
    synth::ui::BuildGangedRandomLfoCommands(invalid, bounds, invalidCommands);
    Require(invalidCommands.size() == 2, "nonfinite value fails closed to background and axis");
    invalid = snapshot;
    invalid.sampleRate = 0.0;
    invalidCommands.clear();
    synth::ui::BuildGangedRandomLfoCommands(invalid, bounds, invalidCommands);
    Require(invalidCommands.size() == 2, "nonpositive sample rate fails closed to background and axis");
    invalid = snapshot;
    invalid.voices[0].waitingIncrement = std::numeric_limits<double>::denorm_min();
    invalidCommands.clear();
    synth::ui::BuildGangedRandomLfoCommands(invalid, bounds, invalidCommands);
    Require(invalidCommands.size() == 2, "nonfinite derived duration fails closed to background and axis");

    synth::GangedRandomLfoUiState<2> retainedState;
    retainedState.revision.store(1, std::memory_order_release);
    synth::ui::GangedRandomLfoVisualizer<2> visualizer(retainedState);
    visualizer.SetBounds(bounds);
    Require(visualizer.Draw().size() == 2, "unstable retained state fails closed");
}

void TestGangedRandomLfoBackgroundOptOut()
{
    // Create a valid snapshot for testing
    synth::GangedRandomLfoSnapshot<2> snapshot;
    snapshot.sampleRate = 48000.0;
    snapshot.roundElapsedSamples = 3.0;
    snapshot.voices[0] = {
        .source = 0.0f,
        .target = 1.0f,
        .output = 0.91f,
        .shape = 0.0f,
        .waitingIncrement = 0.25,
        .movingIncrement = 0.5,
        .color = synth::Color::Cyan,
    };
    snapshot.voices[1] = {
        .source = 1.0f,
        .target = 0.25f,
        .output = 0.02f,
        .shape = 1.0f,
        .waitingIncrement = 0.125,
        .movingIncrement = 0.25,
        .color = synth::Color::Orange,
    };

    const synth::ui::Bounds bounds{10.0f, 20.0f, 180.0f, 90.0f};

    // Build default (with background)
    std::vector<synth::ui::DrawCommand> defaultCommands;
    synth::ui::BuildGangedRandomLfoCommands(snapshot, bounds, defaultCommands);

    // Build with background opted out
    std::vector<synth::ui::DrawCommand> optedOutCommands;
    synth::ui::BuildGangedRandomLfoCommands(snapshot, bounds, optedOutCommands, false);

    // Default must have Fill and Line commands. Pin the full default stream
    // by construction rather than merely bounding it:
    // for this snapshot the present sample (3) lies strictly between 0 and the
    // shared duration (12 samples, from voice 1's ceil(8)+ceil(4)), so every
    // voice emits its maximal past-polyline + dashed-future-polylines + dot
    // set. Expected background/axis field values are derived by hand from
    // AppendBackgroundAndAxis (GangedRandomLfoVisualizer.hpp:57-71) against
    // nodeExtent {0,0,180,90} and its PlotBounds {4,4,172,82}.
    Require(defaultCommands[0].kind == synth::ui::DrawCommand::Kind::Fill, "first command is background fill");
    {
        const synth::ui::Bounds expectedFillBounds{0.0f, 0.0f, bounds.width, bounds.height};
        Require(std::memcmp(&defaultCommands[0].bounds, &expectedFillBounds, sizeof(synth::ui::Bounds)) == 0,
                "background fill covers the full node extent");
        Require(defaultCommands[0].color == synth::Color::Rgb(12, 14, 16),
                "background fill uses the expected panel color");
    }
    Require(defaultCommands[1].kind == synth::ui::DrawCommand::Kind::Line, "second command is axis line");
    {
        const synth::ui::Point expectedFrom{4.0f, 45.0f};
        const synth::ui::Point expectedTo{176.0f, 45.0f};
        Require(std::memcmp(&defaultCommands[1].from, &expectedFrom, sizeof(synth::ui::Point)) == 0,
                "axis line starts at the plot's left inset midline");
        Require(std::memcmp(&defaultCommands[1].to, &expectedTo, sizeof(synth::ui::Point)) == 0,
                "axis line ends at the plot's right inset midline");
        Require(defaultCommands[1].color == synth::Color::Rgb(42, 46, 48),
                "axis line uses the expected axis color");
        Require(defaultCommands[1].strokeWidth == 1.0f, "axis line uses the expected stroke width");
    }

    constexpr std::size_t kDashSegments = (synth::ui::GangedRandomLfoGeometry::kPathSegments + 1) / 2;
    constexpr std::size_t kCommandsPerVoice = 1 + kDashSegments + 1;
    constexpr std::size_t kVoiceCount = 2;
    Require(defaultCommands.size() == 2 + kVoiceCount * kCommandsPerVoice,
            "default stream command count is fully pinned by construction, not merely bounded");
    for (std::size_t voiceIndex = 0; voiceIndex < kVoiceCount; ++voiceIndex)
    {
        const std::size_t voiceStart = 2 + voiceIndex * kCommandsPerVoice;
        Require(defaultCommands[voiceStart].kind == synth::ui::DrawCommand::Kind::Polyline,
                "each voice's first trace command is the solid past polyline");
        for (std::size_t dashIndex = 0; dashIndex < kDashSegments; ++dashIndex)
        {
            Require(defaultCommands[voiceStart + 1 + dashIndex].kind == synth::ui::DrawCommand::Kind::Polyline,
                    "each voice's dashed future segments are polylines");
        }
        Require(defaultCommands[voiceStart + kCommandsPerVoice - 1].kind ==
                    synth::ui::DrawCommand::Kind::FillEllipse,
                "each voice's last trace command is the present-position dot");
    }

    // Opted out must have exactly 2 fewer commands (no Fill, no Line)
    Require(optedOutCommands.size() == defaultCommands.size() - 2,
            "opted-out stream has exactly two fewer commands (no background and no axis)");

    // Verify remaining commands match exactly (skip first two from default).
    // drawBackground does not affect nodeExtent or voice-trace generation, so
    // this element-wise comparison against the independently built opt-out
    // stream fully pins every field (points, strokeWidth, color, kind) of the
    // default stream's trace commands without duplicating the algorithm's
    // float geometry as literals in the test.
    Require(optedOutCommands.size() >= 2, "opted-out stream still has voice traces");
    for (std::size_t i = 0; i < optedOutCommands.size(); ++i)
    {
        const auto& optedCmd = optedOutCommands[i];
        const auto& defaultCmd = defaultCommands[i + 2];
        Require(optedCmd.kind == defaultCmd.kind, "opted-out command types match default (after background)");
        if (optedCmd.kind == synth::ui::DrawCommand::Kind::Polyline)
        {
            Require(optedCmd.points.size() == defaultCmd.points.size(),
                    "opted-out polyline has same point count");
            for (std::size_t pointIx = 0; pointIx < optedCmd.points.size(); ++pointIx)
            {
                Require(std::memcmp(&optedCmd.points[pointIx], &defaultCmd.points[pointIx],
                                     sizeof(synth::ui::Point)) == 0,
                        "opted-out polyline points are byte-identical to default");
            }
            Require(optedCmd.strokeWidth == defaultCmd.strokeWidth,
                    "opted-out polyline has same stroke width");
            Require(optedCmd.color == defaultCmd.color, "opted-out polyline has same color");
        }
        else if (optedCmd.kind == synth::ui::DrawCommand::Kind::FillEllipse)
        {
            Require(std::memcmp(&optedCmd.bounds, &defaultCmd.bounds, sizeof(synth::ui::Bounds)) == 0,
                    "opted-out dot bounds match default");
            Require(optedCmd.color == defaultCmd.color, "opted-out dot has same color");
        }
    }
}

void TestScopeWaveformCommandsAreNodeLocal()
{
    synth::ScopeWriter scope(1, 128);
    FillScopeWriter(scope, 1);
    const std::vector<synth::ui::WaveformLayerDrawState> layers{
        {.connected = true, .scopeColor = synth::Color::Red, .scope = &scope, .scopeChannel = 0},
    };
    const synth::ui::Bounds atOrigin{0.0f, 0.0f, 100.0f, 60.0f};
    const synth::ui::Bounds offset{250.0f, 180.0f, 100.0f, 60.0f};
    const auto a = synth::ui::BuildScopeWaveformCommands(layers, atOrigin, -1.0f, 1.0f, 64, true);
    const auto b = synth::ui::BuildScopeWaveformCommands(layers, offset, -1.0f, 1.0f, 64, true);
    Require(a.size() == b.size(), "the same node extent yields the same scope command count");
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        Require(std::memcmp(&a[i].bounds, &b[i].bounds, sizeof(synth::ui::Bounds)) == 0,
                "scope draw geometry is node-local: identical extents at different positions produce "
                "byte-identical command bounds");
        Require(a[i].points.size() == b[i].points.size(),
                "scope draw geometry is node-local: command point counts match");
        for (std::size_t pointIx = 0; pointIx < a[i].points.size(); ++pointIx)
        {
            Require(std::memcmp(&a[i].points[pointIx], &b[i].points[pointIx], sizeof(synth::ui::Point)) == 0,
                    "scope draw geometry is node-local: identical extents at different positions produce "
                    "byte-identical command points");
        }
    }
}

void TestEncoderDrawIsPositionIndependent()
{
    const auto state = RepresentativeEncoderState();
    const auto a = synth::ui::BuildEncoderDrawCommands(state, {0.0f, 0.0f, 90.0f, 90.0f});
    const auto b = synth::ui::BuildEncoderDrawCommands(state, {300.0f, 200.0f, 90.0f, 90.0f});
    Require(a.size() == b.size(), "the same node extent yields the same encoder command count");
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        Require(std::memcmp(&a[i].bounds, &b[i].bounds, sizeof(synth::ui::Bounds)) == 0,
                "encoder draw commands are identical regardless of node position");
        Require(std::memcmp(&a[i].from, &b[i].from, sizeof(synth::ui::Point)) == 0,
                "encoder line starts are identical regardless of node position");
        Require(std::memcmp(&a[i].to, &b[i].to, sizeof(synth::ui::Point)) == 0,
                "encoder line ends are identical regardless of node position");
        Require(a[i].points.size() == b[i].points.size(),
                "encoder polygon point counts match regardless of node position");
        for (std::size_t pointIx = 0; pointIx < a[i].points.size(); ++pointIx)
        {
            Require(std::memcmp(&a[i].points[pointIx], &b[i].points[pointIx], sizeof(synth::ui::Point)) == 0,
                    "encoder polygon points are identical regardless of node position");
        }
    }
}

void TestStandardModulatorVisualizersRemainPortable()
{
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 15,
        .numScenes = 1,
        .maxParameters = 16,
    });
    synth::StandardModulators<2> standard(group);
    standard.Register();
    standard.Prepare(48000.0);
    standard.Process();
    standard.PublishUiState();
    const synth::ui::Bounds bounds{0.0f, 0.0f, 96.0f, 96.0f};
    for (std::size_t random = 0; random < 4; ++random)
    {
        auto& visualizer = standard.RandomVisualizer(random);
        visualizer.SetBounds(bounds);
        Require(!visualizer.Draw().empty(), "standard random depth underlay is portable");
        Require(group.GetModulators().Metadata(random).visualizer == &visualizer,
                "standard random metadata retains portable underlay");
    }
    standard.ConstantVisualizer().SetBounds(bounds);
    standard.NoiseVisualizer().SetBounds(bounds);
    Require(!standard.ConstantVisualizer().Draw().empty(), "standard constant depth underlay is portable");
    Require(!standard.NoiseVisualizer().Draw().empty(), "standard noise depth underlay is portable");
    Require(group.GetModulators().Metadata(11).visualizer == &standard.ConstantVisualizer(),
            "standard constant metadata retains portable underlay");
    Require(group.GetModulators().Metadata(14).visualizer == &standard.NoiseVisualizer(),
            "standard noise metadata retains portable underlay");
}

void TestBraid4StandardModulationViewsRemainPortable()
{
    synth::ParameterManager manager;
    synth::MessageInBus uiBus(&manager);
    synth::MidiInstrumentConfig instrument;
    synth::RuntimeConfig config = synth_braid4::Braid4Core::Config();
    synth::AppContext context;
    context.parameterManager = &manager;
    context.uiBus = &uiBus;
    context.instrument = &instrument;
    context.config = &config;

    synth_braid4::Braid4Core core;
    core.Init(&context);
    core.PrepareToPlay(48000.0, 64);
    auto uiState = manager.CreateUIState();
    context.uiState = uiState.get();
    synth_braid4::Braid4UiSurface surface;
    surface.Attach(&context, &core);

    core.BankSlot()->HandlePress(4);
    manager.PopulateUIState(*uiState);
    const synth::ui::NodeTree quadTree = surface.BuildTree();
    Require(FindNodeById(quadTree, "braid4.encoder.0.visualizer") != nullptr,
            "Braid4 quad standard source has a portable underlay");
    Require(FindNodeById(quadTree, "braid4.encoder.4") != nullptr,
            "Braid4 quad application source 4 keeps its encoder");
    Require(FindNodeById(quadTree, "braid4.encoder.4.visualizer") == nullptr,
            "Braid4 quad application source 4 remains encoder-only");
    Require(FindNodeById(quadTree, "braid4.encoder.5.visualizer") == nullptr,
            "Braid4 quad application source 5 remains encoder-only");

    core.BankSlot()->SelectBank(core.MatrixBank());
    core.BankSlot()->HandlePress(0);
    manager.PopulateUIState(*uiState);
    const synth::ui::NodeTree monoTree = surface.BuildTree();
    Require(FindNodeById(monoTree, "braid4.encoder.0.visualizer") != nullptr,
            "Braid4 mono standard random source has a portable underlay");
    // The disconnected position used to be left out of the tree entirely. Now
    // that the encoder region is a resolver-driven grid, dropping a cell would
    // widen its neighbours and move every encoder after it, so the cell keeps
    // its place and is inert instead: nothing painted, nothing dispatched.
    const synth::ui::Node* disconnected = FindNodeById(monoTree, "braid4.encoder.11");
    Require(disconnected != nullptr,
            "Braid4 mono disconnected constant position keeps its grid cell");
    Require(disconnected->drawCommands.empty(),
            "Braid4 mono disconnected constant position paints nothing");
    Require(!disconnected->pointerDragAction.has_value() && !disconnected->doubleClickAction.has_value() &&
                !disconnected->action.has_value(),
            "Braid4 mono disconnected constant position dispatches nothing");
    Require(FindNodeById(monoTree, "braid4.encoder.11.visualizer") == nullptr,
            "Braid4 mono disconnected constant position has no visualizer");
    // The two views hide different cells — 11 is inert in the mono view and
    // live in the quad view — so if an inert cell were dropped from the tree
    // instead of held in place, the cells sharing its row (8, 9, 10) would
    // widen and every later cell would move. Comparing all sixteen catches
    // both, where comparing one in another row could not.
    for (std::size_t encoderIx = 0; encoderIx < synth_braid4::Braid4EncoderGridLayout::kEncoderCount;
         ++encoderIx)
    {
        const std::string encoderId = synth_braid4::Braid4NodeIds::Encoder(encoderIx);
        const synth::ui::Node* mono = FindNodeById(monoTree, encoderId);
        const synth::ui::Node* quad = FindNodeById(quadTree, encoderId);
        Require(mono != nullptr && quad != nullptr,
                ("both views keep encoder cell " + encoderId).c_str());
        Require(mono->bounds.x == quad->bounds.x && mono->bounds.y == quad->bounds.y &&
                    mono->bounds.width == quad->bounds.width &&
                    mono->bounds.height == quad->bounds.height,
                ("inert cells hold their place, so " + encoderId + " never moves").c_str());
    }
    Require(!core.MonoGroup()->GetModulators().Metadata(11).connected,
            "Braid4 mono constant source stays disconnected");
    Require(core.MonoGroup()->GetModulators().Metadata(11).visualizer == nullptr,
            "Braid4 mono constant source has no alias visualizer");
}

}  // namespace

static void TestContainersNestToArbitraryDepth()
{
    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.Section("sect", {}, [](synth::ui::Builder& b) {
        b.ScrollArea("scroll", {}, [](synth::ui::Builder& b) {
            b.Row("row", {}, [](synth::ui::Builder& b) { b.Label("leaf", "hello", {}); });
        });
    });
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(FindNode(tree, "root").children[0].value == "sect", "root holds the section");
    Require(FindNode(tree, "sect").kind == synth::ui::NodeKind::Section, "sect is a Section");
    Require(FindNode(tree, "sect").children[0].value == "scroll", "section holds the scroll area");
    Require(FindNode(tree, "scroll").children[0].value == "row", "scroll area holds the row");
    Require(FindNode(tree, "row").children[0].value == "leaf", "row holds the leaf");
}

// A reusable component is an ordinary callable. Nothing else.
//
struct CaptionedRow {
    std::string id, caption;
    void operator()(synth::ui::Builder& b) const {
        b.Row(id, {}, [this](synth::ui::Builder& b) {
            b.Label(id + ".caption", caption, {});
            b.Button(id + ".action", "Go", synth::ui::Action::Named("go"), {});
        });
    }
};

static void TestComponentsComposeComponents()
{
    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.Column("col", {}, [](synth::ui::Builder& b) {
        CaptionedRow{"first", "First"}(b);
        CaptionedRow{"second", "Second"}(b);
    });
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(FindNode(tree, "col").children.size() == 2, "column holds both rows");
    Require(FindNode(tree, "first.caption").text == "First", "each invocation emits in place");
    Require(FindNode(tree, "first").children.size() == 2, "with distinct stable ids");
}

static void TestSpliceGraftsWithoutNestedRoot()
{
    synth::ui::Builder inner;
    inner.Root("inner.root", {0.0f, 0.0f, 100.0f, 50.0f});
    inner.Label("inner.label", "spliced", {});

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Section("host", {}, [&inner](synth::ui::Builder& b) { b.Splice(inner.Build({0.0f, 0.0f, 100.0f, 50.0f})); });

    const synth::ui::NodeTree tree = outer.Build({0.0f, 0.0f, 400.0f, 300.0f});
    std::size_t roots = 0;
    for (const auto& n : tree.nodes) { if (n.kind == synth::ui::NodeKind::Root) ++roots; }
    Require(roots == 1, "exactly one Root survives the splice");
    Require(FindNode(tree, "host").children[0].value == "inner.label",
            "the spliced root's children become the host's children");
}

static void TestRootlessSpliceAttachesForestRoots()
{
    synth::ui::Subtree browser;
    {
        synth::ui::Node row;
        row.id = synth::ui::NodeId("browser.row.0");
        row.kind = synth::ui::NodeKind::Row;
        row.children.push_back(synth::ui::NodeId("browser.row.0.label"));
        synth::ui::Node label;
        label.id = synth::ui::NodeId("browser.row.0.label");
        label.kind = synth::ui::NodeKind::Label;
        label.text = "patch";
        browser.tree.nodes.push_back(std::move(row));
        browser.tree.nodes.push_back(std::move(label));
    }

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Section("file.browser", {}, [&browser](synth::ui::Builder& b) {
        b.Splice(std::move(browser));
    });

    const synth::ui::NodeTree tree = outer.Build({0.0f, 0.0f, 400.0f, 300.0f});
    std::size_t roots = 0;
    for (const auto& n : tree.nodes) {
        if (n.kind == synth::ui::NodeKind::Root) {
            ++roots;
        }
    }
    Require(roots == 1, "exactly one Root survives a rootless splice");
    Require(FindNode(tree, "file.browser").children.size() == 1 &&
                FindNode(tree, "file.browser").children[0].value == "browser.row.0",
            "the spliced forest root is a descendant of the splice point");
    Require(FindNode(tree, "browser.row.0").children[0].value == "browser.row.0.label",
            "nested children inside the spliced forest stay linked");
}

static void TestRootlessScopeMarkerNeverEatsAProducerNode()
{
    // The rootless scope is a builder-side handle. It is dropped by index, so a
    // producer that happens to name a node after it keeps that node -- matching
    // by id would delete it without a word.
    synth::ui::Builder rows;
    rows.Rootless();
    rows.Label("synth.ui.rootless-scope", "a producer may use any id it likes", {});
    rows.Label("second", "and still gets its siblings", {});
    const synth::ui::Subtree subtree = rows.BuildSubtree();
    Require(subtree.tree.nodes.size() == 2, "only the scope marker itself is dropped");
    Require(subtree.tree.nodes.front().id.value == "synth.ui.rootless-scope" &&
                subtree.tree.nodes.front().text == "a producer may use any id it likes",
            "a producer node sharing the scope marker's id survives intact");

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Section("host", {}, [&subtree](synth::ui::Builder& b) { b.Splice(subtree); });
    const synth::ui::NodeTree tree = outer.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(FindNode(tree, "host").children.size() == 2,
            "both forest roots of a rootless subtree attach to the splice point");
}

static void TestSpliceMergesLayoutDeclarations()
{
    synth::ui::LayoutOptions opts;
    opts.padding = 3.0f;
    opts.formGrid = true;

    synth::ui::Builder inner;
    inner.Root("inner.root", {0.0f, 0.0f, 100.0f, 50.0f});
    inner.Column("form", opts, [](synth::ui::Builder& b) {
        b.Label("field", "x", {});
    });

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Section("host", {}, [&inner](synth::ui::Builder& b) {
        b.Splice(inner.BuildSubtree());
    });

    const synth::ui::Subtree host = outer.BuildSubtree();
    Require(host.layout.count("form") == 1, "spliced layout key is registered on the host");
    Require(host.layout.at("form").formGrid, "formGrid survives the splice");
    Require(host.layout.at("form").padding == 3.0f, "padding survives the splice");
}

static void TestConstructionExpressesFullControlState()
{
    synth::ui::ControlStyle style;
    style.color = synth::Color::Rgb(0, 200, 0);
    style.textStyle = synth::ui::TextStyle{16.0f, synth::Color::Rgb(255,255,255),
                                           synth::ui::TextAlign::Center};
    style.selected = true;
    style.enabled = false;

    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.Button("green", "Go", synth::ui::Action::Named("go"), style);

    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const synth::ui::Node& n = FindNode(tree, "green");
    // Color stores channels as r/g/b (plan text said green; the field is g).
    //
    Require(n.color.has_value() && n.color->g == 200, "colour reaches the node record");
    Require(n.textStyle.has_value(), "text style reaches the node record");
    Require(n.selected && !n.enabled, "selected and enabled reach the node record");
}

static void TestContainerConstructionCarriesAppearance()
{
    synth::ui::ControlStyle rootStyle;
    rootStyle.color = synth::Color::Rgb(12, 13, 14);
    rootStyle.borderColor = synth::Color::Rgb(90, 91, 92);
    rootStyle.borderWidth = 3.0f;
    rootStyle.cornerRadius = 7.0f;

    synth::ui::LayoutOptions panelLayout;
    panelLayout.main = synth::ui::Extent::Px(92.0f);
    panelLayout.cross = synth::ui::Extent::Px(120.0f);
    panelLayout.padding = 10.0f;
    panelLayout.gap = 8.0f;

    synth::ui::ControlStyle panelStyle;
    panelStyle.color = synth::Color::Rgb(20, 30, 40);
    panelStyle.borderColor = synth::Color::Rgb(80, 90, 100);
    panelStyle.borderWidth = 2.0f;
    panelStyle.cornerRadius = 6.0f;
    panelStyle.layout.main = synth::ui::Extent::Px(999.0f);
    panelStyle.layout.cross = synth::ui::Extent::Px(999.0f);

    synth::ui::ControlStyle rowStyle;
    rowStyle.color = synth::Color::Rgb(50, 60, 70);
    rowStyle.borderColor = synth::Color::Rgb(100, 110, 120);
    rowStyle.borderWidth = 1.5f;
    rowStyle.cornerRadius = 4.0f;
    synth::ui::LayoutOptions rowLayout;
    rowLayout.main = synth::ui::Extent::Px(24.0f);
    rowLayout.cross = synth::ui::Extent::Weight(1.0f);
    rowStyle.layout.main = synth::ui::Extent::Px(999.0f);

    synth::ui::ControlStyle scrollStyle;
    scrollStyle.color = synth::Color::Rgb(70, 80, 90);
    scrollStyle.borderColor = synth::Color::Rgb(130, 140, 150);
    scrollStyle.borderWidth = 1.0f;
    scrollStyle.cornerRadius = 3.0f;
    synth::ui::LayoutOptions scrollLayout;
    scrollLayout.main = synth::ui::Extent::Weight(1.0f);
    scrollLayout.cross = synth::ui::Extent::Weight(1.0f);
    scrollStyle.layout.main = synth::ui::Extent::Px(999.0f);

    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 200.0f, 120.0f}, rootStyle);
    builder.Section("panel", panelLayout, panelStyle, [&rowStyle, &rowLayout, &scrollStyle, &scrollLayout](synth::ui::Builder& panel) {
        panel.Label("top", "Top", {});
        panel.Row("row", rowLayout, rowStyle, [](synth::ui::Builder& row) {
            row.Label("row.child", "Row", {});
        });
        panel.ScrollArea("scroll", scrollLayout, scrollStyle, [](synth::ui::Builder& scroll) {
            scroll.Label("scroll.child", "Scroll", {});
        });
    });

    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 200.0f, 120.0f});
    const synth::ui::Node& root = FindNode(tree, "root");
    const synth::ui::Node& panel = FindNode(tree, "panel");
    const synth::ui::Node& row = FindNode(tree, "row");
    const synth::ui::Node& scroll = FindNode(tree, "scroll");

    Require(root.color == rootStyle.color && root.borderColor == rootStyle.borderColor,
            "Root carries its own fill and border colour");
    Require(root.borderWidth == rootStyle.borderWidth && root.cornerRadius == rootStyle.cornerRadius,
            "Root carries its own border width and radius");
    Require(panel.color == panelStyle.color && panel.borderColor == panelStyle.borderColor,
            "Section carries its own fill and border colour");
    Require(panel.borderWidth == panelStyle.borderWidth && panel.cornerRadius == panelStyle.cornerRadius,
            "Section carries its own border width and radius");
    RequireNear(panel.bounds.height, 92.0f, 0.0001f,
                "Section uses the explicit LayoutOptions argument rather than ControlStyle::layout");
    Require(row.color == rowStyle.color && row.borderColor == rowStyle.borderColor,
            "Row carries its own fill and border colour");
    Require(row.borderWidth == rowStyle.borderWidth && row.cornerRadius == rowStyle.cornerRadius,
            "Row carries its own border width and radius");
    Require(scroll.color == scrollStyle.color && scroll.borderColor == scrollStyle.borderColor,
            "ScrollArea carries its own fill and border colour");
    Require(scroll.borderWidth == scrollStyle.borderWidth && scroll.cornerRadius == scrollStyle.cornerRadius,
            "ScrollArea carries its own border width and radius");
}

static void TestUnstyledNodesCarryNothing()
{
    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.Button("plain", "Plain", synth::ui::Action::Named("go"), {});
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const synth::ui::Node& n = FindNode(tree, "plain");
    Require(!n.color.has_value() && !n.textStyle.has_value(),
            "an unstyled control carries nothing, so each backend uses its default look");
}

static void TestCaptionIsAnEmittedLabelNodeNotAField()
{
    synth::ui::ControlStyle style;
    style.caption = "Output device";
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;

    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.Column("form", grid, [&style](synth::ui::Builder& b) {
        b.ComboBox("device", {}, "", synth::ui::Action::Named("pick"), style);
    });
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(FindNode(tree, "device.caption").kind == synth::ui::NodeKind::Label,
            "a caption is an ordinary Label node in the control's row");
    Require(FindNode(tree, "device.caption").text == "Output device", "carrying its text");
    Require(FindNode(tree, "device").label.empty(),
            "the caption does not route through ComboBox::label");
    Require(FindNode(tree, "device.row").kind == synth::ui::NodeKind::Row,
            "captioned controls are wrapped in an implicit .row");
    Require(FindNode(tree, "device.row").children.size() == 2 &&
                FindNode(tree, "device.row").children[0].value == "device.caption" &&
                FindNode(tree, "device.row").children[1].value == "device",
            "the .row children are caption then control");
    Require(FindNode(tree, "form").children.size() == 1 &&
                FindNode(tree, "form").children[0].value == "device.row",
            "the author's container holds the .row, not the bare control");
}

static void TestCaptionPlacementDefaultIsBeforeAndUnchanged()
{
    Require(synth::ui::ControlStyle{}.captionPlacement == synth::ui::CaptionPlacement::Before,
            "a default-constructed ControlStyle places the caption Before the control");

    synth::ui::ControlStyle style;
    style.caption = "Output device";

    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.ComboBox("device", {}, "", synth::ui::Action::Named("pick"), style);
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});

    Require(FindNode(tree, "device.row").children.size() == 2 &&
                FindNode(tree, "device.row").children[0].value == "device.caption" &&
                FindNode(tree, "device.row").children[1].value == "device",
            "leaving captionPlacement unset emits the caption before the control, exactly as "
            "before this change");
}

static void TestCaptionPlacementAfterEmitsCaptionAfterControl()
{
    synth::ui::ControlStyle style;
    style.caption = "Output device";
    style.captionPlacement = synth::ui::CaptionPlacement::After;

    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    builder.ComboBox("device", {}, "", synth::ui::Action::Named("pick"), style);
    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});

    Require(FindNode(tree, "device.row").children.size() == 2 &&
                FindNode(tree, "device.row").children[0].value == "device" &&
                FindNode(tree, "device.row").children[1].value == "device.caption",
            "captionPlacement After emits the control then the caption in the same .row");
    Require(FindNode(tree, "device.caption").kind == synth::ui::NodeKind::Label,
            "the trailing caption is still an ordinary Label node");
    Require(FindNode(tree, "device.caption").text == "Output device",
            "the trailing caption's id derivation and text sync match the leading form");
}

static void TestComboBoxAcceptsRuntimeOptionVectors()
{
    synth::ui::Builder builder;
    builder.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    const std::vector<synth::ui::ControlOption> options = {{"system_default", "System Default"},
                                                           {"speakers", "Speakers"}};
    builder.ComboBox("device", options, "speakers", synth::ui::Action::Named("pick"), {});

    const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const synth::ui::Node& combo = FindNode(tree, "device");
    Require(combo.options.size() == 2 && combo.options[1].id == "speakers" &&
                combo.options[1].label == "Speakers",
            "ComboBox carries runtime-provided option vectors");
}

static void TestSyncPageAlignsThroughTheFormGrid()
{
    synth::runtime_ui::SyncPageSnapshot snapshot;
    snapshot.staged = {.sendClock = true,
                       .receiveClock = false,
                       .sendTransport = true,
                       .receiveTransport = false,
                       .ppqn = 96};
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildSyncPageTree(snapshot, {0.0f, 0.0f, 900.0f, 560.0f});

    const synth::ui::Node& back = FindNode(tree, synth::runtime_ui::NodeIds::kSyncBack);
    RequireNear(back.bounds.width,
                synth::runtime_ui::Layout::kBackButtonWidth,
                0.0001f,
                "Sync Back button keeps the recovered compact page width");
    Require(back.color.has_value() && *back.color == synth::pagestyle::kDefaultButton,
            "Sync Back button carries the default page button colour");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.sync.form", 0)),
            "every Sync label starts at the same form-grid x-offset");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.sync.form", 1)),
            "every Sync control starts at the same form-grid x-offset");
    Require(AllEqual(ColumnWidthsOf(tree, "runtime.sync.form", 0)),
            "every Sync caption cell has the same form-grid width");
    Require(SharedColumnXOffsetOf(tree, "runtime.sync.form", 1) >=
                MaxRightEdgeOfColumn(tree, "runtime.sync.form", 0) + synth::ui::kSpacing.labelGap,
            "the Sync control column clears the widest caption cell");
    Require(FindNode(tree, std::string(synth::runtime_ui::NodeIds::kSyncPpqn) + ".caption").text ==
                "PPQN (1-960)",
            "the Sync PPQN field keeps its user-facing caption outside the text field");
    const synth::ui::Node& ppqnCaption =
        FindNode(tree, std::string(synth::runtime_ui::NodeIds::kSyncPpqn) + ".caption");
    Require(ppqnCaption.textStyle.has_value() &&
                TextStyleMatches(*ppqnCaption.textStyle, synth::pagestyle::kDefaultTextStyle),
            "the Sync PPQN caption carries the page text style");
}

static void TestSyncPageFitsWithinTheRuntimeRoot()
{
    synth::runtime_ui::SyncPageSnapshot snapshot;
    snapshot.validationText = "PPQN must be in the range 1 to 960";
    snapshot.warningText = "96 PPQN is nonstandard";
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildSyncPageTree(snapshot, {0.0f, 0.0f, 640.0f, 480.0f});

    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id.value.rfind("runtime.sync.", 0) == 0 &&
            node.id.value != synth::runtime_ui::NodeIds::kSyncRoot)
        {
            RequireNodeContainedInParent(tree, node.id.value);
        }
    }
}

static void TestAudioSelectorsAreCaptionedWhileADeviceIsSelected()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Output"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    snapshot.selectedOutputId = "Built-in Output";
    snapshot.selectedInputId = "Built-in Microphone";
    snapshot.showInputCombo = true;
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildAudioPageTree(snapshot, {0.0f, 0.0f, 900.0f, 560.0f});

    const synth::ui::Node& back = FindNode(tree, synth::runtime_ui::NodeIds::kAudioBack);
    RequireNear(back.bounds.width,
                synth::runtime_ui::Layout::kBackButtonWidth,
                0.0001f,
                "Audio Back button keeps the recovered compact page width");
    Require(FindNode(tree, std::string(synth::runtime_ui::NodeIds::kAudioOutput) + ".caption").text ==
                "Output device",
            "the output selector shows a visible caption while a device is selected");
    Require(FindNode(tree, std::string(synth::runtime_ui::NodeIds::kAudioInput) + ".caption").text ==
                "Input device",
            "the input selector shows a visible caption while a device is selected");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kAudioOutput).label.empty(),
            "the output selector caption does not route through ComboBox::label");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kAudioInput).label.empty(),
            "the input selector caption does not route through ComboBox::label");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.audio.form", 0)),
            "every Audio label starts at the same form-grid x-offset");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.audio.form", 1)),
            "every Audio control starts at the same form-grid x-offset");
    Require(AllEqual(ColumnWidthsOf(tree, "runtime.audio.form", 0)),
            "every Audio caption cell has the same form-grid width");
    Require(SharedColumnXOffsetOf(tree, "runtime.audio.form", 1) >=
                MaxRightEdgeOfColumn(tree, "runtime.audio.form", 0) + synth::ui::kSpacing.labelGap,
            "the Audio control column clears the widest caption cell");
    const synth::ui::Node& outputCaption =
        FindNode(tree, std::string(synth::runtime_ui::NodeIds::kAudioOutput) + ".caption");
    Require(outputCaption.textStyle.has_value() &&
                TextStyleMatches(*outputCaption.textStyle, synth::pagestyle::kDefaultTextStyle),
            "the Audio output caption carries the page text style while selected");
}

static void TestHiddenInputSelectorLeavesNoOrphanedCaption()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Output"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    snapshot.selectedOutputId = "Built-in Output";
    snapshot.selectedInputId = "Built-in Microphone";
    snapshot.showInputCombo = false;
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildAudioPageTree(snapshot, {0.0f, 0.0f, 900.0f, 560.0f});

    Require(!HasNode(tree, (std::string(synth::runtime_ui::NodeIds::kAudioInput) + ".caption").c_str()),
            "hidden input selector leaves no orphaned caption");
    Require(!HasNode(tree, synth::runtime_ui::NodeIds::kAudioInput),
            "hidden input selector leaves no orphaned control");
    Require(!HasNode(tree, synth::runtime_ui::NodeIds::kAudioInputRetry),
            "a page without an input selector offers no input retry");
}

// `Retry Input` exists only while browser capture is offline, and it is a
// form row like every other Audio control -- an uncaptioned button would sit
// outside the form grid's label/control columns and break the shared offsets
// the selectors above it are aligned to.
static void TestOfflineInputCaptureOffersACaptionedRetryRow()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = {{"system_default", "System Default"}};
    snapshot.inputOptions = {{"system_default", "System Default"}};
    snapshot.showInputCombo = true;
    snapshot.showInputRetry = true;
    snapshot.statusLineText = "Input requested 4 / active 0 - microphone permission denied";
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildAudioPageTree(snapshot, {0.0f, 0.0f, 900.0f, 560.0f});

    const synth::ui::Node& retry = FindNode(tree, synth::runtime_ui::NodeIds::kAudioInputRetry);
    Require(retry.kind == synth::ui::NodeKind::Button, "Retry Input is a button");
    Require(retry.label == "Retry Input", "the retry button names the action it performs");
    Require(retry.action.has_value() &&
                retry.action->name == std::string("audio-input-retry") &&
                retry.action->value.empty(),
            "the retry button dispatches the plan's host-neutral retry action name");
    Require(std::string(synth::runtime_ui::Actions::kAudioInputRetry) == "audio-input-retry",
            "the portable retry action keeps the interface's exact action name");
    Require(FindNode(tree, std::string(synth::runtime_ui::NodeIds::kAudioInputRetry) + ".caption").text ==
                "Input capture",
            "the retry row carries a caption cell like the selectors above it");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.audio.form", 0)),
            "the retry row shares the Audio caption column offset");
    Require(AllEqual(ColumnXOffsetsOf(tree, "runtime.audio.form", 1)),
            "the retry row shares the Audio control column offset");
    Require(AllEqual(ColumnWidthsOf(tree, "runtime.audio.form", 0)),
            "the retry row shares the Audio caption column width");

    // The retry button is a captioned FormButton, which declares Intrinsic
    // controlWidth -- it must be sized to its own caption rather than
    // stretched across the control column like the device selector next to it,
    // while still sharing that column's left edge (already covered above by
    // the column-1 AllEqual x-offset check).
    const synth::ui::Node& inputSelector = FindNode(tree, synth::runtime_ui::NodeIds::kAudioInput);
    Require(retry.bounds.width < inputSelector.bounds.width,
            "the retry button is narrower than the full-width input device selector");
    Require(std::fabs(retry.bounds.x - inputSelector.bounds.x) <= 0.0001f,
            "the retry button's left edge matches the input device selector's left edge");
}

static void TestLiveInputCaptureHidesTheRetryRow()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = {{"system_default", "System Default"}};
    snapshot.inputOptions = {{"system_default", "System Default"}};
    snapshot.showInputCombo = true;
    snapshot.showInputRetry = false;
    snapshot.statusLineText = "Input requested 4 / active 4";
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildAudioPageTree(snapshot, {0.0f, 0.0f, 900.0f, 560.0f});

    Require(!HasNode(tree, synth::runtime_ui::NodeIds::kAudioInputRetry),
            "live capture leaves no orphaned retry control");
    Require(!HasNode(tree, (std::string(synth::runtime_ui::NodeIds::kAudioInputRetry) + ".caption").c_str()),
            "live capture leaves no orphaned retry caption");
    Require(HasNode(tree, synth::runtime_ui::NodeIds::kAudioInput),
            "hiding retry does not hide the input selector");
}

// A verbatim copy of BuildAudioPageTree's body as it stood before the
// app-supplied-section change -- the pre-change tree the spec requires the
// default path to stay byte-identical to. Any drift the two-pass
// implementation introduces (node count, ids, kinds, or resolved bounds)
// shows up as a mismatch against this independent reference.
synth::ui::NodeTree ReferenceAudioPageTreeBeforeAppSection(
    const synth::runtime_ui::AudioPageSnapshot& snapshot, synth::ui::Bounds area)
{
    using namespace synth::runtime_ui;
    synth::ui::Builder builder;
    builder.Root(NodeIds::kAudioRoot, area);
    builder.Button(NodeIds::kAudioBack, "Back", synth::ui::Action::Named(Actions::kAudioBack),
                   PageControls::BackButton());
    builder.Column(NodeIds::kAudioForm, PageControls::FormGridLayout(), [&](synth::ui::Builder& form) {
        form.ComboBox(NodeIds::kAudioOutput,
                      PageControls::ControlOptionsFor(snapshot.outputOptions),
                      snapshot.selectedOutputId,
                      synth::ui::Action::Named(Actions::kAudioOutputSelect),
                      PageControls::Field("Output device"));
        if (snapshot.showInputCombo)
        {
            form.ComboBox(NodeIds::kAudioInput,
                          PageControls::ControlOptionsFor(snapshot.inputOptions),
                          snapshot.selectedInputId,
                          synth::ui::Action::Named(Actions::kAudioInputSelect),
                          PageControls::Field("Input device"));
        }
        if (snapshot.showInputRetry)
        {
            form.Button(NodeIds::kAudioInputRetry,
                        "Retry Input",
                        synth::ui::Action::Named(Actions::kAudioInputRetry),
                        PageControls::FormButton("Input capture"));
        }
    });
    builder.ScrollArea(
        NodeIds::kAudioStatus, PageControls::StatusStackLayout(0.0f), [&](synth::ui::Builder& status) {
            if (!snapshot.deviceLineText.empty())
            {
                status.Label(NodeIds::kAudioDeviceLine, snapshot.deviceLineText, PageControls::MutedText());
            }
            if (!snapshot.statusLineText.empty())
            {
                status.StatusText(NodeIds::kAudioStatusLine,
                                  snapshot.statusLineText,
                                  PageControls::MutedText());
            }
        });
    return builder.Build(area);
}

static void TestAudioPageWithNoAppSectionIsByteIdenticalToBeforeTheChange()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = {{"system_default", "System Default"}};
    snapshot.inputOptions = {{"Built-in Microphone", "Built-in Microphone"}};
    snapshot.selectedOutputId = "system_default";
    snapshot.selectedInputId = "Built-in Microphone";
    snapshot.showInputCombo = true;
    snapshot.showInputRetry = true;
    snapshot.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    snapshot.statusLineText = "Audio running";
    Require(!snapshot.appSection, "a default-constructed snapshot supplies no app section builder");
    const synth::ui::Bounds area{0.0f, 0.0f, 900.0f, 560.0f};

    const synth::ui::NodeTree actual = synth::runtime_ui::BuildAudioPageTree(snapshot, area);
    const synth::ui::NodeTree expected = ReferenceAudioPageTreeBeforeAppSection(snapshot, area);

    Require(actual.nodes.size() == expected.nodes.size(),
            "an unset app section builder adds no nodes to the pre-change tree");
    for (std::size_t ix = 0; ix < expected.nodes.size(); ++ix)
    {
        const synth::ui::Node& a = actual.nodes[ix];
        const synth::ui::Node& e = expected.nodes[ix];
        Require(a.id == e.id, "node id order matches the pre-change tree exactly");
        Require(a.kind == e.kind, "node kind matches the pre-change tree exactly");
        RequireNear(a.bounds.x, e.bounds.x, 0.0001f, "node x matches the pre-change tree exactly");
        RequireNear(a.bounds.y, e.bounds.y, 0.0001f, "node y matches the pre-change tree exactly");
        RequireNear(a.bounds.width, e.bounds.width, 0.0001f, "node width matches the pre-change tree exactly");
        RequireNear(a.bounds.height, e.bounds.height, 0.0001f, "node height matches the pre-change tree exactly");
    }
    Require(!HasNode(actual, synth::runtime_ui::NodeIds::kAudioAppSection),
            "no app section mount node exists when no builder is supplied");
}

static void TestAudioPageAppendsSuppliedSectionBeneathDeviceRowsWithinRemainingArea()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = {{"system_default", "System Default"}};
    snapshot.inputOptions = {{"Built-in Microphone", "Built-in Microphone"}};
    snapshot.selectedOutputId = "system_default";
    snapshot.selectedInputId = "Built-in Microphone";
    snapshot.showInputCombo = true;
    snapshot.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    snapshot.statusLineText = "Audio running";
    const synth::ui::Bounds area{0.0f, 0.0f, 900.0f, 560.0f};

    std::optional<synth::ui::Bounds> handedBounds;
    snapshot.appSection = [&handedBounds](synth::ui::Bounds bounds) {
        handedBounds = bounds;
        // ui::Subtree, built the Rootless()/BuildSubtree() way --
        // the same idiom BuildPatchBrowserSubtree/BuildPatchVersionsSubtree
        // use (RuntimePages.hpp:1126, 1178) -- not a Root+Build()
        // NodeTree, so the app's own layout declarations reach the splice.
        synth::ui::Builder appBuilder;
        appBuilder.Rootless();
        appBuilder.Label("app.audio.section.label", "App section", {});
        return appBuilder.BuildSubtree();
    };

    const synth::ui::NodeTree tree = synth::runtime_ui::BuildAudioPageTree(snapshot, area);

    // The remaining area independently: the default-path tree's kAudioStatus
    // region, which TestEveryRebuiltPageAbsorbsAtTheSmallestDeclaredSurface's
    // RequireRegionAbsorbsTheDifference already establishes as the page's
    // remaining area (it absorbs the whole difference between surface
    // heights).
    synth::runtime_ui::AudioPageSnapshot withoutBuilder = snapshot;
    withoutBuilder.appSection = {};
    const synth::ui::NodeTree defaultTree = synth::runtime_ui::BuildAudioPageTree(withoutBuilder, area);
    const synth::ui::Node& statusWithoutBuilder =
        FindNode(defaultTree, synth::runtime_ui::NodeIds::kAudioStatus);

    Require(handedBounds.has_value(), "the supplied builder is invoked while resolving the page");
    RequireNear(handedBounds->x, statusWithoutBuilder.bounds.x, 0.0001f,
                "the handed bounds match the page's remaining area (x)");
    RequireNear(handedBounds->y, statusWithoutBuilder.bounds.y, 0.0001f,
                "the handed bounds match the page's remaining area (y)");
    RequireNear(handedBounds->width, statusWithoutBuilder.bounds.width, 0.0001f,
                "the handed bounds match the page's remaining area (width)");
    RequireNear(handedBounds->height, statusWithoutBuilder.bounds.height, 0.0001f,
                "the handed bounds match the page's remaining area (height)");

    const synth::ui::Node& appSection =
        FindNode(tree, synth::runtime_ui::NodeIds::kAudioAppSection);
    Require(!appSection.children.empty(), "the app-supplied nodes attach beneath the audio page");
    const synth::ui::Node& appLabel = FindNode(tree, "app.audio.section.label");
    Require(appLabel.text == "App section", "the app-built node survives the splice");

    const synth::ui::Node& statusWithSection =
        FindNode(tree, synth::runtime_ui::NodeIds::kAudioStatus);
    Require(!statusWithSection.children.empty() &&
                statusWithSection.children.back().value == synth::runtime_ui::NodeIds::kAudioAppSection,
            "the app section is appended after the existing device/status lines");

    Require(appSection.bounds.x >= 0.0f && appSection.bounds.y >= 0.0f,
            "the app section stays within the handed area's top-left corner");
    RequireNear(appSection.bounds.width, handedBounds->width, 0.0001f,
                "the app section is confined to the width of the area handed to the builder");
    Require(appSection.bounds.height <= handedBounds->height + 0.0001f,
            "the app section does not exceed the height of the area handed to the builder");
    Require(appSection.bounds.y + appSection.bounds.height <= handedBounds->height + 0.0001f,
            "the app section's whole extent, not just its top-left corner, is confined to the handed area");

    // Not "device rows": the form's ComboBox rows are untouched by the append.
    Require(HasNode(tree, synth::runtime_ui::NodeIds::kAudioOutput),
            "device rows are unaffected by an appended section");
    RequireNear(FindNode(tree, synth::runtime_ui::NodeIds::kAudioForm).bounds.height,
                FindNode(defaultTree, synth::runtime_ui::NodeIds::kAudioForm).bounds.height,
                0.0001f,
                "the form's layout is unchanged by an appended section");
}

// This test closes a gap: Splice(NodeTree) carries no layout map (see Splice(NodeTree) in PortableUIBuilders.hpp --
// it forwards to Splice(Subtree{tree, {}, {}})), so a nested Row/Column the
// app declares with a weighted extent or explicit padding would silently
// re-resolve with LayoutOptions{} defaults once the page's outer Build(area)
// walked the spliced nodes from the root. TestAudioPageAppendsSuppliedSection...
// above never nests a container inside the app section, so it could not have
// caught that. This test declares a Row with non-default padding and two
// children weighted 3:1, and asserts the RESOLVED bounds carry those
// numbers rather than the default padding (kSpacing.padding == 12.0f) and an
// even 1:1 split.
static void TestAudioPageAppSectionNestedLayoutSurvivesTheSplice()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = {{"system_default", "System Default"}};
    snapshot.selectedOutputId = "system_default";
    snapshot.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    snapshot.statusLineText = "Audio running";
    const synth::ui::Bounds area{0.0f, 0.0f, 900.0f, 560.0f};

    constexpr float kRowPadding = 40.0f;  // default LayoutOptions padding is kSpacing.padding == 12.0f
    constexpr float kHeavyWeight = 3.0f;
    constexpr float kLightWeight = 1.0f;

    snapshot.appSection = [](synth::ui::Bounds) {
        synth::ui::Builder appBuilder;
        appBuilder.Rootless();
        synth::ui::LayoutOptions rowLayout;
        rowLayout.padding = kRowPadding;
        appBuilder.Row("app.audio.section.row", rowLayout, [](synth::ui::Builder& row) {
            synth::ui::ControlStyle heavy;
            heavy.layout.main = synth::ui::Extent::Weight(kHeavyWeight);
            row.Label("app.audio.section.heavy", "Heavy", heavy);
            synth::ui::ControlStyle light;
            light.layout.main = synth::ui::Extent::Weight(kLightWeight);
            row.Label("app.audio.section.light", "Light", light);
        });
        return appBuilder.BuildSubtree();
    };

    const synth::ui::NodeTree tree = synth::runtime_ui::BuildAudioPageTree(snapshot, area);

    const synth::ui::Node& row = FindNode(tree, "app.audio.section.row");
    const synth::ui::Node& heavy = FindNode(tree, "app.audio.section.heavy");
    const synth::ui::Node& light = FindNode(tree, "app.audio.section.light");

    // Node bounds are parent-relative (a child's bounds are an offset within
    // its own container, not a page-absolute position), so the row's declared
    // padding shows up directly as the first child's x -- not as a difference
    // against the row's own (differently-relative) bounds.x.
    //
    // The declared padding, not the default 12.0f: the first child starts
    // kRowPadding from the row's left edge.
    RequireNear(heavy.bounds.x, kRowPadding, 0.01f,
                "the row's explicit padding is honored, not LayoutOptions{}'s default");

    // The declared 3:1 weight split, not an even 1:1 default (a lost layout
    // entry falls back to Extent::Intrinsic, giving both leaves their
    // natural text width instead of a weighted share).
    Require(heavy.bounds.width > 0.0f && light.bounds.width > 0.0f,
            "both weighted children resolve to a positive width");
    RequireNear(heavy.bounds.width / light.bounds.width, kHeavyWeight / kLightWeight, 0.02f,
                "the declared 3:1 weight split is honored, not an even default split");

    // The gap between them is the default 8.0f (unset in rowLayout), so this
    // pins the light child's left edge relative to the heavy child's right
    // edge, and the row's own resolved width is fully accounted for by
    // padding + children + gap -- nothing left unexplained by a lost entry.
    RequireNear(light.bounds.x, heavy.bounds.x + heavy.bounds.width + synth::ui::kSpacing.gap, 0.01f,
                "the second child starts after the first plus the default gap");
    RequireNear(light.bounds.x + light.bounds.width + kRowPadding, row.bounds.width, 0.01f,
                "the row's own resolved width is fully accounted for by padding + children + gap");
}

// A patch root with more directories than the panel can show at the smallest
// reachable extent. 60 is past the point where the share-and-cap shape this
// replaced stopped producing a usable row at all: it gave every row 0.0392px at
// 47 entries and exactly 0px from 48 on, because the rows and the panel's own
// furniture divided one weighted remainder that the growing gap total ate.
synth::runtime_ui::FilePageSnapshot LongBrowserState(std::size_t entries)
{
    synth::runtime_ui::FilePageSnapshot snapshot = RepresentativeBrowserState();
    snapshot.browserEntries.clear();
    for (std::size_t ix = 0; ix < entries; ++ix)
    {
        const std::string name = "Patch" + std::to_string(ix);
        snapshot.browserEntries.push_back({name, name, ix + 1 == entries});
    }
    return snapshot;
}

synth::runtime_ui::FilePageSnapshot LongVersionsState(std::size_t entries)
{
    synth::runtime_ui::FilePageSnapshot snapshot;
    snapshot.patchNameText = "PatchA";
    snapshot.hasCurrentPatch = true;
    snapshot.statusText = "Ready";
    for (std::size_t ix = 0; ix < entries; ++ix)
    {
        const std::string label = "2024010" + std::to_string(ix) + "T010101Z-000.json";
        snapshot.versionEntries.push_back({label, "/patches/PatchA/" + label});
    }
    return snapshot;
}

void RequireListStaysUsable(const synth::ui::NodeTree& tree,
                            const char* listId,
                            const std::function<std::string(std::size_t)>& rowId,
                            std::size_t entries,
                            const char* label)
{
    const synth::ui::Node& list = FindNode(tree, listId);
    Require(list.kind == synth::ui::NodeKind::ScrollArea,
            "a list longer than its panel is a scroll area");
    for (std::size_t ix = 0; ix < entries; ++ix)
    {
        const synth::ui::Node& row = FindNode(tree, rowId(ix));
        RequireNear(row.bounds.height, synth::runtime_ui::Layout::kBrowserRowHeight, 0.0001f, label);
    }
    const synth::ui::Node& tail = FindNode(tree, rowId(entries - 1));
    Require(tail.bounds.y + tail.bounds.height <= list.scrollContentHeight + 0.0001f,
            "the tail row is inside the scrollable content extent");
    Require(list.scrollContentHeight > list.bounds.height,
            "a list longer than its panel declares a scrollable content extent");
    Require(list.bounds.height > synth::runtime_ui::Layout::kBrowserRowHeight,
            "the list keeps room for more than one row");
}

static void TestPatchBrowserSplicesAsARootlessSubtree()
{
    const synth::runtime_ui::FilePageSnapshot state = RepresentativeBrowserState();
    const synth::ui::Subtree browser = synth::runtime_ui::BuildPatchBrowserSubtree(state);
    for (const synth::ui::Node& node : browser.tree.nodes)
    {
        Require(node.kind != synth::ui::NodeKind::Root,
                "the patch browser produces a rootless subtree");
    }

    // The viewer -- rows, save-name entry, status text and confirm/cancel --
    // is the subtree. The page owns the panel and the splice.
    const std::vector<std::string> roots = ForestRootsOf(browser);
    const std::vector<std::string> expected{
        synth::runtime_ui::NodeIds::kFileBrowserTitle,
        std::string(synth::runtime_ui::NodeIds::kFileBrowserSaveName) + ".row",
        synth::runtime_ui::NodeIds::kFileStatus,
        synth::runtime_ui::NodeIds::kFileBrowserList,
        synth::runtime_ui::NodeIds::kFileBrowserActions,
    };
    Require(roots == expected, "the browser subtree carries the whole viewer, not just its rows");

    const synth::ui::NodeTree page =
        synth::runtime_ui::BuildFilePageTree(state, {0.0f, 0.0f, 900.0f, 560.0f});
    Require(CountRootNodes(page) == 1, "the spliced page has exactly one root");
    Require(IsDescendantOf(page, FirstSubtreeNodeId(browser), synth::runtime_ui::NodeIds::kFileBrowser),
            "the spliced nodes appear as descendants of the splice point");
    // The rows specifically, not just the subtree's first node: they are the
    // deepest thing the splice has to carry, one level below the scroll area.
    const synth::ui::Node& list = FindNode(page, synth::runtime_ui::NodeIds::kFileBrowserList);
    Require(!list.children.empty(), "the spliced scroll area carries its rows");
    Require(IsDescendantOf(page, list.children.front().value, synth::runtime_ui::NodeIds::kFileBrowser),
            "a spliced row is a descendant of the splice point, not just the subtree's first node");
    RequireSubtreeIsSplicedWhole(page,
                                 browser,
                                 synth::runtime_ui::NodeIds::kFileBrowser,
                                 "the browser panel's children are exactly the subtree's forest roots");

    // The subtree carries its layout declarations across the splice: without
    // them the rows would fall back to the default intrinsic extent (0 for a
    // Button's height along a column) rather than the recovered row height.
    Require(browser.layout.count(synth::runtime_ui::NodeIds::FileBrowserEntry(0)) == 1,
            "the subtree declares its own row layout");
    RequireNear(FindNode(page, synth::runtime_ui::NodeIds::FileBrowserEntry(0)).bounds.height,
                synth::runtime_ui::Layout::kBrowserRowHeight,
                0.0001f,
                "the spliced row layout declaration reaches the host resolver");
}

static void TestPatchVersionsSplicesAsARootlessSubtree()
{
    const synth::runtime_ui::FilePageSnapshot state = LongVersionsState(4);
    const synth::ui::Subtree versions = synth::runtime_ui::BuildPatchVersionsSubtree(state);
    const std::vector<std::string> expected{
        synth::runtime_ui::NodeIds::kFileVersionsTitle,
        synth::runtime_ui::NodeIds::kFileVersionsList,
    };
    Require(ForestRootsOf(versions) == expected,
            "the versions subtree carries its title and its scrolling list");

    const synth::ui::NodeTree page =
        synth::runtime_ui::BuildFilePageTree(state, {0.0f, 0.0f, 640.0f, 480.0f});
    Require(CountRootNodes(page) == 1, "the spliced versions page has exactly one root");
    RequireSubtreeIsSplicedWhole(page,
                                 versions,
                                 synth::runtime_ui::NodeIds::kFileVersions,
                                 "the versions section's children are exactly the subtree's forest roots");
    Require(versions.layout.count(synth::runtime_ui::NodeIds::FileVersionEntry(0)) == 1,
            "the versions subtree declares its own row layout");
    RequireNear(FindNode(page, synth::runtime_ui::NodeIds::FileVersionEntry(0)).bounds.height,
                synth::runtime_ui::Layout::kBrowserRowHeight,
                0.0001f,
                "the spliced versions row layout declaration reaches the host resolver");
}

static void TestSplicedListsKeepEveryEntryAtEveryExtent()
{
    // The construction this replaced measured each row against the panel it had
    // already sized and stopped emitting once they no longer fit, so a narrow
    // page silently lost entries. A state-only subtree cannot do that: every
    // entry is present, inside the scrolling content, at every extent the
    // runtime uses. Both lists carried that `break`, so both are checked.
    const synth::runtime_ui::FilePageSnapshot browserState = RepresentativeBrowserState();
    const synth::runtime_ui::FilePageSnapshot versionsState = LongVersionsState(5);
    for (const synth::ui::Bounds area : {synth::ui::Bounds{0.0f, 0.0f, 900.0f, 560.0f},
                                         synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f},
                                         synth::ui::Bounds{0.0f, 0.0f, 360.0f, 360.0f}})
    {
        const synth::ui::NodeTree browserTree =
            synth::runtime_ui::BuildFilePageTree(browserState, area);
        for (std::size_t ix = 0; ix < browserState.browserEntries.size(); ++ix)
        {
            const std::string rowId = synth::runtime_ui::NodeIds::FileBrowserEntry(ix);
            const synth::ui::Node* row = FindNodeById(browserTree, rowId);
            Require(row != nullptr, "every browser entry survives at every extent");
            Require(row->label == browserState.browserEntries[ix].name, "each row keeps its entry name");
            RequireNear(row->bounds.height, synth::runtime_ui::Layout::kBrowserRowHeight, 0.0001f,
                        "each browser row keeps the recovered row height");
        }

        const synth::ui::NodeTree versionsTree =
            synth::runtime_ui::BuildFilePageTree(versionsState, area);
        for (std::size_t ix = 0; ix < versionsState.versionEntries.size(); ++ix)
        {
            const std::string rowId = synth::runtime_ui::NodeIds::FileVersionEntry(ix);
            const synth::ui::Node* row = FindNodeById(versionsTree, rowId);
            Require(row != nullptr, "every version entry survives at every extent");
            Require(row->label == versionsState.versionEntries[ix].label,
                    "each version row keeps its entry label");
            RequireNear(row->bounds.height, synth::runtime_ui::Layout::kBrowserRowHeight, 0.0001f,
                        "each version row keeps the recovered row height");
        }
    }
}

static void TestLongListsKeepReadableRowsAndAReachableTail()
{
    constexpr std::size_t kEntries = 60;
    const synth::ui::Bounds reachable{0.0f, 0.0f, 360.0f, 360.0f};

    RequireListStaysUsable(
        synth::runtime_ui::BuildFilePageTree(LongBrowserState(kEntries), reachable),
        synth::runtime_ui::NodeIds::kFileBrowserList,
        synth::runtime_ui::NodeIds::FileBrowserEntry,
        kEntries,
        "every browser row keeps the recovered readable row height however long the list is");

    RequireListStaysUsable(
        synth::runtime_ui::BuildFilePageTree(LongVersionsState(kEntries), reachable),
        synth::runtime_ui::NodeIds::kFileVersionsList,
        synth::runtime_ui::NodeIds::FileVersionEntry,
        kEntries,
        "every version row keeps the recovered readable row height however long the list is");
}

static void TestFilePageFitsWithinTheRuntimeRoot()
{
    for (const synth::runtime_ui::FilePageSnapshot& state :
         {RepresentativeBrowserState(), synth::runtime_ui::FilePageSnapshot{}})
    {
        for (const synth::ui::Bounds area : {synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f},
                                             synth::ui::Bounds{0.0f, 0.0f, 360.0f, 360.0f}})
        {
            const synth::ui::NodeTree tree = synth::runtime_ui::BuildFilePageTree(state, area);
            for (const synth::ui::Node& node : tree.nodes)
            {
                if (node.id.value != synth::runtime_ui::NodeIds::kFileRoot)
                {
                    RequireNodeContainedInParent(tree, node.id.value);
                }
            }
        }
    }
}

// The smallest surface any first-party app declares. `uiHeight` is
// per-app compile-time config -- braid-4 560, miniapp 560, FakeBrowserApp 480 --
// so 480 is the floor every page and app has to resolve at.
constexpr synth::ui::Bounds kSmallestDeclaredSurface{0.0f, 0.0f, 640.0f, 480.0f};
constexpr synth::ui::Bounds kTallerSurface{0.0f, 0.0f, 640.0f, 560.0f};

std::string ResolutionDiagnostic(const std::function<void()>& build)
{
    try
    {
        build();
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    return {};
}

// Reported with the resolver's own diagnostic, because "the Sync page must
// resolve at 480" is not a useful failure and "…, but: 'runtime.sync.root' has
// 480.00 and its in-flow children need 483.18" is.
void RequireResolves(const std::string& name, const std::function<void()>& build)
{
    const std::string diagnostic = ResolutionDiagnostic(build);
    Require(diagnostic.empty(),
            (name + " must resolve at the smallest declared surface, but: " + diagnostic).c_str());
}

// "The page absorbs" is a claim about WHERE the difference goes, not merely
// that resolution survived: the furniture keeps its own extent at both surface
// heights and one named region takes the whole 80 between them. A page rebuilt
// as a fixed stack passes "it resolved" and fails this.
void RequireRegionAbsorbsTheDifference(const synth::ui::NodeTree& shortSurface,
                                       const synth::ui::NodeTree& tallSurface,
                                       const std::string& absorbingId,
                                       const std::vector<std::string>& furnitureIds,
                                       const char* label)
{
    const synth::ui::Node& shortRegion = FindNode(shortSurface, absorbingId);
    const synth::ui::Node& tallRegion = FindNode(tallSurface, absorbingId);
    RequireNear(tallRegion.bounds.height - shortRegion.bounds.height, 80.0f, 0.01f, label);
    for (const std::string& furnitureId : furnitureIds)
    {
        RequireNear(FindNode(tallSurface, furnitureId.c_str()).bounds.height,
                    FindNode(shortSurface, furnitureId.c_str()).bounds.height,
                    0.01f,
                    "furniture keeps its own extent while the absorbing region takes the difference");
    }
}

// Making Sync and Audio absorb moved their status regions from an intrinsic
// stack under the root into a weighted ScrollArea, and the claim that went with
// it was that nothing moved on screen. That claim is worth exactly as much as
// the assertion behind it, and the absorption pin above is not that assertion:
// it constrains the region's height and the furniture's, and would sit green
// through a status line that shifted, an inserted gap, or a changed left edge.
// An unexplained shift across these two surfaces is the failure mode to
// close here.
void RequireStatusLinesStackFromTheRegionTop(const synth::ui::NodeTree& tree,
                                             const char* regionId,
                                             const std::vector<std::string>& lineIds,
                                             float gap,
                                             const char* label)
{
    const synth::ui::Node& region = FindNode(tree, regionId);
    float expectedY = 0.0f;
    for (const std::string& lineId : lineIds)
    {
        const synth::ui::Node& line = FindNode(tree, lineId);
        RequireNear(line.bounds.y, expectedY, 0.01f, label);
        RequireNear(line.bounds.x, 0.0f, 0.01f,
                    "every status line shares the region's left edge");
        Require(line.bounds.height > 0.0f && line.bounds.width > 0.0f,
                "a pinned status line has a real extent, so its position is a position");
        expectedY += line.bounds.height + gap;
    }
    // Bounds are parent-relative, so pinning the children inside the region and
    // the region under the furniture is what pins the position on the surface.
    Require(region.bounds.height > 0.0f, "the status region resolved to a real extent");
}

static void TestEveryRebuiltPageAbsorbsAtTheSmallestDeclaredSurface()
{
    synth::runtime_ui::SyncPageSnapshot sync;
    sync.validationText = "PPQN must be in the range 1 to 960";
    sync.warningText = "96 PPQN is nonstandard";
    RequireRegionAbsorbsTheDifference(
        synth::runtime_ui::BuildSyncPageTree(sync, kSmallestDeclaredSurface),
        synth::runtime_ui::BuildSyncPageTree(sync, kTallerSurface),
        synth::runtime_ui::NodeIds::kSyncStatus,
        {synth::runtime_ui::NodeIds::kSyncBack, synth::runtime_ui::NodeIds::kSyncForm},
        "the Sync status region absorbs the whole difference between surface heights");

    synth::runtime_ui::AudioPageSnapshot audio;
    audio.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Output"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    audio.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    audio.selectedOutputId = "Built-in Output";
    audio.selectedInputId = "Built-in Microphone";
    audio.showInputCombo = true;
    audio.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    audio.statusLineText = "Audio running";
    RequireRegionAbsorbsTheDifference(
        synth::runtime_ui::BuildAudioPageTree(audio, kSmallestDeclaredSurface),
        synth::runtime_ui::BuildAudioPageTree(audio, kTallerSurface),
        synth::runtime_ui::NodeIds::kAudioStatus,
        {synth::runtime_ui::NodeIds::kAudioBack, synth::runtime_ui::NodeIds::kAudioForm},
        "the Audio status region absorbs the whole difference between surface heights");

    // Sync at the floor surface: Back is 28 high and the five-row form is 172,
    // and the root neither pads nor gaps, so the status region starts at 200 and
    // takes the remaining 280. Its nine lines stack from its top on the page's
    // own row gap, 22 high each, which is where they were before the region
    // existed.
    const synth::ui::NodeTree syncAtFloor =
        synth::runtime_ui::BuildSyncPageTree(sync, kSmallestDeclaredSurface);
    const synth::ui::Node& syncStatus =
        FindNode(syncAtFloor, synth::runtime_ui::NodeIds::kSyncStatus);
    RequireNear(syncStatus.bounds.y,
                FindNode(syncAtFloor, synth::runtime_ui::NodeIds::kSyncBack).bounds.height +
                    FindNode(syncAtFloor, synth::runtime_ui::NodeIds::kSyncForm).bounds.height,
                0.01f,
                "the Sync status region begins exactly where the furniture ends: the root neither "
                "pads nor gaps, so becoming a region moved nothing");
    RequireStatusLinesStackFromTheRegionTop(
        syncAtFloor,
        synth::runtime_ui::NodeIds::kSyncStatus,
        {synth::runtime_ui::NodeIds::kSyncValidation,
         synth::runtime_ui::NodeIds::kSyncWarning,
         synth::runtime_ui::NodeIds::kSyncBpm,
         synth::runtime_ui::NodeIds::kSyncLock,
         synth::runtime_ui::NodeIds::kSyncSource,
         synth::runtime_ui::NodeIds::kSyncOutputLatency,
         synth::runtime_ui::NodeIds::kSyncIgnoredInput,
         synth::runtime_ui::NodeIds::kSyncLateEvents,
         synth::runtime_ui::NodeIds::kSyncDroppedOutput},
        synth::runtime_ui::Layout::kRowGap,
        "each Sync status line keeps its position inside the region that now absorbs the page");
    RequireNear(FindNode(syncAtFloor, synth::runtime_ui::NodeIds::kSyncBpm).bounds.y,
                2.0f * (22.0f + synth::runtime_ui::Layout::kRowGap),
                0.01f,
                "the third Sync status line sits two 22-high lines and two row gaps down");

    // Audio's status region is the one place a gap would be invisible to the
    // absorption pin and visible on screen: its two device lines were direct
    // children of a root whose gap is zero, so the region that now holds them
    // declares a zero gap of its own and they still abut.
    const synth::ui::NodeTree audioAtFloor =
        synth::runtime_ui::BuildAudioPageTree(audio, kSmallestDeclaredSurface);
    RequireNear(FindNode(audioAtFloor, synth::runtime_ui::NodeIds::kAudioStatus).bounds.y,
                FindNode(audioAtFloor, synth::runtime_ui::NodeIds::kAudioBack).bounds.height +
                    FindNode(audioAtFloor, synth::runtime_ui::NodeIds::kAudioForm).bounds.height,
                0.01f,
                "the Audio status region begins exactly where the furniture ends");
    RequireStatusLinesStackFromTheRegionTop(
        audioAtFloor,
        synth::runtime_ui::NodeIds::kAudioStatus,
        {synth::runtime_ui::NodeIds::kAudioDeviceLine,
         synth::runtime_ui::NodeIds::kAudioStatusLine},
        0.0f,
        "the Audio device line and status line still abut, on a region gap of zero");
    const synth::ui::Node& audioDeviceLine =
        FindNode(audioAtFloor, synth::runtime_ui::NodeIds::kAudioDeviceLine);
    const synth::ui::Node& audioStatusLine =
        FindNode(audioAtFloor, synth::runtime_ui::NodeIds::kAudioStatusLine);
    RequireNear(audioStatusLine.bounds.y,
                audioDeviceLine.bounds.y + audioDeviceLine.bounds.height,
                0.01f,
                "the Audio status line starts where the device line ends -- any gap on the region "
                "would separate two lines that were touching");
    RequireNear(audioStatusLine.bounds.x, audioDeviceLine.bounds.x, 0.01f,
                "both Audio status lines share one left edge");

    const synth::runtime_ui::FilePageSnapshot browserState = LongBrowserState(60);
    RequireRegionAbsorbsTheDifference(
        synth::runtime_ui::BuildFilePageTree(browserState, kSmallestDeclaredSurface),
        synth::runtime_ui::BuildFilePageTree(browserState, kTallerSurface),
        synth::runtime_ui::NodeIds::kFileBrowser,
        {synth::runtime_ui::NodeIds::kFileHeader, synth::runtime_ui::NodeIds::kFileCommandStrip},
        "the File browser panel absorbs the whole difference between surface heights");

    // The idle region has two branches and only the has-patch one used to
    // absorb: with a patch the versions list takes what the status line leaves,
    // and without one the placeholder was a second fixed row, so the panel was a
    // fixed stack that happened to fit. Both branches are pinned.
    RequireRegionAbsorbsTheDifference(
        synth::runtime_ui::BuildFilePageTree(LongVersionsState(60), kSmallestDeclaredSurface),
        synth::runtime_ui::BuildFilePageTree(LongVersionsState(60), kTallerSurface),
        synth::runtime_ui::NodeIds::kFileVersionsList,
        {synth::runtime_ui::NodeIds::kFileStatus, synth::runtime_ui::NodeIds::kFileVersionsTitle},
        "the File versions list absorbs the whole difference between surface heights");
    RequireRegionAbsorbsTheDifference(
        synth::runtime_ui::BuildFilePageTree({}, kSmallestDeclaredSurface),
        synth::runtime_ui::BuildFilePageTree({}, kTallerSurface),
        std::string(synth::runtime_ui::NodeIds::kFileIdleRegion) + ".message",
        {synth::runtime_ui::NodeIds::kFileStatus},
        "the File idle placeholder absorbs the whole difference between surface heights");
}

static void TestEveryPageAndAppResolvesAtTheSmallestDeclaredSurface()
{
    synth::runtime_ui::SyncPageSnapshot sync;
    sync.validationText = "PPQN must be in the range 1 to 960";
    sync.warningText = "96 PPQN is nonstandard";

    synth::runtime_ui::AudioPageSnapshot audioWithInput;
    audioWithInput.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Output"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    audioWithInput.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    audioWithInput.selectedOutputId = "Built-in Output";
    audioWithInput.selectedInputId = "Built-in Microphone";
    audioWithInput.showInputCombo = true;
    audioWithInput.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    audioWithInput.statusLineText = "Audio running";

    std::vector<std::pair<std::string, std::function<void()>>> producers;
    producers.emplace_back("Sync", [sync] {
        synth::runtime_ui::BuildSyncPageTree(sync, kSmallestDeclaredSurface);
    });
    producers.emplace_back("Sync (no diagnostics)", [] {
        synth::runtime_ui::BuildSyncPageTree({}, kSmallestDeclaredSurface);
    });
    producers.emplace_back("Audio", [audioWithInput] {
        synth::runtime_ui::BuildAudioPageTree(audioWithInput, kSmallestDeclaredSurface);
    });
    producers.emplace_back("Audio (output only)", [] {
        synth::runtime_ui::BuildAudioPageTree({}, kSmallestDeclaredSurface);
    });
    producers.emplace_back("File (idle, no patch)", [] {
        synth::runtime_ui::BuildFilePageTree({}, kSmallestDeclaredSurface);
    });
    producers.emplace_back("File (60 saved versions)", [] {
        synth::runtime_ui::BuildFilePageTree(LongVersionsState(60), kSmallestDeclaredSurface);
    });
    producers.emplace_back("File (60 browser entries)", [] {
        synth::runtime_ui::BuildFilePageTree(LongBrowserState(60), kSmallestDeclaredSurface);
    });

    for (const auto& [name, build] : producers)
    {
        RequireResolves(name, build);
    }
}

static void TestControllersWizardAndBraid4ResolveAtTheSmallestDeclaredSurface()
{
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot wrldSlot;
    wrldSlot.name = "wrld";
    wrldSlot.kind = synth::MidiProfileKind::WrldBldr;
    wrldSlot.config = synth::WrldBldrDefaultProfileConfig();
    Require(instrument.AddController(std::move(wrldSlot)), "add wrld controller");
    synth::MidiConnectionState connection;
    connection.controllers.push_back({});

    const auto makeSurface = [&instrument, &connection] {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [&instrument] { return instrument; };
        callbacks.connectionState = [&connection] { return connection; };
        auto surface =
            std::make_unique<synth::runtime_ui::ControllersPageSurface>(std::move(callbacks));
        surface->SetContentBounds(kSmallestDeclaredSurface);
        surface->MarkDirty();
        surface->RefreshOnTick();
        return surface;
    };

    const auto candidate = [](const char* suffix) {
        return synth::WizardCandidate{
            .wizardId = "com.sheaf.midi-fighter-twister",
            .displayName = "MIDI Fighter Twister",
            .kind = synth::MidiProfileKind::MfTwister,
            .input = {std::string("twister-in") + suffix, "Midi Fighter Twister"},
            .output = {std::string("twister-out") + suffix, "Midi Fighter Twister"}};
    };

    const auto controllers = makeSurface();
    RequireResolves("the Controllers page", [&] { controllers->BuildTree(); });

    // Two candidates open the chooser; one opens the form directly. Both wizard
    // pages are producers, so both are resolved here.
    const auto chooser = makeSurface();
    chooser->SetDiscovery({.available = {candidate("-a"), candidate("-b")}});
    chooser->DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    RequireResolves("the wizard chooser", [&] { chooser->BuildTree(); });
    Require(FindNodeById(chooser->BuildTree(),
                         synth::runtime_ui::NodeIds::WizardChooserCandidate(candidate("-a"))) != nullptr,
            "the chooser really is the page under test");

    const auto form = makeSurface();
    form->SetDiscovery({.available = {candidate("-a")}});
    form->DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    RequireResolves("the wizard form", [&] { form->BuildTree(); });
    const synth::ui::NodeTree formTree = form->BuildTree();
    Require(FindNodeById(formTree, synth::runtime_ui::NodeIds::kWizardForm) != nullptr,
            "the wizard form really is the page under test");

    // The Twister column's declared height counted two gaps for four stacked
    // children, so the last button row's error band sat six pixels outside it.
    // "The form resolves" is satisfied by any height at or above the content,
    // including one that is too tall, so what is pinned here is the equality --
    // and the gap the old constant omitted, read off the resolved children
    // rather than restated from the producer's private constants.
    const synth::ui::Node& column = FindNode(formTree, "controller-wizard.twister.column.0");
    const synth::ui::Node& heading = FindNode(formTree, "controller-wizard.twister.column.0.heading");
    const synth::ui::Node& firstButton = FindNode(formTree, "controller-wizard.twister.button.0");
    const synth::ui::Node& secondButton = FindNode(formTree, "controller-wizard.twister.button.1");
    const synth::ui::Node& lastButton = FindNode(formTree, "controller-wizard.twister.button.2");
    RequireNear(lastButton.bounds.y + lastButton.bounds.height,
                column.bounds.height,
                0.01f,
                "the column is exactly as tall as its heading, its three button rows and the "
                "three gaps between them -- neither six pixels short nor any pixels spare");
    RequireNear(firstButton.bounds.y - (heading.bounds.y + heading.bounds.height),
                secondButton.bounds.y - (firstButton.bounds.y + firstButton.bounds.height),
                0.01f,
                "the heading is separated from the first button row by the same gap the rows use, "
                "which is the gap the old column height left out");
    Require(lastButton.bounds.height > 0.0f && heading.bounds.height > 0.0f,
            "the pinned column really contains a heading and a last button row");

    synth::ParameterManager manager;
    synth::MessageInBus uiBus(&manager);
    synth::MidiInstrumentConfig braidInstrument;
    synth::RuntimeConfig config = synth_braid4::Braid4Core::Config();
    config.uiWidth = static_cast<int>(kSmallestDeclaredSurface.width);
    config.uiHeight = static_cast<int>(kSmallestDeclaredSurface.height);
    synth::AppContext context;
    context.parameterManager = &manager;
    context.uiBus = &uiBus;
    context.instrument = &braidInstrument;
    context.config = &config;
    synth_braid4::Braid4Core core;
    core.Init(&context);
    core.PrepareToPlay(48000.0, 64);
    auto uiState = manager.CreateUIState();
    context.uiState = uiState.get();
    manager.PopulateUIState(*uiState);
    synth_braid4::Braid4UiSurface braid;
    braid.Attach(&context, &core);
    RequireResolves("Braid 4", [&] { braid.BuildTree(); });
    RequireNear(FindNode(braid.BuildTree(), synth_braid4::Braid4NodeIds::kRoot).bounds.height,
                kSmallestDeclaredSurface.height,
                0.0001f,
                "Braid 4 really was resolved against the 480-high surface, not its own default");
}

// ---------------------------------------------------------------------------
// Named visual criteria over every real surface, and absorbing-region pins for
// the three surfaces that had only "it resolved".
//
// The fixture state below is named once and used by both halves of
// the criteria suite. Lengths are chosen to exercise the scrolling path rather
// than the three-item happy case, because list length changes layout
// materially.
// ---------------------------------------------------------------------------

namespace criteria = synth::ui::criteria;

// 12 controllers put 598 of scroll content behind a 404 viewport at the 480
// floor; 3 would fit and prove nothing about scrolling.
constexpr std::size_t kFixtureControllerCount = 12;
// Two candidates open the CHOOSER; one opens the form directly.
constexpr std::size_t kFixtureCandidateCount = 2;
// Long enough to exceed the File panel's list viewport at either surface.
constexpr std::size_t kFixtureListEntryCount = 24;

// Spacing vocabularies, built from the CONSTANTS each producer declares rather
// than from a measured run, so editing a constant moves the allowed value with
// it while a number chosen at a call site still fails. They are deliberately
// per-producer and not one union: a config-page gap appearing inside an app is
// a defect the union would bless.
std::vector<float> SharedSpacing(std::vector<float> producerValues)
{
    // Zero is a deliberate "no spacing" and is not a magic number.
    std::vector<float> values{0.0f,
                              synth::ui::kSpacing.gap,
                              synth::ui::kSpacing.padding,
                              synth::ui::kSpacing.labelGap};
    values.insert(values.end(), producerValues.begin(), producerValues.end());
    return values;
}

const std::vector<float>& ConfigPageSpacing()
{
    static const std::vector<float> values = SharedSpacing({synth::runtime_ui::Layout::kPageMargin,
                                                            synth::runtime_ui::Layout::kRowGap,
                                                            synth::runtime_ui::Layout::kFilePanelPadding});
    return values;
}

const std::vector<float>& ControllersSpacing()
{
    static const std::vector<float> values =
        SharedSpacing({synth::runtime_ui::ControllersLayout::kPageMargin,
                       synth::runtime_ui::ControllersLayout::kRowGap,
                       synth::runtime_ui::ControllersLayout::kEndpointBoxGap,
                       synth::runtime_ui::ControllersLayout::kAvailableControlGap,
                       synth::runtime_ui::ControllersLayout::kLifecycleControlGap,
                       synth::runtime_ui::ControllersLayout::kStatusLegendPairGap});
    return values;
}

const std::vector<float>& StandardAppSpacing()
{
    static const std::vector<float> values =
        SharedSpacing({synth::ui::kStandardApp.margin,
                       synth::ui::kStandardApp.gap,
                       synth_braid4::Braid4EncoderGridLayout::kGap,
                       synth_braid4::Braid4ScopeGridLayout::kGap});
    return values;
}

// The wizard form's own spacing table, `TwisterFormLayout`, is private to
// `src/ControllerWizard.cpp` and cannot be named from a test. Its values are
// restated here rather than reached, which is weaker than every other entry in
// this file -- and it is the one table that carries producer-side layout
// arithmetic, which the layout contract bans. The outer wizard page furniture
// is the Controllers page's, so those constants ARE named.
const std::vector<float>& WizardFormSpacing()
{
    static const std::vector<float> values =
        SharedSpacing({synth::runtime_ui::ControllersLayout::kPageMargin,
                       synth::runtime_ui::ControllersLayout::kRowGap,
                       8.0f,     // TwisterFormLayout::kMargin / kFieldGap / kFormGridLabelGap
                       16.0f});  // TwisterFormLayout::kColumnGap
    return values;
}

// A surface under test, with the exemptions it needs stated one by one. Every
// entry in an exemption list is a disclosed residual, not a class of escape:
// adding a node to one is a deliberate edit a reviewer can see.
struct CriteriaSurface {
    std::string name;
    synth::ui::NodeTree tree;
    const std::vector<float>* spacing = nullptr;
    // Nodes the producer positions out of flow. They consume no stacking space,
    // so a gap measured against them is meaningless.
    std::set<std::string> outOfFlow;
    // Nodes whose rectangle is deliberately allowed to leave its parent or to
    // intersect a sibling.
    std::set<std::string> containmentExempt;
    std::set<std::string> overlapExempt;
    // Form controls with no visible caption today, each named individually with
    // the reason it has none. A product decision, not a residual
    // the suite has blessed.
    std::map<std::string, std::string> uncaptioned;
    // The exact number of form controls this fixture puts on the surface.
    //
    // This is the anti-vacuity guard, and it is deliberately a total rather
    // than `examined > 0`. On the Controllers page and the wizard form EVERY
    // form control is a table cell, so `examined > 0` is unsatisfiable there
    // without contriving the fixture -- while the risk it stands for, a new
    // uncaptioned control slipping in under an exception, is caught here
    // directly and by name: add a control and the total moves, so the test
    // fails and prints both numbers. Surfaces that do carry captioned controls
    // additionally get `examined > 0` for free, since their total exceeds their
    // exception list.
    std::size_t expectedFormControls = 0;
    // Containers declaring `formGrid`, with the row and column counts the named
    // fixture puts in them. Stated rather than discovered: an alignment check
    // that compares whatever it happens to find passes on an empty grid, which
    // is exactly how this criterion goes quiet.
    struct FormGrid {
        std::string containerId;
        std::size_t rows = 0;
        std::size_t columns = 0;
    };
    std::vector<FormGrid> formGrids;
};

// The three cross-backend residuals disclosed during 2.5a and left unpinned
// are ADJUDICATED here rather than inherited.
//
// All three are real, none is reachable by any producer, and all three are
// ACCEPTED rather than fixed -- fixing any of them means changing one backend's
// rendering, which needs its own justification and its own appearance decision
// after a product owner has already signed off. What is NOT accepted is leaving
// them floating, because two of the three are one ordinary producer declaration
// away from being live:
//
//   (i)  JUCE paints a ScrollArea's border in `paintOverChildren` while the
//        browser's inset `box-shadow` paints beneath content, so a scrolled
//        child would cover the border in the browser and not in JUCE. Reachable
//        by any producer that puts a border on a ScrollArea.
//   (ii) Neither backend clips a rounded ScrollArea's scrolled children to the
//        radius path; only the surface radius is pinned. Reachable by any
//        producer that puts a corner radius on a ScrollArea.
//   (iii) When a declared radius is below half the border width, JUCE's centred
//        stroke floors the path radius at 0 so the outer radius becomes w/2,
//        where the browser rounds at the declared radius. A hairline, inherent
//        to expressing a border as a centred stroke, and accepted permanently.
//
// So this is a tripwire on the PRECONDITION, not a waiver: it asserts that no
// first-party surface declares the shapes that would make a divergence visible.
// The day a producer declares one, this fails and asks for the decision to be
// made, instead of the divergence shipping unnoticed.
void RequireNoProducerReachesAnUnpinnedBackendDivergence(const CriteriaSurface& surface)
{
    const std::string prefix = surface.name + ": ";
    for (const synth::ui::Node& node : surface.tree.nodes)
    {
        if (node.kind == synth::ui::NodeKind::ScrollArea)
        {
            Require(!node.borderColor.has_value() && !node.borderWidth.has_value(),
                    (prefix + "'" + node.id.value +
                     "' declares a ScrollArea border. JUCE paints it over its children and the "
                     "browser paints it under them; decide which is right before shipping it "
                     "(residual (i))")
                        .c_str());
            Require(!node.cornerRadius.has_value(),
                    (prefix + "'" + node.id.value +
                     "' declares a ScrollArea corner radius. Neither backend clips scrolled "
                     "children to the radius path; decide what a rounded scroll area should do "
                     "before shipping it (residual (ii))")
                        .c_str());
        }
        if (node.cornerRadius.has_value() && node.borderWidth.has_value())
        {
            Require(*node.cornerRadius >= *node.borderWidth * 0.5f,
                    (prefix + "'" + node.id.value +
                     "' declares a corner radius below half its border width. JUCE's centred "
                     "stroke floors the path radius at zero there and the browser does not, so "
                     "the two outer radii diverge by a hairline (residual (iii))")
                        .c_str());
        }
    }
}

void RequireSurfaceMeetsTheNamedCriteria(const CriteriaSurface& surface)
{
    const std::string prefix = surface.name + ": ";
    RequireNoProducerReachesAnUnpinnedBackendDivergence(surface);
    const std::vector<std::string> containment =
        criteria::ContainmentViolations(surface.tree, surface.containmentExempt);
    Require(containment.empty(), (prefix + criteria::Join(containment)).c_str());

    std::set<std::string> overlapExempt = surface.overlapExempt;
    overlapExempt.insert(surface.outOfFlow.begin(), surface.outOfFlow.end());
    const std::vector<std::string> overlaps =
        criteria::SiblingOverlapViolations(surface.tree, overlapExempt);
    Require(overlaps.empty(), (prefix + criteria::Join(overlaps)).c_str());

    const std::vector<std::string> underlays = criteria::UnderlayViolations(surface.tree);
    Require(underlays.empty(), (prefix + criteria::Join(underlays)).c_str());

    const criteria::SpacingReport spacing =
        criteria::SpacingConformance(surface.tree, *surface.spacing, surface.outOfFlow);
    Require(spacing.violations.empty(), (prefix + criteria::Join(spacing.violations)).c_str());
    Require(!spacing.observed.empty(),
            (prefix + "the spacing check found no gap or padding to measure").c_str());

    const criteria::CaptionReport captions =
        criteria::UncaptionedFormControls(surface.tree, surface.uncaptioned);
    Require(captions.violations.empty(), (prefix + criteria::Join(captions.violations)).c_str());
    // A named exception that matches nothing is a stale entry pointing at a
    // control that has been renamed or removed, and it would sit here forever
    // waiving something that no longer exists.
    Require(captions.residualsMatched == surface.uncaptioned.size(),
            (prefix + "the caption exception list names " +
             std::to_string(surface.uncaptioned.size()) + " controls but only " +
             std::to_string(captions.residualsMatched) + " are on this surface")
                .c_str());
    Require(captions.examined + captions.residualsMatched == surface.expectedFormControls,
            (prefix + "this surface carries " +
             std::to_string(captions.examined + captions.residualsMatched) +
             " form controls, not the " + std::to_string(surface.expectedFormControls) +
             " the fixture declares -- a control was added, removed or renamed, and if it is new "
             "it has not been looked at")
                .c_str());

    const std::vector<std::string> silent = criteria::EmptyTextNodes(surface.tree);
    Require(silent.empty(), (prefix + criteria::Join(silent)).c_str());

    // "Like-type controls share column positions" is the first named criterion,
    // and the Playwright half can only evaluate it on Sync: the Audio form's
    // second row is the input selector, which the shell emits only when the
    // host offers an input device. Here the fixture decides, so both config
    // pages are checked at every surface extent -- and the declared row and
    // column counts make a silently empty comparison a failure rather than a
    // pass.
    for (const CriteriaSurface::FormGrid& grid : surface.formGrids)
    {
        const criteria::ColumnReport report = criteria::ColumnAlignment(surface.tree, grid.containerId);
        Require(report.violations.empty(), (prefix + criteria::Join(report.violations)).c_str());
        Require(report.comparedRows == grid.rows,
                (prefix + grid.containerId + " compared " + std::to_string(report.comparedRows) +
                 " rows, not the " + std::to_string(grid.rows) + " the fixture declares")
                    .c_str());
        Require(report.comparedColumns == grid.columns,
                (prefix + grid.containerId + " compared " + std::to_string(report.comparedColumns) +
                 " columns, not the " + std::to_string(grid.columns) + " the fixture declares")
                    .c_str());
    }
}

synth::runtime_ui::SyncPageSnapshot FixtureSyncState()
{
    synth::runtime_ui::SyncPageSnapshot snapshot;
    snapshot.validationText = "PPQN must be in the range 1 to 960";
    snapshot.warningText = "96 PPQN is nonstandard";
    return snapshot;
}

synth::runtime_ui::AudioPageSnapshot FixtureAudioState()
{
    synth::runtime_ui::AudioPageSnapshot snapshot;
    snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Output"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Built-in Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    snapshot.selectedOutputId = "Built-in Output";
    snapshot.selectedInputId = "Built-in Microphone";
    snapshot.showInputCombo = true;
    snapshot.deviceLineText = "Built-in Output: 48000 Hz, 512 frames";
    snapshot.statusLineText = "Audio running";
    return snapshot;
}

synth::WizardCandidate FixtureCandidate(const char* suffix)
{
    return synth::WizardCandidate{
        .wizardId = "com.sheaf.midi-fighter-twister",
        .displayName = "MIDI Fighter Twister",
        .kind = synth::MidiProfileKind::MfTwister,
        .input = {std::string("twister-in") + suffix, "Midi Fighter Twister"},
        .output = {std::string("twister-out") + suffix, "Midi Fighter Twister"}};
}

struct ControllersFixture {
    synth::MidiInstrumentConfig instrument;
    synth::MidiConnectionState connection;
    std::unique_ptr<synth::runtime_ui::ControllersPageSurface> surface;

    explicit ControllersFixture(synth::ui::Bounds area, std::size_t controllers)
    {
        for (std::size_t ix = 0; ix < controllers; ++ix)
        {
            synth::MidiControllerSlot slot;
            slot.name = "controller-" + std::to_string(ix);
            slot.kind = synth::MidiProfileKind::WrldBldr;
            slot.config = synth::WrldBldrDefaultProfileConfig();
            Require(instrument.AddController(std::move(slot)), "fixture adds its controllers");
            connection.controllers.push_back({});
        }
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return instrument; };
        callbacks.connectionState = [this] { return connection; };
        surface =
            std::make_unique<synth::runtime_ui::ControllersPageSurface>(std::move(callbacks));
        surface->SetContentBounds(area);
        surface->MarkDirty();
        surface->RefreshOnTick();
    }
};

// ---------------------------------------------------------------------------
// The caption exceptions, one control at a time with one reason each.
//
// These are NOT residuals the suite has decided are acceptable. Every form
// control must carry a visible caption, and each control below fails that
// today. They are recorded here, individually and with a stated reason,
// because whether a table cell should gain a caption or its table should gain a
// column heading is a product decision.
//
// The shape matters as much as the content: an id-to-reason map cannot grow by
// a new control happening to match a pattern, and `residualsMatched` makes an
// entry that no longer names anything visible too.
// ---------------------------------------------------------------------------

void Except(std::map<std::string, std::string>& into, std::string id, std::string reason)
{
    into.emplace(std::move(id), std::move(reason));
}

// 12 rows x {input, output} on the header, the one Name draft row 0's
// expanded editor adds, the add row's one Preset field, and the 13 mapping
// cells row 0's expanded encoders section publishes. The rename draft moved
// out of the header into the editor, so it no longer appears on all 12 rows
// unconditionally -- only on row 0, which this fixture expands.
// Stated so a new control cannot arrive unexamined under an exception.
// A mapping table's cells are identified by their COLUMN HEADING rather than by
// a per-cell caption, and a caption on every cell would repeat the heading on
// every row. That is a design, not a residual -- but it is only a design while
// the headings are actually there, so this derives the cells from the tree and
// refuses to except a single one unless the section really does publish
// headings. A section that lost its headings would fail rather than inherit the
// exclusion.
std::map<std::string, std::string> MappingCellExceptions(const synth::ui::NodeTree& tree)
{
    std::map<std::string, std::string> exceptions;
    std::set<std::string> headedBodies;
    for (const synth::ui::Node& node : tree.nodes)
    {
        const std::string& id = node.id.value;
        const std::size_t header = id.find(".header.");
        if (header != std::string::npos && id.size() > 8 &&
            id.compare(id.size() - 8, 8, ".caption") == 0)
        {
            headedBodies.insert(id.substr(0, header));
        }
    }
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (!synth::ui::criteria::IsFormControl(node.kind))
        {
            continue;
        }
        const std::string& id = node.id.value;
        const std::size_t mapping = id.find(".mapping.");
        if (mapping == std::string::npos || id.find(".field.") == std::string::npos)
        {
            continue;
        }
        const std::string body = id.substr(0, mapping);
        Require(headedBodies.count(body) != 0,
                ("mapping section " + body +
                 " publishes no column headings, so its cells cannot be excused from carrying "
                 "captions")
                    .c_str());
        Except(exceptions, id,
               "mapping table cell in " + body +
                   "; identified by that section's column headings rather than a per-cell caption");
    }
    return exceptions;
}

constexpr std::size_t kFixtureControllerExpectedControls = 12 * 2 + 1 + 1 + 13;

std::map<std::string, std::string> ControllerCaptionExceptions(std::size_t controllers,
                                                               const synth::ui::NodeTree& tree)
{
    // Both the add row's Preset combo and the editor's Name draft carry their
    // own visible caption ("Preset", "Name"), so neither needs an exception
    // here the way the old bare add-row name TextField did.
    std::map<std::string, std::string> exceptions = MappingCellExceptions(tree);
    (void)controllers;
    return exceptions;
}

// Braid 4 needs a live core, parameter manager and UI state before its surface
// resolves, and the surface holds pointers into all three, so they are held
// together and destroyed in one place.
struct Braid4Fixture {
    synth::ParameterManager manager;
    synth::MessageInBus uiBus{&manager};
    synth::MidiInstrumentConfig instrument;
    synth::RuntimeConfig config = synth_braid4::Braid4Core::Config();
    synth::AppContext context;
    synth_braid4::Braid4Core core;
    std::unique_ptr<synth::ParameterManager::UIState> uiState;
    synth_braid4::Braid4UiSurface surface;

    Braid4Fixture(float width, float height)
    {
        config.uiWidth = static_cast<int>(width);
        config.uiHeight = static_cast<int>(height);
        context.parameterManager = &manager;
        context.uiBus = &uiBus;
        context.instrument = &instrument;
        context.config = &config;
        core.Init(&context);
        core.PrepareToPlay(48000.0, 64);
        uiState = manager.CreateUIState();
        context.uiState = uiState.get();
        manager.PopulateUIState(*uiState);
        surface.Attach(&context, &core);
    }
};

// A visible visualizer, so the criteria set contains at least one tree that
// actually CARRIES an underlay. Without one, `UnderlayViolations` would
// return empty over every first-party surface for the uninteresting reason
// that no surface has an underlay, and the overlap criterion's only exception
// would be untested against real producer output.
struct CriteriaVisualizer final : synth::ui::Visualizer {
    std::vector<synth::ui::DrawCommand> DrawVisible() const override
    {
        const synth::ui::Bounds bounds = GetBounds();
        return {synth::ui::DrawCommand::Fill({0.0f, 0.0f, bounds.width, bounds.height},
                                             synth::Color::Cyan)};
    }
};

// Mini App is the plan's second rebuilt app and shares Braid 4's standard
// layout, so it gets the same criteria treatment rather than being covered by
// implication. It is built with a live UI state and one visible modulation
// visualizer on encoder 0, which is the state that emits the underlay.
struct MiniAppFixture {
    synth::ParameterManager manager;
    synth::MessageInBus uiBus{&manager};
    synth::MidiInstrumentConfig instrument;
    synth::GridManager gridManager;
    synth::RuntimeConfig config = synth_miniapp::MiniAppCore::Config();
    synth::AppContext context;
    synth_miniapp::MiniAppCore core;
    std::unique_ptr<synth::ParameterManager::UIState> uiState;
    CriteriaVisualizer visualizer;
    synth_miniapp::MiniAppUiSurface surface;

    MiniAppFixture(float width, float height)
    {
        config.uiWidth = static_cast<int>(width);
        config.uiHeight = static_cast<int>(height);
        context.parameterManager = &manager;
        context.uiBus = &uiBus;
        context.instrument = &instrument;
        context.config = &config;
        context.gridManager = &gridManager;
        core.Init(&context);
        core.PrepareToPlay(48000.0, 64);
        uiState = manager.CreateUIState();
        context.uiState = uiState.get();
        manager.PopulateUIState(*uiState);
        Require(uiState->slotCapacity > 0 && uiState->slots[0].cellCapacity > 0,
                "the Mini App fixture really has an encoder cell to hang a visualizer on");
        uiState->slots[0].cells[0].visualizer.store(&visualizer, std::memory_order_relaxed);
        surface.Attach(&context, &core);
    }
};

std::vector<CriteriaSurface> BuildFixtureSurfaces(synth::ui::Bounds area)
{
    std::vector<CriteriaSurface> surfaces;

    // Sync: four toggles and the PPQN field, each a caption cell and a control
    // cell. Audio: the output selector plus the input selector the named
    // fixture turns on, same two columns.
    surfaces.push_back({.name = "Sync",
                        .tree = synth::runtime_ui::BuildSyncPageTree(FixtureSyncState(), area),
                        .spacing = &ConfigPageSpacing(),
                        .expectedFormControls = 5,  // four toggles and the PPQN field
                        .formGrids = {{synth::runtime_ui::NodeIds::kSyncForm, 5, 2}}});
    surfaces.push_back({.name = "Audio",
                        .tree = synth::runtime_ui::BuildAudioPageTree(FixtureAudioState(), area),
                        .spacing = &ConfigPageSpacing(),
                        .expectedFormControls = 2,  // the output and input selectors
                        .formGrids = {{synth::runtime_ui::NodeIds::kAudioForm, 2, 2}}});
    surfaces.push_back({.name = "File (browser, 24 entries)",
                        .tree = synth::runtime_ui::BuildFilePageTree(
                            LongBrowserState(kFixtureListEntryCount), area),
                        .spacing = &ConfigPageSpacing(),
                        .expectedFormControls = 1});  // the patch-name field
    surfaces.push_back({.name = "File (24 saved versions)",
                        .tree = synth::runtime_ui::BuildFilePageTree(
                            LongVersionsState(kFixtureListEntryCount), area),
                        .spacing = &ConfigPageSpacing(),
                        .expectedFormControls = 0});  // the versions panel carries no form control
    surfaces.push_back({.name = "File (idle)",
                        .tree = synth::runtime_ui::BuildFilePageTree({}, area),
                        .spacing = &ConfigPageSpacing(),
                        .expectedFormControls = 0});  // the idle panel carries no form control
    return surfaces;
}

static void TestNamedVisualCriteriaHoldOnEveryPageAndApp()
{
    // Evaluated at EVERY surface a first-party app declares, not at one
    // comfortable extent: 640x480 is FakeBrowserApp's, 640x560 pairs the
    // narrow width with the tall height, and 900x560 is what Braid 4 and Mini
    // App declare, which is the extent the config pages actually get inside
    // those two apps. A containment or alignment claim checked at one extent is
    // how a 3.18px Sync overflow survived a green test.
    for (const synth::ui::Bounds area :
         {kSmallestDeclaredSurface, kTallerSurface, synth::ui::Bounds{0.0f, 0.0f, 900.0f, 560.0f}})
    {
        for (const CriteriaSurface& surface : BuildFixtureSurfaces(area))
        {
            RequireSurfaceMeetsTheNamedCriteria(surface);
        }

        ControllersFixture controllers(area, kFixtureControllerCount);
        // Row 0 is expanded so the fixture reaches the mapping section. Without
        // it every form control the Controllers page renders is one of the
        // named caption exceptions below, and the caption criterion examines
        // NOTHING on its most control-dense page -- the `.output` failure in a
        // different costume. It also puts the mapping table, its group headers
        // and its per-field cells under every other criterion.
        controllers.surface->DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kToggleConfig, "0"));
        controllers.surface->DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kToggleSection, "0:encoders"));
        controllers.surface->MarkDirty();
        controllers.surface->RefreshOnTick();
        std::vector<synth::WizardCandidate> candidates;
        for (std::size_t ix = 0; ix < kFixtureCandidateCount; ++ix)
        {
            candidates.push_back(FixtureCandidate(("-" + std::to_string(ix)).c_str()));
        }
        controllers.surface->SetDiscovery({.available = candidates});
        // The per-port status dots are laid out through the same LayoutOptions
        // Draw as the legend's dots (ControllersPageUI.hpp), so unlike the
        // header's old hand-centred single Draw they are normal, in-flow,
        // correctly-gapped children and need no out-of-flow exception.
        RequireSurfaceMeetsTheNamedCriteria(
            {.name = "Controllers (12 controllers, 2 available)",
             .tree = controllers.surface->BuildTree(),
             .spacing = &ControllersSpacing(),
             .uncaptioned = ControllerCaptionExceptions(kFixtureControllerCount,
                                                        controllers.surface->BuildTree()),
             .expectedFormControls = kFixtureControllerExpectedControls});

        controllers.surface->DispatchAction(
            synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
        const synth::ui::NodeTree chooser = controllers.surface->BuildTree();
        for (const synth::WizardCandidate& candidate : candidates)
        {
            Require(FindNodeById(chooser,
                                 synth::runtime_ui::NodeIds::WizardChooserCandidate(candidate)) != nullptr,
                    "the chooser really is the surface under test, with every candidate on it");
        }
        RequireSurfaceMeetsTheNamedCriteria(
            {.name = "wizard chooser (2 candidates)",
             .tree = chooser,
             .spacing = &ControllersSpacing()});

        controllers.surface->DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kWizardChoose,
            synth::runtime_ui::NodeIds::WizardCandidateToken(candidates.front())));
        const synth::ui::NodeTree wizardForm = controllers.surface->BuildTree();
        Require(FindNodeById(wizardForm, synth::runtime_ui::NodeIds::kWizardForm) != nullptr,
                "the wizard form really is the surface under test");
        RequireSurfaceMeetsTheNamedCriteria(
            {.name = "wizard form",
             .tree = wizardForm,
             .spacing = &WizardFormSpacing(),
             // No containment exemption. `TwisterFormLayout` still asks for a
             // 684-wide body on a 640 surface, but the page now hosts the
             // spliced form in a `ScrollArea`, so that width is absorbed as
             // scroll content instead of overhanging the page and being clipped
             // away. `TestTheWizardFormIsReachableRatherThanClipped` pins it.
             .expectedFormControls = 13});
    }

    // Braid 4 at the floor every surface must survive, and at the extent it
    // declares for itself.
    for (const synth::ui::Bounds area :
         {kSmallestDeclaredSurface, synth::ui::Bounds{0.0f, 0.0f, 900.0f, 560.0f}})
    {
        Braid4Fixture braid(area.width, area.height);
        RequireSurfaceMeetsTheNamedCriteria(
            {.name = "Braid 4",
             .tree = braid.surface.BuildTree(),
             .spacing = &StandardAppSpacing(),
             // The full-surface background painter is an explicitly bounded
             // out-of-flow Draw covering the whole root.
             .outOfFlow = {synth_braid4::Braid4NodeIds::kBackground},
             .expectedFormControls = 1});

        // Mini App on the same standard layout, with a live underlay on
        // encoder 0. Its `.visualizer` node is NOT exempted from the overlap
        // check: `UnderlayViolations` pins it congruent with the encoder it
        // names, and the overlap check still fails it against any other
        // sibling. That is the whole exception, on real producer output.
        MiniAppFixture mini(area.width, area.height);
        const synth::ui::NodeTree miniTree = mini.surface.BuildTree();
        const std::string underlayId = synth_miniapp::MiniAppNodeIds::Encoder(0) + ".visualizer";
        Require(FindNodeById(miniTree, underlayId) != nullptr,
                "the Mini App fixture really does emit the underlay this exception is for");
        RequireSurfaceMeetsTheNamedCriteria(
            {.name = "Mini App (encoder 0 modulated)",
             .tree = miniTree,
             .spacing = &StandardAppSpacing(),
             // Two sliders plus four toggles that render their own labels, so
             // this surface also gets `examined > 0` on the strength of the
             // controls that DO conform.
             .expectedFormControls = 6});
    }
}

// `ContainmentViolations` alone cannot see a row overflowing its host: today
// `scrollWidth` grows to the row's own minimum width whenever the content is
// narrower (`BuildControllersPageTree`, ControllersPageUI.hpp), so every node
// is inside a container that grew to fit it. `FitsWithinViolations` checks
// against the page's actual content bounds instead, folded over each node's
// ancestor chain, and is what keeps the two-line header's minimum width under
// frogg3rs's 900-wide content.
static void TestControllersRowFitsWithinFroggersNarrowestHost()
{
    synth::MidiInstrumentConfig instrument;
    synth::MidiConnectionState connection;

    synth::MidiControllerSlot twister;
    twister.name = "MIDI Fighter Twister";
    twister.kind = synth::MidiProfileKind::MfTwister;
    twister.input = {"twister-in", "Midi Fighter Twister"};
    twister.output = {"twister-out", "Midi Fighter Twister"};
    twister.config =
        synth::MfTwisterDefaultProfileConfig(synth::MfTwisterDefaultProfileOptions{.slotIx = 0});
    Require(instrument.AddController(std::move(twister)), "fixture adds the Twister row");
    connection.controllers.push_back(
        {.input = {.status = synth::MidiEndpointStatus::Offline},
         .output = {.status = synth::MidiEndpointStatus::Offline}});

    synth::MidiControllerSlot generic;
    generic.name = "Generic Controller";
    generic.kind = synth::MidiProfileKind::Generic;
    Require(instrument.AddController(std::move(generic)), "fixture adds the Generic row");
    connection.controllers.push_back({});

    synth::MidiControllerSlot launchpad;
    launchpad.name = "Launchpad";
    launchpad.kind = synth::MidiProfileKind::Launchpad;
    launchpad.config = synth::LaunchpadDefaultProfileConfig();
    Require(instrument.AddController(std::move(launchpad)), "fixture adds the Launchpad row");
    connection.controllers.push_back({});

    synth::MidiControllerSlot blacklisted;
    blacklisted.name = "Blacklisted Device";
    blacklisted.kind = synth::MidiProfileKind::Generic;
    blacklisted.disposition = synth::MidiControllerDisposition::Blacklisted;
    blacklisted.wizardId = "some.wizard.id";
    blacklisted.input = {"blacklisted-in", "Some Long Device Name"};
    blacklisted.output = {"blacklisted-out", "Some Long Device Name"};
    Require(instrument.AddController(std::move(blacklisted)), "fixture adds the Blacklisted row");
    connection.controllers.push_back({});

    synth::MidiAppCatalog catalog;
    synth::MidiAppDeviceDefault twisterDefault;
    twisterDefault.id = "froggers.twister";
    twisterDefault.displayName = "Midi Fighter Twister (offline)";
    twisterDefault.kind = synth::MidiProfileKind::MfTwister;
    twisterDefault.config =
        synth::MfTwisterDefaultProfileConfig(synth::MfTwisterDefaultProfileOptions{.slotIx = 0});
    catalog.deviceDefaults.push_back(twisterDefault);
    synth::MidiAppDeviceDefault genericDefault;
    genericDefault.id = "froggers.generic";
    genericDefault.displayName = "Akai APC40 mkII (Generic, offline)";
    genericDefault.kind = synth::MidiProfileKind::Generic;
    catalog.deviceDefaults.push_back(genericDefault);
    synth::MidiAppDeviceDefault launchpadDefault;
    launchpadDefault.id = "froggers.launchpad";
    launchpadDefault.displayName = "Novation Launchpad (offline)";
    launchpadDefault.kind = synth::MidiProfileKind::Launchpad;
    launchpadDefault.config = synth::LaunchpadDefaultProfileConfig();
    catalog.deviceDefaults.push_back(launchpadDefault);

    catalog.libraryKinds = {synth::UISystemMessage::ParamIncDec,
                             synth::UISystemMessage::ParamSetAbsolute,
                             synth::UISystemMessage::ParamPush,
                             synth::UISystemMessage::SetSceneBlend,
                             synth::UISystemMessage::HoldDrill};
    const char* const kPlainActionLabels[] = {"Play", "Stop", "Freeze", "Record", "Randomize All",
                                              "Randomize Page", "Reset All", "Reset Page",
                                              "Bank Previous", "Bank Next"};
    const char* const kPlainActionNames[] = {
        "app.play",         "app.stop",         "app.freeze",       "app.record",
        "app.randomize_all", "app.randomize_page", "app.reset_all", "app.reset_page",
        "app.bank_previous", "app.bank_next"};
    for (std::size_t ix = 0; ix < 10; ++ix)
    {
        synth::MidiAppAction action;
        action.action = kPlainActionNames[ix];
        action.value = "";
        action.label = kPlainActionLabels[ix];
        catalog.actions.push_back(std::move(action));
    }
    for (int bank = 0; bank < 6; ++bank)
    {
        synth::MidiAppAction action;
        action.action = "app.bank_select";
        action.value = std::to_string(bank);
        action.label = "Bank " + std::to_string(bank + 1);
        catalog.actions.push_back(std::move(action));
    }
    for (int scene = 0; scene < 2; ++scene)
    {
        synth::MidiAppAction action;
        action.action = "app.scene_select";
        action.value = std::to_string(scene);
        action.label = "Scene " + std::to_string(scene + 1);
        catalog.actions.push_back(std::move(action));
    }
    {
        synth::MidiAppAction bpm;
        bpm.action = "app.bpm";
        bpm.value = "";
        bpm.label = "BPM";
        bpm.analogRange = std::make_pair(30.0f, 300.0f);
        catalog.actions.push_back(std::move(bpm));
    }

    std::string status;
    synth::runtime_ui::ControllersPageCallbacks callbacks;
    callbacks.instrumentSnapshot = [&instrument] { return instrument; };
    callbacks.connectionState = [&connection] { return connection; };
    callbacks.enumerateDevices = [] { return synth::MidiDeviceList{}; };
    callbacks.commitInstrument = [&instrument, &connection](synth::MidiInstrumentConfig out) {
        instrument = std::move(out);
        connection.controllers.resize(instrument.controllers.size());
        return true;
    };
    callbacks.setStatus = [&status](std::string text) { status = std::move(text); };
    callbacks.messageCatalog = synth::MakeUISystemMessageChoices(catalog);
    callbacks.analogActionCatalog = synth::MakeAnalogAppActionChoices(catalog);
    callbacks.layouts = synth::MakeControllerWizardRegistry(catalog);

    synth::runtime_ui::ControllersPageSurface surface(std::move(callbacks));
    const synth::ui::Bounds froggersContentBounds{0.0f, 0.0f, 900.0f, 620.0f};
    surface.SetContentBounds(froggersContentBounds);
    surface.MarkDirty();
    surface.RefreshOnTick();

    const auto requireFits = [&](const char* state) {
        surface.RefreshOnTick();
        const synth::ui::NodeTree stateTree = surface.BuildTree();
        const std::vector<std::string> stateViolations =
            synth::ui::criteria::FitsWithinViolations(stateTree, froggersContentBounds);
        for (const std::string& violation : stateViolations)
        {
            std::fprintf(stderr, "FitsWithinViolations[%s]: %s\n", state, violation.c_str());
        }
        Require(stateViolations.empty(),
                (std::string("the Controllers page fits frogg3rs's 900-wide content: ") + state)
                    .c_str());
        return stateTree;
    };
    const auto requireAdded = [&](const char* label) {
        Require(status.rfind("Refused", 0) != 0, label);
    };

    // Positive control: every requireFits() call below only proves the page
    // still fits AT 900 wide -- a gate that always passes proves nothing
    // about itself. Build the same collapsed state 124px narrower than the
    // header's own 724px minimum and confirm FitsWithinViolations actually
    // reports something, then restore 900 before the real assertions run.
    {
        const synth::ui::Bounds narrowerThanMinimum{0.0f, 0.0f, 600.0f, 620.0f};
        surface.SetContentBounds(narrowerThanMinimum);
        const synth::ui::NodeTree narrowTree = surface.BuildTree();
        const std::vector<std::string> narrowViolations =
            synth::ui::criteria::FitsWithinViolations(narrowTree, narrowerThanMinimum);
        std::fprintf(stderr, "FitsWithinViolations[positive control, 600px]: count=%zu\n",
                    narrowViolations.size());
        Require(!narrowViolations.empty(),
                "positive control: built 124px narrower than the 724px header minimum, the "
                "fits-within gate must report at least one violation here -- zero would mean the "
                "gate itself is dead, not that the page somehow fits");
        surface.SetContentBounds(froggersContentBounds);
    }

    requireFits("collapsed rows: a Twister, a Generic, a Launchpad and a Blacklisted controller "
                "with long device names");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "1"));
    requireFits("Generic expanded");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "1:encoders"));
    requireFits("Generic Encoders open");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                         "1:encoders:encoder_turn"));
    requireAdded("Generic Turn row add accepted");
    requireFits("Generic Turn row added");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                         "1:encoders:encoder_push"));
    requireAdded("Generic Push row add accepted");
    requireFits("Generic Push row added");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection,
                                                         "1:system_messages"));
    requireFits("Generic System Messages open");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                         "1:system_messages:system"));
    requireAdded("Generic system row add accepted");
    const synth::ui::NodeTree systemRowTree = requireFits("Generic system row added");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "1:analogs"));
    requireFits("Generic Analogs open");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                         "1:analogs:analog_gesture"));
    requireAdded("Generic Gesture row add accepted");
    requireFits("Generic Gesture row added");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kAddSingle,
                                                         "1:analogs:analog_app_action"));
    requireAdded("Generic App action row add accepted");
    requireFits("Generic App action row added");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "2"));
    requireFits("Launchpad expanded");

    surface.DispatchAction(synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection,
                                                         "2:system_messages"));
    requireFits("Launchpad System Messages open");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "0"));
    requireFits("Twister expanded");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleSection, "0:encoders"));
    requireFits("Twister Encoders open");

    // Restore, Release and Reclaim are new controller-row lifecycle controls;
    // the row's Preset combo and (for Launchpad) its Variant combo are gone.
    // Cover every row state each control's visibility depends on, plus the
    // Launchpad row -- the one kind that lost two combos instead of one --
    // collapsed and expanded. Each fixture is asserted present/absent in the
    // tree before its requireFits() call, so a passing fits-check can't be
    // trivially true for a control that was never rendered.
    synth::MidiControllerSlot restoreDivergedRow;
    restoreDivergedRow.name = "Restore Diverged Twister";
    restoreDivergedRow.kind = synth::MidiProfileKind::MfTwister;
    restoreDivergedRow.wizardId = "froggers.twister";
    restoreDivergedRow.config = synth::MfTwisterDefaultProfileConfig(
        synth::MfTwisterDefaultProfileOptions{.slotIx = 1});
    restoreDivergedRow.input = {"restore-diverged-in", "Restore Diverged In"};
    restoreDivergedRow.output = {"restore-diverged-out", "Restore Diverged Out"};
    Require(instrument.AddController(std::move(restoreDivergedRow)),
            "fixture adds the Restore-diverged row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree restoreDivergedTree = surface.BuildTree();
    Require(FindNodeById(restoreDivergedTree, synth::runtime_ui::NodeIds::ControllerRestore(4)) !=
                nullptr,
            "Restore is present: resolved wizard id whose stored config no longer matches what "
            "that preset generates");
    requireFits("row showing Restore: resolved preset, config diverged");

    synth::MidiControllerSlot restorePristineRow;
    restorePristineRow.name = "Restore Pristine Twister";
    restorePristineRow.kind = synth::MidiProfileKind::MfTwister;
    restorePristineRow.wizardId = "froggers.twister";
    restorePristineRow.config = synth::MfTwisterDefaultProfileConfig(
        synth::MfTwisterDefaultProfileOptions{.slotIx = 0});
    restorePristineRow.input = {"restore-pristine-in", "Restore Pristine In"};
    restorePristineRow.output = {"restore-pristine-out", "Restore Pristine Out"};
    Require(instrument.AddController(std::move(restorePristineRow)),
            "fixture adds the Restore-pristine row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree restorePristineTree = surface.BuildTree();
    Require(FindNodeById(restorePristineTree, synth::runtime_ui::NodeIds::ControllerRestore(5)) ==
                nullptr,
            "Restore is absent: resolved wizard id whose stored config still matches exactly "
            "what that preset generates");
    requireFits("row NOT showing Restore: resolved preset, config pristine");

    synth::MidiControllerSlot releaseReadyRow;
    releaseReadyRow.name = "Release Ready Generic";
    releaseReadyRow.kind = synth::MidiProfileKind::Generic;
    releaseReadyRow.wizardId = "froggers.generic";
    releaseReadyRow.input = {"release-ready-in", "Release Ready In"};
    releaseReadyRow.output = {"release-ready-out", "Release Ready Out"};
    Require(instrument.AddController(std::move(releaseReadyRow)),
            "fixture adds the Release-ready row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree releaseReadyTree = surface.BuildTree();
    Require(FindNodeById(releaseReadyTree, synth::runtime_ui::NodeIds::ControllerBlacklist(6)) !=
                nullptr,
            "Release is present: resolved wizard id and both endpoints bound");
    requireFits("row showing Release: resolved wizard id, both endpoints bound");

    synth::MidiControllerSlot releaseNoDeviceRow;
    releaseNoDeviceRow.name = "Release No Device Generic";
    releaseNoDeviceRow.kind = synth::MidiProfileKind::Generic;
    releaseNoDeviceRow.wizardId = "froggers.generic";
    Require(instrument.AddController(std::move(releaseNoDeviceRow)),
            "fixture adds the Release-no-device row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree releaseNoDeviceTree = surface.BuildTree();
    Require(FindNodeById(releaseNoDeviceTree, synth::runtime_ui::NodeIds::ControllerBlacklist(7)) ==
                nullptr,
            "Release is absent: resolved wizard id but neither endpoint is bound");
    requireFits("row NOT showing Release: no device bound");

    synth::MidiControllerSlot releaseUnresolvedWizardRow;
    releaseUnresolvedWizardRow.name = "Release Unresolved Wizard";
    releaseUnresolvedWizardRow.kind = synth::MidiProfileKind::Generic;
    releaseUnresolvedWizardRow.wizardId = "com.example.missing-wizard";
    releaseUnresolvedWizardRow.input = {"release-unresolved-in", "Release Unresolved In"};
    releaseUnresolvedWizardRow.output = {"release-unresolved-out", "Release Unresolved Out"};
    Require(instrument.AddController(std::move(releaseUnresolvedWizardRow)),
            "fixture adds the Release-unresolved-wizard row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree releaseUnresolvedWizardTree = surface.BuildTree();
    Require(FindNodeById(releaseUnresolvedWizardTree,
                        synth::runtime_ui::NodeIds::ControllerBlacklist(8)) == nullptr,
            "Release is absent: both endpoints bound but the wizard id resolves to no known "
            "descriptor");
    requireFits("row NOT showing Release: wizard id does not resolve");

    synth::MidiControllerSlot releasedRow;
    releasedRow.name = "Released Twister";
    releasedRow.kind = synth::MidiProfileKind::MfTwister;
    releasedRow.disposition = synth::MidiControllerDisposition::Blacklisted;
    releasedRow.wizardId = "froggers.twister";
    releasedRow.input = {"released-in", "Released Device In"};
    releasedRow.output = {"released-out", "Released Device Out"};
    Require(instrument.AddController(std::move(releasedRow)), "fixture adds the Released row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    surface.RefreshOnTick();
    const synth::ui::NodeTree releasedTree = surface.BuildTree();
    const synth::ui::Node* releasedBadge =
        FindNodeById(releasedTree, synth::runtime_ui::NodeIds::ControllerBadge(9));
    Require(releasedBadge != nullptr && releasedBadge->text == "Released",
            "the released row shows its Released badge");
    Require(FindNodeById(releasedTree, synth::runtime_ui::NodeIds::ControllerRemoveBlacklist(9)) !=
                nullptr,
            "the released row shows Reclaim");
    Require(FindNodeById(releasedTree, synth::runtime_ui::NodeIds::ControllerConfigure(9)) !=
                nullptr,
            "the released row shows Configure because its wizard id resolves");
    requireFits("released row: Released badge, Reclaim, Configure");

    synth::MidiControllerSlot launchpadExtraRow;
    launchpadExtraRow.name = "Launchpad Extra";
    launchpadExtraRow.kind = synth::MidiProfileKind::Launchpad;
    launchpadExtraRow.config = synth::LaunchpadDefaultProfileConfig();
    Require(instrument.AddController(std::move(launchpadExtraRow)),
            "fixture adds a second Launchpad row");
    connection.controllers.push_back({});
    surface.MarkDirty();
    requireFits("Launchpad row collapsed");

    surface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kToggleConfig, "10"));
    requireFits("Launchpad row expanded");

    const synth::ui::Node* messageCombo = FindNodeById(
        systemRowTree, synth::runtime_ui::NodeIds::MappingField(
                           1, synth::MidiConfigSection::SystemMessages, 0,
                           synth::MidiMappingRowVM::Field::MessageKind));
    Require(messageCombo != nullptr, "the Generic system row's Message combo exists");
    Require(messageCombo->kind == synth::ui::NodeKind::ComboBox,
            "the Generic system row's Message field renders as a combo box");
    Require(messageCombo->options.size() == 24,
            "the Generic system row's Message combo offers the app catalog's 24 choices");
}

// `TestEveryPageAndAppResolvesAtTheSmallestDeclaredSurface` and its
// wizard/Braid 4 twin prove these three surfaces RESOLVE at the 480 floor and,
// for the Controllers page, the chooser and Braid 4, nothing more: deleting
// the overflow gate entirely would leave all three green, because only
// Sync, Audio, the File page and the wizard column carry positive geometry
// pins. What follows is the treatment the others already have -- what each
// surface's absorbing region actually DOES with the difference between two
// surface heights, and what the furniture around it keeps.
// The checklist is the contract, and it exists twice: `NamedCriteria()`
// here and `VISUAL_CRITERIA` in `browser/tests/visual-criteria.spec.ts`. Two
// copies of a contract drift, and a criterion silently present in one half and
// absent from the other is precisely how this suite stops meaning what it says.
// `VisualCriteria.hpp` has documented this test as existing since the criteria
// were written; it did not, and in the meantime the two halves were found
// encoding different overlap contracts.
//
// So it is a real cross-check rather than a restated constant: it PARSES the
// spec file. TypeScript cannot be linked against, and a hand-copied duplicate
// of the seven strings would drift exactly as the originals do.
static void TestTheNamedCriteriaAreTheOnesThePlaywrightSuiteNames()
{
    const std::string spec =
        synth::test::ReadSourceFile("projects/synth/browser/tests/visual-criteria.spec.ts");
    const std::string marker = "export const VISUAL_CRITERIA = [";
    const std::size_t start = spec.find(marker);
    Require(start != std::string::npos,
            "the Playwright suite still declares an exported VISUAL_CRITERIA checklist");
    const std::size_t end = spec.find("] as const;", start);
    Require(end != std::string::npos, "the VISUAL_CRITERIA literal is terminated");

    std::vector<std::string> declared;
    const std::string body = spec.substr(start + marker.size(), end - start - marker.size());
    for (std::size_t ix = 0; ix < body.size(); ++ix)
    {
        if (body[ix] != '"')
        {
            continue;
        }
        const std::size_t close = body.find('"', ix + 1);
        Require(close != std::string::npos, "every VISUAL_CRITERIA entry is a closed string");
        declared.push_back(body.substr(ix + 1, close - ix - 1));
        ix = close;
    }

    const std::vector<std::string>& named = synth::ui::criteria::NamedCriteria();
    Require(declared.size() == named.size(),
            ("the two checklists name a different number of criteria: Playwright " +
             std::to_string(declared.size()) + " vs headless " + std::to_string(named.size()))
                .c_str());
    Require(!named.empty(), "the checklist is not empty, so this comparison examined something");
    for (std::size_t ix = 0; ix < named.size(); ++ix)
    {
        Require(declared[ix] == named[ix],
                ("criterion " + std::to_string(ix) + " differs between the halves: Playwright \"" +
                 declared[ix] + "\" vs headless \"" + named[ix] + "\"")
                    .c_str());
    }
}

// A wizard declares its own form width, and the host page cannot re-measure a
// third-party one. `TwisterFormLayout` asked for 664 against the page's 640-wide
// body, so before the page hosted the spliced form in a `ScrollArea` the form
// overhung its parent by 28px and both backends clipped the right column's
// argument fields away silently. The overflow gate is the STACKING axis, so a
// cross-axis overrun of a fixed-extent child went straight past it; the
// containment criterion is what found it.
//
// The form is 684 wide today, not 664: widening `kMessageWidth` so the message
// selectors stopped clipping their own text made the overhang bigger. This test
// therefore pins the mechanism rather than either number.
//
// This pins the repair positively rather than trusting containment's absence of
// a violation: the scroll region publishes a content width that covers the
// form's whole declared width, and the fields that were being cut off are
// inside it.
static void TestTheWizardFormIsReachableRatherThanClipped()
{
    ControllersFixture controllers(kSmallestDeclaredSurface, 0);
    const synth::WizardCandidate candidate = FixtureCandidate("-0");
    controllers.surface->SetDiscovery({.available = {candidate}});
    controllers.surface->DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    const synth::ui::NodeTree tree = controllers.surface->BuildTree();

    const std::string scrollId = std::string(synth::runtime_ui::NodeIds::kWizardForm) + ".scroll";
    const synth::ui::Node& scroll = FindNode(tree, scrollId.c_str());
    const synth::ui::Node& formBody = FindNode(tree, "controller-wizard.twister.body");

    // The premise: the form really is wider than the region showing it, so this
    // test is about a form that does not fit rather than one that happens to.
    Require(formBody.bounds.width > scroll.bounds.width,
            "the Twister form really is wider than the surface the page can give it");
    RequireNear(scroll.scrollContentWidth,
                formBody.bounds.x + formBody.bounds.width,
                0.01f,
                "the scroll region publishes a content width reaching the form's right edge, so "
                "the whole form is reachable rather than clipped");
    Require(scroll.bounds.x + scroll.bounds.width <=
                FindNode(tree, std::string(synth::runtime_ui::NodeIds::kWizardForm) + ".body")
                        .bounds.width +
                    0.01f,
            "and the region itself stays inside the page, so nothing overhangs the surface");

    // The specific controls the old overhang cut off: the right column's
    // argument fields sat past the page's right edge and were unreachable.
    for (int buttonIx = 3; buttonIx < 6; ++buttonIx)
    {
        const synth::ui::Node& argument =
            FindNode(tree,
                     ("controller-wizard.twister.button." + std::to_string(buttonIx) + ".argument")
                         .c_str());
        const synth::ui::Node* row =
            FindNodeById(tree, "controller-wizard.twister.button." + std::to_string(buttonIx));
        Require(row != nullptr && argument.bounds.width > 0.0f,
                "every right-column argument field resolves to a real extent");
    }
}

static void TestControllersChooserAndBraid4PinTheirAbsorbingRegions()
{
    // --- Controllers: a ScrollArea absorbs, and the list stays scrollable. ---
    ControllersFixture shortControllers(kSmallestDeclaredSurface, kFixtureControllerCount);
    ControllersFixture tallControllers(kTallerSurface, kFixtureControllerCount);
    const synth::ui::NodeTree controllersAtFloor = shortControllers.surface->BuildTree();
    const synth::ui::NodeTree controllersTall = tallControllers.surface->BuildTree();
    RequireRegionAbsorbsTheDifference(controllersAtFloor,
                                      controllersTall,
                                      synth::runtime_ui::NodeIds::kScroll,
                                      {std::string(synth::runtime_ui::NodeIds::kRoot) + ".actions",
                                       synth::runtime_ui::NodeIds::kStatus,
                                       synth::runtime_ui::NodeIds::ControllerRow(0),
                                       synth::runtime_ui::NodeIds::ControllerRow(
                                           kFixtureControllerCount - 1)},
                                      "the Controllers scroll area absorbs the whole difference "
                                      "between surface heights");

    const synth::ui::Node& scroll = FindNode(controllersAtFloor, synth::runtime_ui::NodeIds::kScroll);
    const synth::ui::Node& addRow = FindNode(controllersAtFloor, synth::runtime_ui::NodeIds::kAddRow);
    // Absorbing is only half the claim. The other half is that the rows kept
    // their own extent instead of being squeezed to fit, and that the tail the
    // viewport cannot show is inside the content extent the resolver published
    // -- which is exactly what both backends size their scroll surface to.
    for (std::size_t ix = 0; ix < kFixtureControllerCount; ++ix)
    {
        RequireNear(FindNode(controllersAtFloor,
                             synth::runtime_ui::NodeIds::ControllerRow(ix).c_str()).bounds.height,
                    synth::runtime_ui::ControllersLayout::kControllerHeaderHeight,
                    0.01f,
                    "every controller row keeps its declared height however long the list is");
    }
    Require(addRow.bounds.y + addRow.bounds.height > scroll.bounds.height,
            "the 12-controller fixture really does put its tail below the visible viewport");
    RequireNear(scroll.scrollContentHeight,
                addRow.bounds.y + addRow.bounds.height,
                0.01f,
                "the published scroll content ends exactly at the last row, so the tail is reachable "
                "and no dead space follows it");
    RequireNear(FindNode(controllersTall, synth::runtime_ui::NodeIds::kScroll).scrollContentHeight,
                scroll.scrollContentHeight,
                0.01f,
                "the content extent follows the list, not the surface: a taller window scrolls less, "
                "it does not grow the list");

    // --- The wizard chooser: its body absorbs, its candidates keep their rows. ---
    ControllersFixture shortChooser(kSmallestDeclaredSurface, 0);
    ControllersFixture tallChooser(kTallerSurface, 0);
    std::vector<synth::WizardCandidate> candidates;
    for (std::size_t ix = 0; ix < kFixtureCandidateCount; ++ix)
    {
        candidates.push_back(FixtureCandidate(("-" + std::to_string(ix)).c_str()));
    }
    for (ControllersFixture* fixture : {&shortChooser, &tallChooser})
    {
        fixture->surface->SetDiscovery({.available = candidates});
        fixture->surface->DispatchAction(
            synth::ui::Action::Named(synth::runtime_ui::Actions::kWizardOpen));
    }
    const std::string chooserBody = std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".body";
    const synth::ui::NodeTree chooserAtFloor = shortChooser.surface->BuildTree();
    RequireRegionAbsorbsTheDifference(
        chooserAtFloor,
        tallChooser.surface->BuildTree(),
        chooserBody,
        {std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".actions",
         std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".heading",
         synth::runtime_ui::NodeIds::WizardChooserCandidate(candidates.front()),
         synth::runtime_ui::NodeIds::WizardChooserCandidate(candidates.back())},
        "the wizard chooser's body absorbs the whole difference between surface heights");

    const synth::ui::Node& firstCandidate =
        FindNode(chooserAtFloor,
                 synth::runtime_ui::NodeIds::WizardChooserCandidate(candidates.front()).c_str());
    const synth::ui::Node& lastCandidate =
        FindNode(chooserAtFloor,
                 synth::runtime_ui::NodeIds::WizardChooserCandidate(candidates.back()).c_str());
    const synth::ui::Node& chooserHeading =
        FindNode(chooserAtFloor,
                 (std::string(synth::runtime_ui::NodeIds::kWizardChooser) + ".heading").c_str());
    RequireNear(firstCandidate.bounds.height,
                synth::runtime_ui::ControllersLayout::kBackRowHeight,
                0.01f,
                "a chooser candidate keeps a full row's height rather than an intrinsic sliver");
    RequireNear(firstCandidate.bounds.y - (chooserHeading.bounds.y + chooserHeading.bounds.height),
                synth::runtime_ui::ControllersLayout::kRowGap,
                0.01f,
                "the first candidate follows the heading by the chooser's own row gap");
    RequireNear(lastCandidate.bounds.y,
                firstCandidate.bounds.y +
                    static_cast<float>(kFixtureCandidateCount - 1) *
                        (synth::runtime_ui::ControllersLayout::kBackRowHeight +
                         synth::runtime_ui::ControllersLayout::kRowGap),
                0.01f,
                "candidates stack one declared row and one row gap apart, in discovery order");
    Require(lastCandidate.bounds.y + lastCandidate.bounds.height <=
                FindNode(chooserAtFloor, chooserBody).bounds.height,
            "every candidate of the named fixture is inside the body at the 480 floor");

    // --- Braid 4: the body absorbs, the title and bay keep their extents. ---
    Braid4Fixture shortBraid(640.0f, kSmallestDeclaredSurface.height);
    Braid4Fixture tallBraid(640.0f, kTallerSurface.height);
    const synth::ui::NodeTree braidAtFloor = shortBraid.surface.BuildTree();
    RequireRegionAbsorbsTheDifference(braidAtFloor,
                                      tallBraid.surface.BuildTree(),
                                      "braid4.body",
                                      {synth_braid4::Braid4NodeIds::kTitle, "braid4.bay"},
                                      "Braid 4's body absorbs the whole difference between surface "
                                      "heights while the title row and widget bay keep theirs");

    const synth::ui::Node& braidBody = FindNode(braidAtFloor, "braid4.body");
    const synth::ui::Node& visualizers = FindNode(braidAtFloor, "braid4.visualizers");
    const synth::ui::Node& encoders = FindNode(braidAtFloor, "braid4.encoders");
    RequireNear(visualizers.bounds.height, braidBody.bounds.height, 0.01f,
                "the visualizer stack takes the absorbing body's full height");
    RequireNear(encoders.bounds.height, braidBody.bounds.height, 0.01f,
                "and so does the encoder region beside it");
    RequireNear(encoders.bounds.x,
                visualizers.bounds.width + synth::ui::kStandardApp.gap,
                0.01f,
                "the encoder region starts one standard-layout gap after the stack, with no "
                "producer-side arithmetic between them");
    RequireNear(encoders.bounds.width,
                braidBody.bounds.width - visualizers.bounds.width - synth::ui::kStandardApp.gap,
                0.01f,
                "and takes exactly the remainder of the body");

    // Every scope and encoder cell has a real extent inside its grid at the
    // floor. "Braid 4 resolved" is satisfied by sixteen zero-extent encoders.
    for (std::size_t ix = 0; ix < synth_braid4::Braid4EncoderGridLayout::kEncoderCount; ++ix)
    {
        const synth::ui::Node& cell =
            FindNode(braidAtFloor, synth_braid4::Braid4NodeIds::Encoder(ix).c_str());
        Require(cell.bounds.width > 0.0f && cell.bounds.height > 0.0f,
                "every Braid 4 encoder cell resolves to a real extent at the 480 floor");
    }
    for (std::size_t ix = 0; ix < synth_braid4::Braid4ScopeGridLayout::kScopeCount; ++ix)
    {
        const synth::ui::Node& vco =
            FindNode(braidAtFloor, synth_braid4::Braid4NodeIds::VcoScope(ix).c_str());
        const synth::ui::Node& lfo =
            FindNode(braidAtFloor, synth_braid4::Braid4NodeIds::LfoScope(ix).c_str());
        Require(vco.bounds.width > 0.0f && vco.bounds.height > 0.0f,
                "every Braid 4 VCO scope cell resolves to a real extent at the 480 floor");
        Require(lfo.bounds.width > 0.0f && lfo.bounds.height > 0.0f,
                "every Braid 4 LFO scope cell resolves to a real extent at the 480 floor");
    }
}

static void TestFilePagePinsItsResolvedGeometry()
{
    const synth::runtime_ui::FilePageSnapshot state = RepresentativeBrowserState();
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildFilePageTree(state, {0.0f, 0.0f, 640.0f, 480.0f});

    const synth::ui::Node& header = FindNode(tree, synth::runtime_ui::NodeIds::kFileHeader);
    const synth::ui::Node& strip = FindNode(tree, synth::runtime_ui::NodeIds::kFileCommandStrip);
    const synth::ui::Node& browser = FindNode(tree, synth::runtime_ui::NodeIds::kFileBrowser);
    const synth::ui::Node& back = FindNode(tree, synth::runtime_ui::NodeIds::kFileBack);

    // Page margin: the three page regions share one x and stack by their own
    // resolved heights, with no producer-side cursor to drift.
    RequireNear(header.bounds.x, synth::runtime_ui::Layout::kFilePanelPadding, 0.0001f,
                "the File header sits at the page margin");
    RequireNear(header.bounds.y, synth::runtime_ui::Layout::kFilePanelPadding, 0.0001f,
                "the File header starts at the page margin");
    RequireNear(header.bounds.width, 620.0f, 0.0001f, "the File header fills the page content width");
    RequireNear(header.bounds.height, 72.0f, 0.0001f,
                "the File header is as tall as its two text rows, their gap and its padding");
    RequireNear(strip.bounds.x, synth::runtime_ui::Layout::kFilePanelPadding, 0.0001f,
                "the File command strip shares the page margin");
    RequireNear(strip.bounds.y, 86.0f, 0.0001f, "the File command strip follows the header");
    RequireNear(strip.bounds.height, synth::runtime_ui::Layout::kPatchRowHeight, 0.0001f,
                "the File command strip keeps the recovered patch row height");
    RequireNear(browser.bounds.x, synth::runtime_ui::Layout::kFilePanelPadding, 0.0001f,
                "the File browser shares the page margin");
    RequireNear(browser.bounds.y, 124.0f, 0.0001f, "the File browser follows the command strip");
    RequireNear(browser.bounds.height, 346.0f, 0.0001f,
                "the File browser takes the page's remaining height");

    // The Back button is bounded on both axes and right-aligned in the header
    // row, which is what an unbounded declaration would silently lose.
    RequireNear(back.bounds.width, synth::runtime_ui::Layout::kBackButtonWidth, 0.0001f,
                "the File Back button keeps the recovered compact page width");
    RequireNear(back.bounds.height, synth::runtime_ui::Layout::kBackRowHeight, 0.0001f,
                "the File Back button keeps the recovered back row height");
    RequireNear(back.bounds.x + back.bounds.width,
                header.bounds.width - synth::ui::kSpacing.padding,
                0.0001f,
                "the File Back button is flush with the header's trailing padding");

    // Command strip: five equal buttons capped at the recovered patch width,
    // packed in declaration order with no overlap.
    const std::vector<const char*> commandIds{synth::runtime_ui::NodeIds::kFileNew,
                                              synth::runtime_ui::NodeIds::kFileSave,
                                              synth::runtime_ui::NodeIds::kFileSaveAs,
                                              synth::runtime_ui::NodeIds::kFileLoad,
                                              synth::runtime_ui::NodeIds::kFileRevert};
    float expectedX = 0.0f;
    for (const char* id : commandIds)
    {
        const synth::ui::Node& button = FindNode(tree, id);
        RequireNear(button.bounds.width, synth::runtime_ui::Layout::kPatchButtonWidth, 0.0001f,
                    "each File command button is capped at the recovered patch button width");
        RequireNear(button.bounds.height, synth::runtime_ui::Layout::kPatchRowHeight, 0.0001f,
                    "each File command button fills the command strip height");
        RequireNear(button.bounds.x, expectedX, 0.0001f,
                    "the File command buttons pack in declaration order");
        expectedX += button.bounds.width + synth::runtime_ui::Layout::kRowGap;
    }

    // Browser interior: the panel's padding places the scrolling list, and the
    // rows sit at the list's own origin -- a surface coordinate would show up
    // in either as a page-sized offset.
    const synth::ui::Node& list = FindNode(tree, synth::runtime_ui::NodeIds::kFileBrowserList);
    const synth::ui::Node& firstEntry =
        FindNode(tree, synth::runtime_ui::NodeIds::FileBrowserEntry(0));
    RequireNear(list.bounds.x, synth::ui::kSpacing.padding, 0.0001f,
                "the File browser list x is the panel padding");
    RequireNear(list.bounds.y, 108.0f, 0.0001f,
                "the File browser list follows the title, name field and status inside the panel");
    RequireNear(list.bounds.height, 190.0f, 0.0001f,
                "the File browser list takes the panel's remaining height");
    RequireNear(firstEntry.bounds.x, 0.0f, 0.0001f,
                "the File browser row x is the scroll content origin");
    RequireNear(firstEntry.bounds.y, 0.0f, 0.0001f,
                "the File browser row y is the scroll content origin");
    RequireNear(firstEntry.bounds.height, synth::runtime_ui::Layout::kBrowserRowHeight, 0.0001f,
                "a File browser row keeps the recovered browser row height");
    RequireNear(FindNode(tree, synth::runtime_ui::NodeIds::FileBrowserEntry(1)).bounds.y,
                synth::runtime_ui::Layout::kBrowserRowHeight,
                0.0001f,
                "File browser rows abut, as the recovered list's rows did");
    Require(firstEntry.bounds.y < browser.bounds.y,
            "the File browser row y is inside the browser section, not surface-absolute");

    // Confirm and cancel: bounded on the main axis and packed from the left of
    // an actions row that the scrolling list pushes to the panel's foot. The
    // recovered construction right-aligned them against a hand-computed bottom
    // edge; the in-flow left-packed placement is deliberate, and pinning it is
    // what stops either the placement or the width cap regressing unnoticed.
    const synth::ui::Node& actions = FindNode(tree, synth::runtime_ui::NodeIds::kFileBrowserActions);
    const synth::ui::Node& confirm = FindNode(tree, synth::runtime_ui::NodeIds::kFileBrowserConfirm);
    const synth::ui::Node& cancel = FindNode(tree, synth::runtime_ui::NodeIds::kFileBrowserCancel);
    RequireNear(actions.bounds.x, synth::ui::kSpacing.padding, 0.0001f,
                "the File browser actions row shares the panel padding");
    RequireNear(actions.bounds.y, 302.0f, 0.0001f,
                "the File browser actions row follows the scrolling list");
    RequireNear(actions.bounds.y + actions.bounds.height,
                browser.bounds.height - synth::ui::kSpacing.padding,
                0.0001f,
                "the File browser actions row lands on the panel's bottom padding");
    RequireNear(actions.bounds.height, synth::runtime_ui::Layout::kBrowserCommandHeight, 0.0001f,
                "the File browser actions row keeps the recovered command height");
    RequireNear(confirm.bounds.width, synth::runtime_ui::Layout::kBrowserButtonWidth, 0.0001f,
                "confirm is capped at the recovered browser button width");
    RequireNear(cancel.bounds.width, synth::runtime_ui::Layout::kBrowserButtonWidth, 0.0001f,
                "cancel is capped at the recovered browser button width");
    RequireNear(confirm.bounds.x, 0.0f, 0.0001f, "confirm packs from the start of the actions row");
    RequireNear(cancel.bounds.x,
                confirm.bounds.width + synth::runtime_ui::Layout::kRowGap,
                0.0001f,
                "cancel packs after confirm on the shared row gap");
    RequireNear(confirm.bounds.y, 0.0f, 0.0001f, "confirm fills its actions row");
    RequireNear(confirm.bounds.height, actions.bounds.height, 0.0001f,
                "confirm fills the actions row height");
    RequireNear(cancel.bounds.height, actions.bounds.height, 0.0001f,
                "cancel fills the actions row height");

    // Save-name field: the caption is a library-emitted sibling Label, not the
    // field's own hidden label.
    const std::string saveNameId = synth::runtime_ui::NodeIds::kFileBrowserSaveName;
    Require(FindNode(tree, saveNameId + ".caption").text == "Patch name",
            "the File save-name field keeps its user-facing caption outside the field");
    Require(FindNode(tree, saveNameId).label.empty(),
            "the File save-name caption does not route through the field's own label");
    const synth::ui::Node& saveNameRow = FindNode(tree, saveNameId + ".row");
    const synth::ui::Node& saveNameCaption = FindNode(tree, saveNameId + ".caption");
    const synth::ui::Node& saveNameField = FindNode(tree, saveNameId);
    RequireNear(saveNameCaption.bounds.x, 0.0f, 0.0001f,
                "the File save-name caption starts at its row's origin");
    RequireNear(saveNameField.bounds.x,
                saveNameCaption.bounds.width + synth::ui::kSpacing.labelGap,
                0.0001f,
                "the File save-name field clears its caption by the shared label gap");
    RequireNear(saveNameField.bounds.x + saveNameField.bounds.width, saveNameRow.bounds.width, 0.0001f,
                "the File save-name field fills the rest of its row");
}

static void TestFileIdleRegionPinsItsResolvedGeometry()
{
    synth::runtime_ui::FilePageSnapshot state;
    state.patchNameText = "PatchA";
    state.hasCurrentPatch = true;
    state.statusText = "Ready";
    state.versionEntries.push_back({"20240202T020202Z-000.json", "/patches/PatchA/20240202T020202Z-000.json"});
    state.versionEntries.push_back({"20240101T010101Z-000.json", "/patches/PatchA/20240101T010101Z-000.json"});
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildFilePageTree(state, {0.0f, 0.0f, 640.0f, 480.0f});

    const synth::ui::Node& idle = FindNode(tree, synth::runtime_ui::NodeIds::kFileIdleRegion);
    const synth::ui::Node& versions = FindNode(tree, synth::runtime_ui::NodeIds::kFileVersions);
    const synth::ui::Node& title = FindNode(tree, synth::runtime_ui::NodeIds::kFileVersionsTitle);
    const synth::ui::Node& newest = FindNode(tree, synth::runtime_ui::NodeIds::FileVersionEntry(0));

    RequireNear(idle.bounds.x, synth::runtime_ui::Layout::kFilePanelPadding, 0.0001f,
                "the File idle region shares the page margin");
    RequireNear(idle.bounds.y, 124.0f, 0.0001f, "the File idle region follows the command strip");
    RequireNear(versions.bounds.x, synth::ui::kSpacing.padding, 0.0001f,
                "the File versions section x is idle-relative");
    RequireNear(versions.bounds.y, 44.0f, 0.0001f,
                "the File versions section follows the idle status line");
    Require(versions.bounds.y < idle.bounds.y,
            "the File versions section y is inside the idle region, not surface-absolute");
    RequireNear(title.bounds.x, 0.0f, 0.0001f, "the File versions title x is versions-relative");
    RequireNear(title.bounds.y, 0.0f, 0.0001f, "the File versions title y is versions-relative");
    RequireNear(newest.bounds.height, synth::runtime_ui::Layout::kBrowserRowHeight, 0.0001f,
                "a File version row is capped at the recovered browser row height");
    Require(newest.label == "20240202T020202Z-000.json", "the File versions list is newest first");
    Require(newest.doubleClickAction.has_value() &&
                newest.doubleClickAction->name == synth::runtime_ui::Actions::kFileConfirmedLoad &&
                newest.doubleClickAction->value == "/patches/PatchA/20240202T020202Z-000.json",
            "a File version row loads its own file on double click");
    Require(!newest.action.has_value(),
            "a File version row carries no plain click action rather than an empty-named one");
}

static void TestFilePageCarriesPageColoursAndTextStyles()
{
    const synth::runtime_ui::FilePageSnapshot state = RepresentativeBrowserState();
    const synth::ui::NodeTree tree =
        synth::runtime_ui::BuildFilePageTree(state, {0.0f, 0.0f, 640.0f, 480.0f});

    Require(FindNode(tree, synth::runtime_ui::NodeIds::kFileSave).color ==
                std::optional<synth::Color>(synth::pagestyle::kPrimaryButton),
            "Save is the File page's primary command");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kFileLoad).color ==
                std::optional<synth::Color>(synth::pagestyle::kDefaultButton),
            "the other File commands carry the default page button colour");
    const synth::ui::Node& selectedRow =
        FindNode(tree, synth::runtime_ui::NodeIds::FileBrowserEntry(1));
    Require(selectedRow.color == std::optional<synth::Color>(synth::pagestyle::kListRowButton),
            "a browser row carries the list row colour, selected or not");
    Require(selectedRow.selected, "the selected browser row carries its selection state");
    Require(!FindNode(tree, synth::runtime_ui::NodeIds::FileBrowserEntry(0)).selected,
            "an unselected browser row does not");

    const synth::ui::Node& patchName = FindNode(tree, synth::runtime_ui::NodeIds::kFilePatchName);
    Require(patchName.textStyle.has_value() &&
                TextStyleMatches(*patchName.textStyle, synth::pagestyle::kTitleTextStyle),
            "a current patch name renders as a title");
    synth::runtime_ui::FilePageSnapshot noPatch;
    const synth::ui::Node& emptyName =
        FindNode(synth::runtime_ui::BuildFilePageTree(noPatch, {0.0f, 0.0f, 640.0f, 480.0f}),
                 synth::runtime_ui::NodeIds::kFilePatchName);
    Require(emptyName.textStyle.has_value() &&
                TextStyleMatches(*emptyName.textStyle, synth::pagestyle::kMutedTitleTextStyle),
            "no current patch renders as a muted title");

    synth::runtime_ui::FilePageSnapshot errorState = RepresentativeBrowserState();
    errorState.statusText = "Patch already exists";
    const synth::ui::Node& errorStatus =
        FindNode(synth::runtime_ui::BuildFilePageTree(errorState, {0.0f, 0.0f, 640.0f, 480.0f}),
                 synth::runtime_ui::NodeIds::kFileStatus);
    Require(errorStatus.textStyle.has_value() &&
                TextStyleMatches(*errorStatus.textStyle, synth::pagestyle::kDangerTextStyle),
            "a File error status renders in the danger text style");
    const synth::ui::Node& readyStatus = FindNode(tree, synth::runtime_ui::NodeIds::kFileStatus);
    Require(readyStatus.textStyle.has_value() &&
                TextStyleMatches(*readyStatus.textStyle, synth::pagestyle::kMutedTextStyle),
            "an ordinary File status renders in the muted text style");
}

static void TestFilePanelsCarryAppearanceWithoutUnderlays()
{
    struct ExpectedPanel
    {
        synth::runtime_ui::FilePageSnapshot state;
        const char* id = "";
        synth::Color fill{};
        synth::Color border{};
    };

    const std::vector<ExpectedPanel> panels{
        {RepresentativeBrowserState(), synth::runtime_ui::NodeIds::kFileHeader,
         synth::pagestyle::kHeaderPanelFill, synth::pagestyle::kHeaderPanelBorder},
        {RepresentativeBrowserState(), synth::runtime_ui::NodeIds::kFileBrowser,
         synth::pagestyle::kBrowserPanelFill, synth::pagestyle::kBrowserPanelBorder},
        {synth::runtime_ui::FilePageSnapshot{}, synth::runtime_ui::NodeIds::kFileIdleRegion,
         synth::pagestyle::kIdlePanelFill, synth::pagestyle::kIdlePanelBorder},
    };
    for (const ExpectedPanel& expected : panels)
    {
        const synth::ui::NodeTree tree =
            synth::runtime_ui::BuildFilePageTree(expected.state, {0.0f, 0.0f, 640.0f, 480.0f});
        const synth::ui::Node& panel = FindNode(tree, expected.id);
        Require(FindNodeById(tree, std::string(expected.id) + ".background") == nullptr,
                "File panels no longer emit out-of-flow Draw underlays");
        Require(panel.color == expected.fill, "File panel fill lives on the container");
        Require(panel.borderColor == expected.border, "File panel border colour lives on the container");
        Require(panel.borderWidth == std::optional<float>(synth::pagestyle::kPanelBorderWidth),
                "File panel border width lives on the container");
        Require(panel.cornerRadius == std::optional<float>(synth::pagestyle::kPanelCornerRadius),
                "File panel corner radius lives on the container");
    }
}

static void TestFilePageDelegatesItsListsToSplicedSubtrees()
{
    const std::string source =
        synth::test::ReadSourceFile("projects/synth/include/synth/RuntimePages.hpp");

    for (const std::string& functionName : {"BuildSyncPageTree", "BuildAudioPageTree", "BuildFilePageTree"})
    {
        Require(!FunctionBodyContains(source, functionName, "float y"),
                "rebuilt page construction declares layout instead of a y cursor");
        Require(!FunctionBodyContains(source, functionName, "y +="),
                "rebuilt page construction has no page-level y offset accumulation");
    }

    // The identity comparisons in the splice tests would also hold for an
    // inline construction that happened to produce the same nodes, so pin the
    // route too: the page splices both subtrees, and it cannot name a row of
    // either -- the row ids exist only inside the producers.
    Require(FunctionBodyContains(source, "BuildFilePageTree", "Splice(BuildPatchBrowserSubtree(snapshot))"),
            "the File page reaches its browser rows only through the spliced subtree");
    Require(FunctionBodyContains(source, "BuildFilePageTree", "Splice(BuildPatchVersionsSubtree(snapshot))"),
            "the File page reaches its version rows only through the spliced subtree");
    Require(!FunctionBodyContains(source, "BuildFilePageTree", "FileBrowserEntry"),
            "the File page names no browser row of its own");
    Require(!FunctionBodyContains(source, "BuildFilePageTree", "FileVersionEntry"),
            "the File page names no version row of its own");

    // Neither list producer may name an extent: what fits is the resolver's
    // decision, and both producers take state and nothing else.
    for (const std::string& producer : {"BuildPatchBrowserSubtree", "BuildPatchVersionsSubtree"})
    {
        Require(!FunctionBodyContains(source, producer, "bounds", "ui::Subtree"),
                "a spliced list producer names no bounds of its own");
        Require(!FunctionBodyContains(source, producer, "area", "ui::Subtree"),
                "a spliced list producer names no surface extent");
    }
    for (const std::string& rows : {"PatchBrowserRows", "PatchVersionRows"})
    {
        Require(!FunctionBodyContains(source, rows, "bounds", "std::vector<PageControls::ListRowSpec>"),
                "a list row producer names no bounds of its own");
    }
}

static void TestSidebarDeadlineNodeTextIsWholePercent()
{
    synth::runtime_ui::SidebarSnapshot snapshot;
    snapshot.deadlinePercent = 12.4f;
    const synth::ui::NodeTree tree = synth::runtime_ui::BuildSidebarTree(snapshot);
    const synth::ui::Node* deadline = FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarDeadline);
    Require(deadline != nullptr, "sidebar deadline node exists");
    Require(deadline->text == "CPU 12%",
            "the sidebar's own tree carries the whole-percent text, not a tenth");
}

// A control a page renders whose action the main component refuses to route is
// a dead button: it looks live, it clicks, and nothing happens. That shipped
// once -- `audio-input-permission` rendered and dispatched into nothing because
// the router kept its own copy of the action list. Each surface's list is one
// list now, and this walks the built page to prove the page cannot emit an
// action that list omits.
template <std::size_t N>
void RequireEveryEmittedActionIsRoutable(const synth::ui::NodeTree& tree,
                                         const std::string_view (&routed)[N],
                                         std::size_t leastEmitted,
                                         const char* unroutableLabel,
                                         const char* silentPageLabel)
{
    std::size_t emitted = 0;
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (!node.action.has_value() || node.action->name.empty())
        {
            continue;
        }
        ++emitted;
        bool routable = false;
        for (const std::string_view candidate : routed)
        {
            if (node.action->name == candidate)
            {
                routable = true;
                break;
            }
        }
        Require(routable, unroutableLabel);
    }
    // POSITIVE CONTROL: a page that emitted nothing would pass the loop above
    // without checking anything at all.
    Require(emitted >= leastEmitted, silentPageLabel);
}

void TestEveryRuntimePageActionIsRoutable()
{
    using namespace synth::runtime_ui;
    AudioPageSnapshot audio;
    audio.showInputCombo = true;
    audio.showInputRetry = true;
    audio.showInputPermissionRequest = true;
    audio.inputOptions = {{kNoInputOptionId, kNoInputOptionLabel}};
    audio.outputOptions = {{kSystemDefaultOptionId, kSystemDefaultOptionLabel}};
    RequireEveryEmittedActionIsRoutable(
        BuildAudioPageTree(audio, {0.0f, 0.0f, 900.0f, 560.0f}),
        Actions::kAudioActions,
        3,
        "the Audio page emits an action the main component will not route",
        "the Audio page under test emits its controls");

    SyncPageSnapshot sync;
    RequireEveryEmittedActionIsRoutable(
        BuildSyncPageTree(sync, {0.0f, 0.0f, 900.0f, 560.0f}),
        Actions::kSyncActions,
        3,
        "the Sync page emits an action the main component will not route",
        "the Sync page under test emits its controls");

    // Two File states, because the browser is the same page in another state
    // and a single snapshot cannot show both sets of controls at once.
    FilePageSnapshot file;
    file.hasCurrentPatch = true;
    RequireEveryEmittedActionIsRoutable(
        BuildFilePageTree(file, {0.0f, 0.0f, 900.0f, 560.0f}),
        Actions::kFileActions,
        3,
        "the File page emits an action the main component will not route",
        "the File page under test emits its controls");
    RequireEveryEmittedActionIsRoutable(
        BuildFilePageTree(RepresentativeBrowserState(), {0.0f, 0.0f, 900.0f, 560.0f}),
        Actions::kFileActions,
        3,
        "the File browser emits an action the main component will not route",
        "the File browser under test emits its controls");
}

int main()
{
    TestEveryRuntimePageActionIsRoutable();
    TestContainersNestToArbitraryDepth();
    TestComponentsComposeComponents();
    TestSpliceGraftsWithoutNestedRoot();
    TestRootlessSpliceAttachesForestRoots();
    TestRootlessScopeMarkerNeverEatsAProducerNode();
    TestSpliceMergesLayoutDeclarations();
    TestConstructionExpressesFullControlState();
    TestContainerConstructionCarriesAppearance();
    TestUnstyledNodesCarryNothing();
    TestCaptionIsAnEmittedLabelNodeNotAField();
    TestCaptionPlacementDefaultIsBeforeAndUnchanged();
    TestCaptionPlacementAfterEmitsCaptionAfterControl();
    TestComboBoxAcceptsRuntimeOptionVectors();
    TestSyncPageAlignsThroughTheFormGrid();
    TestSyncPageFitsWithinTheRuntimeRoot();
    TestAudioSelectorsAreCaptionedWhileADeviceIsSelected();
    TestHiddenInputSelectorLeavesNoOrphanedCaption();
    TestOfflineInputCaptureOffersACaptionedRetryRow();
    TestLiveInputCaptureHidesTheRetryRow();
    TestAudioPageWithNoAppSectionIsByteIdenticalToBeforeTheChange();
    TestAudioPageAppendsSuppliedSectionBeneathDeviceRowsWithinRemainingArea();
    TestAudioPageAppSectionNestedLayoutSurvivesTheSplice();
    TestPatchBrowserSplicesAsARootlessSubtree();
    TestPatchVersionsSplicesAsARootlessSubtree();
    TestSplicedListsKeepEveryEntryAtEveryExtent();
    TestLongListsKeepReadableRowsAndAReachableTail();
    TestFilePageFitsWithinTheRuntimeRoot();
    TestFilePagePinsItsResolvedGeometry();
    TestFileIdleRegionPinsItsResolvedGeometry();
    TestFilePageCarriesPageColoursAndTextStyles();
    TestFilePanelsCarryAppearanceWithoutUnderlays();
    TestFilePageDelegatesItsListsToSplicedSubtrees();
    TestSidebarDeadlineNodeTextIsWholePercent();
    TestEveryRebuiltPageAbsorbsAtTheSmallestDeclaredSurface();
    TestEveryPageAndAppResolvesAtTheSmallestDeclaredSurface();
    TestControllersWizardAndBraid4ResolveAtTheSmallestDeclaredSurface();
    TestNamedVisualCriteriaHoldOnEveryPageAndApp();
    TestControllersRowFitsWithinFroggersNarrowestHost();
    TestTheNamedCriteriaAreTheOnesThePlaywrightSuiteNames();
    TestTheWizardFormIsReachableRatherThanClipped();
    TestControllersChooserAndBraid4PinTheirAbsorbingRegions();

    TestGangedRandomLfoVisualizer();
    TestGangedRandomLfoBackgroundOptOut();
    TestScopeWaveformCommandsAreNodeLocal();
    TestEncoderDrawIsPositionIndependent();
    TestStandardModulatorVisualizersRemainPortable();
    TestBraid4StandardModulationViewsRemainPortable();
    synth::Parameter::UIState parameterState(1, 1, 1);
    parameterState.connected.store(true);
    parameterState.voiceCount.store(1);
    parameterState.baseColor.Store(synth::Color::Red);
    parameterState.indicatorColors[0].Store(synth::Color::Blue);
    parameterState.modulatorsAffectingMask.store(1u);
    parameterState.gesturesAffectingMask.store(1u);
    parameterState.modulatorColorCount.store(1);
    parameterState.modulatorSourceColors[0].Store(synth::Color::Cyan);
    parameterState.gestureColorCount.store(1);
    parameterState.gestureColors[0].Store(synth::Color::Orange);
    const synth::ui::EncoderDrawState snapshotEncoder =
        synth::ui::EncoderDrawStateFromParameter(parameterState);
    Require(snapshotEncoder.baseColor == synth::Color::Red, "encoder uses snapshot base color");
    Require(snapshotEncoder.voices[0].indicatorColor == synth::Color::Blue,
            "encoder uses snapshot voice-zero indicator color");
    Require(snapshotEncoder.modulatorColors == std::vector<synth::Color>{synth::Color::Cyan},
            "encoder uses snapshot source badge colors");
    Require(snapshotEncoder.gestureColors == std::vector<synth::Color>{synth::Color::Orange},
            "encoder uses snapshot gesture badge colors");

    Require(synth::ui::EncoderGeometry::BadgeText(false, 16) == "17", "gesture 16 badge is one-based");
    Require(synth::ui::EncoderGeometry::BadgeText(false, 62) == "63", "gesture 62 badge is one-based");
    Require(synth::ui::EncoderGeometry::BadgeText(false, 63) == "64", "gesture 63 badge is one-based");
    synth::Parameter::UIState highGestureState(1, 0, 64);
    highGestureState.connected.store(true);
    highGestureState.voiceCount.store(1);
    highGestureState.gesturesAffectingMask.store(std::uint64_t{1} << 63);
    highGestureState.gestureColorCount.store(64);
    for (std::size_t gestureIx = 0; gestureIx < 64; ++gestureIx) {
        highGestureState.gestureColors[gestureIx].Store(synth::Color::Orange);
    }
    const synth::ui::EncoderDrawState highGestureEncoder =
        synth::ui::EncoderDrawStateFromParameter(highGestureState);
    Require(highGestureEncoder.gesturesAffectingMask == (std::uint64_t{1} << 63),
            "encoder snapshot preserves gesture bit 63");
    const auto highGestureCommands = synth::ui::BuildEncoderDrawCommands(
        highGestureEncoder, {0.0f, 0.0f, 128.0f, 128.0f});
    Require(std::any_of(highGestureCommands.begin(), highGestureCommands.end(), [](const auto& command) {
                return command.kind == synth::ui::DrawCommand::Kind::Text && command.text == "64";
            }),
            "encoder renders gesture 63 as badge 64");

    static_assert(synth::SynthApplication<TestApp>);
    static_assert(!synth::ui::kPortableUiUsesJuce);
    static_assert(std::is_same_v<decltype(synth::ui::WaveformLayerDrawState::scope), const synth::ScopeWriter*>);
    static_assert(!std::is_copy_constructible_v<synth::ui::Visualizer>);
    static_assert(!std::is_copy_assignable_v<synth::ui::Visualizer>);
    static_assert(!std::is_move_constructible_v<synth::ui::Visualizer>);
    static_assert(!std::is_move_assignable_v<synth::ui::Visualizer>);
    TestVisualizer visualizer;
    Require(visualizer.Visible(), "visualizer is visible by default");
    visualizer.SetBounds({11.0f, 12.0f, 44.0f, 45.0f});
    RequireNear(visualizer.GetBounds().x, 11.0f, 0.0001f, "visualizer stores bounds x");
    RequireNear(visualizer.GetBounds().height, 45.0f, 0.0001f, "visualizer stores bounds height");
    Require(visualizer.Draw().size() == 1, "visible visualizer emits commands");
    visualizer.SetVisible(false);
    Require(visualizer.Draw().empty(), "hidden visualizer emits no commands");
    visualizer.SetVisible(true);
    synth::ui::Builder visualizerBuilder;
    visualizerBuilder.Root("viz.root", {0.0f, 0.0f, 100.0f, 100.0f})
        .Visualizer("viz.node", &visualizer, {});
    const synth::ui::NodeTree visualizerTree = visualizerBuilder.Build({0.0f, 0.0f, 100.0f, 100.0f});
    const synth::ui::Node* visualizerNode = FindNodeById(visualizerTree, "viz.node");
    Require(visualizerNode != nullptr, "visible visualizer node exists");
    Require(visualizerNode->kind == synth::ui::NodeKind::Draw, "visualizer node is a draw node");
    RequireNear(visualizerNode->bounds.width, 44.0f, 0.0001f, "visualizer node uses stored bounds");
    Require(visualizerNode->drawCommands.size() == 1, "visualizer node uses draw commands");
    visualizer.SetVisible(false);
    synth::ui::Builder hiddenBuilder;
    hiddenBuilder.Root("viz.hidden.root", {0.0f, 0.0f, 100.0f, 100.0f})
        .Visualizer("viz.hidden.node", &visualizer, {});
    Require(FindNodeById(hiddenBuilder.Build({0.0f, 0.0f, 100.0f, 100.0f}), "viz.hidden.node") == nullptr, "hidden visualizer node absent");

    {
        const std::array<float, 4> values{0.0f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f};
        synth::ui::ConstantBarVisualizer visualizer(values, synth::Color::Yellow);
        const synth::ui::Bounds bounds{10.0f, 20.0f, 80.0f, 120.0f};
        visualizer.SetBounds(bounds);
        const auto commands = visualizer.Draw();
        const synth::ui::Bounds nodeExtent{0.0f, 0.0f, bounds.width, bounds.height};
        Require(!visualizer.WantsEncoderFrame(),
                "constant visualizer suppresses the shared encoder frame");
        Require(commands.size() == values.size(), "constant visualizer emits one command per voice");
        const float slotWidth = bounds.width / static_cast<float>(values.size());
        const float gap = std::min(2.0f, slotWidth * 0.2f);
        const float expectedBarWidth = (slotWidth - gap) * 0.5f;
        for (std::size_t voice = 0; voice < values.size(); ++voice) {
            Require(commands[voice].kind == synth::ui::DrawCommand::Kind::Fill,
                    "constant visualizer emits only filled rectangles");
            Require(commands[voice].color == synth::Color::Yellow,
                    "constant visualizer retains source color");
            Require(commands[voice].bounds.width > 0.0f,
                    "constant visualizer keeps positive bar width");
            RequireNear(commands[voice].bounds.width, expectedBarWidth, 0.0001f,
                        "constant visualizer halves the post-gap bar width");
            RequireNear(commands[voice].bounds.x + commands[voice].bounds.width * 0.5f,
                        (static_cast<float>(voice) + 0.5f) * slotWidth,
                        0.0001f,
                        "constant visualizer centers each narrow bar in its voice slot");
            Require(commands[voice].bounds.x >= nodeExtent.x &&
                    commands[voice].bounds.x + commands[voice].bounds.width <= nodeExtent.x + nodeExtent.width,
                    "constant visualizer bar stays horizontally bounded");
            RequireNear(commands[voice].bounds.y + commands[voice].bounds.height,
                        nodeExtent.height, 0.0001f,
                        "constant visualizer bars share the bottom edge");
        }
        RequireNear(commands[0].bounds.height, bounds.height / 12.0f, 0.0001f,
                    "zero voice remains visible");
        RequireNear(commands[3].bounds.y, nodeExtent.height / 12.0f, 0.0001f,
                    "one voice keeps a top margin");
        Require(commands[1].bounds.height > commands[2].bounds.height,
                "bar heights retain voice-order values without sorting");
        const auto repeated = visualizer.Draw();
        for (std::size_t voice = 0; voice < values.size(); ++voice) {
            RequireNear(repeated[voice].bounds.y, commands[voice].bounds.y, 0.0001f,
                        "immutable repeated draw keeps bar geometry");
        }
    }

    {
        const std::array<float, 2> values{0.0f, 1.0f};
        synth::ui::ConstantBarVisualizer narrow(values, synth::Color::Cyan);
        narrow.SetBounds({1.0f, 2.0f, 0.5f, 3.0f});
        const auto commands = narrow.Draw();
        Require(commands.size() == 2, "narrow constant visualizer retains both bars");
        Require(commands[0].bounds.width > 0.0f && commands[1].bounds.width > 0.0f,
                "slot-relative gaps preserve positive width");

        const std::span<const float> empty;
        synth::ui::ConstantBarVisualizer emptyVisualizer(empty, synth::Color::White);
        emptyVisualizer.SetBounds({0.0f, 0.0f, 10.0f, 10.0f});
        Require(emptyVisualizer.Draw().empty(), "empty constant values draw nothing");
        for (const synth::ui::Bounds invalid : {
                 synth::ui::Bounds{0.0f, 0.0f, 0.0f, 1.0f},
                 synth::ui::Bounds{0.0f, 0.0f, 1.0f, 0.0f},
                 synth::ui::Bounds{0.0f, 0.0f, -1.0f, 1.0f},
                 synth::ui::Bounds{0.0f, 0.0f, std::numeric_limits<float>::infinity(), 1.0f},
                 synth::ui::Bounds{std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f}}) {
            narrow.SetBounds(invalid);
            Require(narrow.Draw().empty(), "invalid constant visualizer bounds are safe");
        }
    }

    {
        static_assert(!std::is_copy_constructible_v<synth::ui::ConstantBarVisualizer>);
        static_assert(!std::is_copy_assignable_v<synth::ui::ConstantBarVisualizer>);
        static_assert(!std::is_move_constructible_v<synth::ui::ConstantBarVisualizer>);
        static_assert(!std::is_move_assignable_v<synth::ui::ConstantBarVisualizer>);

        const std::array<float, 2> stackValues{0.0f, 1.0f};
        synth::ui::ConstantBarVisualizer stackingVisualizer(stackValues, synth::Color::Yellow);
        const synth::ui::Bounds stackBounds{20.0f, 20.0f, 64.0f, 64.0f};
        stackingVisualizer.SetBounds(stackBounds);
        synth::ui::Visualizer* const stableAddress = &stackingVisualizer;
        synth::ui::Builder builder;
        builder.Root("constant.stack.root", {0.0f, 0.0f, 100.0f, 100.0f})
            .Visualizer("constant.stack.visualizer", &stackingVisualizer, {})
            .DrawInteractive("constant.stack.encoder", stackBounds,
                             {synth::ui::DrawCommand::StrokeEllipse(
                                 stackBounds, synth::Color::White, 1.0f)},
                             synth::ui::Action::Named("drag"), std::nullopt, {});
        const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 100.0f, 100.0f});
        const synth::ui::Node* root = FindNodeById(tree, "constant.stack.root");
        const synth::ui::Node* node = FindNodeById(tree, "constant.stack.visualizer");
        Require(stableAddress == static_cast<synth::ui::Visualizer*>(&stackingVisualizer),
                "constant visualizer remains address-stable");
        Require(node != nullptr && node->drawCommands.size() == stackValues.size(),
                "constant builder carries one bar per voice");
        RequireNear(node->bounds.width, stackBounds.width, 0.0001f,
                    "constant builder preserves bounds");
        Require(root != nullptr && root->children.size() == 2,
                "constant visualizer and encoder are both appended");
        Require(root->children[0] == synth::ui::NodeId("constant.stack.visualizer"),
                "constant visualizer precedes encoder");
        Require(root->children[1] == synth::ui::NodeId("constant.stack.encoder"),
                "constant encoder follows visualizer");
        stackingVisualizer.SetVisible(false);
        synth::ui::Builder hiddenBuilder;
        hiddenBuilder.Root("constant.hidden.root", {0.0f, 0.0f, 100.0f, 100.0f})
            .Visualizer("constant.hidden.visualizer", &stackingVisualizer, {});
        Require(FindNodeById(hiddenBuilder.Build({0.0f, 0.0f, 100.0f, 100.0f}), "constant.hidden.visualizer") == nullptr,
                "hidden constant visualizer emits no builder node");
    }

    {
        synth::ui::NoiseWaveformVisualizer left(synth::Color::Yellow, 1234);
        synth::ui::NoiseWaveformVisualizer right(synth::Color::Yellow, 1234);
        const synth::ui::Bounds bounds{10.0f, 20.0f, 64.0f, 30.0f};
        left.SetBounds(bounds);
        right.SetBounds(bounds);
        const auto leftCommands = left.Draw();
        const auto rightCommands = right.Draw();
        const synth::ui::Bounds nodeExtent{0.0f, 0.0f, bounds.width, bounds.height};
        Require(leftCommands.size() == 1, "noise visualizer emits one polyline");
        Require(rightCommands.size() == 1, "same seed emits one noise polyline");
        Require(leftCommands[0].kind == synth::ui::DrawCommand::Kind::Polyline,
                "noise visualizer command is a polyline");
        Require(leftCommands[0].color == synth::Color::Yellow,
                "noise visualizer retains its color");
        Require(leftCommands[0].points.size() == 65,
                "noise visualizer covers integer columns including both edges");
        Require(leftCommands[0].points.size() == rightCommands[0].points.size(),
                "same seed produces same point count");
        for (std::size_t point = 0; point < leftCommands[0].points.size(); ++point)
        {
            RequireNear(leftCommands[0].points[point].x,
                        static_cast<float>(point), 0.0001f,
                        "noise visualizer x matches integer column");
            RequireNear(leftCommands[0].points[point].x, rightCommands[0].points[point].x,
                        0.0001f, "same seed reproduces x");
            RequireNear(leftCommands[0].points[point].y, rightCommands[0].points[point].y,
                        0.0001f, "same seed reproduces y");
            Require(leftCommands[0].points[point].y > nodeExtent.y,
                    "noise visualizer y is above the open lower edge");
            Require(leftCommands[0].points[point].y < nodeExtent.y + nodeExtent.height,
                    "noise visualizer y is below the open upper edge");
        }
    }

    {
        synth::ui::NoiseWaveformVisualizer visualizer(synth::Color::White, 99);
        visualizer.SetBounds({0.0f, 0.0f, 16.0f, 10.0f});
        const auto first = visualizer.Draw();
        const auto second = visualizer.Draw();
        Require(first.size() == 1 && second.size() == 1,
                "consecutive visible noise draws emit one polyline each");
        bool differs = false;
        for (std::size_t point = 0; point < first[0].points.size(); ++point)
        {
            differs = differs || first[0].points[point].y != second[0].points[point].y;
        }
        Require(differs, "noise visualizer regenerates geometry on every visible draw");
    }

    {
        synth::ui::NoiseWaveformVisualizer visualizer(synth::Color::White, 7);
        visualizer.SetBounds({3.0f, 4.0f, 8.0f, 9.0f});
        Require(visualizer.Visible(), "noise visualizer is intrinsically visible");
        Require(!visualizer.Draw().empty(), "visible noise visualizer draws");
        visualizer.SetVisible(false);
        Require(visualizer.Draw().empty(), "hidden noise visualizer does not draw");
        visualizer.SetVisible(true);
        for (const synth::ui::Bounds invalid : {
                 synth::ui::Bounds{0.0f, 0.0f, 0.0f, 1.0f},
                 synth::ui::Bounds{0.0f, 0.0f, 1.0f, 0.0f},
                 synth::ui::Bounds{0.0f, 0.0f, -1.0f, 1.0f},
                 synth::ui::Bounds{0.0f, 0.0f, std::numeric_limits<float>::infinity(), 1.0f},
                 synth::ui::Bounds{0.0f, 0.0f, 1.0f, std::numeric_limits<float>::infinity()},
                 synth::ui::Bounds{std::numeric_limits<float>::quiet_NaN(), 0.0f, 1.0f, 1.0f},
                 synth::ui::Bounds{0.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f, 1.0f}})
        {
            visualizer.SetBounds(invalid);
            Require(visualizer.Draw().empty(), "invalid noise visualizer bounds are safe");
        }
    }

    {
        synth::ui::NoiseWaveformVisualizer visualizer(synth::Color::Cyan, 123);
        const synth::ui::Bounds bounds{5.5f, 8.0f, 2.25f, 12.0f};
        visualizer.SetBounds(bounds);
        const auto commands = visualizer.Draw();
        const synth::ui::Bounds nodeExtent{0.0f, 0.0f, bounds.width, bounds.height};
        Require(commands.size() == 1, "fractional-width noise visualizer emits one polyline");
        Require(commands[0].points.size() == 4,
                "fractional-width noise visualizer covers columns and right edge");
        const std::array<float, 4> expectedX{0.0f, 1.0f, 2.0f, bounds.width};
        for (std::size_t point = 0; point < expectedX.size(); ++point)
        {
            RequireNear(commands[0].points[point].x, expectedX[point], 0.0001f,
                        "fractional-width noise visualizer x matches expected coverage");
            Require(PointInside(commands[0].points[point], nodeExtent),
                    "fractional-width noise visualizer point stays in bounds");
            if (point > 0)
            {
                Require(commands[0].points[point - 1].x < commands[0].points[point].x,
                        "fractional-width noise visualizer x coordinates are distinct");
            }
        }
    }

    {
        static_assert(!std::is_copy_constructible_v<synth::ui::NoiseWaveformVisualizer>);
        static_assert(!std::is_copy_assignable_v<synth::ui::NoiseWaveformVisualizer>);
        static_assert(!std::is_move_constructible_v<synth::ui::NoiseWaveformVisualizer>);
        static_assert(!std::is_move_assignable_v<synth::ui::NoiseWaveformVisualizer>);

        synth::ui::NoiseWaveformVisualizer visualizer(synth::Color::Orange, 456);
        const synth::ui::Bounds bounds{14.0f, 15.0f, 24.0f, 25.0f};
        visualizer.SetBounds(bounds);
        synth::ui::Visualizer* const stableAddress = &visualizer;
        synth::ui::Builder builder;
        builder.Root("noise.stack.root", {0.0f, 0.0f, 100.0f, 100.0f})
            .Visualizer("noise.stack.visualizer", &visualizer, {})
            .DrawInteractive("noise.stack.encoder", bounds,
                             {synth::ui::DrawCommand::StrokeEllipse(
                                 {0.0f, 0.0f, bounds.width, bounds.height}, synth::Color::White, 1.0f)},
                             synth::ui::Action::Named("drag"), std::nullopt, {});
        const synth::ui::NodeTree tree = builder.Build({0.0f, 0.0f, 100.0f, 100.0f});
        Require(stableAddress == static_cast<synth::ui::Visualizer*>(&visualizer),
                "noise visualizer remains address-stable through builder composition");
        const synth::ui::Node* root = FindNodeById(tree, "noise.stack.root");
        const synth::ui::Node* node = FindNodeById(tree, "noise.stack.visualizer");
        Require(node != nullptr, "noise visualizer builder emits a draw node");
        RequireNear(node->bounds.x, bounds.x, 0.0001f, "noise builder preserves bounds x");
        RequireNear(node->bounds.y, bounds.y, 0.0001f, "noise builder preserves bounds y");
        RequireNear(node->bounds.width, bounds.width, 0.0001f, "noise builder preserves bounds width");
        RequireNear(node->bounds.height, bounds.height, 0.0001f, "noise builder preserves bounds height");
        Require(root != nullptr && root->children.size() == 2,
                "noise visualizer and encoder are both appended");
        Require(root->children[0] == synth::ui::NodeId("noise.stack.visualizer"),
                "noise visualizer draw node precedes encoder node");
        Require(root->children[1] == synth::ui::NodeId("noise.stack.encoder"),
                "noise encoder node follows visualizer draw node");

        visualizer.SetVisible(false);
        synth::ui::Builder hiddenBuilder;
        hiddenBuilder.Root("noise.hidden.root", {0.0f, 0.0f, 100.0f, 100.0f})
            .Visualizer("noise.hidden.visualizer", &visualizer, {});
        Require(FindNodeById(hiddenBuilder.Build({0.0f, 0.0f, 100.0f, 100.0f}), "noise.hidden.visualizer") == nullptr,
                "hidden noise visualizer emits no builder node");
    }

    synth::ScopeWriter scope(4, 128);
    FillScopeWriter(scope, 4);
    std::vector<synth::ui::WaveformLayerDrawState> waveformLayers{
        {.connected = true, .scopeColor = synth::Color::Red, .scope = &scope, .scopeChannel = 0},
        {.connected = true, .scopeColor = synth::Color::Cyan, .scope = &scope, .scopeChannel = 1},
        {.connected = false, .scopeColor = synth::Color::Green, .scope = &scope, .scopeChannel = 2},
    };
    const auto leftWaveform = synth::ui::BuildScopeWaveformCommands(
        waveformLayers, {10.0f, 20.0f, 180.0f, 90.0f}, -1.1f, 1.1f, 64, true);
    const auto rightWaveform = synth::ui::BuildScopeWaveformCommands(
        waveformLayers, {240.0f, 20.0f, 180.0f, 90.0f}, -1.1f, 1.1f, 64, true);
    RequireWaveformGeometryInside(leftWaveform, {0.0f, 0.0f, 180.0f, 90.0f},
                                  "left waveform geometry stays inside bounds");
    RequireWaveformGeometryInside(rightWaveform, {0.0f, 0.0f, 180.0f, 90.0f},
                                  "right waveform geometry stays inside bounds");

    synth::ScopeWriter inFlightScope(1, 128);
    auto inFlightHolder = inFlightScope.ReserveChans(1);
    inFlightHolder.RecordStart();
    for (std::size_t frame = 0; frame < 32; ++frame)
    {
        inFlightHolder.Write(std::sin(static_cast<float>(frame) * 0.2f));
        inFlightScope.AdvanceIndex();
    }
    inFlightHolder.RecordStart();
    for (std::size_t frame = 0; frame < 16; ++frame)
    {
        inFlightHolder.Write(std::sin(static_cast<float>(frame) * 0.2f));
        inFlightScope.AdvanceIndex();
    }
    inFlightScope.Publish();

    const std::vector<synth::ui::WaveformLayerDrawState> inFlightLayer{
        {.connected = true, .scopeColor = synth::Color::Red, .scope = &inFlightScope, .scopeChannel = 0},
    };
    const auto publishedCommands = synth::ui::BuildScopeWaveformCommands(
        inFlightLayer, {10.0f, 120.0f, 180.0f, 90.0f}, -1.1f, 1.1f, 64, true);
    const bool publishedHasPolyline = std::any_of(
        publishedCommands.begin(), publishedCommands.end(), [](const synth::ui::DrawCommand& command) {
            return command.kind == synth::ui::DrawCommand::Kind::Polyline;
        });
    inFlightHolder.RecordStart();
    inFlightHolder.Write(0.25f);
    inFlightScope.AdvanceIndex();
    const auto inFlightCommands = synth::ui::BuildScopeWaveformCommands(
        inFlightLayer, {10.0f, 120.0f, 180.0f, 90.0f}, -1.1f, 1.1f, 64, true);
    const bool inFlightHasPolyline = std::any_of(
        inFlightCommands.begin(), inFlightCommands.end(), [](const synth::ui::DrawCommand& command) {
            return command.kind == synth::ui::DrawCommand::Kind::Polyline;
        });
    Require(publishedHasPolyline, "published scope waveform is visible before the next marker");
    Require(inFlightHasPolyline, "scope waveform remains visible while a new marker is unpublished");

    for (int cell = 0; cell < 4; ++cell)
    {
        std::vector<synth::ui::WaveformLayerDrawState> singleLayer{
            {.connected = true,
             .scopeColor = cell % 2 == 0 ? synth::Color::Yellow : synth::Color::Blue,
             .scope = &scope,
             .scopeChannel = static_cast<std::size_t>(cell)},
        };
        const synth::ui::Bounds cellBounds{
            12.0f + static_cast<float>(cell % 2) * 160.0f,
            160.0f + static_cast<float>(cell / 2) * 120.0f,
            140.0f,
            100.0f,
        };
        const auto commands = synth::ui::BuildScopeWaveformCommands(singleLayer, cellBounds, -1.1f, 1.1f, 64, true);
        RequireWaveformGeometryInside(commands, {0.0f, 0.0f, cellBounds.width, cellBounds.height},
                                      "quad waveform geometry stays inside its cell");
    }

    synth_miniapp::VcoWaveformDrawState miniVcoState;
    miniVcoState.layers = waveformLayers;
    const synth::ui::Bounds wrapperBounds{30.0f, 300.0f, 240.0f, 120.0f};
    const auto sharedVco = synth::ui::BuildScopeWaveformCommands(
        waveformLayers,
        wrapperBounds,
        synth_miniapp::VcoWaveformDrawState::x_MinY,
        synth_miniapp::VcoWaveformDrawState::x_MaxY,
        synth_miniapp::VcoWaveformDrawState::x_NumSamples,
        true);
    const auto miniVco = synth_miniapp::BuildVcoWaveformCommands(miniVcoState, wrapperBounds);
    Require(sharedVco.size() == miniVco.size(), "miniapp vco wrapper command count matches shared helper");
    for (std::size_t i = 0; i < sharedVco.size(); ++i)
    {
        Require(sharedVco[i].kind == miniVco[i].kind, "miniapp vco wrapper command kind matches shared helper");
        Require(sharedVco[i].color.r == miniVco[i].color.r && sharedVco[i].color.g == miniVco[i].color.g &&
                    sharedVco[i].color.b == miniVco[i].color.b && sharedVco[i].color.a == miniVco[i].color.a,
                "miniapp vco wrapper colors match shared helper");
    }

    synth_miniapp::LfoWaveformDrawState miniLfoState;
    miniLfoState.layers = {{.connected = true, .scopeColor = synth::Color::Orange, .scope = &scope, .scopeChannel = 0}};
    const auto sharedLfo = synth::ui::BuildScopeWaveformCommands(
        miniLfoState.layers,
        wrapperBounds,
        synth_miniapp::LfoWaveformDrawState::x_MinY,
        synth_miniapp::LfoWaveformDrawState::x_MaxY,
        synth_miniapp::LfoWaveformDrawState::x_NumSamples,
        true);
    const auto miniLfo = synth_miniapp::BuildLfoWaveformCommands(miniLfoState, wrapperBounds);
    Require(sharedLfo.size() == miniLfo.size(), "miniapp lfo wrapper command count matches shared helper");
    Require(sharedLfo.front().bounds.width == miniLfo.front().bounds.width &&
                sharedLfo.front().bounds.height == miniLfo.front().bounds.height,
            "miniapp lfo wrapper fill bounds match shared helper");

    TestScopeLayerState layerA;
    TestScopeLayerState layerB;
    layerA.connected.store(true);
    layerA.scope.store(&scope);
    layerA.scopeChannel.store(0);
    layerA.scopeColor.Store(synth::Color::Red);
    layerB.connected.store(false);
    layerB.scope.store(&scope);
    layerB.scopeChannel.store(1);
    layerB.scopeColor.Store(synth::Color::Green);
    std::array<TestScopeLayerState*, 2> scopeLayers{&layerA, &layerB};
    synth::ui::ScopeVisualizer<TestScopeLayerState> scopeVisualizer(scopeLayers, -1.1f, 1.1f, 64, true);
    scopeVisualizer.SetBounds({50.0f, 60.0f, 140.0f, 90.0f});
    const auto scopeVisualizerCommands = scopeVisualizer.Draw();
    RequireWaveformGeometryInside(scopeVisualizerCommands, {0.0f, 0.0f, 140.0f, 90.0f},
                                  "scope visualizer geometry stays inside bounds");
    const bool sawRed = std::any_of(scopeVisualizerCommands.begin(), scopeVisualizerCommands.end(),
                                    [](const synth::ui::DrawCommand& command) {
                                        return command.kind == synth::ui::DrawCommand::Kind::Polyline &&
                                               command.color == synth::Color::Red;
                                    });
    Require(sawRed, "scope visualizer reads connected layer color");
    const bool sawGreen = std::any_of(scopeVisualizerCommands.begin(), scopeVisualizerCommands.end(),
                                      [](const synth::ui::DrawCommand& command) {
                                          return command.kind == synth::ui::DrawCommand::Kind::Polyline &&
                                                 command.color == synth::Color::Green;
                                      });
    Require(!sawGreen, "scope visualizer skips disconnected layer");
    layerA.scopeChannel.store(1);
    layerA.scopeColor.Store(synth::Color::Yellow);
    const auto updatedScopeCommands = scopeVisualizer.Draw();
    const bool sawYellow = std::any_of(updatedScopeCommands.begin(), updatedScopeCommands.end(),
                                       [](const synth::ui::DrawCommand& command) {
                                           return command.kind == synth::ui::DrawCommand::Kind::Polyline &&
                                                  command.color == synth::Color::Yellow;
                                       });
    Require(sawYellow, "scope visualizer reads updated atomic color without reconstruction");

    Require(synth_braid4::Braid4NodeIds::kRoot == std::string("braid4.root"),
            "braid4 root stable id");
    Require(synth_braid4::Braid4NodeIds::VcoScope(0) == "braid4.scope.vco.0",
            "braid4 vco scope zero stable id");
    Require(synth_braid4::Braid4NodeIds::VcoScope(3) == "braid4.scope.vco.3",
            "braid4 vco scope three stable id");
    Require(synth_braid4::Braid4NodeIds::LfoScope(0) == "braid4.scope.lfo.0",
            "braid4 lfo scope zero stable id");
    Require(synth_braid4::Braid4NodeIds::LfoScope(3) == "braid4.scope.lfo.3",
            "braid4 lfo scope three stable id");
    Require(synth_braid4::Braid4NodeIds::Encoder(0) == "braid4.encoder.0",
            "braid4 encoder zero stable id");
    Require(synth_braid4::Braid4NodeIds::Encoder(15) == "braid4.encoder.15",
            "braid4 encoder fifteen stable id");
    Require(synth_braid4::Braid4NodeIds::SceneButton(0) == "braid4.scene.0",
            "braid4 scene zero stable id");
    Require(synth_braid4::Braid4NodeIds::SceneButton(1) == "braid4.scene.1",
            "braid4 scene one stable id");
    Require(synth_braid4::Braid4NodeIds::kSceneBlend == std::string("braid4.scene.blend"),
            "braid4 scene blend stable id");

    const synth::ui::Bounds braidRoot = synth_braid4::Braid4PageLayout::RootBounds(nullptr);
    RequireNear(braidRoot.width, 900.0f, 0.0001f, "braid4 default width");
    RequireNear(braidRoot.height, 560.0f, 0.0001f, "braid4 default height");

    // Braid 4 no longer computes region or cell geometry: every one of these
    // bounds is resolved by the standard application layout, so the claim to
    // pin is that the resolved tree contains each node inside its parent.
    synth_braid4::Braid4UiSurface braidSurface;
    braidSurface.Attach(nullptr, nullptr);
    const synth::ui::NodeTree braidTree = braidSurface.BuildTree();
    for (std::size_t scopeIx = 0; scopeIx < synth_braid4::Braid4ScopeGridLayout::kScopeCount; ++scopeIx)
    {
        RequireNodeContainedInParent(braidTree, synth_braid4::Braid4NodeIds::VcoScope(scopeIx));
        RequireNodeContainedInParent(braidTree, synth_braid4::Braid4NodeIds::LfoScope(scopeIx));
    }
    for (std::size_t encoderIx = 0; encoderIx < synth_braid4::Braid4EncoderGridLayout::kEncoderCount; ++encoderIx)
    {
        RequireNodeContainedInParent(braidTree, synth_braid4::Braid4NodeIds::Encoder(encoderIx));
    }
    for (const char* region : {"braid4.title", "braid4.body", "braid4.visualizers", "braid4.slot.upper",
                               "braid4.slot.lower", "braid4.encoders", "braid4.bay"})
    {
        RequireNodeContainedInParent(braidTree, region);
    }
    Require(BoundsInside(FindNodeById(braidTree, "braid4.page")->bounds, braidRoot),
            "braid4 page stays inside the default root extent");

    const auto disconnectedEncoderCommands = synth::ui::BuildEncoderDrawCommands(
        synth::ui::EncoderDrawState{.connected = false},
        {10.0f, 10.0f, 92.0f, 92.0f});
    Require(disconnectedEncoderCommands.empty(), "braid4 disconnected encoder uses shared empty encoder state");

    synth::ui::EncoderDrawState ordinaryEncoder;
    ordinaryEncoder.connected = true;
    ordinaryEncoder.baseColor = synth::Color::Cyan;
    ordinaryEncoder.voiceCount = 1;
    ordinaryEncoder.voices.push_back({.value = 0.5f, .indicatorColor = synth::Color::Orange});
    const auto ordinaryEncoderCommands = synth::ui::BuildEncoderDrawCommands(
        ordinaryEncoder,
        {10.0f, 10.0f, 92.0f, 92.0f});
    Require(ordinaryEncoderCommands.size() > 4, "ordinary connected encoder emits body and value commands");
    Require(ordinaryEncoderCommands[0].kind == synth::ui::DrawCommand::Kind::FillEllipse,
            "ordinary encoder first command is body fill");
    Require(ordinaryEncoderCommands[0].color.a == 255,
            "ordinary encoder body fill remains opaque");

    synth::ui::EncoderDrawState underlayEncoder = ordinaryEncoder;
    underlayEncoder.hasVisualizerUnderlay = true;
    const auto underlayEncoderCommands = synth::ui::BuildEncoderDrawCommands(
        underlayEncoder,
        {10.0f, 10.0f, 92.0f, 92.0f});
    Require(underlayEncoderCommands.size() == ordinaryEncoderCommands.size(),
            "underlay encoder preserves command count");
    Require(underlayEncoderCommands[0].kind == synth::ui::DrawCommand::Kind::FillEllipse,
            "underlay encoder first command is body fill");
    Require(underlayEncoderCommands[0].color.a > 0 && underlayEncoderCommands[0].color.a < 255,
            "underlay encoder body fill is translucent");
    Require(underlayEncoderCommands[1].color.a < ordinaryEncoderCommands[1].color.a,
            "underlay encoder inner color fill is also softened");
    Require(std::any_of(underlayEncoderCommands.begin(), underlayEncoderCommands.end(),
                        [](const synth::ui::DrawCommand& command) {
                            return command.kind == synth::ui::DrawCommand::Kind::StrokeRoundedRect;
                        }),
            "default visualizer underlay retains the rounded encoder frame");
    for (std::size_t commandIx = 2; commandIx < ordinaryEncoderCommands.size(); ++commandIx)
    {
        Require(underlayEncoderCommands[commandIx].kind == ordinaryEncoderCommands[commandIx].kind,
                "underlay encoder preserves non-body command kinds");
        Require(underlayEncoderCommands[commandIx].color == ordinaryEncoderCommands[commandIx].color,
                "underlay encoder preserves non-body command colors");
        RequireNear(underlayEncoderCommands[commandIx].strokeWidth,
                    ordinaryEncoderCommands[commandIx].strokeWidth,
                    0.0001f,
                    "underlay encoder preserves non-body stroke widths");
    }

    synth::ui::EncoderDrawState framelessUnderlayEncoder = underlayEncoder;
    framelessUnderlayEncoder.wantsFrame = false;
    const auto framelessUnderlayCommands = synth::ui::BuildEncoderDrawCommands(
        framelessUnderlayEncoder,
        {10.0f, 10.0f, 92.0f, 92.0f});
    Require(framelessUnderlayCommands.size() + 1 == underlayEncoderCommands.size(),
            "frameless visualizer removes exactly one encoder command");
    Require(std::none_of(framelessUnderlayCommands.begin(), framelessUnderlayCommands.end(),
                         [](const synth::ui::DrawCommand& command) {
                             return command.kind == synth::ui::DrawCommand::Kind::StrokeRoundedRect;
                         }),
            "frameless visualizer removes the rounded encoder frame");

    const std::vector<synth::ui::WaveformLayerDrawState> braidScopeLayer{
        {.connected = true, .scopeColor = synth::Color::Red, .scope = &scope, .scopeChannel = 0},
    };
    const synth::ui::Bounds braidScopeBounds{80.0f, 80.0f, 180.0f, 96.0f};
    const auto sharedBraidScope = synth::ui::BuildScopeWaveformCommands(
        braidScopeLayer,
        braidScopeBounds,
        synth_braid4::Braid4ScopeDrawState::x_MinY,
        synth_braid4::Braid4ScopeDrawState::x_MaxY,
        synth_braid4::Braid4ScopeDrawState::x_NumSamples,
        true);
    const auto wrappedBraidScope = synth_braid4::BuildBraid4ScopeCommands(
        synth_braid4::Braid4ScopeDrawState{.layers = braidScopeLayer},
        braidScopeBounds);
    Require(sharedBraidScope.size() == wrappedBraidScope.size(),
            "braid4 waveform wrapper uses shared scope helper");

    synth::ui::Builder builder;
    builder.Root("root", synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f})
        .Label("title", "Synth Params", {})
        .Button("start", "Start", synth::ui::Action::Named("start"), {})
        .Toggle("gesture", "Gesture", true, synth::ui::Action::Named("gesture.toggle"), {})
        .Slider("blend", "Blend", 0.25f, 0.0f, 1.0f, 0.001f, synth::ui::Action::Named("blend.set"), {})
        .ComboBox("device", {{"a", "Built In"}, {"b", "External"}}, "a",
                  synth::ui::Action::Named("device.select"), {})
        .TextField("value", "Value", "64", synth::ui::Action::Named("value.commit"), {})
        .Draw("scope", synth::ui::Bounds{10.0f, 10.0f, 100.0f, 80.0f},
              {synth::ui::DrawCommand::Fill(synth::Color::Rgb(24, 26, 28)),
               synth::ui::DrawCommand::Line({0.0f, 0.0f}, {100.0f, 80.0f}, synth::Color::Rgb(255, 255, 255), 1.0f)}, {});

    const synth::ui::NodeTree tree = builder.Build(synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f});
    Require(tree.nodes.size() == 8, "tree should contain root plus seven children");
    Require(tree.nodes[0].id == synth::ui::NodeId("root"), "root id");
    Require(tree.nodes[7].drawCommands.size() == 2, "draw commands");

    TestVisualizer stackingVisualizer;
    stackingVisualizer.SetBounds({20.0f, 20.0f, 64.0f, 64.0f});
    synth::ui::Builder stackingBuilder;
    stackingBuilder.Root("stack.root", {0.0f, 0.0f, 120.0f, 120.0f})
        .Visualizer("stack.encoder.0.visualizer", &stackingVisualizer, {})
        .DrawInteractive("stack.encoder.0",
                         stackingVisualizer.GetBounds(),
                         {synth::ui::DrawCommand::StrokeEllipse(
                             {0.0f,
                              0.0f,
                              stackingVisualizer.GetBounds().width,
                              stackingVisualizer.GetBounds().height},
                             synth::Color::White,
                             1.0f)},
                         synth::ui::Action::Named("drag"),
                         synth::ui::Action::Named("push"), {});
    const synth::ui::NodeTree stackingTree = stackingBuilder.Build({0.0f, 0.0f, 120.0f, 120.0f});
    const synth::ui::Node* stackingRoot = FindNodeById(stackingTree, "stack.root");
    Require(stackingRoot != nullptr, "stacking root exists");
    Require(stackingRoot->children.size() == 2, "visualizer and encoder both appended");
    Require(stackingRoot->children[0] == synth::ui::NodeId("stack.encoder.0.visualizer"),
            "visualizer precedes encoder");
    Require(stackingRoot->children[1] == synth::ui::NodeId("stack.encoder.0"),
            "encoder follows visualizer");
    const synth::ui::Node* stackingEncoder = FindNodeById(stackingTree, "stack.encoder.0");
    Require(stackingEncoder != nullptr && stackingEncoder->pointerDragAction.has_value(),
            "encoder retains drag action");
    Require(stackingEncoder != nullptr && stackingEncoder->doubleClickAction.has_value(),
            "encoder retains double-click action");

    synth::runtime_ui::SidebarSnapshot sidebarSnapshot;
    sidebarSnapshot.deadlinePercent = 12.4f;
    const synth::ui::NodeTree sidebarTree = synth::runtime_ui::BuildSidebarTree(sidebarSnapshot);
    Require(FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarAudio) != nullptr, "sidebar audio node");
    Require(FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarControllers) != nullptr,
            "sidebar controllers node");
    Require(FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarSync) != nullptr,
            "sidebar sync node");
    Require(FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarFile) != nullptr, "sidebar file node");
    Require(FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarDeadline) != nullptr,
            "sidebar deadline node");
    const synth::ui::Node* deadlineNode = FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarDeadline);
    Require(deadlineNode->text == "CPU 12%", "deadline readout text");
    // Re-pinned, not loosened, when the sidebar moved onto the library (7.1):
    // the Controllers entry is now a row so its warning badge can be an
    // out-of-flow overlay in the row's own space, so the root's second child is
    // that row rather than the button. Everything else is identical, and the
    // resolved geometry below is pinned exactly where the hand-assembled
    // version put it.
    Require(sidebarTree.nodes.front().children.size() == 5,
            "sidebar contains Audio, Controllers, Sync, File, deadline");
    Require(sidebarTree.nodes.front().children[0] ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarAudio) &&
                sidebarTree.nodes.front().children[1] ==
                synth::ui::NodeId(std::string(synth::runtime_ui::NodeIds::kSidebarControllers) +
                                  ".row") &&
                sidebarTree.nodes.front().children[2] ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarSync) &&
                sidebarTree.nodes.front().children[3] ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarFile) &&
                sidebarTree.nodes.front().children[4] ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarDeadline),
            "sidebar order is fixed");
    Require(sidebarTree.nodes.front().bounds.height == 200.0f,
            "sidebar root is five fixed 40 px rows high");
    {
        // The resolver stacks the five rows exactly where the hand-rolled
        // `kSidebarButtonHeight * index` arithmetic did. This is the assertion
        // that makes the rebuild a no-op on screen rather than a claim that it
        // is one.
        const char* const stacked[] = {synth::runtime_ui::NodeIds::kSidebarAudio,
                                       nullptr,
                                       synth::runtime_ui::NodeIds::kSidebarSync,
                                       synth::runtime_ui::NodeIds::kSidebarFile,
                                       synth::runtime_ui::NodeIds::kSidebarDeadline};
        for (std::size_t rowIx = 0; rowIx < 5; ++rowIx)
        {
            const std::string id =
                stacked[rowIx] != nullptr
                    ? std::string(stacked[rowIx])
                    : std::string(synth::runtime_ui::NodeIds::kSidebarControllers) + ".row";
            const synth::ui::Node* row = FindNodeById(sidebarTree, id.c_str());
            Require(row != nullptr, "every sidebar row resolves");
            RequireNear(row->bounds.x, 0.0f, 0.0001f, "sidebar rows are flush left");
            RequireNear(row->bounds.y,
                        synth::runtime_ui::Layout::kSidebarButtonHeight *
                            static_cast<float>(rowIx),
                        0.0001f,
                        "sidebar rows stack one button height apart");
            RequireNear(row->bounds.width,
                        synth::runtime_ui::Layout::kSidebarWidth,
                        0.0001f,
                        "sidebar rows are the full sidebar width");
            RequireNear(row->bounds.height,
                        synth::runtime_ui::Layout::kSidebarButtonHeight,
                        0.0001f,
                        "sidebar rows are one button height tall");
        }
        const synth::ui::Node* controllersButton =
            FindNodeById(sidebarTree, synth::runtime_ui::NodeIds::kSidebarControllers);
        Require(controllersButton != nullptr, "the Controllers button resolves inside its row");
        RequireNear(controllersButton->bounds.width,
                    synth::runtime_ui::Layout::kSidebarWidth,
                    0.0001f,
                    "the Controllers button still fills its row, badge or no badge");

        synth::runtime_ui::SidebarSnapshot warned;
        warned.controllersWarning = true;
        const synth::ui::NodeTree warnedTree = synth::runtime_ui::BuildSidebarTree(warned);
        const synth::ui::Node* badge =
            FindNodeById(warnedTree, synth::runtime_ui::NodeIds::kSidebarControllersWarning);
        Require(badge != nullptr, "the warning badge is emitted when discovery has a candidate");
        RequireNear(badge->bounds.x,
                    synth::runtime_ui::Layout::kSidebarWidth -
                        synth::runtime_ui::Layout::kWarningBadgeTrailingInset,
                    0.0001f,
                    "the badge keeps its trailing-edge inset");
        RequireNear(badge->bounds.y, 0.0f, 0.0001f,
                    "the badge is positioned in its row's space, not the root's");
        const synth::ui::Node* warnedRow = FindNodeById(
            warnedTree,
            (std::string(synth::runtime_ui::NodeIds::kSidebarControllers) + ".row").c_str());
        Require(warnedRow != nullptr, "the Controllers row survives the badge");
        RequireNear(warnedRow->bounds.y,
                    synth::runtime_ui::Layout::kSidebarButtonHeight,
                    0.0001f,
                    "an out-of-flow badge consumes no stacking space");
        Require(badge->bounds.x + badge->bounds.width <= warnedRow->bounds.width + 0.0001f,
                "the badge sits inside the button it annotates");
    }

    synth::runtime_ui::AudioPageSnapshot audioSnapshot;
    audioSnapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Speakers", "Headphones"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    audioSnapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Mic"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    Require(synth::runtime_ui::Layout::SelectedDeviceOptionId(
                "Headphones", audioSnapshot.outputOptions, synth::runtime_ui::kSystemDefaultOptionId) ==
                "Headphones",
            "known audio device option stays selected");
    Require(synth::runtime_ui::Layout::SelectedDeviceOptionId(
                "Vanished Device", audioSnapshot.outputOptions, synth::runtime_ui::kSystemDefaultOptionId) ==
                synth::runtime_ui::kSystemDefaultOptionId,
            "unknown audio device option falls back to system default");
    audioSnapshot.selectedOutputId = "Speakers";
    audioSnapshot.selectedInputId = synth::runtime_ui::kSystemDefaultOptionId;
    audioSnapshot.showInputCombo = true;
    audioSnapshot.deviceLineText = "Speakers: 48000 Hz, 512 frames";
    audioSnapshot.statusLineText = "Audio: Speakers";
    const synth::ui::NodeTree audioTree =
        synth::runtime_ui::BuildAudioPageTree(audioSnapshot, synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f});
    Require(FindNodeById(audioTree, synth::runtime_ui::NodeIds::kAudioBack) != nullptr, "audio back node");
    Require(FindNodeById(audioTree, synth::runtime_ui::NodeIds::kAudioOutput) != nullptr, "audio output node");
    Require(FindNodeById(audioTree, synth::runtime_ui::NodeIds::kAudioInput) != nullptr, "audio input node");
    Require(FindNodeById(audioTree, synth::runtime_ui::NodeIds::kAudioDeviceLine) != nullptr, "audio device line");
    Require(FindNodeById(audioTree, synth::runtime_ui::NodeIds::kAudioStatusLine) != nullptr, "audio status line");

    {
        const synth::ui::Bounds originZero{0.0f, 0.0f, 640.0f, 480.0f};
        const synth::ui::Bounds shifted{12.0f, 6.0f, 640.0f, 480.0f};
        const synth::ui::NodeTree audioOriginZero =
            synth::runtime_ui::BuildAudioPageTree(audioSnapshot, originZero);
        const synth::ui::NodeTree audioShifted =
            synth::runtime_ui::BuildAudioPageTree(audioSnapshot, shifted);
        const synth::ui::Node* audioBackZero =
            FindNodeById(audioOriginZero, synth::runtime_ui::NodeIds::kAudioBack);
        const synth::ui::Node* audioBackShifted =
            FindNodeById(audioShifted, synth::runtime_ui::NodeIds::kAudioBack);
        Require(audioBackZero != nullptr && audioBackShifted != nullptr,
                "audio back exists for origin-zero and shifted areas");
        Require(audioBackZero->bounds.x == audioBackShifted->bounds.x &&
                    audioBackZero->bounds.y == audioBackShifted->bounds.y &&
                    audioBackZero->bounds.width == audioBackShifted->bounds.width &&
                    audioBackZero->bounds.height == audioBackShifted->bounds.height,
                "audio root-level children are root-relative, not area-origin absolute");

        synth::runtime_ui::SyncPageSnapshot syncSnapshot;
        const synth::ui::NodeTree syncOriginZero =
            synth::runtime_ui::BuildSyncPageTree(syncSnapshot, originZero);
        const synth::ui::NodeTree syncShifted =
            synth::runtime_ui::BuildSyncPageTree(syncSnapshot, shifted);
        const synth::ui::Node* syncBackZero =
            FindNodeById(syncOriginZero, synth::runtime_ui::NodeIds::kSyncBack);
        const synth::ui::Node* syncBackShifted =
            FindNodeById(syncShifted, synth::runtime_ui::NodeIds::kSyncBack);
        const synth::ui::Node* syncSendZero =
            FindNodeById(syncOriginZero, synth::runtime_ui::NodeIds::kSyncSendClock);
        const synth::ui::Node* syncSendShifted =
            FindNodeById(syncShifted, synth::runtime_ui::NodeIds::kSyncSendClock);
        Require(syncBackZero != nullptr && syncBackShifted != nullptr &&
                    syncSendZero != nullptr && syncSendShifted != nullptr,
                "sync root-level children exist for origin-zero and shifted areas");
        Require(syncBackZero->bounds.x == syncBackShifted->bounds.x &&
                    syncBackZero->bounds.y == syncBackShifted->bounds.y &&
                    syncBackZero->bounds.width == syncBackShifted->bounds.width &&
                    syncBackZero->bounds.height == syncBackShifted->bounds.height,
                "sync root-level children are root-relative, not area-origin absolute");
        Require(syncSendZero->bounds.y == syncSendShifted->bounds.y,
                "sync send-clock y is root-relative");
        Require(syncSendZero->bounds.height == syncSendShifted->bounds.height,
                "sync remaining-height allocation ignores the area origin");

        synth::MidiInstrumentConfig controllerInstrument;
        synth::MidiConnectionState controllerConnection;
        synth::runtime_ui::ControllersPageCallbacks controllerCallbacks;
        controllerCallbacks.instrumentSnapshot = [&controllerInstrument] {
            return controllerInstrument;
        };
        controllerCallbacks.connectionState = [&controllerConnection] {
            return controllerConnection;
        };
        synth::runtime_ui::ControllersPageSurface controllersSurface(std::move(controllerCallbacks));
        controllersSurface.SetContentBounds(originZero);
        const synth::ui::NodeTree controllersOriginZero = controllersSurface.BuildTree();
        controllersSurface.SetContentBounds(shifted);
        const synth::ui::NodeTree controllersShifted = controllersSurface.BuildTree();
        const synth::ui::Node* controllersBackZero =
            FindNodeById(controllersOriginZero, synth::runtime_ui::NodeIds::kBack);
        const synth::ui::Node* controllersBackShifted =
            FindNodeById(controllersShifted, synth::runtime_ui::NodeIds::kBack);
        const synth::ui::Node* controllersScrollZero =
            FindNodeById(controllersOriginZero, synth::runtime_ui::NodeIds::kScroll);
        const synth::ui::Node* controllersScrollShifted =
            FindNodeById(controllersShifted, synth::runtime_ui::NodeIds::kScroll);
        Require(controllersBackZero != nullptr && controllersBackShifted != nullptr &&
                    controllersScrollZero != nullptr && controllersScrollShifted != nullptr,
                "controllers root-level children exist for origin-zero and shifted areas");
        Require(controllersBackZero->bounds.x == controllersBackShifted->bounds.x &&
                    controllersBackZero->bounds.y == controllersBackShifted->bounds.y &&
                    controllersBackZero->bounds.width == controllersBackShifted->bounds.width &&
                    controllersBackZero->bounds.height == controllersBackShifted->bounds.height,
                "controllers root-level children are root-relative, not area-origin absolute");
        Require(controllersScrollZero->bounds.y == controllersScrollShifted->bounds.y,
                "controllers scroll y is root-relative");
        Require(controllersScrollZero->bounds.height == controllersScrollShifted->bounds.height,
                "controllers scrollBottom ignores the area origin");
    }

    synth::runtime_ui::FilePageSnapshot fileSnapshot;
    fileSnapshot.patchNameText = "my_patch";
    fileSnapshot.statusText = "Save requested";
    fileSnapshot.hasCurrentPatch = true;
    const synth::ui::NodeTree fileTree =
        synth::runtime_ui::BuildFilePageTree(fileSnapshot, synth::ui::Bounds{0.0f, 0.0f, 640.0f, 480.0f});
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileBack) != nullptr, "file back node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileNew) != nullptr, "file new node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileSave) != nullptr, "file save node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileSaveAs) != nullptr, "file save as node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileLoad) != nullptr, "file load node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileRevert) != nullptr, "file revert node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFilePatchName) != nullptr, "file patch name node");
    Require(FindNodeById(fileTree, synth::runtime_ui::NodeIds::kFileStatus) != nullptr, "file status node");

    synth::runtime_ui::SidebarSurface sidebarSurface;
    sidebarSurface.SetDeadlinePercent(3.0f);
    const synth::ui::NodeTree sidebarBuilt = sidebarSurface.BuildTree();
    Require(FindNodeById(sidebarBuilt, synth::runtime_ui::NodeIds::kSidebarDeadline)->text == "CPU 3%",
            "sidebar surface deadline refresh");

    synth::runtime_ui::AudioPageSurface audioSurface;
    audioSurface.Snapshot() = audioSnapshot;
    audioSurface.SetContentBounds({0.0f, 0.0f, 640.0f, 480.0f});
    Require(audioSurface.BuildTree().nodes.size() >= 5, "audio surface builds semantic tree");

    synth::runtime_ui::SyncPageSurface syncSurface;
    syncSurface.SetContentBounds({0.0f, 0.0f, 240.0f, 560.0f});
    syncSurface.BeginEdit({});
    const synth::ui::NodeTree defaultSyncTree = syncSurface.BuildTree();
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncBack) != nullptr,
            "sync back node");
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncSendClock)->kind ==
                synth::ui::NodeKind::Toggle,
            "sync send-clock is a portable toggle");
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncReceiveClock)->kind ==
                synth::ui::NodeKind::Toggle,
            "sync receive-clock is a portable toggle");
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncSendTransport)->kind ==
                synth::ui::NodeKind::Toggle,
            "sync send-transport is a portable toggle");
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncReceiveTransport)->kind ==
                synth::ui::NodeKind::Toggle,
            "sync receive-transport is a portable toggle");
    Require(FindNodeById(defaultSyncTree, synth::runtime_ui::NodeIds::kSyncPpqn)->kind ==
                synth::ui::NodeKind::TextField,
            "sync PPQN is a portable text field");
    Require(syncSurface.StagedConfiguration() == synth::SyncConfig{},
            "sync defaults are four false flags and PPQN 24");

    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncSendClock, "1"));
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncReceiveClock, "1"));
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncSendTransport, "1"));
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncReceiveTransport, "1"));
    Require(syncSurface.StagedConfiguration() == synth::SyncConfig{true, true, true, true, 24},
            "all four exact toggle actions update staged state");
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncSendClock, "unexpected"));
    Require(syncSurface.StagedConfiguration().sendClock,
            "toggle ignores values outside backend 1/0 contract");

    const std::vector<std::string> invalidPpqn = {
        "", "12x", "12.0", "1e2", "+24", "-1", "0", "961", " 24", "24 ",
        "999999999999999999999999999999999999"};
    for (const std::string& invalid : invalidPpqn)
    {
        syncSurface.DispatchAction(synth::ui::Action::WithValue(
            synth::runtime_ui::Actions::kSyncPpqn, invalid));
        Require(syncSurface.StagedConfiguration().ppqn == 24,
                "invalid PPQN retains prior valid value");
        Require(!syncSurface.ValidationText().empty(), "invalid PPQN shows inline validation");
    }
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncPpqn, "1"));
    Require(syncSurface.StagedConfiguration().ppqn == 1 &&
                syncSurface.ValidationText().empty(),
            "PPQN lower boundary is accepted and clears validation");
    Require(syncSurface.WarningText().find("nonstandard") != std::string::npos,
            "non-24 PPQN shows peer compatibility warning");
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncPpqn, "960"));
    Require(syncSurface.StagedConfiguration().ppqn == 960,
            "PPQN upper boundary is accepted");
    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncPpqn, "24"));
    Require(syncSurface.WarningText().empty(), "standard PPQN 24 clears warning");

    syncSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kSyncPpqn, "96"));
    const synth::SyncConfig stagedBeforeRefresh = syncSurface.StagedConfiguration();
    syncSurface.RefreshStatus({.currentBpm = 119.75,
                               .lockState = "FreeRun",
                               .sourceName = "Clock Controller",
                               .outputLatencyMicros = 7'500,
                               .ignoredInputCount = 9,
                               .lateEventCount = 10,
                               .droppedOutputCount = 11});
    Require(syncSurface.StagedConfiguration() == stagedBeforeRefresh,
            "diagnostic refresh never overwrites staged edits");
    const synth::ui::NodeTree statusTree = syncSurface.BuildTree();
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncBpm)->text ==
                "Current BPM: 119.75",
            "sync current BPM has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncLock)->text ==
                "Lock: FreeRun",
            "sync lock has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncSource)->text ==
                "Source: Clock Controller",
            "sync source has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncOutputLatency)->text ==
                "Output latency: 7.500 ms",
            "sync output latency has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncIgnoredInput)->text ==
                "Ignored input: 9",
            "sync ignored-input count has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncLateEvents)->text ==
                "Late events: 10",
            "sync late-event count has stable human-readable label");
    Require(FindNodeById(statusTree, synth::runtime_ui::NodeIds::kSyncDroppedOutput)->text ==
                "Dropped output: 11",
            "sync dropped-output count has stable human-readable label");
    for (const synth::ui::Node& node : statusTree.nodes)
    {
        Require(node.bounds.x >= 0.0f && node.bounds.y >= 0.0f &&
                    node.bounds.x + node.bounds.width <= 240.0f &&
                    node.bounds.y + node.bounds.height <= 560.0f,
                "all sync nodes remain within narrow content bounds");
    }

    synth::runtime_ui::FilePageSurface fileSurface;
    fileSurface.Snapshot() = fileSnapshot;
    const std::filesystem::path patchRoot =
        std::filesystem::temp_directory_path() / "sheaf_portable_file_page_test";
    std::filesystem::remove_all(patchRoot);
    std::filesystem::create_directories(patchRoot / "PatchA");
    const std::filesystem::path canonicalPatchRoot = std::filesystem::weakly_canonical(patchRoot);
    fileSurface.Snapshot().patchesRoot = patchRoot.string();
    fileSurface.SetContentBounds({0.0f, 0.0f, 640.0f, 480.0f});
    fileSurface.SetStatus("Ready");
    Require(FindNodeById(fileSurface.BuildTree(), synth::runtime_ui::NodeIds::kFileStatus)->text == "Ready",
            "file surface status refresh");

    synth::ui::Action lastFileAction;
    fileSurface.SetActionHandler([&lastFileAction](const synth::ui::Action& action) {
        lastFileAction = action;
    });
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSaveAs));
    synth::ui::NodeTree saveAsTree = fileSurface.BuildTree();
    Require(FindNodeById(saveAsTree, synth::runtime_ui::NodeIds::kFileBrowser) != nullptr,
            "save-as browser opens in file page tree");
    Require(FindNodeById(saveAsTree, synth::runtime_ui::NodeIds::kFileBrowserSaveName) != nullptr,
            "save-as browser exposes patch-name field");
    RequireBrowserIsRootlessDescendant(saveAsTree);
    {
        const synth::ui::Node& browser = FindNode(saveAsTree, synth::runtime_ui::NodeIds::kFileBrowser);
        const synth::ui::Node& firstEntry =
            FindNode(saveAsTree, synth::runtime_ui::NodeIds::FileBrowserEntry(0));
        const synth::ui::Node& list = FindNode(saveAsTree, synth::runtime_ui::NodeIds::kFileBrowserList);
        RequireNear(browser.bounds.x, 10.0f, 0.0001f, "file browser remains placed in root space");
        // The rows moved one level down into the browser's scrolling list, so
        // the panel-relative 12 is now the list's and the rows sit at the
        // list's own origin. Both are pinned rather than one.
        RequireNear(list.bounds.x, 12.0f, 0.0001f, "file browser list x is parent-relative");
        RequireNear(firstEntry.bounds.x, 0.0f, 0.0001f, "file browser row x is list-relative");
        Require(list.bounds.y < browser.bounds.y,
                "file browser list y is inside the browser section, not surface-absolute");
        Require(firstEntry.bounds.y < browser.bounds.y,
                "file browser row y is inside the scrolling list, not surface-absolute");
        Require(list.bounds.x + list.bounds.width <= browser.bounds.width,
                "file browser list width fits inside its parent-relative section");
        Require(firstEntry.bounds.x + firstEntry.bounds.width <= list.bounds.width,
                "file browser row width fits inside its parent-relative list");
    }
    Require(fileSurface.Snapshot().browserEntries.size() == 1, "save-as browser lists one patch directory");
    Require(fileSurface.Snapshot().browserEntries[0].name == "PatchA", "save-as browser lists deterministic patch name");

    fileSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kFileBrowserSaveName, "../Outside"));
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(lastFileAction.name.empty(), "invalid save-as target does not dispatch");
    Require(fileSurface.Snapshot().browserOpen, "invalid save-as target keeps browser open");

    fileSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kFileBrowserSaveName, "PatchA"));
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(lastFileAction.name.empty(), "existing save-as target does not dispatch");
    Require(fileSurface.Snapshot().browserOpen, "existing save-as target keeps browser open");
    Require(fileSurface.Snapshot().statusText.find("exists") != std::string::npos,
            "existing save-as target reports exists status");

    std::ofstream(patchRoot / "PatchFile").put('x');
    fileSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kFileBrowserSaveName, "PatchFile"));
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(lastFileAction.name.empty(), "existing save-as file target does not dispatch");
    Require(fileSurface.Snapshot().browserOpen, "existing save-as file target keeps browser open");
    Require(fileSurface.Snapshot().statusText.find("exists") != std::string::npos,
            "existing save-as file target reports exists status");

    fileSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kFileBrowserSaveName, "New Patch"));
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(lastFileAction.name == synth::runtime_ui::Actions::kFileConfirmedSaveAs,
            "save-as browser confirms with resolved path action");
    Require(lastFileAction.value == (canonicalPatchRoot / "New Patch").string(),
            "save-as path resolves under patch root");

    lastFileAction = {};
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));
    synth::ui::NodeTree loadTree = fileSurface.BuildTree();
    Require(FindNodeById(loadTree, synth::runtime_ui::NodeIds::FileBrowserEntry(0).c_str()) != nullptr,
            "load browser lists patch directory");
    RequireBrowserIsRootlessDescendant(loadTree);
    fileSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kFileBrowserSelect, "0"));
    Require(!fileSurface.Snapshot().browserEntries.empty() && fileSurface.Snapshot().browserEntries[0].selected,
            "load browser exposes selected row state");
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(lastFileAction.name == synth::runtime_ui::Actions::kFileConfirmedLoad,
            "load browser confirms with resolved path action");
    Require(lastFileAction.value == (canonicalPatchRoot / "PatchA").string(),
            "load path resolves selected patch directory");

    lastFileAction = {};
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserCancel));
    Require(lastFileAction.name.empty(), "browser cancel closes without dispatch");
    Require(!fileSurface.Snapshot().browserOpen, "browser cancel closes browser");

    std::filesystem::create_directories(patchRoot / "Beta" / "Nested");
    lastFileAction = {};
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));
    Require(fileSurface.Snapshot().browserEntries.size() == 2, "load browser lists deterministic entries");
    Require(fileSurface.Snapshot().browserEntries[0].name == "Beta", "load browser orders beta first");
    Require(fileSurface.Snapshot().browserEntries[1].name == "PatchA", "load browser orders patch second");
    const synth::ui::NodeTree flatLoadTree = fileSurface.BuildTree();
    Require(FindNodeById(flatLoadTree, synth::runtime_ui::NodeIds::kFileBrowserParent) == nullptr,
            "flat browser has no parent button");
    Require(FindNodeById(flatLoadTree, synth::runtime_ui::NodeIds::FileBrowserEntryOpen(0)) == nullptr,
            "flat browser has no open button");
    const synth::ui::Node* firstLoadRow =
        FindNodeById(flatLoadTree, synth::runtime_ui::NodeIds::FileBrowserEntry(0));
    Require(firstLoadRow != nullptr && firstLoadRow->doubleClickAction.has_value(),
            "load row exposes double-click action");
    fileSurface.DispatchAction(*firstLoadRow->doubleClickAction);
    Require(lastFileAction.name == synth::runtime_ui::Actions::kFileConfirmedLoad,
            "load row double-click confirms selected patch");
    Require(lastFileAction.value == (canonicalPatchRoot / "Beta").string(),
            "load row double-click dispatches row patch directory");

    lastFileAction = {};
    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSaveAs));
    const synth::ui::NodeTree saveOverwriteTree = fileSurface.BuildTree();
    const synth::ui::Node* firstSaveRow =
        FindNodeById(saveOverwriteTree, synth::runtime_ui::NodeIds::FileBrowserEntry(0));
    Require(firstSaveRow != nullptr && firstSaveRow->doubleClickAction.has_value(),
            "save-as row exposes double-click overwrite action");
    fileSurface.DispatchAction(*firstSaveRow->doubleClickAction);
    Require(lastFileAction.name == synth::runtime_ui::Actions::kFileConfirmedOverwriteSaveAs,
            "save-as row double-click confirms overwrite save-as");
    Require(lastFileAction.value == (canonicalPatchRoot / "Beta").string(),
            "save-as row double-click dispatches existing patch directory");

    synth::runtime_ui::FilePageSurface versionsSurface;
    versionsSurface.Snapshot().patchesRoot = patchRoot.string();
    versionsSurface.Snapshot().hasCurrentPatch = true;
    versionsSurface.Snapshot().patchNameText = "PatchA";
    {
        std::ofstream(patchRoot / "PatchA" / "20240101T010101Z-000.json").put('1');
        std::ofstream(patchRoot / "PatchA" / "20240202T020202Z-000.json").put('2');
    }
    const synth::ui::NodeTree versionsTree = versionsSurface.BuildTree();
    Require(FindNodeById(versionsTree, synth::runtime_ui::NodeIds::kFileVersions) != nullptr,
            "current patch shows versions section");
    {
        const synth::ui::Node& idle = FindNode(versionsTree, synth::runtime_ui::NodeIds::kFileIdleRegion);
        const synth::ui::Node& versions = FindNode(versionsTree, synth::runtime_ui::NodeIds::kFileVersions);
        const synth::ui::Node& versionsTitle = FindNode(versionsTree, synth::runtime_ui::NodeIds::kFileVersionsTitle);
        RequireNear(versions.bounds.x, 12.0f, 0.0001f, "file versions section x is idle-relative");
        Require(versions.bounds.y < idle.bounds.y,
                "file versions section y is inside the idle region, not surface-absolute");
        RequireNear(versionsTitle.bounds.x, 0.0f, 0.0001f, "file versions title x is versions-relative");
        RequireNear(versionsTitle.bounds.y, 0.0f, 0.0001f, "file versions title y is versions-relative");
    }
    const synth::ui::Node* newestVersion =
        FindNodeById(versionsTree, synth::runtime_ui::NodeIds::FileVersionEntry(0));
    // A version row's user-visible string is its `label`: both backends render
    // a Button's label, and the rebuilt page no longer duplicates it into the
    // unread `text` field the hand-rolled construction also set.
    Require(newestVersion != nullptr && newestVersion->label.find("20240202") != std::string::npos,
            "versions list is newest first");
    Require(newestVersion->doubleClickAction.has_value(), "version row exposes double-click load action");
    synth::ui::Action versionAction;
    versionsSurface.SetActionHandler([&versionAction](const synth::ui::Action& action) {
        versionAction = action;
    });
    versionsSurface.DispatchAction(*newestVersion->doubleClickAction);
    Require(versionAction.name == synth::runtime_ui::Actions::kFileConfirmedLoad,
            "version double-click dispatches load");
    Require(versionAction.value == (patchRoot / "PatchA" / "20240202T020202Z-000.json").string(),
            "version double-click dispatches exact version file");

    std::filesystem::remove_all(patchRoot);

    synth::runtime_ui::FilePageSurface emptyLoadSurface;
    const std::filesystem::path emptyRoot =
        std::filesystem::temp_directory_path() / "sheaf_portable_file_page_empty_load_test";
    std::filesystem::remove_all(emptyRoot);
    std::filesystem::create_directories(emptyRoot);
    emptyLoadSurface.Snapshot().patchesRoot = emptyRoot.string();
    synth::ui::Action emptyLoadAction;
    emptyLoadSurface.SetActionHandler([&emptyLoadAction](const synth::ui::Action& action) {
        emptyLoadAction = action;
    });
    emptyLoadSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));
    emptyLoadSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileBrowserConfirm));
    Require(emptyLoadAction.name.empty(), "missing load selection does not dispatch");
    Require(emptyLoadSurface.Snapshot().browserOpen, "missing load selection keeps browser open");
    std::filesystem::remove_all(emptyRoot);

    synth::runtime_ui::FilePageSurface firstSaveSurface;
    const std::filesystem::path firstSaveRoot =
        std::filesystem::temp_directory_path() / "sheaf_portable_file_page_first_save_test";
    std::filesystem::remove_all(firstSaveRoot);
    std::filesystem::create_directories(firstSaveRoot);
    firstSaveSurface.Snapshot().patchesRoot = firstSaveRoot.string();
    synth::ui::Action firstSaveAction;
    firstSaveSurface.SetActionHandler([&firstSaveAction](const synth::ui::Action& action) {
        firstSaveAction = action;
    });
    firstSaveSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSave));
    Require(firstSaveAction.name.empty(), "first save opens browser without dispatch");
    Require(firstSaveSurface.Snapshot().browserOpen, "first save opens save-as browser");
    Require(firstSaveSurface.Snapshot().browserKind == synth::runtime_ui::FileBrowserKind::SaveAs,
            "first save uses save-as browser kind");
    Require(FindNodeById(firstSaveSurface.BuildTree(), synth::runtime_ui::NodeIds::kFileBrowserSaveName) != nullptr,
            "first save exposes save name field");
    std::filesystem::remove_all(firstSaveRoot);

    synth::MidiInstrumentConfig controllerInstrument;
    synth::MidiControllerSlot wrldSlot;
    wrldSlot.name = "wrld";
    wrldSlot.kind = synth::MidiProfileKind::WrldBldr;
    wrldSlot.config = synth::WrldBldrDefaultProfileConfig();
    Require(controllerInstrument.AddController(std::move(wrldSlot)), "add wrld controller");
    synth::MidiConnectionState controllerConnection;
    controllerConnection.controllers.push_back({});

    synth::runtime_ui::ControllersPageCallbacks controllerCallbacks;
    controllerCallbacks.instrumentSnapshot = [&controllerInstrument] { return controllerInstrument; };
    controllerCallbacks.connectionState = [&controllerConnection] { return controllerConnection; };
    synth::runtime_ui::ControllersPageSurface controllersSurface(std::move(controllerCallbacks));
    controllersSurface.SetContentBounds({0.0f, 0.0f, 800.0f, 600.0f});
    controllersSurface.MarkDirty();
    controllersSurface.RefreshOnTick();
    Require(controllersSurface.BuildTree().nodes.size() >= 4, "controllers surface builds semantic tree");
    controllersSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kToggleConfig, "0"));
    controllersSurface.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kToggleSection, "0:encoders"));
    controllersSurface.MarkDirty();
    controllersSurface.RefreshOnTick();
    const synth::ui::NodeTree controllersExpandedTree = controllersSurface.BuildTree();
    for (const synth::ui::Node& node : controllersExpandedTree.nodes)
    {
        const std::string& id = node.id.value;
        if (node.kind == synth::ui::NodeKind::Row &&
            id.find(".header.") != std::string::npos &&
            id.find(".caption") == std::string::npos)
        {
            Require(!node.children.empty(),
                    ("controllers group header row " + id + " must not reserve an empty band").c_str());
        }
    }

    synth::WizardCandidate twisterCandidate{
        .wizardId = "com.sheaf.midi-fighter-twister",
        .displayName = "MIDI Fighter Twister",
        .kind = synth::MidiProfileKind::MfTwister,
        .input = {"twister-in", "Midi Fighter Twister"},
        .output = {"twister-out", "Midi Fighter Twister"}};
    controllersSurface.SetDiscovery({.available = {twisterCandidate}});
    const synth::ui::NodeTree launchTree = controllersSurface.BuildTree();
    const synth::ui::Node* wizardLaunch = FindNodeById(
        launchTree, synth::runtime_ui::NodeIds::kWizardLaunch);
    Require(wizardLaunch != nullptr && wizardLaunch->enabled && wizardLaunch->action.has_value() &&
                wizardLaunch->action->name == synth::runtime_ui::Actions::kWizardOpen,
            "portable Controllers page exposes the enabled wizard launch action");
    controllersSurface.DispatchAction(*wizardLaunch->action);
    const synth::ui::NodeTree wizardTree = controllersSurface.BuildTree();
    const synth::ui::Node* wizardRoot = FindNodeById(
        wizardTree, synth::runtime_ui::NodeIds::kWizardForm);
    const std::string wizardBody = std::string(synth::runtime_ui::NodeIds::kWizardForm) + ".body";
    const std::string wizardActions =
        std::string(synth::runtime_ui::NodeIds::kWizardForm) + ".actions";
    Require(CountRootNodes(wizardTree) == 1 &&
                wizardRoot != nullptr &&
                NodeHasChild(wizardRoot, synth::ui::NodeId(wizardBody)) &&
                IsDescendantOf(wizardTree, "controller-wizard.twister.body", wizardBody) &&
                IsDescendantOf(wizardTree, "controller-wizard.twister.encoder-slot", wizardBody) &&
                NodeHasChild(FindNodeById(wizardTree, wizardActions),
                             synth::ui::NodeId(synth::runtime_ui::NodeIds::kWizardSubmit)) &&
                NodeHasChild(FindNodeById(wizardTree, wizardActions),
                             synth::ui::NodeId(synth::runtime_ui::NodeIds::kWizardIgnore)),
            "portable wizard session composes the form and workflow actions into one tree");

    return 0;
}
