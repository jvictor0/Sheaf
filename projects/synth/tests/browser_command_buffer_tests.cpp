#include "synth/PortableUI.hpp"
#include "synth/RuntimePages.hpp"
#include "synth/GangedRandomLfoVisualizer.hpp"
#include "synth/StandardModulators.hpp"
#include "synth/browser/BrowserCommandBuffer.hpp"

#include "../apps/miniapp/MiniAppDraw.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#ifdef JUCE_MAJOR_VERSION
#error "browser command buffer tests must not see JUCE"
#endif

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
    return std::abs(left - right) < 0.0001f;
}

// Version-2 schema shape, pinned at compile time (design.md OQ1 and OQ5).
//
// OQ1: `variant` carried appearance only, so `Node::color`/`Node::textStyle`
// replace it outright and it leaves the wire. Nothing replaces it: there is no
// residual interaction-semantics string and no explicit residual field.
// OQ5: a caption is a library-emitted `Label` node, so no combo-box caption or
// placeholder field enters the schema either.
template <typename T>
concept CarriesVariant = requires(T record) { record.variant; };
template <typename T>
concept CarriesPlaceholder = requires(T record) { record.placeholder; };

static_assert(!CarriesVariant<synth_browser::DecodedNode>,
              "version 2 carries no variant string on the wire");
// Task 7.2 finished the retirement at the source: the model itself has no
// `variant` member, so no producer can set one and no codec can read one. This
// is the assertion that makes the runtime sweep below a check on the CODEC
// rather than on the producers -- the tokens have no way in from the model.
static_assert(!CarriesVariant<synth::ui::Node>,
              "the model carries no retired variant string either");
static_assert(!CarriesPlaceholder<synth_browser::DecodedNode>,
              "version 2 carries no combo-box placeholder on the wire");
static_assert(!CarriesPlaceholder<synth::ui::Node>,
              "the model gains no combo-box placeholder field");

const synth_browser::DecodedNode& FindNode(const synth_browser::DecodedCommandBuffer& buffer, const char* id)
{
    const auto found = std::find_if(buffer.nodes.begin(), buffer.nodes.end(), [id](const auto& node) {
        return node.id == id;
    });
    if (found == buffer.nodes.end())
    {
        throw std::runtime_error("decoded node missing");
    }
    return *found;
}

synth::ui::NodeTree MakeCompleteTree()
{
    using namespace synth::ui;
    NodeTree tree;
    tree.nodes = {
        Node{.id = NodeId("root"), .kind = NodeKind::Root, .bounds = {0, 0, 800, 600},
             .children = {NodeId("scroll"), NodeId("button"), NodeId("slider"), NodeId("combo"),
                          NodeId("field"), NodeId("status"), NodeId("draw")}},
        Node{.id = NodeId("scroll"), .kind = NodeKind::ScrollArea, .bounds = {2, 3, 240, 180},
             .scrollContentWidth = 720, .scrollContentHeight = 900, .children = {NodeId("button")}},
        Node{.id = NodeId("button"), .kind = NodeKind::Button, .bounds = {4, 5, 100, 24}, .label = "Save",
             .action = Action::WithValue("file.save", "current")},
        Node{.id = NodeId("slider"), .kind = NodeKind::Slider, .bounds = {4, 36, 180, 20}, .label = "Gain",
             .value = 0.25f, .minValue = -1.0f, .maxValue = 1.0f, .step = 0.05f,
             .action = Action::Named("gain.set")},
        Node{.id = NodeId("combo"), .kind = NodeKind::ComboBox, .bounds = {4, 62, 180, 24}, .label = "Mode",
             .options = {{"saw", "Saw"}, {"square", "Square"}}, .selectedOption = "square",
             .action = Action::Named("mode.select")},
        Node{.id = NodeId("field"), .kind = NodeKind::TextField, .bounds = {4, 92, 180, 24}, .label = "Name",
             .text = "Bright", .action = Action::Named("name.commit")},
        Node{.id = NodeId("status"), .kind = NodeKind::StatusText, .bounds = {4, 122, 180, 20}, .text = "Ready"},
        Node{.id = NodeId("draw"), .kind = NodeKind::Draw, .bounds = {260, 8, 300, 200},
             .drawCommands = {
                 DrawCommand::Fill({1, 2, 30, 40}, synth::Color::Rgb(1, 2, 3)),
                 DrawCommand::StrokeRect({4, 5, 6, 7}, synth::Color::Rgb(4, 5, 6), 2),
                 DrawCommand::Line({8, 9}, {10, 11}, synth::Color::Rgb(7, 8, 9), 3),
                 DrawCommand::Arc({12, 13, 14, 15}, 0.1f, 2.1f, synth::Color::Rgb(10, 11, 12), 4),
                 DrawCommand::Text({16, 17, 18, 19}, "Scope", TextStyle{15, synth::Color::Rgb(13, 14, 15), TextAlign::Center}),
                 DrawCommand::FillEllipse({20, 21, 22, 23}, synth::Color::Rgb(16, 17, 18)),
                 DrawCommand::StrokeEllipse({24, 25, 26, 27}, synth::Color::Rgb(19, 20, 21), 5),
                 DrawCommand::FillRoundedRect({28, 29, 30, 31}, 6, synth::Color::Rgb(22, 23, 24)),
                 DrawCommand::StrokeRoundedRect({32, 33, 34, 35}, 7, synth::Color::Rgb(25, 26, 27), 8),
                 DrawCommand::Polyline({{36, 37}, {38, 39}, {40, 41}}, synth::Color::Rgb(28, 29, 30), 9),
                 DrawCommand::FillPolygon({{42, 43}, {44, 45}, {46, 47}}, synth::Color::Rgb(31, 32, 33)),
             }},
    };
    return tree;
}

void TestCompleteTreeRoundTrips()
{
    const synth_browser::CommandBuffer encoded = synth_browser::SerializeNodeTree(MakeCompleteTree());
    Require(encoded.bytes.size() > 32, "buffer has payload");
    Require(encoded.bytes[0] == std::byte{'S'} && encoded.bytes[1] == std::byte{'B'} &&
                encoded.bytes[2] == std::byte{'C'} && encoded.bytes[3] == std::byte{'B'},
            "buffer magic");

    const synth_browser::DecodedCommandBuffer decoded = synth_browser::DecodeCommandBuffer(encoded.bytes);
    Require(decoded.version == 2, "buffer version");
    Require(decoded.diagnostics.empty(), "complete tree diagnostics");
    Require(decoded.nodes.size() == 8, "node count");
    Require(decoded.actions.size() == 4, "action count");
    Require(decoded.drawCommands.size() == 11, "draw command count");
    Require(std::find(decoded.strings.begin(), decoded.strings.end(), "Scope") != decoded.strings.end(), "draw text string");
    const std::array expectedDrawKinds = {
        synth_browser::CommandDrawKind::Fill,
        synth_browser::CommandDrawKind::StrokeRect,
        synth_browser::CommandDrawKind::Line,
        synth_browser::CommandDrawKind::Arc,
        synth_browser::CommandDrawKind::Text,
        synth_browser::CommandDrawKind::FillEllipse,
        synth_browser::CommandDrawKind::StrokeEllipse,
        synth_browser::CommandDrawKind::FillRoundedRect,
        synth_browser::CommandDrawKind::StrokeRoundedRect,
        synth_browser::CommandDrawKind::Polyline,
        synth_browser::CommandDrawKind::FillPolygon,
    };
    for (std::size_t index = 0; index < expectedDrawKinds.size(); ++index)
    {
        Require(decoded.drawCommands[index].kind == expectedDrawKinds[index], "draw kind mapping");
    }

    const auto& root = FindNode(decoded, "root");
    Require(root.kind == synth_browser::CommandNodeKind::Root, "root kind");
    Require(root.bounds.width == 800.0f && root.bounds.height == 600.0f, "root bounds");
    Require(root.children == std::vector<std::string>{"scroll", "button", "slider", "combo", "field", "status", "draw"},
            "root child ids preserve order");
    const auto& scroll = FindNode(decoded, "scroll");
    Require(scroll.kind == synth_browser::CommandNodeKind::ScrollArea && scroll.scrollContentHeight == 900.0f,
            "scroll extents");
    const auto& slider = FindNode(decoded, "slider");
    Require(slider.kind == synth_browser::CommandNodeKind::Slider && std::abs(slider.value - 0.25f) < 0.0001f,
            "slider value");
    const auto& combo = FindNode(decoded, "combo");
    Require(combo.options.size() == 2 && combo.options[1].id == "square" && combo.selectedOption == "square",
            "combo options");
    const auto& button = FindNode(decoded, "button");
    Require(button.action.has_value() && button.action->name == "file.save" && button.action->value == "current",
            "button action");
    const auto& draw = FindNode(decoded, "draw");
    Require(draw.drawCount == 11 && decoded.drawCommands[draw.drawStart + 4].kind == synth_browser::CommandDrawKind::Text,
            "draw range and text kind");
    Require(decoded.drawCommands[draw.drawStart + 9].points.size() == 3 &&
                decoded.drawCommands[draw.drawStart + 10].kind == synth_browser::CommandDrawKind::FillPolygon,
            "polyline and polygon");
}

void TestNodeIdsAreStableAcrossFrames()
{
    synth::ui::NodeTree first = MakeCompleteTree();
    synth::ui::NodeTree second = first;
    second.nodes[3].value = 0.75f;
    second.nodes[7].drawCommands[0] = synth::ui::DrawCommand::Fill(synth::Color::Rgb(90, 91, 92));

    const auto firstDecoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(first).bytes);
    const auto secondDecoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(second).bytes);
    Require(firstDecoded.nodes.size() == secondDecoded.nodes.size(), "stable frame node counts");
    for (std::size_t index = 0; index < firstDecoded.nodes.size(); ++index)
    {
        Require(firstDecoded.nodes[index].id == secondDecoded.nodes[index].id, "stable node id");
    }
    Require(FindNode(secondDecoded, "slider").value == 0.75f, "second frame value changes");
}

void TestDisabledSemanticNodesCarryEnabledState()
{
    using namespace synth::ui;
    NodeTree tree;
    tree.nodes = {
        Node{.id = NodeId("root"), .kind = NodeKind::Root, .bounds = {0, 0, 320, 240},
             .children = {NodeId("button"), NodeId("combo"), NodeId("field"), NodeId("toggle"),
                          NodeId("slider"), NodeId("row"), NodeId("draw")}},
        Node{.id = NodeId("button"), .kind = NodeKind::Button, .enabled = false, .bounds = {4, 4, 100, 24},
             .label = "Submit", .action = Action::Named("generic.button")},
        Node{.id = NodeId("combo"), .kind = NodeKind::ComboBox, .enabled = false, .bounds = {4, 32, 100, 24},
             .label = "Message", .options = {{"one", "One"}, {"two", "Two"}}, .selectedOption = "two",
             .action = Action::Named("generic.combo")},
        Node{.id = NodeId("field"), .kind = NodeKind::TextField, .enabled = false, .bounds = {4, 60, 100, 24},
             .label = "Argument", .text = "7", .action = Action::Named("generic.text")},
        Node{.id = NodeId("toggle"), .kind = NodeKind::Toggle, .enabled = false, .bounds = {4, 88, 100, 24},
             .label = "Feedback", .action = Action::Named("generic.toggle")},
        Node{.id = NodeId("slider"), .kind = NodeKind::Slider, .enabled = false, .bounds = {4, 116, 100, 24},
             .label = "Gain", .action = Action::Named("generic.slider")},
        Node{.id = NodeId("row"), .kind = NodeKind::Row, .enabled = false, .bounds = {120, 4, 100, 24},
             .doubleClickAction = Action::Named("generic.row")},
        Node{.id = NodeId("draw"), .kind = NodeKind::Draw, .enabled = false, .bounds = {120, 32, 100, 40},
             .pointerDragAction = Action::Named("generic.drag"),
             .doubleClickAction = Action::Named("generic.draw")},
    };

    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.diagnostics.empty(), "disabled semantic nodes need no browser fallback");
    Require(FindNode(decoded, "root").enabled, "root keeps its default enabled state");
    for (const char* id : {"button", "combo", "field", "toggle", "slider", "row", "draw"})
    {
        Require(!FindNode(decoded, id).enabled, "disabled semantic node keeps its disabled state");
    }
    Require(FindNode(decoded, "combo").selectedOption == "two",
            "disabled combo box keeps its selected option");
    Require(FindNode(decoded, "field").text == "7", "disabled text field keeps its text");
    Require(FindNode(decoded, "row").doubleClickAction.has_value() &&
                FindNode(decoded, "draw").pointerDragAction.has_value(),
            "disabled semantic nodes still carry their actions for the backend to suppress");
}

void TestSyncPageRoundTripsPortableControlsAndActions()
{
    synth::runtime_ui::SyncPageSurface surface;
    surface.BeginEdit({.sendClock = true,
                       .receiveClock = false,
                       .sendTransport = true,
                       .receiveTransport = false,
                       .ppqn = 96});
    surface.RefreshStatus({.currentBpm = 121.25,
                           .lockState = "Locked",
                           .sourceName = "Clock Controller",
                           .outputLatencyMicros = 5'500,
                           .ignoredInputCount = 2,
                           .lateEventCount = 3,
                           .droppedOutputCount = 4});
    surface.SetContentBounds({0.0f, 0.0f, 240.0f, 560.0f});
    const auto decoded = synth_browser::DecodeCommandBuffer(
        synth_browser::SerializeNodeTree(surface.BuildTree()).bytes);
    Require(decoded.diagnostics.empty(), "Sync tree needs no browser fallback");
    const auto& sendClock = FindNode(decoded, synth::runtime_ui::NodeIds::kSyncSendClock);
    Require(sendClock.kind == synth_browser::CommandNodeKind::Toggle && sendClock.checked,
            "browser command buffer preserves Sync toggle kind/state");
    Require(sendClock.action.has_value() &&
                sendClock.action->name == synth::runtime_ui::Actions::kSyncSendClock,
            "browser command buffer preserves Sync toggle action");
    const auto& ppqn = FindNode(decoded, synth::runtime_ui::NodeIds::kSyncPpqn);
    Require(ppqn.kind == synth_browser::CommandNodeKind::TextField && ppqn.text == "96",
            "browser command buffer preserves Sync PPQN editor");
    Require(ppqn.action.has_value() && ppqn.action->name == synth::runtime_ui::Actions::kSyncPpqn,
            "browser command buffer preserves Sync PPQN action");
    Require(FindNode(decoded, synth::runtime_ui::NodeIds::kSyncSource).text ==
                "Source: Clock Controller",
            "browser command buffer preserves Sync diagnostic text");
}

void TestUnsupportedPortableFeatureIsGeneric()
{
    synth::ui::NodeTree tree = MakeCompleteTree();
    tree.nodes[1].kind = synth_browser::testing::UnsupportedNodeKind();
    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.diagnostics.size() == 2, "unsupported node diagnostic count");
    const auto hasNodeKind = std::any_of(decoded.diagnostics.begin(), decoded.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == synth_browser::DiagnosticCode::UnsupportedPortableFeature && diagnostic.feature == "node kind";
    });
    const auto hasChildNode = std::any_of(decoded.diagnostics.begin(), decoded.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == synth_browser::DiagnosticCode::UnsupportedPortableFeature && diagnostic.feature == "child node";
    });
    Require(hasNodeKind, "generic unsupported feature name");
    Require(hasChildNode, "generic child prune feature name");
    for (const auto& diagnostic : decoded.diagnostics)
        Require(diagnostic.feature.find("miniapp") == std::string::npos, "no app fallback diagnostic");
    const auto& root = FindNode(decoded, "root");
    Require(std::find(root.children.begin(), root.children.end(), "scroll") == root.children.end(),
            "unsupported child id is pruned");
    Require(std::find(root.children.begin(), root.children.end(), "button") != root.children.end(),
            "supported child ids remain");
}

void TestUnsupportedDrawFeatureIsGeneric()
{
    synth::ui::NodeTree tree = MakeCompleteTree();
    tree.nodes[7].drawCommands[0].kind = synth_browser::testing::UnsupportedDrawKind();
    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.diagnostics.size() == 1, "unsupported draw diagnostic count");
    Require(decoded.diagnostics[0].code == synth_browser::DiagnosticCode::UnsupportedPortableFeature,
            "unsupported draw diagnostic code");
    Require(decoded.diagnostics[0].feature == "draw command kind", "generic unsupported draw feature name");
}

void TestVersionTwoCarriesStyleAndParentRelativeBounds()
{
    synth::ui::NodeTree tree;
    synth::ui::Node root;
    root.id = synth::ui::NodeId("root");
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0, 0, 400, 300};
    root.children.push_back(synth::ui::NodeId("child"));
    synth::ui::Node child;
    child.id = synth::ui::NodeId("child");
    child.kind = synth::ui::NodeKind::Button;
    child.bounds = {10, 20, 80, 24};  // parent-relative
    child.color = synth::Color::Rgb(0, 200, 0);
    child.textStyle = synth::ui::TextStyle{16.0f, synth::Color::Rgb(255, 255, 255),
                                           synth::ui::TextAlign::Center};
    child.borderColor = synth::Color::Rgb(40, 50, 60);
    child.borderWidth = 2.5f;
    child.cornerRadius = 6.0f;
    tree.nodes.push_back(root);
    tree.nodes.push_back(child);

    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.version == 2, "version two is advertised on the wire");
    const auto& out = FindNode(decoded, "child");
    Require(NearlyEqual(out.bounds.x, 10.0f) && NearlyEqual(out.bounds.y, 20.0f),
            "parent-relative bounds survive encode/decode unchanged");
    Require(out.color.has_value() && out.color->g == 200, "carried colour survives");
    Require(out.color->r == 0 && out.color->b == 0 && out.color->a == 255,
            "carried colour survives channel for channel");
    Require(out.textStyle.has_value() && NearlyEqual(out.textStyle->size, 16.0f),
            "carried text style survives");
    Require(out.textStyle->align == synth::ui::TextAlign::Center,
            "carried text alignment survives");
    Require(out.textStyle->color == synth::Color::Rgb(255, 255, 255),
            "carried glyph colour survives");
    Require(out.borderColor.has_value() && *out.borderColor == synth::Color::Rgb(40, 50, 60),
            "carried border colour survives");
    Require(out.borderWidth.has_value() && NearlyEqual(*out.borderWidth, 2.5f),
            "carried border width survives");
    Require(out.cornerRadius.has_value() && NearlyEqual(*out.cornerRadius, 6.0f),
            "carried corner radius survives");
}

void TestPresentZeroBorderMetricsStayPresent()
{
    synth::ui::NodeTree tree;
    synth::ui::Node root;
    root.id = synth::ui::NodeId("root");
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0, 0, 400, 300};
    root.borderWidth = 0.0f;
    root.cornerRadius = 0.0f;
    tree.nodes.push_back(root);

    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    const auto& out = FindNode(decoded, "root");
    Require(out.borderWidth.has_value() && NearlyEqual(*out.borderWidth, 0.0f),
            "a producer explicitly choosing zero border width is not read as absent");
    Require(out.cornerRadius.has_value() && NearlyEqual(*out.cornerRadius, 0.0f),
            "a producer explicitly choosing zero corner radius is not read as absent");
}

void TestAbsentStyleStaysAbsent()
{
    synth::ui::NodeTree tree;
    synth::ui::Node root;
    root.id = synth::ui::NodeId("root");
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0, 0, 400, 300};
    tree.nodes.push_back(root);

    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    const auto& out = FindNode(decoded, "root");
    Require(!out.color.has_value(),
            "an absent colour decodes as absent, not as a sentinel value");
    Require(!out.textStyle.has_value(),
            "an absent text style decodes as absent, not as a sentinel value");
    Require(!out.borderColor.has_value(),
            "an absent border colour decodes as absent, not as a sentinel value");
    Require(!out.borderWidth.has_value(),
            "an absent border width decodes as absent, not as a sentinel value");
    Require(!out.cornerRadius.has_value(),
            "an absent corner radius decodes as absent, not as a sentinel value");
}

void TestFullyTransparentBlackIsAPresentColour()
{
    synth::ui::NodeTree tree;
    synth::ui::Node root;
    root.id = synth::ui::NodeId("root");
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0, 0, 400, 300};
    root.color = synth::Color::Rgba(0, 0, 0, 0);
    root.borderColor = synth::Color::Rgba(0, 0, 0, 0);
    tree.nodes.push_back(root);

    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    const auto& out = FindNode(decoded, "root");
    Require(out.color.has_value() && *out.color == synth::Color::Rgba(0, 0, 0, 0),
            "a producer legitimately choosing transparent black is not read as absent");
    Require(out.borderColor.has_value() && *out.borderColor == synth::Color::Rgba(0, 0, 0, 0),
            "a producer legitimately choosing a transparent black border is not read as absent");
}

void TestCorruptPresenceFlagIsRejected()
{
    Require(!synth_browser::detail::DecodePresence(0) && synth_browser::detail::DecodePresence(1),
            "0 and 1 are the only presence flags the encoder emits");
    bool threwOnFlag = false;
    try
    {
        synth_browser::detail::DecodePresence(2);
    }
    catch (const std::runtime_error&)
    {
        threwOnFlag = true;
    }
    Require(threwOnFlag, "a presence flag the encoder can never emit is rejected, as in protocol.ts");

    // Poke each first-node appearance presence byte to prove the strictness is
    // reachable through the whole decode path rather than only in the helper.
    // Buffer layout: 4 magic + 2 version + 2 reserved + 5 section lengths,
    // then the string section, then the node section's u32 node count, then
    // the first node record. Within a record the colour presence byte follows
    // the id (u32), four u8 flags, the bounds (4 floats), three string
    // indices (u32), and six floats; subsequent appearance fields in this
    // all-absent fixture are consecutive presence bytes.
    const auto encodeRoot = [](std::optional<synth::Color> color) {
        synth::ui::NodeTree tree;
        synth::ui::Node root;
        root.id = synth::ui::NodeId("root");
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = {0, 0, 400, 300};
        root.color = color;
        tree.nodes.push_back(root);
        return synth_browser::SerializeNodeTree(tree).bytes;
    };
    auto bytes = encodeRoot(std::nullopt);
    const auto readU32 = [](const std::vector<std::byte>& from, std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            value |= static_cast<std::uint32_t>(from[at + byte]) << (8 * byte);
        return value;
    };
    const std::size_t firstNodeRecord = 28 + readU32(bytes, 8) + 4;
    const std::size_t colourPresence = firstNodeRecord + 60;
    // The same offset reads 0 for an unstyled root and 1 for a styled one,
    // which is what proves the arithmetic lands on the presence byte rather
    // than on some other zero. Both trees have identical string sections.
    Require(bytes[colourPresence] == std::byte{0}, "the unstyled root encodes an absent colour");
    Require(encodeRoot(synth::Color::Rgb(1, 2, 3))[colourPresence] == std::byte{1},
            "the styled root sets the same byte, so it is the colour presence flag");

    const auto requireRejectsCorruptPresence = [&](std::size_t offset, const char* label) {
        std::vector<std::byte> corrupted = bytes;
        corrupted[offset] = std::byte{2};
        bool threwOnBuffer = false;
        try
        {
            synth_browser::DecodeCommandBuffer(corrupted);
        }
        catch (const std::runtime_error&)
        {
            threwOnBuffer = true;
        }
        Require(threwOnBuffer, label);
    };
    requireRejectsCorruptPresence(colourPresence,
                                  "a corrupt colour presence byte fails the decode instead of inventing a colour");
    requireRejectsCorruptPresence(colourPresence + 1,
                                  "a corrupt text-style presence byte fails the decode instead of inventing a style");
    requireRejectsCorruptPresence(colourPresence + 2,
                                  "a corrupt border-colour presence byte fails the decode instead of inventing a border colour");
    requireRejectsCorruptPresence(colourPresence + 3,
                                  "a corrupt border-width presence byte fails the decode instead of inventing a border width");
    requireRejectsCorruptPresence(colourPresence + 4,
                                  "a corrupt corner-radius presence byte fails the decode instead of inventing a radius");
}

void TestMovingAParentChangesOnlyTheParentRecord()
{
    const auto at = [](float parentY) {
        synth::ui::NodeTree tree;
        synth::ui::Node root;
        root.id = synth::ui::NodeId("root");
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = {0, 0, 400, 300};
        root.children.push_back(synth::ui::NodeId("parent"));
        synth::ui::Node parent;
        parent.id = synth::ui::NodeId("parent");
        parent.kind = synth::ui::NodeKind::Section;
        parent.bounds = {0, parentY, 400, 100};
        parent.children.push_back(synth::ui::NodeId("child"));
        synth::ui::Node child;
        child.id = synth::ui::NodeId("child");
        child.kind = synth::ui::NodeKind::Label;
        child.bounds = {4, 4, 80, 20};
        tree.nodes.push_back(root);
        tree.nodes.push_back(parent);
        tree.nodes.push_back(child);
        return synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    };

    const auto first = at(0.0f);
    const auto second = at(50.0f);
    Require(std::memcmp(&FindNode(first, "child").bounds, &FindNode(second, "child").bounds,
                        sizeof(synth::ui::Bounds)) == 0,
            "moving a parent leaves every descendant's serialized bounds byte-identical");
    Require(FindNode(first, "parent").bounds.y != FindNode(second, "parent").bounds.y,
            "only the moved parent's record differs");
}

void TestVariantCarriesNoAppearanceStrings()
{
    synth::ui::NodeTree tree;
    synth::ui::Node root;
    root.id = synth::ui::NodeId("root");
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0, 0, 400, 300};
    root.children.push_back(synth::ui::NodeId("row"));
    synth::ui::Node row;
    row.id = synth::ui::NodeId("row");
    row.kind = synth::ui::NodeKind::Section;
    row.bounds = {0, 0, 400, 40};
    // The two lines that used to sit here set `variant` on these nodes. They
    // cannot be written any more, which is the point: with the model field
    // deleted the only remaining way a retired appearance token could reach the
    // wire is the codec inventing one, and that is what the sweep below checks.
    root.color = synth::kSurfaceBackground;
    row.color = synth::Color::Rgb(30, 32, 34);
    row.selected = true;
    tree.nodes.push_back(root);
    tree.nodes.push_back(row);

    const auto encoded = synth_browser::SerializeNodeTree(tree);
    const auto decoded = synth_browser::DecodeCommandBuffer(encoded.bytes);
    // Anti-vacuity: a codec that interned nothing at all would satisfy the
    // sweep below while saying nothing, so require the table to hold the ids
    // this tree really carries before believing what it does not hold.
    Require(std::find(decoded.strings.begin(), decoded.strings.end(), "root") !=
                    decoded.strings.end() &&
                std::find(decoded.strings.begin(), decoded.strings.end(), "row") !=
                    decoded.strings.end(),
            "the decoded string table holds the ids this tree carries");
    for (const char* retired :
         {"danger", "primary", "quiet", "secondary", "field", "title", "muted", "muted-title",
          "list-row", "panel"})
    {
        Require(std::find(decoded.strings.begin(), decoded.strings.end(), retired) ==
                    decoded.strings.end(),
                "no variant string reaches the version-two wire");
    }
}

void TestVersionMismatchFailsLoudly()
{
    auto bytes = synth_browser::SerializeNodeTree(MakeCompleteTree()).bytes;
    Require(bytes[4] == std::byte{2} && bytes[5] == std::byte{0},
            "the serialized header advertises version two");
    bytes[4] = std::byte{1};
    bool threw = false;
    try
    {
        synth_browser::DecodeCommandBuffer(bytes);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    Require(threw, "a version-one buffer is rejected outright with no fallback decode");
}

void TestPredictiveGangedLfoUsesExistingDrawSchema()
{
    synth::GangedRandomLfoSnapshot<2> snapshot;
    snapshot.sampleRate = 48000.0;
    snapshot.roundElapsedSamples = 2.0;
    snapshot.voices[0] = {.source = 0.0f, .target = 1.0f, .output = 0.8f, .shape = 0.0f,
                          .waitingIncrement = 0.25, .movingIncrement = 0.5,
                          .color = synth::Color::Cyan};
    snapshot.voices[1] = {.source = 1.0f, .target = 0.0f, .output = 0.2f, .shape = 1.0f,
                          .waitingIncrement = 0.125, .movingIncrement = 0.25,
                          .color = synth::Color::Orange};
    std::vector<synth::ui::DrawCommand> commands;
    synth::ui::BuildGangedRandomLfoCommands(snapshot, {0, 0, 160, 80}, commands);

    synth::ui::NodeTree tree;
    tree.nodes = {
        synth::ui::Node{.id = synth::ui::NodeId("root"), .kind = synth::ui::NodeKind::Root,
                        .bounds = {0, 0, 160, 80}, .children = {synth::ui::NodeId("predictive")}},
        synth::ui::Node{.id = synth::ui::NodeId("predictive"), .kind = synth::ui::NodeKind::Draw,
                        .bounds = {0, 0, 160, 80}, .drawCommands = std::move(commands)},
    };
    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.version == synth_browser::kCommandBufferVersion,
            "predictive geometry preserves browser schema version");
    Require(decoded.diagnostics.empty(), "predictive geometry needs no browser protocol extension");
    const auto& node = FindNode(decoded, "predictive");
    std::size_t polylines = 0;
    std::size_t ellipses = 0;
    for (std::size_t index = 0; index < node.drawCount; ++index)
    {
        const auto kind = decoded.drawCommands[node.drawStart + index].kind;
        polylines += kind == synth_browser::CommandDrawKind::Polyline ? 1u : 0u;
        ellipses += kind == synth_browser::CommandDrawKind::FillEllipse ? 1u : 0u;
    }
    Require(polylines > 2, "browser consumes predictive polyline commands");
    Require(ellipses == 2, "browser consumes predictive ellipse commands");
}

void TestMiniAppTwoScopeCommandsUseExistingBrowserSchema()
{
    synth::ScopeWriter scope(2, 16);
    for (std::size_t sample = 0; sample < 16; ++sample)
    {
        scope.Write(0, static_cast<float>(sample) / 15.0f);
        scope.Write(1, 1.0f - static_cast<float>(sample) / 15.0f);
    }
    synth_miniapp::VcoWaveformDrawState vcoState;
    vcoState.layers = {{.connected = true, .scopeColor = synth::Color::Cyan,
                        .scope = &scope, .scopeChannel = 0}};
    synth_miniapp::LfoWaveformDrawState lfoState;
    lfoState.layers = {{.connected = true, .scopeColor = synth::Color::Green,
                        .scope = &scope, .scopeChannel = 1}};
    const std::array<synth::ui::Bounds, 2> bounds{{
        {8.0f, 8.0f, 144.0f, 64.0f},
        {168.0f, 8.0f, 144.0f, 64.0f},
    }};
    synth::ui::NodeTree tree;
    tree.nodes = {
        synth::ui::Node{.id = synth::ui::NodeId("root"), .kind = synth::ui::NodeKind::Root,
                        .bounds = {0, 0, 320, 80},
                        .children = {synth::ui::NodeId("vco"), synth::ui::NodeId("lfo")}},
        synth::ui::Node{.id = synth::ui::NodeId("vco"), .kind = synth::ui::NodeKind::Draw,
                        .bounds = bounds[0],
                        .drawCommands = synth_miniapp::BuildVcoWaveformCommands(vcoState, bounds[0])},
        synth::ui::Node{.id = synth::ui::NodeId("lfo"), .kind = synth::ui::NodeKind::Draw,
                        .bounds = bounds[1],
                        .drawCommands = synth_miniapp::BuildLfoWaveformCommands(lfoState, bounds[1])},
    };
    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.nodes.size() == 3, "MiniApp browser tree contains root plus two scopes");
    Require(decoded.version == synth_browser::kCommandBufferVersion,
            "two-scope MiniApp tree keeps browser command version");
    Require(decoded.diagnostics.empty(), "two-scope MiniApp tree needs no browser fallback");
    Require(FindNode(decoded, "vco").drawCount > 0, "browser consumes VCO panel commands");
    Require(FindNode(decoded, "lfo").drawCount > 0, "browser consumes LFO panel commands");
}

void TestStandardModulatorUnderlaysUseExistingBrowserSchema()
{
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 2, .numModulators = 15, .numScenes = 1, .maxParameters = 16});
    synth::StandardModulators<2> standard(group);
    standard.Register();
    standard.Prepare(48000.0);
    standard.Process();
    standard.PublishUiState();

    const synth::ui::Bounds bounds{0.0f, 0.0f, 96.0f, 96.0f};
    synth::ui::NodeTree tree;
    tree.nodes.push_back(synth::ui::Node{
        .id = synth::ui::NodeId("root"), .kind = synth::ui::NodeKind::Root,
        .bounds = bounds,
        .children = {synth::ui::NodeId("random"), synth::ui::NodeId("constant"), synth::ui::NodeId("noise")}});
    for (synth::ui::Visualizer* visualizer : {
             static_cast<synth::ui::Visualizer*>(&standard.RandomVisualizer(0)),
             static_cast<synth::ui::Visualizer*>(&standard.ConstantVisualizer()),
             static_cast<synth::ui::Visualizer*>(&standard.NoiseVisualizer())})
    {
        visualizer->SetBounds(bounds);
    }
    tree.nodes.push_back(synth::ui::Node{.id = synth::ui::NodeId("random"), .kind = synth::ui::NodeKind::Draw,
                                         .bounds = bounds, .drawCommands = standard.RandomVisualizer(0).Draw()});
    tree.nodes.push_back(synth::ui::Node{.id = synth::ui::NodeId("constant"), .kind = synth::ui::NodeKind::Draw,
                                         .bounds = bounds, .drawCommands = standard.ConstantVisualizer().Draw()});
    tree.nodes.push_back(synth::ui::Node{.id = synth::ui::NodeId("noise"), .kind = synth::ui::NodeKind::Draw,
                                         .bounds = bounds, .drawCommands = standard.NoiseVisualizer().Draw()});
    const auto decoded = synth_browser::DecodeCommandBuffer(synth_browser::SerializeNodeTree(tree).bytes);
    Require(decoded.version == synth_browser::kCommandBufferVersion,
            "standard underlays preserve browser command version");
    Require(decoded.diagnostics.empty(), "standard underlays need no browser fallback");
    Require(FindNode(decoded, "random").drawCount > 0, "browser consumes standard random underlay");
    Require(FindNode(decoded, "constant").drawCount == 2, "browser consumes standard constant underlay");
    Require(FindNode(decoded, "noise").drawCount == 1, "browser consumes standard noise underlay");
}

}  // namespace

int main()
{
    TestCompleteTreeRoundTrips();
    TestNodeIdsAreStableAcrossFrames();
    TestDisabledSemanticNodesCarryEnabledState();
    TestSyncPageRoundTripsPortableControlsAndActions();
    TestUnsupportedPortableFeatureIsGeneric();
    TestUnsupportedDrawFeatureIsGeneric();
    TestVersionTwoCarriesStyleAndParentRelativeBounds();
    TestPresentZeroBorderMetricsStayPresent();
    TestAbsentStyleStaysAbsent();
    TestFullyTransparentBlackIsAPresentColour();
    TestCorruptPresenceFlagIsRejected();
    TestMovingAParentChangesOnlyTheParentRecord();
    TestVariantCarriesNoAppearanceStrings();
    TestVersionMismatchFailsLoudly();
    TestPredictiveGangedLfoUsesExistingDrawSchema();
    TestMiniAppTwoScopeCommandsUseExistingBrowserSchema();
    TestStandardModulatorUnderlaysUseExistingBrowserSchema();
    return 0;
}
