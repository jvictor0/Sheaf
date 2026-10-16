#pragma once

// sru-48's named visual acceptance criteria and the structural checks that
// enforce the machine-checkable subset over a RESOLVED portable tree.
//
// This header is the headless half of the criteria (design.md D13). Bounds are
// in the tree, so containment, alignment, sibling overlap, spacing conformance
// and caption presence are all assertable with no browser and no toolkit; the
// Playwright half in `browser/tests/visual-criteria.spec.ts` asserts the same
// criteria over the rendered DOM plus the two that need real rendering (text
// fit and contrast).
//
// Every check returns a list of violations naming the offending nodes rather
// than a bool, because a criterion that fails without saying where is a
// criterion nobody can act on.
//
// It knows only the model and the library's spacing metrics: no JUCE, no
// backend, no producer.

#include "synth/PortableUI.hpp"
#include "synth/PortableUILayout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace synth::ui::criteria {

// The named criteria, verbatim in both halves of the suite. The browser spec's
// `VISUAL_CRITERIA` constant carries the same seven strings in the same order,
// and `TestTheNamedCriteriaAreTheOnesThePlaywrightSuiteNames` in
// `portable_ui_tests.cpp` pins that they agree by PARSING the spec file, so
// neither list can drift into naming a criterion the other does not.
inline const std::vector<std::string>& NamedCriteria()
{
    static const std::vector<std::string> criteria{
        "like-type controls share column positions",
        "all spacing drawn from the library's shared spacing metrics",
        "every form control has a visible caption",
        "no overlapping nodes and no container overflow on either axis",
        "every text element renders within its allocated extent",
        "text contrast meets WCAG AA 4.5:1 against its effective background",
        "no text conveys no information to the user",
    };
    return criteria;
}

inline std::string Format(float value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

inline std::string Describe(Bounds bounds)
{
    return "(" + Format(bounds.x) + "," + Format(bounds.y) + " " + Format(bounds.width) + "x" +
           Format(bounds.height) + ")";
}

// A tolerance in resolved pixels. Extents come out of the resolver as floats
// through fractions and weight division, so exact equality would fail on
// arithmetic that is visually identical.
inline constexpr float kTolerance = 0.01f;

struct Index {
    std::unordered_map<std::string, const Node*> byId;
    std::unordered_map<std::string, std::string> parentOf;

    explicit Index(const NodeTree& tree)
    {
        for (const Node& node : tree.nodes)
        {
            byId.emplace(node.id.value, &node);
        }
        for (const Node& node : tree.nodes)
        {
            for (const NodeId& child : node.children)
            {
                parentOf.emplace(child.value, node.id.value);
            }
        }
    }

    const Node* Find(const std::string& id) const
    {
        const auto it = byId.find(id);
        return it == byId.end() ? nullptr : it->second;
    }

    const Node* Parent(const std::string& id) const
    {
        const auto it = parentOf.find(id);
        return it == parentOf.end() ? nullptr : Find(it->second);
    }

    std::vector<const Node*> Children(const Node& parent) const
    {
        std::vector<const Node*> children;
        for (const NodeId& child : parent.children)
        {
            if (const Node* node = Find(child.value))
            {
                children.push_back(node);
            }
        }
        return children;
    }
};

// The rectangle a parent must contain its children within. For a `ScrollArea`
// that is its DECLARED SCROLL-CONTENT rectangle, not its viewport (sru-5,
// sprs-10): a Controllers row below the visible viewport is contained, not
// overflowing, and both backends size their content surface to exactly this
// `max(bounds, declared)`. Checking such children against the viewport box
// instead would assert that a scrolling list never scrolls. Clipping and
// scroll-reachability are separate claims, asserted separately.
inline Bounds ContainingRectOf(const Node& parent)
{
    if (parent.kind == NodeKind::ScrollArea)
    {
        return Bounds{0.0f,
                      0.0f,
                      std::max(parent.bounds.width, parent.scrollContentWidth),
                      std::max(parent.bounds.height, parent.scrollContentHeight)};
    }
    return Bounds{0.0f, 0.0f, parent.bounds.width, parent.bounds.height};
}

inline bool RectanglesIntersect(Bounds left, Bounds right)
{
    return left.x + kTolerance < right.x + right.width &&
           right.x + kTolerance < left.x + left.width &&
           left.y + kTolerance < right.y + right.height &&
           right.y + kTolerance < left.y + left.height;
}

inline bool SameRectangle(Bounds left, Bounds right)
{
    return std::abs(left.x - right.x) <= kTolerance && std::abs(left.y - right.y) <= kTolerance &&
           std::abs(left.width - right.width) <= kTolerance &&
           std::abs(left.height - right.height) <= kTolerance;
}

// sru-48: every node's rectangle lies inside its parent's containing rectangle
// on BOTH axes. Bounds are parent-relative, so this is a claim about the node
// against its own parent's box, never against the surface.
inline std::vector<std::string> ContainmentViolations(const NodeTree& tree,
                                                      const std::set<std::string>& exemptIds = {})
{
    const Index index(tree);
    std::vector<std::string> violations;
    for (const Node& node : tree.nodes)
    {
        if (exemptIds.count(node.id.value) != 0)
        {
            continue;
        }
        const Node* parent = index.Parent(node.id.value);
        if (parent == nullptr)
        {
            continue;  // the parentless root's bounds are surface coordinates
        }
        const Bounds contained = ContainingRectOf(*parent);
        const Bounds own = node.bounds;
        if (own.x < -kTolerance || own.y < -kTolerance ||
            own.x + own.width > contained.width + kTolerance ||
            own.y + own.height > contained.height + kTolerance)
        {
            violations.push_back(node.id.value + " " + Describe(own) + " overflows " +
                                 parent->id.value + " " + Describe(contained));
        }
    }
    return violations;
}

// sru-25's visualizer underlay is the one overlay a first-party tree declares
// through `overlayOf`, and the resolver gives it exactly its target's bounds.
// So rather than exempting it from the overlap check, this pins what it is:
// `<encoderId>.visualizer` must be congruent with `<encoderId>`. An underlay
// that drifted off its encoder, or covered two cells, fails here instead of
// disappearing into a blanket "declared overlays" exemption.
inline std::string UnderlayTargetOf(const std::string& id)
{
    static const std::string kSuffix = ".visualizer";
    if (id.size() <= kSuffix.size() || id.compare(id.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
    {
        return {};
    }
    return id.substr(0, id.size() - kSuffix.size());
}

inline std::vector<std::string> UnderlayViolations(const NodeTree& tree)
{
    const Index index(tree);
    std::vector<std::string> violations;
    for (const Node& node : tree.nodes)
    {
        const std::string targetId = UnderlayTargetOf(node.id.value);
        if (targetId.empty())
        {
            continue;
        }
        const Node* target = index.Find(targetId);
        if (target == nullptr)
        {
            violations.push_back(node.id.value + " overlays no node named " + targetId);
            continue;
        }
        const Node* underlayParent = index.Parent(node.id.value);
        const Node* targetParent = index.Parent(targetId);
        if (underlayParent == nullptr || targetParent == nullptr ||
            underlayParent->id.value != targetParent->id.value)
        {
            violations.push_back(node.id.value + " is not a sibling of the node it overlays");
            continue;
        }
        if (!SameRectangle(node.bounds, target->bounds))
        {
            violations.push_back(node.id.value + " " + Describe(node.bounds) +
                                 " is not congruent with " + targetId + " " + Describe(target->bounds));
        }
    }
    return violations;
}

// sru-48: no two sibling rectangles intersect. The ONLY exceptions in the
// first-party trees are named, not a class: the sru-25 visualizer underlays,
// which are congruent with the single encoder they overlay (pinned by
// `UnderlayViolations`, so their exemption here costs no coverage), and any
// id the caller names explicitly. The File page's panel underlays that this
// clause was originally written for no longer exist -- containers carry their
// own fill and border under sru-55, and `PanelUnderlayFill` is deleted.
inline std::vector<std::string> SiblingOverlapViolations(const NodeTree& tree,
                                                         const std::set<std::string>& exemptIds = {})
{
    const Index index(tree);
    std::vector<std::string> violations;
    for (const Node& parent : tree.nodes)
    {
        const std::vector<const Node*> children = index.Children(parent);
        for (std::size_t left = 0; left < children.size(); ++left)
        {
            for (std::size_t right = left + 1; right < children.size(); ++right)
            {
                const Node& a = *children[left];
                const Node& b = *children[right];
                if (exemptIds.count(a.id.value) != 0 || exemptIds.count(b.id.value) != 0)
                {
                    continue;
                }
                // An underlay is allowed to intersect exactly the sibling it
                // names and nothing else.
                if (UnderlayTargetOf(a.id.value) == b.id.value ||
                    UnderlayTargetOf(b.id.value) == a.id.value)
                {
                    continue;
                }
                if (RectanglesIntersect(a.bounds, b.bounds))
                {
                    violations.push_back(a.id.value + " " + Describe(a.bounds) + " intersects " +
                                         b.id.value + " " + Describe(b.bounds) + " under " +
                                         parent.id.value);
                }
            }
        }
    }
    return violations;
}

// sru-48: every gap and padding between stacked siblings is a value drawn from
// a shared spacing table rather than a number chosen at the call site.
//
// The check measures the resolved tree, so what it actually catches is any
// spacing a producer introduced by hand: a `layout.gap` literal at a call site,
// a child whose resolved extent does not fill its slot, or a hand-positioned
// out-of-flow child sitting between two stacked ones. The caller supplies the
// allowed set from the spacing CONSTANTS its producers declare, never from a
// measured run, so editing a constant moves the allowed value with it while a
// new per-site number still fails.
//
// Containers with fewer than two in-flow children have no gap to measure and
// contribute nothing.
struct SpacingReport {
    std::vector<std::string> violations;
    std::set<float> observed;
};

inline bool IsHorizontalStack(const Node& node)
{
    return node.kind == NodeKind::Row;
}

inline SpacingReport SpacingConformance(const NodeTree& tree,
                                        const std::vector<float>& allowedValues,
                                        const std::set<std::string>& outOfFlowIds = {})
{
    const Index index(tree);
    SpacingReport report;
    const auto allowed = [&allowedValues](float value) {
        return std::any_of(allowedValues.begin(), allowedValues.end(), [value](float candidate) {
            return std::abs(candidate - value) <= kTolerance;
        });
    };
    const auto record = [&report, &allowed](float gap,
                                            const std::string& before,
                                            const std::string& after,
                                            const std::string& parent) {
        report.observed.insert(std::round(gap * 100.0f) / 100.0f);
        if (!allowed(gap))
        {
            report.violations.push_back("gap " + Format(gap) + " between " + before + " and " + after +
                                        " under " + parent + " is not a shared spacing-metric value");
        }
    };

    for (const Node& parent : tree.nodes)
    {
        std::vector<const Node*> children;
        for (const Node* child : index.Children(parent))
        {
            if (outOfFlowIds.count(child->id.value) == 0 && UnderlayTargetOf(child->id.value).empty())
            {
                children.push_back(child);
            }
        }
        if (children.empty())
        {
            continue;
        }
        const bool horizontal = IsHorizontalStack(parent);

        // The container's own leading inset on both axes is its padding, and
        // sru-48 names padding alongside gaps. Only the LEADING inset is
        // measurable: design.md D3 rule 5 leaves residual space no eligible
        // child can absorb unallocated at the END of the container, so a
        // trailing inset is legitimately padding plus slack and asserting on it
        // would fail every page with a fixed-height region.
        float leadingX = children.front()->bounds.x;
        float leadingY = children.front()->bounds.y;
        for (const Node* child : children)
        {
            leadingX = std::min(leadingX, child->bounds.x);
            leadingY = std::min(leadingY, child->bounds.y);
        }
        record(leadingX, parent.id.value, "its leading edge on x", parent.id.value);
        record(leadingY, parent.id.value, "its leading edge on y", parent.id.value);

        if (children.size() < 2)
        {
            continue;
        }
        const auto mainStart = [horizontal](const Node* node) {
            return horizontal ? node->bounds.x : node->bounds.y;
        };
        const auto mainExtent = [horizontal](const Node* node) {
            return horizontal ? node->bounds.width : node->bounds.height;
        };
        const auto crossStart = [horizontal](const Node* node) {
            return horizontal ? node->bounds.y : node->bounds.x;
        };
        const auto crossExtent = [horizontal](const Node* node) {
            return horizontal ? node->bounds.height : node->bounds.width;
        };

        // A wrapping row puts its children on several lines, so measuring one
        // sorted sequence along the main axis would read the wrap itself as a
        // large negative gap. Children are grouped by cross-axis offset first:
        // gaps WITHIN a line are main-axis gaps, and gaps BETWEEN lines are
        // cross-axis gaps. A non-wrapping row and an ordinary column both
        // collapse to a single line, so this costs them nothing.
        std::map<float, std::vector<const Node*>> lines;
        for (const Node* child : children)
        {
            lines[std::round(crossStart(child) * 100.0f) / 100.0f].push_back(child);
        }

        for (auto& [offset, line] : lines)
        {
            (void)offset;
            std::sort(line.begin(), line.end(), [&mainStart](const Node* a, const Node* b) {
                return mainStart(a) < mainStart(b);
            });
            for (std::size_t ix = 1; ix < line.size(); ++ix)
            {
                record(mainStart(line[ix]) - (mainStart(line[ix - 1]) + mainExtent(line[ix - 1])),
                       line[ix - 1]->id.value,
                       line[ix]->id.value,
                       parent.id.value);
            }
        }

        const auto lineExtent = [&crossExtent](const std::vector<const Node*>& line) {
            float tallest = 0.0f;
            for (const Node* node : line)
            {
                tallest = std::max(tallest, crossExtent(node));
            }
            return tallest;
        };
        for (auto next = ++lines.begin(); next != lines.end(); ++next)
        {
            auto previous = std::prev(next);
            record(next->first - (previous->first + lineExtent(previous->second)),
                   previous->second.front()->id.value,
                   next->second.front()->id.value,
                   parent.id.value);
        }
    }
    return report;
}

// sru-48: like-type controls share column positions. A form grid aligns the
// label and control columns of its participating rows to a shared left edge,
// so the check is that every row of `containerId` puts its n-th child at the
// same x. Width is NOT compared: it is a per-control declaration -- a button
// sized to its own caption instead of stretching across the column is
// intended, not a violation, as long as it still starts where the rest of the
// column starts. Rows with a different child count are not part of the same
// column set and are skipped, which is why the caller gets told how many rows
// were actually compared.
struct ColumnReport {
    std::vector<std::string> violations;
    std::size_t comparedRows = 0;
    std::size_t comparedColumns = 0;
};

inline ColumnReport ColumnAlignment(const NodeTree& tree, const std::string& containerId)
{
    const Index index(tree);
    ColumnReport report;
    const Node* container = index.Find(containerId);
    if (container == nullptr)
    {
        report.violations.push_back("no form-grid container named " + containerId);
        return report;
    }

    std::map<std::size_t, std::vector<const Node*>> columns;
    std::size_t rows = 0;
    std::size_t width = 0;
    for (const Node* row : index.Children(*container))
    {
        const std::vector<const Node*> cells = index.Children(*row);
        if (cells.empty())
        {
            continue;
        }
        if (rows == 0)
        {
            width = cells.size();
        }
        else if (cells.size() != width)
        {
            continue;
        }
        ++rows;
        for (std::size_t ix = 0; ix < cells.size(); ++ix)
        {
            columns[ix].push_back(cells[ix]);
        }
    }
    report.comparedRows = rows;
    report.comparedColumns = columns.size();

    for (const auto& [column, cells] : columns)
    {
        for (std::size_t ix = 1; ix < cells.size(); ++ix)
        {
            if (std::abs(cells[ix]->bounds.x - cells[0]->bounds.x) > kTolerance)
            {
                report.violations.push_back(cells[ix]->id.value + " x=" + Format(cells[ix]->bounds.x) +
                                            " leaves column " + std::to_string(column) + " held by " +
                                            cells[0]->id.value + " at x=" + Format(cells[0]->bounds.x));
            }
        }
    }
    return report;
}

// sru-48: every form control has a visible caption.
//
// "Visible" is the operative word and it is why this is not a presence check on
// `Node::label`. design.md OQ5 retired `ComboBox::label`, and neither backend
// renders `TextField::label` either, so a combo box or text field whose only
// identifying string sits in `label` is unlabelled ON SCREEN. A `Toggle` and a
// `Button` do render their own label, so a non-empty one identifies them.
//
// The caption convention is design.md D5's: a sibling `Label` node whose id is
// the control's id plus ".caption".
inline bool RendersItsOwnLabel(NodeKind kind)
{
    return kind == NodeKind::Toggle || kind == NodeKind::Button;
}

inline bool IsFormControl(NodeKind kind)
{
    return kind == NodeKind::ComboBox || kind == NodeKind::TextField || kind == NodeKind::Toggle ||
           kind == NodeKind::Slider;
}

// The exceptions are a MAP FROM ID TO REASON, not a set of patterns. Every
// entry is one control a human named and justified, so the list cannot grow by
// a new control happening to match a suffix -- which is what the Playwright
// half's `.output` class did before it was found swallowing a conforming
// control. `residualsMatched` lets a caller require the list was actually used,
// so a stale entry naming a control that no longer exists is visible too.
struct CaptionReport {
    std::vector<std::string> violations;
    std::size_t examined = 0;         // form controls that had to carry a caption
    std::size_t residualsMatched = 0; // named exceptions actually present in this tree
};

inline CaptionReport UncaptionedFormControls(const NodeTree& tree,
                                             const std::map<std::string, std::string>& exceptions = {})
{
    const Index index(tree);
    CaptionReport report;
    for (const Node& node : tree.nodes)
    {
        if (!IsFormControl(node.kind))
        {
            continue;
        }
        if (exceptions.count(node.id.value) != 0)
        {
            ++report.residualsMatched;
            continue;
        }
        ++report.examined;
        if (index.Find(node.id.value + ".caption") != nullptr)
        {
            continue;
        }
        if (RendersItsOwnLabel(node.kind) && !node.label.empty())
        {
            continue;
        }
        report.violations.push_back(node.id.value + " has no rendered caption");
    }
    return report;
}

// sru-48: no text conveys no information to the user. The machine-checkable
// half is the empty case -- a text-bearing node that reserves extent and paints
// nothing is furniture the user cannot read. Whether a non-empty string is
// informative is a judgement made at the sign-off gate.
inline std::vector<std::string> EmptyTextNodes(const NodeTree& tree)
{
    std::vector<std::string> violations;
    for (const Node& node : tree.nodes)
    {
        if (node.kind != NodeKind::Label && node.kind != NodeKind::StatusText)
        {
            continue;
        }
        if (node.text.empty() && node.label.empty() && node.bounds.height > kTolerance &&
            node.bounds.width > kTolerance)
        {
            violations.push_back(node.id.value + " reserves " + Describe(node.bounds) +
                                 " and renders no text");
        }
    }
    return violations;
}

inline std::string Join(const std::vector<std::string>& violations)
{
    std::string joined;
    for (const std::string& violation : violations)
    {
        if (!joined.empty())
        {
            joined += "; ";
        }
        joined += violation;
    }
    return joined;
}

}  // namespace synth::ui::criteria
