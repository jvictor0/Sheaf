#pragma once

#include "synth/PortableUI.hpp"
#include "synth/PortableUIMetrics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace synth::ui {

using DrawFactory = std::function<std::vector<DrawCommand>(Bounds nodeExtent)>;

struct SpacingMetrics {
    float padding = 12.0f;
    float gap = 8.0f;
    float rowHeight = 24.0f;
    float labelGap = 8.0f;
};
inline constexpr SpacingMetrics kSpacing{};

struct Extent {
    enum class Mode { Fixed, Intrinsic, Fraction, Weighted };
    Mode mode = Mode::Intrinsic;
    float value = 0.0f;   // px when Fixed; 0..1 when Fraction; weight when Weighted
    float minimum = 0.0f;
    float maximum = 0.0f; // 0 means "no maximum"

    static Extent Px(float pixels);
    static Extent Intrinsic();
    static Extent Fraction(float fractionOfContentExtent);
    static Extent Weight(float weight);
    Extent& Min(float pixels);
    Extent& Max(float pixels);
};

inline Extent Extent::Px(float p)       { return Extent{Mode::Fixed, p, 0.0f, 0.0f}; }
inline Extent Extent::Intrinsic()       { return Extent{Mode::Intrinsic, 0.0f, 0.0f, 0.0f}; }
inline Extent Extent::Fraction(float f) { return Extent{Mode::Fraction, f, 0.0f, 0.0f}; }
inline Extent Extent::Weight(float w)   { return Extent{Mode::Weighted, w, 0.0f, 0.0f}; }
inline Extent& Extent::Min(float p) { minimum = p; return *this; }
inline Extent& Extent::Max(float p) { maximum = p; return *this; }

struct LayoutOptions {
    Extent main = Extent::Intrinsic();
    Extent cross = Extent::Weight(1.0f);
    float padding = kSpacing.padding;
    float gap = kSpacing.gap;
    bool wrap = false;      // Row only
    bool formGrid = false;
    // Set => this node is explicitly positioned and OUT OF FLOW.
    // Out-of-flow is a positioning mode, never a property of a node kind.
    std::optional<Bounds> explicitBounds{};
    // Set => this node is OUT OF FLOW and resolves to the bounds of the named
    // in-flow SIBLING, whatever the resolver decides those are. This is the
    // second out-of-flow mode, and it exists because the first one cannot
    // express the overlays the model already promises: an sru-25 visualizer
    // underlay must cover exactly the encoder above it, but once the encoder
    // is an in-flow grid cell the producer no longer knows its extent, so it
    // has no explicit bounds to declare. Anchoring to the target by id keeps
    // that arithmetic out of the producer without a wrapper container.
    std::optional<std::string> overlayOf{};
};

namespace layout_detail {

enum class Axis { Horizontal, Vertical };

inline float AxisExtent(Bounds bounds, Axis axis)
{
    return axis == Axis::Horizontal ? bounds.width : bounds.height;
}

inline float CrossExtent(Bounds bounds, Axis axis)
{
    return axis == Axis::Horizontal ? bounds.height : bounds.width;
}

inline float AxisOffset(Bounds bounds, Axis axis)
{
    return axis == Axis::Horizontal ? bounds.x : bounds.y;
}

inline float LeafIntrinsicExtent(const Node& node, Axis axis)
{
    const Bounds intrinsic = metrics::IntrinsicFor(node);
    return AxisExtent(intrinsic, axis);
}

inline const LayoutOptions& LayoutFor(const std::map<std::string, LayoutOptions>& layoutByNodeId,
                                      const NodeId& id,
                                      const LayoutOptions& fallback)
{
    const auto found = layoutByNodeId.find(id.value);
    return found == layoutByNodeId.end() ? fallback : found->second;
}

// Both positioning modes take the node out of flow: it consumes no stacking
// space and its stacked siblings resolve exactly as if it were absent.
inline bool IsOutOfFlow(const LayoutOptions& layout)
{
    return layout.explicitBounds.has_value() || layout.overlayOf.has_value();
}

inline bool IsContainer(NodeKind kind)
{
    return kind == NodeKind::Root ||
           kind == NodeKind::Row ||
           kind == NodeKind::Section ||
           kind == NodeKind::ScrollArea;
}

inline Axis MainAxisFor(const Node& node)
{
    return node.kind == NodeKind::Row ? Axis::Horizontal : Axis::Vertical;
}

inline float ClampExtent(float value, const Extent& extent)
{
    value = std::max(value, extent.minimum);
    if (extent.maximum > 0.0f)
    {
        value = std::min(value, extent.maximum);
    }
    return value;
}

struct ResolvedExtent {
    float value = 0.0f;
    bool weighted = false;
    bool clamped = false;
    float weight = 0.0f;
};

inline float ResolveSimpleExtent(const Node& node,
                                 Axis axis,
                                 const Extent& extent,
                                 float contentExtent,
                                 const std::function<float(const Node&, Axis)>& intrinsicForNode)
{
    switch (extent.mode)
    {
        case Extent::Mode::Fixed:
            return extent.value;
        case Extent::Mode::Fraction:
            return contentExtent * extent.value;
        case Extent::Mode::Weighted:
            assert(false && "weighted extents are resolved by AllocateExtents");
            return 0.0f;
        case Extent::Mode::Intrinsic:
        default:
            return intrinsicForNode(node, axis);
    }
}

inline std::vector<ResolvedExtent> AllocateExtents(const std::vector<const Node*>& nodes,
                                                   const std::vector<LayoutOptions>& layouts,
                                                   Axis axis,
                                                   float containerExtent,
                                                   float padding,
                                                   float gap,
                                                   const std::function<float(const Node&, Axis)>& intrinsicForNode)
{
    std::vector<ResolvedExtent> resolved(nodes.size());
    if (nodes.empty())
    {
        return resolved;
    }

    const float contentExtent = std::max(0.0f, containerExtent - padding * 2.0f);
    const float totalGaps = gap * static_cast<float>(nodes.size() - 1);
    float nonWeighted = 0.0f;
    float totalWeight = 0.0f;

    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        const Extent& extent = layouts[i].main;
        if (extent.mode == Extent::Mode::Weighted)
        {
            resolved[i].weighted = true;
            resolved[i].weight = std::max(0.0f, extent.value);
            totalWeight += resolved[i].weight;
            continue;
        }

        float value = ResolveSimpleExtent(*nodes[i], axis, extent, contentExtent, intrinsicForNode);
        resolved[i].value = value;
        nonWeighted += value;
    }

    const float remaining = contentExtent - totalGaps - nonWeighted;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        if (!resolved[i].weighted)
        {
            continue;
        }
        resolved[i].value = totalWeight > 0.0f ? remaining * resolved[i].weight / totalWeight : 0.0f;
    }

    float beforeClamp = 0.0f;
    float afterClamp = 0.0f;
    float eligibleWeight = 0.0f;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        beforeClamp += resolved[i].value;
        const float clamped = ClampExtent(resolved[i].value, layouts[i].main);
        resolved[i].clamped = std::abs(clamped - resolved[i].value) > 0.0001f;
        resolved[i].value = clamped;
        afterClamp += clamped;
        if (resolved[i].weighted && !resolved[i].clamped)
        {
            eligibleWeight += resolved[i].weight;
        }
    }

    const float delta = beforeClamp - afterClamp;
    if (std::abs(delta) > 0.0001f && eligibleWeight > 0.0f)
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (!resolved[i].weighted || resolved[i].clamped)
            {
                continue;
            }
            const float adjusted = resolved[i].value + delta * resolved[i].weight / eligibleWeight;
            resolved[i].value = ClampExtent(adjusted, layouts[i].main);
        }
    }

    return resolved;
}

// Dividing a container's extent across its children accumulates a little float
// error, while a real overflow is whole pixels of clipped content. A hundredth
// of a pixel is on the right side of both.
inline constexpr float kOverflowTolerance = 0.01f;

inline std::string FormatExtent(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(value));
    return buffer;
}

// sru-54. A container whose in-flow children cannot fit its stacking axis has
// no rendering outcome to offer: node content clips to its bounds, so the
// overflow is not visible, it is cut off. D3 rule 6's earlier disposition
// blessed that as "visible"; it never was. The resolver is the only layer that
// still holds the numbers, so it is the layer that fails, and the message
// carries them rather than leaving them to be reconstructed.
//
// Judged on the children's FINAL bounds rather than on the extents allocated to
// them, because allocation is not the last word on where a child lands: a form
// grid re-columns its rows after the row itself has placed them, so a row whose
// allocated cells overrun can still be laid out correctly, and the geometry a
// backend renders is the geometry that has to fit.
inline void RequireContainerHoldsItsChildren(const Node& container,
                                             const LayoutOptions& opts,
                                             Axis mainAxis,
                                             const std::vector<const Node*>& inFlow)
{
    // The one structural absorber, and it is a fact about the node kind rather
    // than a number: a ScrollArea places its children in scroll-content space
    // and publishes the extent that reaches the tail, so content past the
    // viewport is reachable rather than lost.
    //
    // A WRAPPING ROW IS NOT EXEMPT. It looked like a second absorber and it is
    // not one: a line only breaks once the cursor has moved off the padding, so
    // the first child of a line is placed however wide it is, and a child wider
    // than the content box has no earlier line to be pushed onto. Exempting
    // wrapping rows let exactly that case overflow in silence. Nothing is lost
    // by checking them, because a legitimate wrap already keeps every line
    // inside the content box -- the placement loop breaks against the same
    // content edge this gate measures against.
    if (container.kind == NodeKind::ScrollArea)
    {
        return;
    }

    const float available = AxisExtent(container.bounds, mainAxis);
    const float contentEdge = available - opts.padding;
    const Node* firstThatDoesNotFit = nullptr;
    float furthestEdge = opts.padding;
    for (const Node* child : inFlow)
    {
        const float edge = AxisOffset(child->bounds, mainAxis) + AxisExtent(child->bounds, mainAxis);
        furthestEdge = std::max(furthestEdge, edge);
        if (firstThatDoesNotFit == nullptr && edge > contentEdge + kOverflowTolerance)
        {
            firstThatDoesNotFit = child;
        }
    }
    if (firstThatDoesNotFit == nullptr)
    {
        return;
    }

    throw std::runtime_error(
        std::string("portable UI container overflows its ") +
        (mainAxis == Axis::Horizontal ? "horizontal" : "vertical") + " extent: '" +
        container.id.value + "' has " + FormatExtent(available) + " and its in-flow children need " +
        FormatExtent(furthestEdge + opts.padding) + "; the first child that does not fit is '" +
        firstThatDoesNotFit->id.value +
        "'. Declare a ScrollArea or one weighted in-flow child so the container absorbs its own "
        "overflow.");
}

inline float ResolveCrossExtent(const Node& node,
                                Axis mainAxis,
                                const LayoutOptions& layout,
                                float containerCrossExtent,
                                float padding,
                                const std::function<float(const Node&, Axis)>& intrinsicForNode)
{
    const float contentExtent = std::max(0.0f, containerCrossExtent - padding * 2.0f);
    const Axis crossAxis = mainAxis == Axis::Horizontal ? Axis::Vertical : Axis::Horizontal;
    float value = 0.0f;
    switch (layout.cross.mode)
    {
        case Extent::Mode::Fixed:
            value = layout.cross.value;
            break;
        case Extent::Mode::Fraction:
            value = contentExtent * layout.cross.value;
            break;
        case Extent::Mode::Weighted:
            // Cross-axis weights behave as fill fractions: the cross axis is
            // not stacked, so weights above 1 fill but do not overflow.
            value = contentExtent * std::min(1.0f, std::max(0.0f, layout.cross.value));
            break;
        case Extent::Mode::Intrinsic:
        default:
            value = intrinsicForNode(node, crossAxis);
            break;
    }
    return ClampExtent(value, layout.cross);
}

// Root is the parentless surface extent; applying ordinary container padding
// here would shrink every page and app implicitly.
inline LayoutOptions RootLayoutOptions()
{
    LayoutOptions options;
    options.main = Extent::Intrinsic();
    options.cross = Extent::Weight(1.0f);
    options.padding = 0.0f;
    options.gap = 0.0f;
    return options;
}

inline std::map<std::string, std::size_t> BuildNodeIndex(const NodeTree& tree)
{
    std::map<std::string, std::size_t> byId;
    for (std::size_t i = 0; i < tree.nodes.size(); ++i)
    {
        byId[tree.nodes[i].id.value] = i;
    }
    return byId;
}

struct Resolver {
    struct DeferredOverlay {
        Node* node = nullptr;
        const Node* target = nullptr;
    };

    NodeTree& tree;
    const std::map<std::string, LayoutOptions>& layoutByNodeId;
    const std::map<std::string, DrawFactory>& drawFactories;
    std::map<std::string, std::size_t> byId;
    std::vector<DeferredOverlay> deferredOverlays{};

    Node* Find(const NodeId& id)
    {
        const auto found = byId.find(id.value);
        if (found == byId.end())
        {
            return nullptr;
        }
        return &tree.nodes[found->second];
    }

    const Node* Find(const NodeId& id) const
    {
        const auto found = byId.find(id.value);
        if (found == byId.end())
        {
            return nullptr;
        }
        return &tree.nodes[found->second];
    }

    void InvokeDrawFactory(Node& node)
    {
        const auto found = drawFactories.find(node.id.value);
        if (found == drawFactories.end())
        {
            return;
        }
        node.drawCommands = found->second(Bounds{0.0f, 0.0f, node.bounds.width, node.bounds.height});
    }

    float IntrinsicFromExtent(const Node& node,
                              const Extent& extent,
                              Axis axis,
                              std::optional<float> knownCrossExtent = std::nullopt)
    {
        switch (extent.mode)
        {
            case Extent::Mode::Fixed:
                return ClampExtent(extent.value, extent);
            case Extent::Mode::Fraction:
            case Extent::Mode::Weighted:
                return ClampExtent(IntrinsicForNode(node, axis, knownCrossExtent), extent);
            case Extent::Mode::Intrinsic:
            default:
                return ClampExtent(IntrinsicForNode(node, axis, knownCrossExtent), extent);
        }
    }

    float IntrinsicForWrappingRow(const Node& row, float rowWidth)
    {
        const LayoutOptions fallback;
        const LayoutOptions& opts = LayoutFor(layoutByNodeId, row.id, fallback);
        const float contentWidth = std::max(0.0f, rowWidth - opts.padding * 2.0f);
        std::vector<const Node*> inFlow;
        std::vector<LayoutOptions> childLayouts;
        for (const NodeId& childId : row.children)
        {
            const LayoutOptions childLayout = LayoutFor(layoutByNodeId, childId, fallback);
            if (IsOutOfFlow(childLayout))
            {
                continue;
            }
            const Node* child = Find(childId);
            if (child == nullptr)
            {
                continue;
            }
            inFlow.push_back(child);
            childLayouts.push_back(childLayout);
        }

        const auto intrinsicForNode = [this](const Node& node, Axis axis) {
            return IntrinsicForNode(node, axis);
        };
        const auto mainExtents = AllocateExtents(inFlow,
                                                 childLayouts,
                                                 Axis::Horizontal,
                                                 rowWidth,
                                                 opts.padding,
                                                 opts.gap,
                                                 intrinsicForNode);
        float lineMain = 0.0f;
        float lineCross = 0.0f;
        float totalCross = opts.padding;
        bool lineHasChild = false;

        for (std::size_t i = 0; i < inFlow.size(); ++i)
        {
            const float childMain = mainExtents[i].value;
            const float childCross = IntrinsicFromExtent(*inFlow[i], childLayouts[i].cross, Axis::Vertical);
            const float nextMain = lineHasChild ? lineMain + opts.gap + childMain : childMain;
            if (lineHasChild && nextMain > contentWidth)
            {
                totalCross += lineCross + opts.gap;
                lineMain = childMain;
                lineCross = childCross;
            }
            else
            {
                lineMain = nextMain;
                lineCross = std::max(lineCross, childCross);
            }
            lineHasChild = true;
        }

        if (lineHasChild)
        {
            totalCross += lineCross;
        }
        return totalCross + opts.padding;
    }

    float IntrinsicForNode(const Node& node, Axis axis, std::optional<float> knownCrossExtent = std::nullopt)
    {
        if (!IsContainer(node.kind))
        {
            return LeafIntrinsicExtent(node, axis);
        }

        const LayoutOptions fallback;
        const LayoutOptions& opts = LayoutFor(layoutByNodeId, node.id, fallback);
        const Axis mainAxis = MainAxisFor(node);
        if (node.kind == NodeKind::Row && opts.wrap && axis == Axis::Vertical)
        {
            const float rowWidth = knownCrossExtent.value_or(IntrinsicForNode(node, Axis::Horizontal));
            return IntrinsicForWrappingRow(node, rowWidth);
        }

        float result = 0.0f;
        std::size_t inFlowCount = 0;

        const auto intrinsicForNode = [this](const Node& child, Axis childAxis) {
            return IntrinsicForNode(child, childAxis);
        };

        for (const NodeId& childId : node.children)
        {
            const LayoutOptions childLayout = LayoutFor(layoutByNodeId, childId, fallback);
            if (IsOutOfFlow(childLayout))
            {
                continue;
            }
            const Node* child = Find(childId);
            if (child == nullptr)
            {
                continue;
            }
            if (axis == mainAxis)
            {
                // A child's own extent along this node's cross axis is what a
                // wrapping descendant needs in order to break its lines, so it
                // is threaded down rather than recomputed from the descendant's
                // unwrapped natural extent two levels below.
                std::optional<float> childCrossExtent;
                if (knownCrossExtent.has_value())
                {
                    childCrossExtent = ResolveCrossExtent(
                        *child, mainAxis, childLayout, *knownCrossExtent, opts.padding, intrinsicForNode);
                }
                result += IntrinsicFromExtent(*child, childLayout.main, axis, childCrossExtent);
                ++inFlowCount;
            }
            else
            {
                result = std::max(result, IntrinsicFromExtent(*child, childLayout.cross, axis));
            }
        }

        if (axis == mainAxis && inFlowCount > 1)
        {
            result += opts.gap * static_cast<float>(inFlowCount - 1);
        }
        return result + opts.padding * 2.0f;
    }

    void ResolveNode(Node& node, bool isRoot)
    {
        InvokeDrawFactory(node);
        if (!IsContainer(node.kind))
        {
            return;
        }
        ResolveContainer(node, isRoot);
    }

    void ResolveContainer(Node& container, bool isRoot)
    {
        const LayoutOptions rootLayout = RootLayoutOptions();
        const LayoutOptions fallback;
        const LayoutOptions& opts = isRoot ? rootLayout : LayoutFor(layoutByNodeId, container.id, fallback);
        const Axis mainAxis = MainAxisFor(container);

        std::vector<Node*> inFlow;
        std::vector<const Node*> inFlowConst;
        std::vector<LayoutOptions> childLayouts;
        std::vector<Node*> outOfFlow;
        std::vector<std::pair<Node*, NodeId>> overlays;

        for (const NodeId& childId : container.children)
        {
            Node* child = Find(childId);
            if (child == nullptr)
            {
                continue;
            }
            LayoutOptions childLayout = LayoutFor(layoutByNodeId, childId, fallback);
            if (childLayout.overlayOf.has_value())
            {
                overlays.emplace_back(child, NodeId(*childLayout.overlayOf));
                continue;
            }
            if (childLayout.explicitBounds.has_value())
            {
                child->bounds = *childLayout.explicitBounds;
                outOfFlow.push_back(child);
                continue;
            }
            inFlow.push_back(child);
            inFlowConst.push_back(child);
            childLayouts.push_back(childLayout);
        }

        const auto fallbackIntrinsicForNode = [this](const Node& node, Axis axis) {
            return IntrinsicForNode(node, axis);
        };
        const float containerCross = CrossExtent(container.bounds, mainAxis);
        std::map<std::string, float> childCrossExtents;
        for (std::size_t i = 0; i < inFlow.size(); ++i)
        {
            childCrossExtents[inFlow[i]->id.value] =
                ResolveCrossExtent(*inFlow[i], mainAxis, childLayouts[i], containerCross, opts.padding, fallbackIntrinsicForNode);
        }

        const auto intrinsicForNode = [this, &childCrossExtents](const Node& node, Axis axis) {
            const auto found = childCrossExtents.find(node.id.value);
            return found == childCrossExtents.end() ? IntrinsicForNode(node, axis) : IntrinsicForNode(node, axis, found->second);
        };
        const auto mainExtents = AllocateExtents(inFlowConst,
                                                 childLayouts,
                                                 mainAxis,
                                                 AxisExtent(container.bounds, mainAxis),
                                                 opts.padding,
                                                 opts.gap,
                                                 intrinsicForNode);

        float mainCursor = opts.padding;
        float crossCursor = opts.padding;
        float lineCrossExtent = 0.0f;

        for (std::size_t i = 0; i < inFlow.size(); ++i)
        {
            Node& child = *inFlow[i];
            const float mainExtent = mainExtents[i].value;
            const float crossExtent = container.kind == NodeKind::Row && opts.wrap
                                          ? IntrinsicFromExtent(child, childLayouts[i].cross, Axis::Vertical)
                                          : childCrossExtents[child.id.value];

            if (container.kind == NodeKind::Row && opts.wrap && mainCursor > opts.padding &&
                mainCursor + mainExtent > std::max(opts.padding, AxisExtent(container.bounds, Axis::Horizontal) - opts.padding))
            {
                mainCursor = opts.padding;
                crossCursor += lineCrossExtent + opts.gap;
                lineCrossExtent = 0.0f;
            }

            if (mainAxis == Axis::Horizontal)
            {
                child.bounds = {mainCursor, crossCursor, mainExtent, crossExtent};
            }
            else
            {
                child.bounds = {crossCursor, mainCursor, crossExtent, mainExtent};
            }
            mainCursor += mainExtent + opts.gap;
            lineCrossExtent = std::max(lineCrossExtent, crossExtent);
            ResolveNode(child, false);
        }

        for (Node* child : outOfFlow)
        {
            ResolveNode(*child, false);
        }

        // sru-44: an overlay takes the bounds of the IN-FLOW sibling it names.
        // A target that is out of flow itself — explicitly positioned, or
        // another overlay — is not a valid anchor, and an overlay pointed at
        // one collapses to nothing rather than copying a position it was never
        // promised. That verdict is declarative, so it is settled here; the
        // bounds copy is not, so it is deferred (see ResolveDeferredOverlays).
        for (const auto& [child, targetId] : overlays)
        {
            const Node* target = Find(targetId);
            const bool namesASibling =
                target != nullptr &&
                std::find(container.children.begin(), container.children.end(), targetId) !=
                    container.children.end();
            if (namesASibling && !IsOutOfFlow(LayoutFor(layoutByNodeId, targetId, fallback)))
            {
                deferredOverlays.push_back({child, target});
                continue;
            }
            child->bounds = Bounds{};
            ResolveNode(*child, false);
        }

        if (opts.formGrid)
        {
            ApplyFormGrid(container);
        }

        if (container.kind == NodeKind::ScrollArea)
        {
            SetScrollContentExtent(container, opts.padding);
        }
    }

    // A ScrollArea's children are placed in scroll-CONTENT space, so how far
    // that space reaches is a fact about the resolved children -- the only
    // layer that knows it. Leaving it to the producer would mean re-deriving
    // row heights and gaps by hand, and leaving it unset makes both backends
    // clamp the content to the viewport (`max(bounds, declared)`), which is
    // exactly a list whose tail is unreachable.
    // Contract for later consumers: this OVERWRITES whatever the container
    // carried, unconditionally, because the resolver has just placed the
    // children and is therefore the only layer that knows their true extent. A
    // producer that wants a content floor *wider* than its children -- the
    // hand-set minimum-width pattern in `ControllersPageUI.hpp`, for instance --
    // cannot get it by pre-setting these fields, and needs an explicit library
    // way to express a floor instead. No such producer exists yet; the first one
    // that does should add it rather than reaching around this.
    void SetScrollContentExtent(Node& container, float padding)
    {
        float right = 0.0f;
        float bottom = 0.0f;
        bool hasChild = false;
        for (const NodeId& childId : container.children)
        {
            const Node* child = Find(childId);
            if (child == nullptr)
            {
                continue;
            }
            hasChild = true;
            right = std::max(right, child->bounds.x + child->bounds.width);
            bottom = std::max(bottom, child->bounds.y + child->bounds.height);
        }
        container.scrollContentWidth = hasChild ? right + padding : 0.0f;
        container.scrollContentHeight = hasChild ? bottom + padding : 0.0f;
    }

    // A form grid moves and resizes its cells after their own container has
    // finished placing them, so an overlay resolved inside that container
    // would go stale. Every overlay therefore takes its target's bounds only
    // once the whole tree has stopped moving.
    //
    // An overlay may itself be a container holding an overlay of its own, and
    // resolving it appends that one here. So this walks by index and copies
    // each entry: a range-for would hold an iterator across a push_back that
    // can reallocate, and would stop at the end sentinel it started with,
    // silently leaving every nested overlay at zero.
    void ResolveDeferredOverlays()
    {
        for (std::size_t i = 0; i < deferredOverlays.size(); ++i)
        {
            const DeferredOverlay overlay = deferredOverlays[i];
            overlay.node->bounds = overlay.target->bounds;
            ResolveNode(*overlay.node, false);
        }
        deferredOverlays.clear();
    }

    // sru-54's gate, walked once the whole tree has stopped moving -- after the
    // deferred overlays and every form grid, so what it judges is the geometry
    // a backend would render. Pre-order, so the OUTERMOST container that cannot
    // hold its content is the one reported: a descendant squeezed by an
    // ancestor's overflow is a symptom, and repairing it would not fix the page.
    void RequireEveryContainerHoldsItsChildren(const Node& container, bool isRoot)
    {
        if (!IsContainer(container.kind))
        {
            return;
        }
        const LayoutOptions rootLayout = RootLayoutOptions();
        const LayoutOptions fallback;
        const LayoutOptions& opts =
            isRoot ? rootLayout : LayoutFor(layoutByNodeId, container.id, fallback);

        std::vector<const Node*> inFlow;
        for (const NodeId& childId : container.children)
        {
            const Node* child = Find(childId);
            if (child != nullptr && !IsOutOfFlow(LayoutFor(layoutByNodeId, childId, fallback)))
            {
                inFlow.push_back(child);
            }
        }
        RequireContainerHoldsItsChildren(container, opts, MainAxisFor(container), inFlow);

        // Every child, not just the in-flow ones: an out-of-flow container is
        // still a container, and its own children still have to fit it.
        for (const NodeId& childId : container.children)
        {
            const Node* child = Find(childId);
            if (child != nullptr)
            {
                RequireEveryContainerHoldsItsChildren(*child, false);
            }
        }
    }

    std::vector<Node*> InFlowChildrenOf(const Node& row)
    {
        const LayoutOptions fallback;
        std::vector<Node*> result;
        for (const NodeId& childId : row.children)
        {
            const LayoutOptions& layout = LayoutFor(layoutByNodeId, childId, fallback);
            if (IsOutOfFlow(layout))
            {
                continue;
            }
            Node* child = Find(childId);
            if (child != nullptr)
            {
                result.push_back(child);
            }
        }
        return result;
    }

    void ApplyFormGrid(Node& container)
    {
        const LayoutOptions fallback;
        float labelColumnWidth = 0.0f;
        std::vector<Node*> rows;
        for (const NodeId& childId : container.children)
        {
            Node* row = Find(childId);
            if (row == nullptr || row->kind != NodeKind::Row)
            {
                continue;
            }
            const auto cells = InFlowChildrenOf(*row);
            // Participation is structural: the first two in-flow children of a
            // row are the label/control cells, so producers cannot forget a
            // per-node form-grid marker.
            if (cells.size() < 2)
            {
                continue;
            }
            rows.push_back(row);
            labelColumnWidth = std::max(labelColumnWidth, cells[0]->bounds.width);
        }

        for (Node* row : rows)
        {
            const LayoutOptions& rowOpts = LayoutFor(layoutByNodeId, row->id, fallback);
            const float controlOffset = rowOpts.padding + labelColumnWidth + kSpacing.labelGap;
            auto cells = InFlowChildrenOf(*row);
            cells[0]->bounds.x = rowOpts.padding;
            cells[0]->bounds.width = labelColumnWidth;
            cells[1]->bounds.x = controlOffset;
            // The control cell's width does two jobs: size a weighted control to
            // fill the column, and rescue a row whose cells overran after the
            // label column was realigned above (see
            // RequireContainerHoldsItsChildren). A non-weighted control keeps its
            // already-resolved width -- e.g. Intrinsic sized to its own caption --
            // clamped to whatever the column has left, so it never overruns.
            const float remainingWidth = std::max(0.0f, row->bounds.width - controlOffset - rowOpts.padding);
            const LayoutOptions& controlOpts = LayoutFor(layoutByNodeId, cells[1]->id, fallback);
            cells[1]->bounds.width = controlOpts.main.mode == Extent::Mode::Weighted
                                          ? remainingWidth
                                          : std::min(cells[1]->bounds.width, remainingWidth);
            float extraCursor = cells[1]->bounds.x + cells[1]->bounds.width + rowOpts.gap;
            for (std::size_t i = 2; i < cells.size(); ++i)
            {
                cells[i]->bounds.x = extraCursor;
                extraCursor += cells[i]->bounds.width + rowOpts.gap;
            }
            for (Node* cell : cells)
            {
                ResolveNode(*cell, false);
            }
        }
    }
};

}  // namespace layout_detail

inline void ResolveLayout(NodeTree& tree,
                          const NodeId& rootId,
                          Bounds rootExtent,
                          const std::map<std::string, LayoutOptions>& layoutByNodeId,
                          const std::map<std::string, DrawFactory>& drawFactories)
{
    layout_detail::Resolver resolver{
        .tree = tree,
        .layoutByNodeId = layoutByNodeId,
        .drawFactories = drawFactories,
        .byId = layout_detail::BuildNodeIndex(tree),
    };
    Node* root = resolver.Find(rootId);
    if (root == nullptr)
    {
        return;
    }
    root->bounds = rootExtent;
    resolver.ResolveNode(*root, true);
    resolver.ResolveDeferredOverlays();
    resolver.RequireEveryContainerHoldsItsChildren(*root, true);
}

}  // namespace synth::ui
