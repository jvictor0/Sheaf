#pragma once

// JUCE adapter for synth::ui::Surface — semantic controls plus draw-command
// painting. JUCE is confined to projects/synth/juce and explicit runtime hosts.

#include "synth/PortableUI.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth_juce {

inline juce::Colour UiToJuceColour(synth::Color color)
{
    return juce::Colour(color.r, color.g, color.b, color.a);
}

inline juce::Rectangle<int> UiToJuceRect(const synth::ui::Bounds& bounds)
{
    return juce::Rectangle<int>(static_cast<int>(std::lround(bounds.x)),
                                static_cast<int>(std::lround(bounds.y)),
                                static_cast<int>(std::lround(bounds.width)),
                                static_cast<int>(std::lround(bounds.height)));
}

inline juce::Rectangle<float> UiToJuceRectF(const synth::ui::Bounds& bounds)
{
    return juce::Rectangle<float>(bounds.x, bounds.y, bounds.width, bounds.height);
}

inline synth::ui::Bounds JuceToUiBounds(juce::Rectangle<float> bounds)
{
    return {bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight()};
}

inline juce::Justification ToJuceJustification(synth::ui::TextAlign align)
{
    switch (align)
    {
        case synth::ui::TextAlign::Center:
            return juce::Justification::centred;
        case synth::ui::TextAlign::Right:
            return juce::Justification::centredRight;
        case synth::ui::TextAlign::Left:
        default:
            return juce::Justification::centredLeft;
    }
}

inline bool HasExplicitBounds(const synth::ui::Bounds& bounds)
{
    return bounds.width > 0.0f && bounds.height > 0.0f;
}

inline juce::Rectangle<float> ResolveDrawBounds(const synth::ui::Bounds& bounds)
{
    return UiToJuceRectF(bounds);
}

inline juce::Point<float> ResolveDrawPoint(const synth::ui::Point& point)
{
    return {point.x, point.y};
}

inline void PaintDrawCommand(juce::Graphics& graphics,
                             const synth::ui::DrawCommand& command,
                             juce::Rectangle<float> nodeBounds)
{
    switch (command.kind)
    {
        case synth::ui::DrawCommand::Kind::Fill:
        {
            graphics.setColour(UiToJuceColour(command.color));
            graphics.fillRect(HasExplicitBounds(command.bounds) ? ResolveDrawBounds(command.bounds)
                                                                : nodeBounds);
            break;
        }
        case synth::ui::DrawCommand::Kind::StrokeRect:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const auto rect = ResolveDrawBounds(command.bounds);
            graphics.drawRect(rect, command.strokeWidth);
            break;
        }
        case synth::ui::DrawCommand::Kind::Line:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const juce::Point<float> from = ResolveDrawPoint(command.from);
            const juce::Point<float> to = ResolveDrawPoint(command.to);
            graphics.drawLine(from.x,
                              from.y,
                              to.x,
                              to.y,
                              command.strokeWidth);
            break;
        }
        case synth::ui::DrawCommand::Kind::Arc:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const auto rect = ResolveDrawBounds(command.bounds);
            juce::Path path;
            path.addCentredArc(rect.getCentreX(),
                               rect.getCentreY(),
                               rect.getWidth() * 0.5f,
                               rect.getHeight() * 0.5f,
                               0.0f,
                               command.startRadians,
                               command.endRadians,
                               true);
            graphics.strokePath(path,
                                juce::PathStrokeType(command.strokeWidth,
                                                     juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
            break;
        }
        case synth::ui::DrawCommand::Kind::Text:
        {
            const auto rect = ResolveDrawBounds(command.bounds);
            graphics.setColour(UiToJuceColour(command.textStyle.color));
            graphics.setFont(juce::Font(juce::FontOptions(command.textStyle.size)));
            graphics.drawText(command.text,
                              rect,
                              ToJuceJustification(command.textStyle.align),
                              false);
            break;
        }
        case synth::ui::DrawCommand::Kind::FillEllipse:
        {
            graphics.setColour(UiToJuceColour(command.color));
            graphics.fillEllipse(HasExplicitBounds(command.bounds) ? ResolveDrawBounds(command.bounds)
                                                                   : nodeBounds);
            break;
        }
        case synth::ui::DrawCommand::Kind::StrokeEllipse:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const auto rect = ResolveDrawBounds(command.bounds);
            graphics.drawEllipse(rect, command.strokeWidth);
            break;
        }
        case synth::ui::DrawCommand::Kind::FillRoundedRect:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const auto rect = ResolveDrawBounds(command.bounds);
            graphics.fillRoundedRectangle(rect, command.cornerRadius);
            break;
        }
        case synth::ui::DrawCommand::Kind::StrokeRoundedRect:
        {
            graphics.setColour(UiToJuceColour(command.color));
            const auto rect = ResolveDrawBounds(command.bounds);
            graphics.drawRoundedRectangle(rect, command.cornerRadius, command.strokeWidth);
            break;
        }
        case synth::ui::DrawCommand::Kind::Polyline:
        {
            if (command.points.size() < 2)
            {
                break;
            }
            graphics.setColour(UiToJuceColour(command.color));
            juce::Path path;
            const juce::Point<float> first = ResolveDrawPoint(command.points.front());
            path.startNewSubPath(first.x, first.y);
            for (std::size_t pointIx = 1; pointIx < command.points.size(); ++pointIx)
            {
                const juce::Point<float> point = ResolveDrawPoint(command.points[pointIx]);
                path.lineTo(point.x, point.y);
            }
            graphics.strokePath(path, juce::PathStrokeType(command.strokeWidth));
            break;
        }
        case synth::ui::DrawCommand::Kind::FillPolygon:
        {
            if (command.points.size() < 3)
            {
                break;
            }
            graphics.setColour(UiToJuceColour(command.color));
            juce::Path path;
            const juce::Point<float> first = ResolveDrawPoint(command.points.front());
            path.startNewSubPath(first.x, first.y);
            for (std::size_t pointIx = 1; pointIx < command.points.size(); ++pointIx)
            {
                const juce::Point<float> point = ResolveDrawPoint(command.points[pointIx]);
                path.lineTo(point.x, point.y);
            }
            path.closeSubPath();
            graphics.fillPath(path);
            break;
        }
    }
}

struct PortableControlEntry
{
    synth::ui::NodeId id;
    synth::ui::NodeKind kind = synth::ui::NodeKind::Label;
    std::unique_ptr<juce::Component> component;
};

class PortableComponent final : public juce::Component
{
public:
    struct ResolvedNode
    {
        juce::Rectangle<int> surfaceBounds;
        std::optional<synth::ui::NodeId> parentId;
    };

    explicit PortableComponent(synth::ui::Surface& surface)
        : m_surface(surface)
    {
    }

    void RefreshFromSurface()
    {
        m_tree = m_surface.BuildTree();
        ResolveTree();
        RebuildControls();
        LayoutControls();
        repaint();
    }

    juce::Component* FindByNodeId(const std::string& id)
    {
        if (const synth::ui::Node* root = RootNode(); root != nullptr && root->id.value == id)
        {
            return this;
        }
        const auto it = m_controlIndexById.find(id);
        if (it == m_controlIndexById.end())
        {
            return nullptr;
        }
        return m_controls[it->second].component.get();
    }

    juce::Rectangle<int> SurfaceBoundsForNode(const std::string& id) const
    {
        const auto resolved = m_resolvedByNodeId.find(id);
        return resolved != m_resolvedByNodeId.end() ? resolved->second.surfaceBounds
                                                    : juce::Rectangle<int>();
    }

    void resized() override
    {
        LayoutControls();
    }

    void paint(juce::Graphics& graphics) override
    {
        const synth::ui::Node* root = RootNode();
        if (root == nullptr)
        {
            graphics.fillAll(UiToJuceColour(synth::kSurfaceBackground));
            return;
        }

        PaintContainerAppearance(graphics,
                                 getLocalBounds().toFloat(),
                                 root->color.has_value()
                                     ? std::optional<juce::Colour>(
                                           StateColourFor(*root->color, root->selected, root->enabled))
                                     : std::optional<juce::Colour>(UiToJuceColour(synth::kSurfaceBackground)),
                                 BorderColourForNode(*root),
                                 BorderWidthForNode(*root),
                                 CornerRadiusForNode(*root));
    }

private:
    static constexpr float kPointerDragSensitivity = 0.0025f;
    static constexpr float kPointerDragThreshold = 0.001f;

    static juce::Colour StateColourFor(synth::Color color, bool selected, bool enabled)
    {
        juce::Colour juceColour = UiToJuceColour(color);
        if (!enabled)
        {
            return juceColour.darker(0.35f);
        }
        return selected ? juceColour.brighter(0.14f) : juceColour;
    }

    static std::optional<juce::Colour> BorderColourForNode(const synth::ui::Node& node)
    {
        return node.borderColor.has_value() ? std::optional<juce::Colour>(UiToJuceColour(*node.borderColor))
                                            : std::nullopt;
    }

    static std::optional<float> BorderWidthForNode(const synth::ui::Node& node)
    {
        if (!node.borderWidth.has_value() || *node.borderWidth <= 0.0f)
        {
            return std::nullopt;
        }
        return node.borderWidth;
    }

    static float CornerRadiusForNode(const synth::ui::Node& node)
    {
        return node.cornerRadius.value_or(0.0f);
    }

    static float ClampedCornerRadius(juce::Rectangle<float> bounds, float cornerRadius)
    {
        return std::clamp(cornerRadius, 0.0f, std::min(bounds.getWidth(), bounds.getHeight()) * 0.5f);
    }

    static void PaintContainerAppearance(juce::Graphics& graphics,
                                         juce::Rectangle<float> bounds,
                                         std::optional<juce::Colour> fill,
                                         std::optional<juce::Colour> border,
                                         std::optional<float> borderWidth,
                                         float cornerRadius)
    {
        const float clampedRadius = ClampedCornerRadius(bounds, cornerRadius);
        if (fill.has_value())
        {
            graphics.setColour(*fill);
            if (clampedRadius > 0.0f)
            {
                graphics.fillRoundedRectangle(bounds, clampedRadius);
            }
            else
            {
                graphics.fillRect(bounds);
            }
        }
        if (border.has_value() && borderWidth.has_value())
        {
            graphics.setColour(*border);
            if (clampedRadius > 0.0f)
            {
                const float pathRadius = std::max(0.0f, clampedRadius - *borderWidth * 0.5f);
                graphics.drawRoundedRectangle(bounds.reduced(*borderWidth * 0.5f),
                                              pathRadius,
                                              *borderWidth);
            }
            else
            {
                graphics.drawRect(bounds, *borderWidth);
            }
        }
    }

    class ScopedDispatchSuppression
    {
    public:
        explicit ScopedDispatchSuppression(bool& suppressed)
            : suppressed_(suppressed)
            , previous_(suppressed)
        {
            suppressed_ = true;
        }

        ~ScopedDispatchSuppression()
        {
            suppressed_ = previous_;
        }

        ScopedDispatchSuppression(const ScopedDispatchSuppression&) = delete;
        ScopedDispatchSuppression& operator=(const ScopedDispatchSuppression&) = delete;

    private:
        bool& suppressed_;
        bool previous_ = false;
    };

    class SemanticPanelComponent final : public juce::Component
    {
    public:
        void SetAppearance(std::optional<juce::Colour> fill,
                           std::optional<juce::Colour> border,
                           std::optional<float> borderWidth,
                           float cornerRadius)
        {
            fill_ = fill;
            border_ = border;
            borderWidth_ = borderWidth;
            cornerRadius_ = cornerRadius;
            repaint();
        }

        void paint(juce::Graphics& graphics) override
        {
            PaintContainerAppearance(graphics,
                                     getLocalBounds().toFloat(),
                                     fill_,
                                     border_,
                                     borderWidth_,
                                     cornerRadius_);
        }

    private:
        std::optional<juce::Colour> fill_;
        std::optional<juce::Colour> border_;
        std::optional<float> borderWidth_;
        float cornerRadius_ = 0.0f;
    };

    class PortableScrollAreaComponent final : public juce::Component
    {
    public:
        PortableScrollAreaComponent()
        {
            viewport_.setViewedComponent(&content_, false);
            viewport_.setScrollBarsShown(true, true);
            addAndMakeVisible(viewport_);
        }

        juce::Component& ContentComponent() noexcept
        {
            return content_;
        }

        juce::Viewport& Viewport() noexcept
        {
            return viewport_;
        }

        void SetAppearance(std::optional<juce::Colour> fill,
                           std::optional<juce::Colour> border,
                           std::optional<float> borderWidth,
                           float cornerRadius)
        {
            fill_ = fill;
            border_ = border;
            borderWidth_ = borderWidth;
            cornerRadius_ = cornerRadius;
            content_.SetAppearance(cornerRadius > 0.0f ? std::nullopt : fill,
                                   std::nullopt,
                                   std::nullopt,
                                   0.0f);
            repaint();
        }

        void SetContentExtent(int width, int height)
        {
            declaredContentWidth_ = width;
            declaredContentHeight_ = height;
            UpdateContentSize();
        }

        void resized() override
        {
            viewport_.setBounds(getLocalBounds());
            UpdateContentSize();
        }

        void paint(juce::Graphics& graphics) override
        {
            PaintContainerAppearance(graphics,
                                     getLocalBounds().toFloat(),
                                     fill_,
                                     std::nullopt,
                                     std::nullopt,
                                     cornerRadius_);
        }

        void paintOverChildren(juce::Graphics& graphics) override
        {
            PaintContainerAppearance(graphics,
                                     getLocalBounds().toFloat(),
                                     std::nullopt,
                                     border_,
                                     borderWidth_,
                                     cornerRadius_);
        }

    private:
        void UpdateContentSize()
        {
            const juce::Point<int> viewPosition = viewport_.getViewPosition();
            viewport_.setViewPosition(0, 0);
            content_.setSize(std::max(getWidth(), declaredContentWidth_),
                             std::max(getHeight(), declaredContentHeight_));
            viewport_.setViewPosition(viewPosition);
        }

        SemanticPanelComponent content_;
        juce::Viewport viewport_;
        int declaredContentWidth_ = 0;
        int declaredContentHeight_ = 0;
        std::optional<juce::Colour> fill_;
        std::optional<juce::Colour> border_;
        std::optional<float> borderWidth_;
        float cornerRadius_ = 0.0f;
    };

    class SemanticTextButton final : public juce::TextButton
    {
    public:
        using juce::TextButton::TextButton;

        std::function<void()> onDoubleClick;

        void mouseDoubleClick(const juce::MouseEvent& event) override
        {
            if (onDoubleClick)
            {
                onDoubleClick();
                return;
            }
            juce::TextButton::mouseDoubleClick(event);
        }
    };

    class RetainedDrawComponent final : public juce::Component
    {
    public:
        explicit RetainedDrawComponent(std::function<void(const synth::ui::NodeId&)> clickDispatch,
                                       std::function<void(const synth::ui::NodeId&, float)> dragDispatch,
                                       std::function<void(const synth::ui::NodeId&)> doubleClickDispatch)
            : clickDispatch_(std::move(clickDispatch))
            , dragDispatch_(std::move(dragDispatch))
            , doubleClickDispatch_(std::move(doubleClickDispatch))
        {
        }

        void SetNode(synth::ui::NodeId id,
                     std::vector<synth::ui::DrawCommand> commands,
                     juce::Rectangle<float> nodeBounds,
                     bool acceptsClick,
                     bool acceptsDrag,
                     bool acceptsDoubleClick)
        {
            id_ = std::move(id);
            commands_ = std::move(commands);
            nodeBounds_ = nodeBounds;
            acceptsClick_ = acceptsClick;
            acceptsDrag_ = acceptsDrag;
            acceptsDoubleClick_ = acceptsDoubleClick;
            // sru-52 widens this to the plain-click case. The inert case is
            // unchanged: a Draw node carrying no action at all still intercepts
            // nothing, so sru-25's translucent visualizer underlays keep passing
            // clicks through to the encoders beneath them.
            setInterceptsMouseClicks(acceptsClick_ || acceptsDrag_ || acceptsDoubleClick_, false);
            repaint();
        }

        void paint(juce::Graphics& graphics) override
        {
            for (const synth::ui::DrawCommand& command : commands_)
            {
                PaintDrawCommand(graphics, command, nodeBounds_);
            }
        }

        void mouseDown(const juce::MouseEvent& event) override
        {
            lastMousePosition_ = event.position;
            draggedPastThreshold_ = false;
        }

        void mouseDrag(const juce::MouseEvent& event) override
        {
            if (!acceptsDrag_)
            {
                return;
            }
            const auto deltaPoint = event.position - lastMousePosition_;
            const float delta = (deltaPoint.x - deltaPoint.y) * kPointerDragSensitivity;
            if (std::abs(delta) < kPointerDragThreshold)
            {
                return;
            }
            draggedPastThreshold_ = true;
            if (dragDispatch_)
            {
                dragDispatch_(id_, delta);
            }
            lastMousePosition_ = event.position;
        }

        // sru-52: the plain click is derived from the drag bookkeeping above
        // rather than from a threshold of its own, so a gesture that already
        // moved past `kPointerDragThreshold` is a drag and never also a click.
        // A release off the node is not a click either — `juce::Button` needs
        // `isOver` at mouse-up, and the DOM fires `click` on the common ancestor
        // of the press and the release — so neither backend reads one as one.
        void mouseUp(const juce::MouseEvent& event) override
        {
            if (!acceptsClick_ || draggedPastThreshold_
                || !getLocalBounds().toFloat().contains(event.position))
            {
                return;
            }
            if (clickDispatch_)
            {
                clickDispatch_(id_);
            }
        }

        void mouseDoubleClick(const juce::MouseEvent&) override
        {
            if (acceptsDoubleClick_ && doubleClickDispatch_)
            {
                doubleClickDispatch_(id_);
            }
        }

    private:
        synth::ui::NodeId id_;
        std::vector<synth::ui::DrawCommand> commands_;
        juce::Rectangle<float> nodeBounds_;
        juce::Point<float> lastMousePosition_;
        std::function<void(const synth::ui::NodeId&)> clickDispatch_;
        std::function<void(const synth::ui::NodeId&, float)> dragDispatch_;
        std::function<void(const synth::ui::NodeId&)> doubleClickDispatch_;
        bool acceptsClick_ = false;
        bool acceptsDrag_ = false;
        bool acceptsDoubleClick_ = false;
        bool draggedPastThreshold_ = false;
    };

    const synth::ui::Node* RootNode() const
    {
        if (!m_rootNodeId.has_value())
        {
            return nullptr;
        }
        return FindNode(*m_rootNodeId);
    }

    const synth::ui::Node* FindNode(const synth::ui::NodeId& id) const
    {
        for (const synth::ui::Node& node : m_tree.nodes)
        {
            if (node.id == id)
            {
                return &node;
            }
        }
        return nullptr;
    }

    void ResolveTree()
    {
        m_parentByNodeId.clear();
        m_resolvedByNodeId.clear();
        m_rootNodeId.reset();

        std::unordered_map<std::string, const synth::ui::Node*> nodesById;
        nodesById.reserve(m_tree.nodes.size());
        for (const synth::ui::Node& node : m_tree.nodes)
        {
            if (!nodesById.emplace(node.id.value, &node).second)
            {
                throw std::runtime_error("duplicate node id in portable JUCE frame: " + node.id.value);
            }
        }

        std::optional<std::string> multipleParentError;
        for (const synth::ui::Node& node : m_tree.nodes)
        {
            for (const synth::ui::NodeId& childId : node.children)
            {
                if (nodesById.find(childId.value) == nodesById.end())
                {
                    throw std::runtime_error("unknown child node in portable JUCE frame: " + childId.value);
                }
                const auto existing = m_parentByNodeId.find(childId.value);
                if (existing != m_parentByNodeId.end() && existing->second != node.id)
                {
                    if (!multipleParentError.has_value())
                    {
                        multipleParentError = "node " + childId.value + " has multiple parents";
                    }
                }
                else
                {
                    m_parentByNodeId[childId.value] = node.id;
                }
            }
        }

        std::vector<const synth::ui::Node*> roots;
        for (const synth::ui::Node& node : m_tree.nodes)
        {
            if (m_parentByNodeId.find(node.id.value) == m_parentByNodeId.end())
            {
                roots.push_back(&node);
            }
        }
        if (!m_tree.nodes.empty() && roots.size() != 1)
        {
            throw std::runtime_error("portable JUCE frame requires one parentless root, found "
                                     + std::to_string(roots.size()));
        }
        if (!roots.empty() && roots.front()->kind != synth::ui::NodeKind::Root)
        {
            throw std::runtime_error("parentless portable JUCE node must be a root");
        }

        std::unordered_map<std::string, int> validationState;
        std::function<void(const synth::ui::Node&)> validate = [&](const synth::ui::Node& node) {
            const int state = validationState[node.id.value];
            if (state == 1)
            {
                throw std::runtime_error("cycle in portable JUCE frame at " + node.id.value);
            }
            if (state == 2)
            {
                return;
            }
            validationState[node.id.value] = 1;
            for (const synth::ui::NodeId& childId : node.children)
            {
                validate(*nodesById.at(childId.value));
            }
            validationState[node.id.value] = 2;
        };
        for (const synth::ui::Node& node : m_tree.nodes)
        {
            validate(node);
        }
        if (multipleParentError.has_value())
        {
            throw std::runtime_error(*multipleParentError);
        }
        if (roots.empty())
        {
            return;
        }

        m_rootNodeId = roots.front()->id;
        std::function<void(const synth::ui::Node&, juce::Point<int>)> resolve =
            [&](const synth::ui::Node& node, juce::Point<int> parentOrigin) {
                juce::Rectangle<int> bounds = UiToJuceRect(node.bounds);
                bounds.translate(parentOrigin.x, parentOrigin.y);
                const auto parent = m_parentByNodeId.find(node.id.value);
                m_resolvedByNodeId[node.id.value] = {
                    bounds,
                    parent == m_parentByNodeId.end()
                        ? std::optional<synth::ui::NodeId>()
                        : std::optional<synth::ui::NodeId>(parent->second)};
                const juce::Point<int> childOrigin(bounds.getX(), bounds.getY());
                for (const synth::ui::NodeId& childId : node.children)
                {
                    // Child ids and cycles were validated above, so this pure
                    // coordinate fold can use the indexed child lookup directly.
                    resolve(*nodesById.at(childId.value), childOrigin);
                }
            };
        resolve(*roots.front(), {});
    }

    juce::Component* SemanticHostFor(const ResolvedNode& resolved)
    {
        std::optional<synth::ui::NodeId> parentId = resolved.parentId;
        while (parentId.has_value())
        {
            const auto control = m_controlIndexById.find(parentId->value);
            if (control != m_controlIndexById.end()
                && IsSemanticHostKind(m_controls[control->second].kind))
            {
                juce::Component* host = m_controls[control->second].component.get();
                if (m_controls[control->second].kind == synth::ui::NodeKind::ScrollArea)
                {
                    auto& scroll = static_cast<PortableScrollAreaComponent&>(*host);
                    return &scroll.ContentComponent();
                }
                return host;
            }
            const auto parentResolved = m_resolvedByNodeId.find(parentId->value);
            parentId = parentResolved != m_resolvedByNodeId.end()
                           ? parentResolved->second.parentId
                           : std::optional<synth::ui::NodeId>();
        }
        return this;
    }

    static bool IsSemanticHostKind(synth::ui::NodeKind kind)
    {
        return kind == synth::ui::NodeKind::Row || kind == synth::ui::NodeKind::Section
               || kind == synth::ui::NodeKind::ScrollArea;
    }

    juce::Rectangle<int> HostLocalBounds(const ResolvedNode& resolved) const
    {
        juce::Rectangle<int> bounds = resolved.surfaceBounds;
        std::optional<synth::ui::NodeId> parentId = resolved.parentId;
        while (parentId.has_value())
        {
            const auto control = m_controlIndexById.find(parentId->value);
            if (control != m_controlIndexById.end()
                && IsSemanticHostKind(m_controls[control->second].kind))
            {
                const auto hostResolved = m_resolvedByNodeId.find(parentId->value);
                if (hostResolved != m_resolvedByNodeId.end())
                {
                    bounds.translate(-hostResolved->second.surfaceBounds.getX(),
                                     -hostResolved->second.surfaceBounds.getY());
                }
                return bounds;
            }

            const auto parentResolved = m_resolvedByNodeId.find(parentId->value);
            parentId = parentResolved != m_resolvedByNodeId.end()
                           ? parentResolved->second.parentId
                           : std::optional<synth::ui::NodeId>();
        }
        return bounds;
    }

    void DispatchBackendAction(const synth::ui::Action& action)
    {
        if (m_suppressActionDispatch)
        {
            return;
        }
        m_surface.DispatchAction(action);
    }

    // sru-34: a disabled semantic node never dispatches its user action. Every
    // backend-originated dispatch resolves its node through this gate, so
    // click, change, double-click, and pointer drag are all covered by one
    // rule instead of relying on each JUCE control's own disabled handling.
    const synth::ui::Node* EnabledNode(const synth::ui::NodeId& id) const
    {
        const synth::ui::Node* node = FindNode(id);
        return node != nullptr && node->enabled ? node : nullptr;
    }

    void DispatchCurrentNodeAction(const synth::ui::NodeId& id)
    {
        if (const synth::ui::Node* node = EnabledNode(id); node != nullptr && node->action.has_value())
        {
            DispatchBackendAction(*node->action);
        }
    }

    void DispatchCurrentNodeActionWithAppendedValue(const synth::ui::NodeId& id,
                                                    std::string value)
    {
        if (const synth::ui::Node* node = EnabledNode(id); node != nullptr && node->action.has_value())
        {
            synth::ui::Action dispatched = *node->action;
            if (!dispatched.value.empty())
            {
                dispatched.value += ':';
            }
            dispatched.value += value;
            DispatchBackendAction(dispatched);
        }
    }

    void DispatchCurrentNodePointerDragAction(const synth::ui::NodeId& id, float delta)
    {
        const synth::ui::Node* node = EnabledNode(id);
        if (node == nullptr || !node->pointerDragAction.has_value())
        {
            return;
        }
        synth::ui::Action dispatched = *node->pointerDragAction;
        const std::string deltaValue = std::to_string(delta);
        if (dispatched.value.empty())
        {
            dispatched.value = deltaValue;
        }
        else
        {
            const std::size_t lastColon = dispatched.value.rfind(':');
            if (lastColon != std::string::npos)
            {
                dispatched.value = dispatched.value.substr(0, lastColon + 1) + deltaValue;
            }
            else
            {
                dispatched.value = deltaValue;
            }
        }
        DispatchBackendAction(dispatched);
    }

    void DispatchCurrentNodeDoubleClickAction(const synth::ui::NodeId& id)
    {
        if (const synth::ui::Node* node = EnabledNode(id); node != nullptr && node->doubleClickAction.has_value())
        {
            DispatchBackendAction(*node->doubleClickAction);
        }
    }

    void RebuildControls()
    {
        const synth::ui::Node* root = RootNode();
        if (root == nullptr)
        {
            m_controls.clear();
            m_controlIndexById.clear();
            return;
        }

        std::unordered_map<std::string, std::size_t> oldIndexById = m_controlIndexById;
        std::unordered_map<std::string, std::size_t> newIndexById;
        std::vector<PortableControlEntry> nextControls;
        nextControls.reserve(m_controls.size());

        m_renderedNodeIds.clear();
        CollectRenderableDescendants(*root);

        for (const synth::ui::NodeId& nodeId : m_renderedNodeIds)
        {
            const synth::ui::Node* node = FindNode(nodeId);
            if (node == nullptr || !IsRenderableKind(node->kind))
            {
                continue;
            }

            const auto existing = oldIndexById.find(node->id.value);
            if (existing != oldIndexById.end() && existing->second < m_controls.size()
                && m_controls[existing->second].kind == node->kind)
            {
                PortableControlEntry entry = std::move(m_controls[existing->second]);
                UpdateControlFromNode(*entry.component, *node);
                nextControls.push_back(std::move(entry));
                newIndexById[node->id.value] = nextControls.size() - 1;
                oldIndexById.erase(existing);
                continue;
            }

            PortableControlEntry entry;
            entry.id = node->id;
            entry.kind = node->kind;
            entry.component = CreateControlForNode(*node);
            UpdateControlFromNode(*entry.component, *node);
            nextControls.push_back(std::move(entry));
            newIndexById[node->id.value] = nextControls.size() - 1;
        }

        m_controls = std::move(nextControls);
        m_controlIndexById = std::move(newIndexById);

        for (const synth::ui::NodeId& nodeId : m_renderedNodeIds)
        {
            const auto controlIt = m_controlIndexById.find(nodeId.value);
            const auto resolvedIt = m_resolvedByNodeId.find(nodeId.value);
            if (controlIt == m_controlIndexById.end() || resolvedIt == m_resolvedByNodeId.end())
            {
                continue;
            }

            juce::Component& component = *m_controls[controlIt->second].component;
            juce::Component* host = SemanticHostFor(resolvedIt->second);
            if (host == nullptr)
            {
                host = this;
            }
            const bool hadKeyboardFocus = component.hasKeyboardFocus(true);
            juce::Component::SafePointer<juce::Component> safeComponent(&component);
            if (component.getParentComponent() != host)
            {
                host->addAndMakeVisible(component);
            }
            component.setBounds(HostLocalBounds(resolvedIt->second));
            component.toFront(false);
            if (hadKeyboardFocus && safeComponent != nullptr
                && !safeComponent->hasKeyboardFocus(true))
            {
                safeComponent->grabKeyboardFocus();
            }
        }
    }

    void LayoutControls()
    {
        for (const synth::ui::NodeId& nodeId : m_renderedNodeIds)
        {
            const auto controlIt = m_controlIndexById.find(nodeId.value);
            const auto resolvedIt = m_resolvedByNodeId.find(nodeId.value);
            if (controlIt == m_controlIndexById.end() || resolvedIt == m_resolvedByNodeId.end())
            {
                continue;
            }

            juce::Component& control = *m_controls[controlIt->second].component;
            juce::Component* host = control.getParentComponent();
            if (host != nullptr)
            {
                control.setBounds(HostLocalBounds(resolvedIt->second));
            }
        }
    }

    void CollectRenderableDescendants(const synth::ui::Node& parent)
    {
        for (const synth::ui::NodeId& childId : parent.children)
        {
            const synth::ui::Node* node = FindNode(childId);
            if (node == nullptr)
            {
                continue;
            }

            if (IsRenderableKind(node->kind))
            {
                m_renderedNodeIds.push_back(node->id);
            }

            CollectRenderableDescendants(*node);
        }
    }

    static bool IsRenderableKind(synth::ui::NodeKind kind)
    {
        switch (kind)
        {
            case synth::ui::NodeKind::Row:
            case synth::ui::NodeKind::Section:
            case synth::ui::NodeKind::ScrollArea:
            case synth::ui::NodeKind::Label:
            case synth::ui::NodeKind::StatusText:
            case synth::ui::NodeKind::Button:
            case synth::ui::NodeKind::Toggle:
            case synth::ui::NodeKind::Slider:
            case synth::ui::NodeKind::ComboBox:
            case synth::ui::NodeKind::TextField:
            case synth::ui::NodeKind::Draw:
                return true;
            default:
                return false;
        }
    }

    static juce::Colour TextColourForNode(const synth::ui::Node& node)
    {
        if (!node.enabled)
        {
            return juce::Colour(125, 132, 138);
        }
        return juce::Colours::white;
    }

    static juce::Colour GlyphColourForNode(const synth::ui::Node& node)
    {
        return node.textStyle.has_value() ? UiToJuceColour(node.textStyle->color)
                                          : TextColourForNode(node);
    }

    static float TextSizeForNode(const synth::ui::Node& node)
    {
        return node.textStyle.has_value() ? node.textStyle->size : 13.0f;
    }

    static juce::Colour ButtonColourForNode(const synth::ui::Node& node)
    {
        if (!node.enabled)
        {
            return juce::Colour(45, 49, 53);
        }
        if (node.selected)
        {
            return juce::Colour(54, 91, 110);
        }
        return juce::Colour(42, 47, 52);
    }

    static juce::Colour ControlFillForNode(const synth::ui::Node& node)
    {
        return node.color.has_value() ? StateColourFor(*node.color, node.selected, node.enabled)
                                      : ButtonColourForNode(node);
    }

    static std::optional<juce::Colour> BackgroundFillForNode(const synth::ui::Node& node)
    {
        if (!node.color.has_value())
        {
            return std::nullopt;
        }
        return StateColourFor(*node.color, node.selected, node.enabled);
    }

    static bool ComboOptionsMatch(const juce::ComboBox& combo, const std::vector<synth::ui::ControlOption>& options)
    {
        if (combo.getNumItems() != static_cast<int>(options.size()))
        {
            return false;
        }

        for (int ix = 0; ix < static_cast<int>(options.size()); ++ix)
        {
            const synth::ui::ControlOption& option = options[static_cast<std::size_t>(ix)];
            if (combo.getItemId(ix) != ix + 1 || combo.getItemText(ix) != juce::String(option.label))
            {
                return false;
            }
        }

        return true;
    }

    static void RebuildComboOptions(juce::ComboBox& combo, const std::vector<synth::ui::ControlOption>& options)
    {
        combo.clear(juce::dontSendNotification);
        for (int ix = 0; ix < static_cast<int>(options.size()); ++ix)
        {
            combo.addItem(options[static_cast<std::size_t>(ix)].label, ix + 1);
        }
    }

    std::unique_ptr<juce::Component> CreateControlForNode(const synth::ui::Node& node)
    {
        switch (node.kind)
        {
            case synth::ui::NodeKind::Label:
            case synth::ui::NodeKind::StatusText:
            {
                auto label = std::make_unique<juce::Label>();
                label->setJustificationType(juce::Justification::centredLeft);
                label->setInterceptsMouseClicks(false, false);
                return label;
            }
            case synth::ui::NodeKind::Row:
            case synth::ui::NodeKind::Section:
            {
                return std::make_unique<SemanticPanelComponent>();
            }
            case synth::ui::NodeKind::ScrollArea:
            {
                return std::make_unique<PortableScrollAreaComponent>();
            }
            case synth::ui::NodeKind::Button:
            {
                auto button = std::make_unique<SemanticTextButton>();
                button->setButtonText(node.label);
                const synth::ui::NodeId id = node.id;
                button->onClick = [this, id] {
                    DispatchCurrentNodeAction(id);
                };
                button->onDoubleClick = [this, id] {
                    DispatchCurrentNodeDoubleClickAction(id);
                };
                return button;
            }
            case synth::ui::NodeKind::Toggle:
            {
                auto toggle = std::make_unique<juce::ToggleButton>(node.label);
                const synth::ui::NodeId id = node.id;
                toggle->onClick = [this, toggle = toggle.get(), id] {
                    DispatchCurrentNodeActionWithAppendedValue(
                        id, toggle->getToggleState() ? "1" : "0");
                };
                ScopedDispatchSuppression suppress(m_suppressActionDispatch);
                toggle->setToggleState(node.checked, juce::dontSendNotification);
                return toggle;
            }
            case synth::ui::NodeKind::Slider:
            {
                auto slider = std::make_unique<juce::Slider>();
                slider->setRange(node.minValue, node.maxValue, node.step);
                slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18);
                if (!node.label.empty())
                {
                    slider->setName(node.label);
                }
                const synth::ui::NodeId id = node.id;
                slider->onValueChange = [this, slider = slider.get(), id] {
                    if (m_suppressActionDispatch)
                    {
                        return;
                    }
                    DispatchCurrentNodeActionWithAppendedValue(
                        id, juce::String(slider->getValue()).toStdString());
                };
                ScopedDispatchSuppression suppress(m_suppressActionDispatch);
                slider->setValue(node.value, juce::dontSendNotification);
                return slider;
            }
            case synth::ui::NodeKind::ComboBox:
            {
                auto combo = std::make_unique<juce::ComboBox>();
                int selectedIndex = -1;
                for (int ix = 0; ix < static_cast<int>(node.options.size()); ++ix)
                {
                    const synth::ui::ControlOption& option = node.options[static_cast<std::size_t>(ix)];
                    combo->addItem(option.label, ix + 1);
                    if (option.id == node.selectedOption)
                    {
                        selectedIndex = ix + 1;
                    }
                }
                const synth::ui::NodeId id = node.id;
                combo->onChange = [this, combo = combo.get(), id] {
                    if (m_suppressActionDispatch)
                    {
                        return;
                    }
                    const synth::ui::Node* current = FindNode(id);
                    if (current == nullptr || !current->action.has_value())
                    {
                        return;
                    }
                    const int selected = combo->getSelectedItemIndex();
                    if (selected < 0 || selected >= static_cast<int>(current->options.size()))
                    {
                        return;
                    }
                    DispatchCurrentNodeActionWithAppendedValue(
                        id, current->options[static_cast<std::size_t>(selected)].id);
                };
                ScopedDispatchSuppression suppress(m_suppressActionDispatch);
                if (selectedIndex > 0)
                {
                    combo->setSelectedId(selectedIndex, juce::dontSendNotification);
                }
                return combo;
            }
            case synth::ui::NodeKind::TextField:
            {
                auto editor = std::make_unique<juce::TextEditor>();
                editor->setText(node.text);
                if (!node.label.empty())
                {
                    editor->setTextToShowWhenEmpty(node.label, juce::Colours::grey);
                }
                const synth::ui::NodeId id = node.id;
                auto textCommitted = std::make_shared<bool>(false);
                const auto commitText = [this, editor = editor.get(), id, textCommitted] {
                    if (*textCommitted)
                    {
                        return;
                    }
                    *textCommitted = true;
                    DispatchCurrentNodeActionWithAppendedValue(id, editor->getText().toStdString());
                };
                editor->onTextChange = [textCommitted] {
                    *textCommitted = false;
                };
                editor->onReturnKey = [editor = editor.get(), commitText] {
                    commitText();
                    editor->giveAwayKeyboardFocus();
                };
                editor->onFocusLost = [commitText] {
                    commitText();
                };
                return editor;
            }
            case synth::ui::NodeKind::Draw:
            {
                auto draw = std::make_unique<RetainedDrawComponent>(
                    [this](const synth::ui::NodeId& id) {
                        DispatchCurrentNodeAction(id);
                    },
                    [this](const synth::ui::NodeId& id, float delta) {
                        DispatchCurrentNodePointerDragAction(id, delta);
                    },
                    [this](const synth::ui::NodeId& id) {
                        DispatchCurrentNodeDoubleClickAction(id);
                    });
                return draw;
            }
            default:
                return std::make_unique<juce::Component>();
        }
    }

    void UpdateControlFromNode(juce::Component& component, const synth::ui::Node& node)
    {
        ScopedDispatchSuppression suppress(m_suppressActionDispatch);
        component.setEnabled(node.enabled);
        component.setAlpha(node.enabled ? 1.0f : 0.58f);
        switch (node.kind)
        {
            case synth::ui::NodeKind::Row:
            case synth::ui::NodeKind::Section:
            {
                auto& panel = static_cast<SemanticPanelComponent&>(component);
                panel.SetAppearance(BackgroundFillForNode(node),
                                    BorderColourForNode(node),
                                    BorderWidthForNode(node),
                                    CornerRadiusForNode(node));
                break;
            }
            case synth::ui::NodeKind::ScrollArea:
            {
                auto& scroll = static_cast<PortableScrollAreaComponent&>(component);
                scroll.SetAppearance(BackgroundFillForNode(node),
                                     BorderColourForNode(node),
                                     BorderWidthForNode(node),
                                     CornerRadiusForNode(node));
                scroll.SetContentExtent(static_cast<int>(std::lround(node.scrollContentWidth)),
                                        static_cast<int>(std::lround(node.scrollContentHeight)));
                break;
            }
            case synth::ui::NodeKind::Label:
            case synth::ui::NodeKind::StatusText:
            {
                auto& label = static_cast<juce::Label&>(component);
                label.setText(node.text.empty() ? node.label : node.text, juce::dontSendNotification);
                label.setColour(juce::Label::textColourId, GlyphColourForNode(node));
                if (const auto background = BackgroundFillForNode(node); background.has_value())
                {
                    label.setColour(juce::Label::backgroundColourId, *background);
                }
                else
                {
                    label.removeColour(juce::Label::backgroundColourId);
                }
                label.setFont(juce::Font(juce::FontOptions(TextSizeForNode(node))));
                label.setJustificationType(node.textStyle.has_value()
                                               ? ToJuceJustification(node.textStyle->align)
                                               : juce::Justification::centredLeft);
                break;
            }
            case synth::ui::NodeKind::Button:
            {
                auto& button = static_cast<juce::TextButton&>(component);
                button.setButtonText(node.label);
                const juce::Colour fill = ControlFillForNode(node);
                button.setColour(juce::TextButton::buttonColourId, fill);
                button.setColour(juce::TextButton::buttonOnColourId,
                                 node.color.has_value()
                                     ? UiToJuceColour(*node.color).brighter(0.24f)
                                     : fill.brighter(0.14f));
                button.setColour(juce::TextButton::textColourOffId, GlyphColourForNode(node));
                button.setColour(juce::TextButton::textColourOnId, GlyphColourForNode(node));
                break;
            }
            case synth::ui::NodeKind::Toggle:
            {
                auto& toggle = static_cast<juce::ToggleButton&>(component);
                toggle.setButtonText(node.label);
                toggle.setToggleState(node.checked, juce::dontSendNotification);
                if (node.color.has_value())
                {
                    // JUCE ToggleButton exposes no fill id, so carried control
                    // colour is represented by the checkmark/tick colour.
                    const juce::Colour fill = StateColourFor(*node.color, node.selected || node.checked, node.enabled);
                    toggle.setColour(juce::ToggleButton::tickColourId, fill);
                    toggle.setColour(juce::ToggleButton::tickDisabledColourId,
                                     StateColourFor(*node.color, node.selected || node.checked, false));
                }
                else
                {
                    toggle.removeColour(juce::ToggleButton::tickColourId);
                    toggle.removeColour(juce::ToggleButton::tickDisabledColourId);
                }
                toggle.setColour(juce::ToggleButton::textColourId, GlyphColourForNode(node));
                break;
            }
            case synth::ui::NodeKind::Slider:
            {
                auto& slider = static_cast<juce::Slider&>(component);
                slider.setRange(node.minValue, node.maxValue, node.step);
                if (node.color.has_value())
                {
                    slider.setColour(juce::Slider::trackColourId,
                                     StateColourFor(*node.color, node.selected, node.enabled));
                }
                else
                {
                    slider.removeColour(juce::Slider::trackColourId);
                }
                if (node.textStyle.has_value())
                {
                    slider.setColour(juce::Slider::textBoxTextColourId, UiToJuceColour(node.textStyle->color));
                }
                else
                {
                    slider.removeColour(juce::Slider::textBoxTextColourId);
                }
                if (!slider.isMouseButtonDown(true))
                {
                    slider.setValue(node.value, juce::dontSendNotification);
                }
                break;
            }
            case synth::ui::NodeKind::ComboBox:
            {
                auto& combo = static_cast<juce::ComboBox&>(component);
                if (node.color.has_value())
                {
                    combo.setColour(juce::ComboBox::backgroundColourId,
                                    StateColourFor(*node.color, node.selected, node.enabled));
                }
                else
                {
                    combo.removeColour(juce::ComboBox::backgroundColourId);
                }
                combo.setColour(juce::ComboBox::textColourId, GlyphColourForNode(node));
                if (!ComboOptionsMatch(combo, node.options))
                {
                    RebuildComboOptions(combo, node.options);
                }
                for (int ix = 0; ix < static_cast<int>(node.options.size()); ++ix)
                {
                    if (node.options[static_cast<std::size_t>(ix)].id == node.selectedOption)
                    {
                        combo.setSelectedId(ix + 1, juce::dontSendNotification);
                        break;
                    }
                }
                break;
            }
            case synth::ui::NodeKind::TextField:
            {
                auto& editor = static_cast<juce::TextEditor&>(component);
                editor.setColour(juce::TextEditor::backgroundColourId,
                                 node.color.has_value()
                                     ? StateColourFor(*node.color, node.selected, node.enabled)
                                     : juce::Colour(22, 25, 28));
                editor.setColour(juce::TextEditor::textColourId, GlyphColourForNode(node));
                editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(72, 84, 94));
                if (!editor.hasKeyboardFocus(true) && editor.getText() != juce::String(node.text))
                {
                    editor.setText(node.text, juce::dontSendNotification);
                }
                break;
            }
            case synth::ui::NodeKind::Draw:
            {
                auto& draw = static_cast<RetainedDrawComponent&>(component);
                const juce::Rectangle<float> nodeBounds(0.0f,
                                                        0.0f,
                                                        node.bounds.width,
                                                        node.bounds.height);
                draw.SetNode(node.id,
                             node.drawCommands,
                             nodeBounds,
                             node.action.has_value(),
                             node.pointerDragAction.has_value(),
                             node.doubleClickAction.has_value());
                break;
            }
            default:
                break;
        }
    }

    synth::ui::Surface& m_surface;
    synth::ui::NodeTree m_tree;
    std::vector<PortableControlEntry> m_controls;
    std::unordered_map<std::string, std::size_t> m_controlIndexById;
    std::unordered_map<std::string, synth::ui::NodeId> m_parentByNodeId;
    std::unordered_map<std::string, ResolvedNode> m_resolvedByNodeId;
    std::optional<synth::ui::NodeId> m_rootNodeId;
    std::vector<synth::ui::NodeId> m_renderedNodeIds;
    bool m_suppressActionDispatch = false;
};

}  // namespace synth_juce
