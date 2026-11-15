#pragma once

#include "synth/PortableUI.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace synth_browser {

inline constexpr std::array<std::byte, 4> kCommandBufferMagic = {
    std::byte{'S'}, std::byte{'B'}, std::byte{'C'}, std::byte{'B'}};
// Version 2: node bounds are parent-relative, `Draw` geometry is
// node-local, `Node::color`/`Node::textStyle` and the container border
// fields cross the wire behind explicit presence bytes, and `Node::variant` is
// gone. A hard break -- both ends check strict equality and there is no
// version-1 fallback or negotiation. Every
// artifact that advertises the UI protocol version moves together: this
// constant, `COMMAND_BUFFER_VERSION` in `browser/src/protocol.ts`, and each
// Wasm package's exported `synth_browser_ui_protocol_version()`.
inline constexpr std::uint16_t kCommandBufferVersion = 2;

enum class CommandNodeKind : std::uint8_t {
    Root,
    Row,
    Section,
    ScrollArea,
    Label,
    Button,
    Toggle,
    Slider,
    ComboBox,
    TextField,
    StatusText,
    Draw,
};

enum class CommandDrawKind : std::uint8_t {
    Fill,
    StrokeRect,
    Line,
    Arc,
    Text,
    FillEllipse,
    StrokeEllipse,
    FillRoundedRect,
    StrokeRoundedRect,
    Polyline,
    FillPolygon,
};

enum class DiagnosticCode : std::uint8_t {
    UnsupportedPortableFeature = 1,
};

struct CommandBuffer {
    std::vector<std::byte> bytes;
};

struct DecodedAction {
    std::string name;
    std::string value;
};

struct DecodedOption {
    std::string id;
    std::string label;
};

struct DecodedDrawCommand {
    CommandDrawKind kind{};
    synth::ui::Bounds bounds{};
    synth::ui::Point from{};
    synth::ui::Point to{};
    synth::Color color{};
    float strokeWidth = 1.0f;
    float startRadians = 0.0f;
    float endRadians = 0.0f;
    float cornerRadius = 0.0f;
    std::string text;
    synth::ui::TextStyle textStyle{};
    std::vector<synth::ui::Point> points;
};

struct DecodedNode {
    std::string id;
    CommandNodeKind kind{};
    synth::ui::Bounds bounds{};
    std::string label;
    std::string text;
    bool checked = false;
    bool selected = false;
    bool enabled = true;
    float value = 0.0f;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.001f;
    float scrollContentWidth = 0.0f;
    float scrollContentHeight = 0.0f;
    std::vector<DecodedOption> options;
    std::string selectedOption;
    std::optional<synth::Color> color;
    std::optional<synth::ui::TextStyle> textStyle;
    std::optional<synth::Color> borderColor;
    std::optional<float> borderWidth;
    std::optional<float> cornerRadius;
    std::optional<DecodedAction> action;
    std::optional<DecodedAction> pointerDragAction;
    std::optional<DecodedAction> doubleClickAction;
    std::vector<std::string> children;
    std::uint32_t drawStart = 0;
    std::uint32_t drawCount = 0;
};

struct Diagnostic {
    DiagnosticCode code{};
    std::string feature;
};

struct DecodedCommandBuffer {
    std::uint16_t version = 0;
    std::vector<std::string> strings;
    std::vector<DecodedNode> nodes;
    std::vector<DecodedAction> actions;
    std::vector<DecodedDrawCommand> drawCommands;
    std::vector<Diagnostic> diagnostics;
};

namespace detail {

inline void AppendU8(std::vector<std::byte>& output, std::uint8_t value)
{
    output.push_back(static_cast<std::byte>(value));
}

inline void AppendU16(std::vector<std::byte>& output, std::uint16_t value)
{
    for (unsigned shift = 0; shift < 16; shift += 8)
    {
        AppendU8(output, static_cast<std::uint8_t>(value >> shift));
    }
}

inline void AppendU32(std::vector<std::byte>& output, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
        AppendU8(output, static_cast<std::uint8_t>(value >> shift));
    }
}

inline void AppendI32(std::vector<std::byte>& output, std::int32_t value)
{
    AppendU32(output, static_cast<std::uint32_t>(value));
}

inline void AppendFloat(std::vector<std::byte>& output, float value)
{
    AppendU32(output, std::bit_cast<std::uint32_t>(value));
}

inline void AppendBounds(std::vector<std::byte>& output, const synth::ui::Bounds& bounds)
{
    AppendFloat(output, bounds.x);
    AppendFloat(output, bounds.y);
    AppendFloat(output, bounds.width);
    AppendFloat(output, bounds.height);
}

inline void AppendPoint(std::vector<std::byte>& output, const synth::ui::Point& point)
{
    AppendFloat(output, point.x);
    AppendFloat(output, point.y);
}

inline void AppendColor(std::vector<std::byte>& output, synth::Color color)
{
    AppendU8(output, color.r);
    AppendU8(output, color.g);
    AppendU8(output, color.b);
    AppendU8(output, color.a);
}

// Optional node fields carry an explicit presence byte. Version 1 had no
// presence encoding at all -- `DrawCommand::color` is raw RGBA -- and a
// sentinel colour would be indistinguishable from a producer legitimately
// choosing that value, so absence is spelled out rather than encoded in-band.
inline void AppendOptionalColor(std::vector<std::byte>& output, const std::optional<synth::Color>& color)
{
    AppendU8(output, color.has_value() ? 1 : 0);
    if (color.has_value()) AppendColor(output, *color);
}

inline void AppendOptionalTextStyle(std::vector<std::byte>& output,
                                    const std::optional<synth::ui::TextStyle>& textStyle)
{
    AppendU8(output, textStyle.has_value() ? 1 : 0);
    if (!textStyle.has_value()) return;
    AppendFloat(output, textStyle->size);
    AppendColor(output, textStyle->color);
    AppendU8(output, static_cast<std::uint8_t>(textStyle->align));
}

inline void AppendOptionalFloat(std::vector<std::byte>& output, const std::optional<float>& value)
{
    AppendU8(output, value.has_value() ? 1 : 0);
    if (value.has_value()) AppendFloat(output, *value);
}

class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    std::uint8_t U8() { return ReadByte(); }
    std::uint16_t U16() { return static_cast<std::uint16_t>(U8()) | (static_cast<std::uint16_t>(U8()) << 8); }
    std::uint32_t U32()
    {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8) value |= static_cast<std::uint32_t>(U8()) << shift;
        return value;
    }
    std::int32_t I32() { return static_cast<std::int32_t>(U32()); }
    float Float() { return std::bit_cast<float>(U32()); }
    synth::ui::Bounds Bounds() { return {Float(), Float(), Float(), Float()}; }
    synth::ui::Point Point() { return {Float(), Float()}; }
    synth::Color Color() { return {U8(), U8(), U8(), U8()}; }
    std::span<const std::byte> Section(std::uint32_t size)
    {
        Require(size);
        const auto section = bytes_.subspan(position_, size);
        position_ += size;
        return section;
    }
    bool Empty() const { return position_ == bytes_.size(); }

private:
    std::uint8_t ReadByte()
    {
        Require(1);
        return std::to_integer<std::uint8_t>(bytes_[position_++]);
    }
    void Require(std::size_t count) const
    {
        if (count > bytes_.size() - position_) throw std::runtime_error("truncated browser command buffer");
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

inline std::optional<CommandNodeKind> MapNodeKind(synth::ui::NodeKind kind)
{
    using Source = synth::ui::NodeKind;
    switch (kind)
    {
    case Source::Root: return CommandNodeKind::Root;
    case Source::Row: return CommandNodeKind::Row;
    case Source::Section: return CommandNodeKind::Section;
    case Source::ScrollArea: return CommandNodeKind::ScrollArea;
    case Source::Label: return CommandNodeKind::Label;
    case Source::Button: return CommandNodeKind::Button;
    case Source::Toggle: return CommandNodeKind::Toggle;
    case Source::Slider: return CommandNodeKind::Slider;
    case Source::ComboBox: return CommandNodeKind::ComboBox;
    case Source::TextField: return CommandNodeKind::TextField;
    case Source::StatusText: return CommandNodeKind::StatusText;
    case Source::Draw: return CommandNodeKind::Draw;
    }
    return std::nullopt;
}

inline std::optional<CommandDrawKind> MapDrawKind(synth::ui::DrawCommand::Kind kind)
{
    using Source = synth::ui::DrawCommand::Kind;
    switch (kind)
    {
    case Source::Fill: return CommandDrawKind::Fill;
    case Source::StrokeRect: return CommandDrawKind::StrokeRect;
    case Source::Line: return CommandDrawKind::Line;
    case Source::Arc: return CommandDrawKind::Arc;
    case Source::Text: return CommandDrawKind::Text;
    case Source::FillEllipse: return CommandDrawKind::FillEllipse;
    case Source::StrokeEllipse: return CommandDrawKind::StrokeEllipse;
    case Source::FillRoundedRect: return CommandDrawKind::FillRoundedRect;
    case Source::StrokeRoundedRect: return CommandDrawKind::StrokeRoundedRect;
    case Source::Polyline: return CommandDrawKind::Polyline;
    case Source::FillPolygon: return CommandDrawKind::FillPolygon;
    }
    return std::nullopt;
}

inline void AppendDraw(std::vector<std::byte>& output, const synth::ui::DrawCommand& draw, std::uint32_t textIndex)
{
    const auto kind = MapDrawKind(draw.kind);
    if (!kind) throw std::logic_error("unsupported draw command serialized");
    AppendU8(output, static_cast<std::uint8_t>(*kind));
    AppendU8(output, static_cast<std::uint8_t>(draw.textStyle.align));
    AppendU16(output, 0);
    AppendBounds(output, draw.bounds);
    AppendPoint(output, draw.from);
    AppendPoint(output, draw.to);
    AppendColor(output, draw.color);
    AppendFloat(output, draw.strokeWidth);
    AppendFloat(output, draw.startRadians);
    AppendFloat(output, draw.endRadians);
    AppendFloat(output, draw.cornerRadius);
    AppendU32(output, textIndex);
    AppendFloat(output, draw.textStyle.size);
    AppendColor(output, draw.textStyle.color);
    AppendU32(output, static_cast<std::uint32_t>(draw.points.size()));
    for (const auto& point : draw.points) AppendPoint(output, point);
}

inline CommandNodeKind DecodeNodeKind(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(CommandNodeKind::Draw)) throw std::runtime_error("invalid command node kind");
    return static_cast<CommandNodeKind>(value);
}

inline CommandDrawKind DecodeDrawKind(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(CommandDrawKind::FillPolygon)) throw std::runtime_error("invalid command draw kind");
    return static_cast<CommandDrawKind>(value);
}

inline synth::ui::TextAlign DecodeTextAlign(std::uint8_t value)
{
    if (value > static_cast<std::uint8_t>(synth::ui::TextAlign::Right)) throw std::runtime_error("invalid text align");
    return static_cast<synth::ui::TextAlign>(value);
}

// A presence byte is 0 or 1 and nothing else. Rejecting every other value
// keeps this decoder exactly as strict as `presence()` in `protocol.ts`: a
// byte the encoder can never produce means the reader has lost its place in
// the node section, and silently reading the next four bytes as a colour
// would turn that into a plausible-looking frame.
inline bool DecodePresence(std::uint8_t value)
{
    if (value > 1) throw std::runtime_error("invalid presence flag");
    return value == 1;
}

inline std::optional<synth::Color> ReadOptionalColor(Reader& reader)
{
    if (!DecodePresence(reader.U8())) return std::nullopt;
    return reader.Color();
}

inline std::optional<synth::ui::TextStyle> ReadOptionalTextStyle(Reader& reader)
{
    if (!DecodePresence(reader.U8())) return std::nullopt;
    synth::ui::TextStyle style;
    style.size = reader.Float();
    style.color = reader.Color();
    style.align = DecodeTextAlign(reader.U8());
    return style;
}

inline std::optional<float> ReadOptionalFloat(Reader& reader)
{
    if (!DecodePresence(reader.U8())) return std::nullopt;
    return reader.Float();
}

}  // namespace detail

inline CommandBuffer SerializeNodeTree(const synth::ui::NodeTree& tree)
{
    std::vector<std::string> strings;
    std::unordered_map<std::string, std::uint32_t> stringIndices;
    auto intern = [&strings, &stringIndices](const std::string& value) {
        const auto found = stringIndices.find(value);
        if (found != stringIndices.end()) return found->second;
        strings.push_back(value);
        const auto index = static_cast<std::uint32_t>(strings.size() - 1);
        stringIndices.emplace(strings.back(), index);
        return index;
    };

    std::vector<Diagnostic> diagnostics;
    std::vector<synth::ui::Node> supportedNodes;
    std::unordered_set<std::string> droppedNodeIds;
    for (const auto& node : tree.nodes)
    {
        if (detail::MapNodeKind(node.kind))
        {
            supportedNodes.push_back(node);
            for (const auto& draw : node.drawCommands)
            {
                if (!detail::MapDrawKind(draw.kind))
                {
                    diagnostics.push_back({DiagnosticCode::UnsupportedPortableFeature, "draw command kind"});
                }
            }
        }
        else
        {
            diagnostics.push_back({DiagnosticCode::UnsupportedPortableFeature, "node kind"});
            droppedNodeIds.insert(node.id.value);
        }
    }
    for (auto& node : supportedNodes)
    {
        std::vector<synth::ui::NodeId> supportedChildren;
        supportedChildren.reserve(node.children.size());
        for (const auto& child : node.children)
        {
            if (droppedNodeIds.contains(child.value))
            {
                diagnostics.push_back({DiagnosticCode::UnsupportedPortableFeature, "child node"});
                continue;
            }
            supportedChildren.push_back(child);
        }
        node.children = std::move(supportedChildren);
    }

    std::vector<DecodedAction> actions;
    std::vector<std::array<std::int32_t, 3>> nodeActionIndices;
    for (const auto& node : supportedNodes)
    {
        std::array<std::int32_t, 3> indices = {-1, -1, -1};
        const std::array<const std::optional<synth::ui::Action>*, 3> nodeActions = {
            &node.action, &node.pointerDragAction, &node.doubleClickAction};
        for (std::size_t actionIndex = 0; actionIndex < nodeActions.size(); ++actionIndex)
        {
            if (!nodeActions[actionIndex]->has_value()) continue;
            const auto& action = **nodeActions[actionIndex];
            indices[actionIndex] = static_cast<std::int32_t>(actions.size());
            actions.push_back({action.name, action.value});
            intern(action.name);
            intern(action.value);
        }
        nodeActionIndices.push_back(indices);
    }

    std::vector<std::byte> stringSection;
    for (const auto& node : supportedNodes)
    {
        intern(node.id.value);
        intern(node.label);
        intern(node.text);
        intern(node.selectedOption);
        for (const auto& option : node.options)
        {
            intern(option.id);
            intern(option.label);
        }
        for (const auto& child : node.children) intern(child.value);
        for (const auto& draw : node.drawCommands) intern(draw.text);
    }
    for (const auto& diagnostic : diagnostics) intern(diagnostic.feature);
    detail::AppendU32(stringSection, static_cast<std::uint32_t>(strings.size()));
    for (const auto& value : strings)
    {
        detail::AppendU32(stringSection, static_cast<std::uint32_t>(value.size()));
        for (const char character : value) detail::AppendU8(stringSection, static_cast<std::uint8_t>(character));
    }

    auto stringIndex = [&stringIndices](const std::string& value) {
        const auto found = stringIndices.find(value);
        if (found != stringIndices.end()) return found->second;
        throw std::logic_error("missing command buffer string");
    };

    std::vector<std::byte> actionSection;
    detail::AppendU32(actionSection, static_cast<std::uint32_t>(actions.size()));
    for (const auto& action : actions)
    {
        detail::AppendU32(actionSection, stringIndex(action.name));
        detail::AppendU32(actionSection, stringIndex(action.value));
    }

    std::vector<std::byte> drawSection;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> drawRanges;
    std::uint32_t drawCount = 0;
    for (const auto& node : supportedNodes)
    {
        const std::uint32_t start = drawCount;
        for (const auto& draw : node.drawCommands)
        {
            if (!detail::MapDrawKind(draw.kind))
            {
                continue;
            }
            detail::AppendDraw(drawSection, draw, stringIndex(draw.text));
            ++drawCount;
        }
        drawRanges.emplace_back(start, drawCount - start);
    }
    std::vector<std::byte> drawTable;
    detail::AppendU32(drawTable, drawCount);
    drawTable.insert(drawTable.end(), drawSection.begin(), drawSection.end());

    std::vector<std::byte> nodeSection;
    detail::AppendU32(nodeSection, static_cast<std::uint32_t>(supportedNodes.size()));
    for (std::size_t index = 0; index < supportedNodes.size(); ++index)
    {
        const auto& node = supportedNodes[index];
        detail::AppendU32(nodeSection, stringIndex(node.id.value));
        detail::AppendU8(nodeSection, static_cast<std::uint8_t>(*detail::MapNodeKind(node.kind)));
        detail::AppendU8(nodeSection, node.checked ? 1 : 0);
        detail::AppendU8(nodeSection, node.selected ? 1 : 0);
        detail::AppendU8(nodeSection, node.enabled ? 1 : 0);
        detail::AppendBounds(nodeSection, node.bounds);
        detail::AppendU32(nodeSection, stringIndex(node.label));
        detail::AppendU32(nodeSection, stringIndex(node.text));
        detail::AppendU32(nodeSection, stringIndex(node.selectedOption));
        detail::AppendFloat(nodeSection, node.value);
        detail::AppendFloat(nodeSection, node.minValue);
        detail::AppendFloat(nodeSection, node.maxValue);
        detail::AppendFloat(nodeSection, node.step);
        detail::AppendFloat(nodeSection, node.scrollContentWidth);
        detail::AppendFloat(nodeSection, node.scrollContentHeight);
        detail::AppendOptionalColor(nodeSection, node.color);
        detail::AppendOptionalTextStyle(nodeSection, node.textStyle);
        detail::AppendOptionalColor(nodeSection, node.borderColor);
        detail::AppendOptionalFloat(nodeSection, node.borderWidth);
        detail::AppendOptionalFloat(nodeSection, node.cornerRadius);
        for (const auto actionIndex : nodeActionIndices[index]) detail::AppendI32(nodeSection, actionIndex);
        detail::AppendU32(nodeSection, drawRanges[index].first);
        detail::AppendU32(nodeSection, drawRanges[index].second);
        detail::AppendU32(nodeSection, static_cast<std::uint32_t>(node.options.size()));
        for (const auto& option : node.options)
        {
            detail::AppendU32(nodeSection, stringIndex(option.id));
            detail::AppendU32(nodeSection, stringIndex(option.label));
        }
        detail::AppendU32(nodeSection, static_cast<std::uint32_t>(node.children.size()));
        for (const auto& child : node.children) detail::AppendU32(nodeSection, stringIndex(child.value));
    }

    std::vector<std::byte> diagnosticSection;
    detail::AppendU32(diagnosticSection, static_cast<std::uint32_t>(diagnostics.size()));
    for (const auto& diagnostic : diagnostics)
    {
        detail::AppendU8(diagnosticSection, static_cast<std::uint8_t>(diagnostic.code));
        detail::AppendU8(diagnosticSection, 0);
        detail::AppendU16(diagnosticSection, 0);
        detail::AppendU32(diagnosticSection, stringIndex(diagnostic.feature));
    }

    CommandBuffer result;
    result.bytes.insert(result.bytes.end(), kCommandBufferMagic.begin(), kCommandBufferMagic.end());
    detail::AppendU16(result.bytes, kCommandBufferVersion);
    detail::AppendU16(result.bytes, 0);
    for (const auto size : {stringSection.size(), nodeSection.size(), actionSection.size(), drawTable.size(), diagnosticSection.size()})
    {
        detail::AppendU32(result.bytes, static_cast<std::uint32_t>(size));
    }
    result.bytes.insert(result.bytes.end(), stringSection.begin(), stringSection.end());
    result.bytes.insert(result.bytes.end(), nodeSection.begin(), nodeSection.end());
    result.bytes.insert(result.bytes.end(), actionSection.begin(), actionSection.end());
    result.bytes.insert(result.bytes.end(), drawTable.begin(), drawTable.end());
    result.bytes.insert(result.bytes.end(), diagnosticSection.begin(), diagnosticSection.end());
    return result;
}

inline DecodedCommandBuffer DecodeCommandBuffer(std::span<const std::byte> bytes)
{
    detail::Reader header(bytes);
    for (const auto expected : kCommandBufferMagic)
    {
        if (header.U8() != std::to_integer<std::uint8_t>(expected)) throw std::runtime_error("invalid browser command buffer magic");
    }
    DecodedCommandBuffer result;
    result.version = header.U16();
    if (result.version != kCommandBufferVersion) throw std::runtime_error("unsupported browser command buffer version");
    header.U16();
    const auto stringBytes = header.U32();
    const auto nodeBytes = header.U32();
    const auto actionBytes = header.U32();
    const auto drawBytes = header.U32();
    const auto diagnosticBytes = header.U32();
    const detail::Reader stringReader(header.Section(stringBytes));
    const detail::Reader nodeReader(header.Section(nodeBytes));
    const detail::Reader actionReader(header.Section(actionBytes));
    const detail::Reader drawReader(header.Section(drawBytes));
    const detail::Reader diagnosticReader(header.Section(diagnosticBytes));
    if (!header.Empty()) throw std::runtime_error("trailing browser command buffer bytes");

    auto readStrings = [](detail::Reader reader) {
        std::vector<std::string> strings;
        const auto count = reader.U32();
        strings.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto length = reader.U32();
            std::string value;
            value.reserve(length);
            for (std::uint32_t offset = 0; offset < length; ++offset) value.push_back(static_cast<char>(reader.U8()));
            strings.push_back(std::move(value));
        }
        if (!reader.Empty()) throw std::runtime_error("invalid string table");
        return strings;
    };
    result.strings = readStrings(stringReader);
    auto stringAt = [&result](std::uint32_t index) -> const std::string& {
        if (index >= result.strings.size()) throw std::runtime_error("invalid command buffer string index");
        return result.strings[index];
    };

    auto decodeActions = [&stringAt](detail::Reader reader) {
        std::vector<DecodedAction> actions;
        const auto count = reader.U32();
        actions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) actions.push_back({stringAt(reader.U32()), stringAt(reader.U32())});
        if (!reader.Empty()) throw std::runtime_error("invalid action table");
        return actions;
    };
    result.actions = decodeActions(actionReader);
    auto actionAt = [&result](std::int32_t index) -> std::optional<DecodedAction> {
        if (index == -1) return std::nullopt;
        if (index < 0 || static_cast<std::size_t>(index) >= result.actions.size()) throw std::runtime_error("invalid action index");
        return result.actions[static_cast<std::size_t>(index)];
    };

    {
        detail::Reader reader(drawReader);
        const auto count = reader.U32();
        result.drawCommands.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DecodedDrawCommand draw;
            draw.kind = detail::DecodeDrawKind(reader.U8());
            draw.textStyle.align = detail::DecodeTextAlign(reader.U8());
            reader.U16();
            draw.bounds = reader.Bounds();
            draw.from = reader.Point();
            draw.to = reader.Point();
            draw.color = reader.Color();
            draw.strokeWidth = reader.Float();
            draw.startRadians = reader.Float();
            draw.endRadians = reader.Float();
            draw.cornerRadius = reader.Float();
            draw.text = stringAt(reader.U32());
            draw.textStyle.size = reader.Float();
            draw.textStyle.color = reader.Color();
            const auto pointCount = reader.U32();
            draw.points.reserve(pointCount);
            for (std::uint32_t pointIndex = 0; pointIndex < pointCount; ++pointIndex) draw.points.push_back(reader.Point());
            result.drawCommands.push_back(std::move(draw));
        }
        if (!reader.Empty()) throw std::runtime_error("invalid draw table");
    }

    {
        detail::Reader reader(nodeReader);
        const auto count = reader.U32();
        result.nodes.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            DecodedNode node;
            node.id = stringAt(reader.U32());
            node.kind = detail::DecodeNodeKind(reader.U8());
            node.checked = reader.U8() != 0;
            node.selected = reader.U8() != 0;
            node.enabled = reader.U8() != 0;
            node.bounds = reader.Bounds();
            node.label = stringAt(reader.U32());
            node.text = stringAt(reader.U32());
            node.selectedOption = stringAt(reader.U32());
            node.value = reader.Float();
            node.minValue = reader.Float();
            node.maxValue = reader.Float();
            node.step = reader.Float();
            node.scrollContentWidth = reader.Float();
            node.scrollContentHeight = reader.Float();
            node.color = detail::ReadOptionalColor(reader);
            node.textStyle = detail::ReadOptionalTextStyle(reader);
            node.borderColor = detail::ReadOptionalColor(reader);
            node.borderWidth = detail::ReadOptionalFloat(reader);
            node.cornerRadius = detail::ReadOptionalFloat(reader);
            node.action = actionAt(reader.I32());
            node.pointerDragAction = actionAt(reader.I32());
            node.doubleClickAction = actionAt(reader.I32());
            node.drawStart = reader.U32();
            node.drawCount = reader.U32();
            if (node.drawStart > result.drawCommands.size() || node.drawCount > result.drawCommands.size() - node.drawStart)
                throw std::runtime_error("invalid node draw range");
            const auto optionCount = reader.U32();
            node.options.reserve(optionCount);
            for (std::uint32_t optionIndex = 0; optionIndex < optionCount; ++optionIndex)
                node.options.push_back({stringAt(reader.U32()), stringAt(reader.U32())});
            const auto childCount = reader.U32();
            node.children.reserve(childCount);
            for (std::uint32_t childIndex = 0; childIndex < childCount; ++childIndex) node.children.push_back(stringAt(reader.U32()));
            result.nodes.push_back(std::move(node));
        }
        if (!reader.Empty()) throw std::runtime_error("invalid node table");
    }

    {
        detail::Reader reader(diagnosticReader);
        const auto count = reader.U32();
        result.diagnostics.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            const auto code = static_cast<DiagnosticCode>(reader.U8());
            reader.U8();
            reader.U16();
            if (code != DiagnosticCode::UnsupportedPortableFeature) throw std::runtime_error("invalid diagnostic code");
            result.diagnostics.push_back({code, stringAt(reader.U32())});
        }
        if (!reader.Empty()) throw std::runtime_error("invalid diagnostic table");
    }
    return result;
}

namespace testing {
inline constexpr synth::ui::NodeKind UnsupportedNodeKind()
{
    return static_cast<synth::ui::NodeKind>(0xff);
}

inline constexpr synth::ui::DrawCommand::Kind UnsupportedDrawKind()
{
    return static_cast<synth::ui::DrawCommand::Kind>(0xff);
}
}  // namespace testing

}  // namespace synth_browser
