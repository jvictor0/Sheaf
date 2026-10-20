#pragma once

#include "synth/Color.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace synth::ui {

inline constexpr bool kPortableUiUsesJuce = false;

struct NodeId {
    std::string value;
    NodeId() = default;
    explicit NodeId(std::string v) : value(std::move(v)) {}
    NodeId(const char* v) : value(v) {}
    friend bool operator==(const NodeId&, const NodeId&) = default;
};

struct Point {
    float x = 0.0f;
    float y = 0.0f;
};

// Coordinate contract (sru-46, command buffer version 2).
//
// A node's `bounds` are expressed in its PARENT's coordinate space:
//   * the single parentless root's bounds are surface coordinates;
//   * a ScrollArea's children are relative to the scroll-CONTENT origin, so
//     scrolling is purely a backend view transform and producers never see a
//     scroll offset;
//   * every other node's bounds are relative to its parent's origin.
//
// A node's `drawCommands` geometry is NODE-LOCAL: against the owning node's
// own (0, 0, width, height) box. Node content clips to the node's bounds. A
// producer whose drawing overhangs its node box must grow the node's bounds;
// there is no classification and no rescue.
//
// No backend infers, guesses, or classifies any of the above. A node's
// rendered position is exactly its own bounds folded over the accumulated
// origins of its ancestor chain, plus scroll offset and uniform surface scale
// where applicable.
struct Bounds {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

enum class TextAlign {
    Left,
    Center,
    Right
};

struct TextStyle {
    float size = 14.0f;
    Color color = Color::Rgb(255, 255, 255);
    TextAlign align = TextAlign::Left;
};

struct Action {
    std::string name;
    std::string value;
    static Action Named(std::string actionName) {
        return Action{std::move(actionName), {}};
    }
    static Action WithValue(std::string actionName, std::string actionValue) {
        return Action{std::move(actionName), std::move(actionValue)};
    }
};

struct ControlOption {
    std::string id;
    std::string label;
};

struct DrawCommand {
    enum class Kind {
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
        FillPolygon
    };
    Kind kind = Kind::Fill;
    Bounds bounds{};
    Point from{};
    Point to{};
    Color color{};
    float strokeWidth = 1.0f;
    float startRadians = 0.0f;
    float endRadians = 0.0f;
    float cornerRadius = 0.0f;
    std::string text;
    TextStyle textStyle{};
    std::vector<Point> points{};

    static DrawCommand Fill(Color color);
    static DrawCommand Fill(Bounds bounds, Color color);
    static DrawCommand StrokeRect(Bounds bounds, Color color, float strokeWidth);
    static DrawCommand Line(Point from, Point to, Color color, float strokeWidth);
    static DrawCommand Arc(Bounds bounds, float startRadians, float endRadians, Color color, float strokeWidth);
    static DrawCommand Text(Bounds bounds, std::string text, TextStyle style);
    static DrawCommand FillEllipse(Bounds bounds, Color color);
    static DrawCommand StrokeEllipse(Bounds bounds, Color color, float strokeWidth);
    static DrawCommand FillRoundedRect(Bounds bounds, float cornerRadius, Color color);
    static DrawCommand StrokeRoundedRect(Bounds bounds, float cornerRadius, Color color, float strokeWidth);
    static DrawCommand Polyline(std::vector<Point> points, Color color, float strokeWidth);
    static DrawCommand FillPolygon(std::vector<Point> points, Color color);
};

class Visualizer {
public:
    Visualizer() = default;
    virtual ~Visualizer() = default;
    Visualizer(const Visualizer&) = delete;
    Visualizer& operator=(const Visualizer&) = delete;
    Visualizer(Visualizer&&) = delete;
    Visualizer& operator=(Visualizer&&) = delete;

    void SetBounds(Bounds bounds) { bounds_ = bounds; }
    const Bounds& GetBounds() const { return bounds_; }
    void SetVisible(bool visible) { visible_ = visible; }
    bool Visible() const { return visible_; }
    virtual bool WantsEncoderFrame() const noexcept { return true; }

    std::vector<DrawCommand> Draw() const
    {
        if (!visible_)
        {
            return {};
        }
        return DrawVisible();
    }

protected:
    virtual std::vector<DrawCommand> DrawVisible() const = 0;

private:
    Bounds bounds_{};
    bool visible_ = true;
};

enum class NodeKind {
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
    Draw
};

// Coordinate contract (sru-46, command buffer version 2).
//
// A node's `bounds` are expressed in its PARENT's coordinate space:
//   * the single parentless root's bounds are surface coordinates;
//   * a ScrollArea's children are relative to the scroll-CONTENT origin, so
//     scrolling is purely a backend view transform and producers never see a
//     scroll offset;
//   * every other node's bounds are relative to its parent's origin.
//
// A node's `drawCommands` geometry is NODE-LOCAL: against the owning node's
// own (0, 0, width, height) box. Node content clips to the node's bounds. A
// producer whose drawing overhangs its node box must grow the node's bounds;
// there is no classification and no rescue.
//
// No backend infers, guesses, or classifies any of the above. A node's
// rendered position is exactly its own bounds folded over the accumulated
// origins of its ancestor chain, plus scroll offset and uniform surface scale
// where applicable.
//
// Appearance contract (sru-45, sru-55). `color`, `textStyle`, and container
// border fields are optional: absent means the backend's plain default look,
// including that backend's existing selected and disabled treatment. A carried
// value beats every backend constant for that node, and selected/hover/pressed/
// disabled are DERIVED from the carried colour, never substituted from a
// palette. `color`'s meaning is per-kind:
//
//   Button, Toggle              the control fill
//   ComboBox, TextField         the field background
//   Slider                      the filled-track accent
//   Root, Row, Section,
//   ScrollArea                  the container background fill
//   Label, StatusText           the text background -- NEVER the glyphs
//   Draw                        nothing; draw commands carry their own colours
//
// Glyph colour ALWAYS comes from `textStyle`, never from `color`, so the two
// can never compete for the same pixel.
//
// For Root, Row, Section, and ScrollArea, a border is carried by three
// independent optional fields: `borderColor`, `borderWidth`, and
// `cornerRadius`. They use explicit wire presence flags, exactly like `color`
// and `textStyle`; a missing field is missing, never a sentinel value. A
// backend paints a border only when a border colour and width are both present,
// using a missing radius as zero. A present corner radius also rounds the
// container fill, which is what lets a real panel replace a rounded Draw
// underlay.
//
// `variant` is RETIRED and the residual set is EMPTY (design.md OQ1, task 1.2,
// completed in task 7.2). Every one of the nine strings the field ever carried
// -- `panel`, `quiet`, `title`, `muted-title`, `muted`, `primary`, `secondary`,
// `list-row`, `field` -- decided appearance and nothing else: glyph colour,
// control fill, label font size, or container background fill. All four are now
// carried directly by `color`, `textStyle`, `selected`, and the border fields
// above. There is no interaction-semantics residual to pin and no replacement
// field: the JUCE backend has no hover code at all, and the fill that
// `SemanticPanelComponent::SetSemantics` used to pick is now the carried
// `color`. So the model has no `variant` member, the version-2 command buffer
// neither encodes nor decodes one, and neither backend has a per-variant colour
// table -- which `scripts/check_ui_boundary.sh` enforces so none can come back.
//
// A caption is NOT a field. The component library emits a caption as an
// ordinary sibling `Label` node in the control's form-grid row, with a stable
// id derived from the control's own id -- `<controlId>.caption` inside a
// `<controlId>.row` container (design.md D5). Neither this record nor the
// command buffer gains a caption field, so no control kind can ever hide its
// caption the way `ComboBox::label` did (design.md OQ5).
struct Node {
    NodeId id;
    NodeKind kind = NodeKind::Label;
    Bounds bounds{};
    // Per-kind text the control renders itself: the button/toggle caption
    // text, the slider name, the text field's own label, a Label's fallback
    // text. `ComboBox` IGNORES it -- design.md OQ5 retired the combo-box
    // meaning of this field, which the JUCE backend used to feed to
    // `setTextWhenNothingSelected()` so that a producer's intended caption
    // vanished the moment an option was selected. A combo box's caption is a
    // sibling `Label` node like every other control's, and no placeholder
    // field replaces the retired meaning.
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
    std::vector<ControlOption> options;
    std::string selectedOption;
    // Direct appearance properties (sru-45); see the contract above this
    // struct for the per-kind meaning of `color`. Optional with explicit wire
    // presence flags: absent decodes as absent, never as a sentinel value a
    // producer could also have chosen deliberately.
    std::optional<Color> color{};
    std::optional<TextStyle> textStyle{};
    std::optional<Color> borderColor{};
    std::optional<float> borderWidth{};
    std::optional<float> cornerRadius{};
    std::optional<Action> action;
    std::optional<Action> pointerDragAction;
    std::optional<Action> doubleClickAction;
    std::vector<NodeId> children;
    std::vector<DrawCommand> drawCommands;
};

struct NodeTree {
    std::vector<Node> nodes;
};

class Surface {
public:
    using ActionHandler = std::function<void(const Action&)>;
    virtual ~Surface() = default;
    virtual NodeTree BuildTree() = 0;
    // DispatchAction is the authoritative route for backend-originated UI
    // actions. SetActionHandler registers an optional observer hook; it must
    // not duplicate the surface's own action routing.
    virtual void SetActionHandler(ActionHandler handler) = 0;
    virtual void DispatchAction(const Action& action) = 0;
};

// Optional capability (sprs-13): a Surface that resolves its BuildTree()
// output against a caller-supplied live content extent rather than a
// compiled-in size. Base Surface stays extent-free -- accessors like
// SynthApplication::PortableSurface() are constrained to return exactly
// `Surface&`, erasing the concrete type, so a compile-time trait on that
// accessor's result can never see anything beyond the base interface. A
// runtime check against this separate, polymorphic interface is the
// detection idiom that still works through that erasure: the shell
// dynamic_casts the Surface& it already holds, and a surface that doesn't
// additionally derive from ExtentAwareSurface simply resolves BuildTree()
// at its own compiled-in size (composition unchanged).
class ExtentAwareSurface {
public:
    virtual ~ExtentAwareSurface() = default;
    virtual void SetContentExtent(Bounds extent) = 0;
};

inline DrawCommand DrawCommand::Fill(Color color) {
    DrawCommand command;
    command.kind = Kind::Fill;
    command.color = color;
    return command;
}

inline DrawCommand DrawCommand::Fill(Bounds bounds, Color color) {
    DrawCommand command;
    command.kind = Kind::Fill;
    command.bounds = bounds;
    command.color = color;
    return command;
}

inline DrawCommand DrawCommand::StrokeRect(Bounds bounds, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::StrokeRect;
    command.bounds = bounds;
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::Line(Point from, Point to, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::Line;
    command.from = from;
    command.to = to;
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::Arc(Bounds bounds, float startRadians, float endRadians, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::Arc;
    command.bounds = bounds;
    command.startRadians = startRadians;
    command.endRadians = endRadians;
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::Text(Bounds bounds, std::string text, TextStyle style) {
    DrawCommand command;
    command.kind = Kind::Text;
    command.bounds = bounds;
    command.text = std::move(text);
    command.textStyle = style;
    return command;
}

inline DrawCommand DrawCommand::FillEllipse(Bounds bounds, Color color) {
    DrawCommand command;
    command.kind = Kind::FillEllipse;
    command.bounds = bounds;
    command.color = color;
    return command;
}

inline DrawCommand DrawCommand::StrokeEllipse(Bounds bounds, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::StrokeEllipse;
    command.bounds = bounds;
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::FillRoundedRect(Bounds bounds, float cornerRadius, Color color) {
    DrawCommand command;
    command.kind = Kind::FillRoundedRect;
    command.bounds = bounds;
    command.cornerRadius = cornerRadius;
    command.color = color;
    return command;
}

inline DrawCommand DrawCommand::StrokeRoundedRect(Bounds bounds, float cornerRadius, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::StrokeRoundedRect;
    command.bounds = bounds;
    command.cornerRadius = cornerRadius;
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::Polyline(std::vector<Point> points, Color color, float strokeWidth) {
    DrawCommand command;
    command.kind = Kind::Polyline;
    command.points = std::move(points);
    command.color = color;
    command.strokeWidth = strokeWidth;
    return command;
}

inline DrawCommand DrawCommand::FillPolygon(std::vector<Point> points, Color color) {
    DrawCommand command;
    command.kind = Kind::FillPolygon;
    command.points = std::move(points);
    command.color = color;
    return command;
}

}  // namespace synth::ui
