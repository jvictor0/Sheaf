#include "synth/PortableUIBuilders.hpp"
#include "synth/PortableUIMetrics.hpp"
#include "synth/PortableUIStandardLayout.hpp"

#include "support/VisualCriteria.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef JUCE_MAJOR_VERSION
#error "portable UI layout tests must not see JUCE"
#endif

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

void Require(bool condition, const std::string& label)
{
    Require(condition, label.c_str());
}

// The test binary runs from projects/synth, while the plan names repository
// relative paths. Try both, and fail loudly on a path that exists in neither
// so a typo can never make a source assertion pass vacuously.
std::string ReadSource(const std::string& repoRelativePath)
{
    for (const std::string& candidate : {repoRelativePath, "../../" + repoRelativePath})
    {
        std::ifstream stream(candidate);
        if (stream)
        {
            std::ostringstream contents;
            contents << stream.rdbuf();
            return contents.str();
        }
    }
    throw std::runtime_error("missing source file: " + repoRelativePath);
}

bool SourceContains(const std::string& repoRelativePath, const std::string& needle)
{
    return ReadSource(repoRelativePath).find(needle) != std::string::npos;
}

const synth::ui::Node& FindNode(const synth::ui::NodeTree& tree, const char* id)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id == synth::ui::NodeId(id))
        {
            return node;
        }
    }
    throw std::runtime_error(std::string("missing node: ") + id);
}

bool NearlyEqual(float a, float b)
{
    return std::fabs(a - b) < 0.01f;
}

bool SameBounds(synth::ui::Bounds a, synth::ui::Bounds b)
{
    return NearlyEqual(a.x, b.x) && NearlyEqual(a.y, b.y) && NearlyEqual(a.width, b.width) &&
           NearlyEqual(a.height, b.height);
}

synth::ui::ControlStyle MainOf(synth::ui::Extent e)
{
    synth::ui::ControlStyle s;
    s.layout.main = e;
    return s;
}

synth::ui::LayoutOptions LayoutMain(synth::ui::Extent e)
{
    synth::ui::LayoutOptions o;
    o.main = e;
    return o;
}

// sru-54 makes the DIAGNOSTIC the deliverable, not the throw: someone hitting
// this months from now has to be able to repair their layout from the message
// alone. So these tests read what the resolver said, not merely that it said
// something.
std::string ResolutionDiagnostic(const std::function<synth::ui::NodeTree()>& build)
{
    try
    {
        build();
    }
    catch (const std::runtime_error& error)
    {
        return error.what();
    }
    return {};
}

bool Mentions(const std::string& diagnostic, const std::string& needle)
{
    return diagnostic.find(needle) != std::string::npos;
}

synth::ui::LayoutOptions StackLayout(synth::ui::Extent main)
{
    synth::ui::LayoutOptions o;
    o.main = main;
    o.padding = 12.0f;
    o.gap = 8.0f;
    return o;
}

void TestWeightsDivideRemainingSpaceDeterministically()
{
    const auto build = [] {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
        b.Row("row", {}, [](synth::ui::Builder& b) {
            b.Label("fixed", "f", MainOf(synth::ui::Extent::Px(100.0f)));
            b.Label("a", "a", MainOf(synth::ui::Extent::Weight(1.0f)));
            b.Label("b", "b", MainOf(synth::ui::Extent::Weight(1.0f)));
        });
        return b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    };
    const auto first = build();
    const auto second = build();
    Require(NearlyEqual(FindNode(first, "a").bounds.width, FindNode(first, "b").bounds.width),
            "equal weights resolve to equal widths");
    Require(FindNode(first, "a").bounds.width > 0.0f, "weighted children get real width");
    Require(std::memcmp(&FindNode(first, "a").bounds,
                        &FindNode(second, "a").bounds,
                        sizeof(synth::ui::Bounds)) == 0,
            "re-resolving identical inputs yields byte-identical bounds");
}

void TestMaximumClampsAndRedistributesOnce()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 1000.0f, 300.0f});
    b.Row("row", {}, [](synth::ui::Builder& b) {
        b.Label("capped", "c", MainOf(synth::ui::Extent::Weight(1.0f).Max(100.0f)));
        b.Label("open", "o", MainOf(synth::ui::Extent::Weight(1.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 1000.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "capped").bounds.width, 100.0f),
            "a weighted child never exceeds its declared maximum");
    Require(FindNode(tree, "open").bounds.width > 400.0f,
            "freed space is redistributed to the weighted, unclamped sibling");
}

void TestClampingRedistributionDoesNotRepeat()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 1000.0f, 300.0f});
    b.Row("row", {}, [](synth::ui::Builder& b) {
        b.Label("capped", "c", MainOf(synth::ui::Extent::Weight(1.0f).Max(100.0f)));
        b.Label("limited", "l", MainOf(synth::ui::Extent::Weight(1.0f).Max(330.0f)));
        b.Label("open", "o", MainOf(synth::ui::Extent::Weight(1.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 1000.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "limited").bounds.width, 330.0f),
            "the first redistribution pass may clamp an eligible child");
    Require(NearlyEqual(FindNode(tree, "open").bounds.width, 430.0f),
            "the pass does not repeat to force residual space elsewhere");
}

void TestFractionIsOfContentExtentNotRemainingSpace()
{
    synth::ui::LayoutOptions row;
    row.padding = 16.0f;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 900.0f, 560.0f});
    b.Row("row", row, [](synth::ui::Builder& b) {
        b.Label("stack", "s", MainOf(synth::ui::Extent::Fraction(0.46f).Max(390.0f)));
        b.Label("rest", "r", MainOf(synth::ui::Extent::Weight(1.0f).Max(462.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 900.0f, 560.0f});
    Require(NearlyEqual(FindNode(tree, "stack").bounds.width, 390.0f),
            "a fraction is taken of content extent, then clamped by the maximum");
    Require(NearlyEqual(FindNode(tree, "rest").bounds.width, 462.0f),
            "the weighted sibling clamps at its own maximum");
}

void TestUnclampedFractionPinsContentExtentBasis()
{
    synth::ui::LayoutOptions row;
    row.padding = 16.0f;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 900.0f, 560.0f});
    b.Row("row", row, [](synth::ui::Builder& b) {
        b.Label("fraction", "s", MainOf(synth::ui::Extent::Fraction(0.46f)));
        b.Label("rest", "r", MainOf(synth::ui::Extent::Weight(1.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 900.0f, 560.0f});
    Require(NearlyEqual(FindNode(tree, "fraction").bounds.width, 399.28f),
            "an unclamped fraction is taken from content extent, not container or post-gap extent");
}

void TestInfeasibleMinimaFailLoudlyInDeclarationOrder()
{
    // D3 rule 6, as amended by sru-54: no child shrinks below its minimum, and
    // the container that cannot hold them fails. The earlier disposition let
    // them "overflow in declaration order so the failure is visible"; node
    // content clips to its bounds, so it was never visible, it was cut off.
    // Declaration order survives as the order the diagnostic reports in: the
    // 100-wide row leaves 76 of content, so the FIRST 80-minimum child is
    // already past the edge and 12 + 80 + 8 + 80 + 12 is what it would take.
    const auto build = [] {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 100.0f, 300.0f});
        b.Row("row", {}, [](synth::ui::Builder& b) {
            b.Label("first", "a", MainOf(synth::ui::Extent::Weight(1.0f).Min(80.0f)));
            b.Label("second", "b", MainOf(synth::ui::Extent::Weight(1.0f).Min(80.0f)));
        });
        return b.Build({0.0f, 0.0f, 100.0f, 300.0f});
    };
    const std::string diagnostic = ResolutionDiagnostic(build);
    Require(!diagnostic.empty(), "minima that cannot fit are a producer defect, not a clipped render");
    Require(Mentions(diagnostic, "'row'"),
            "the diagnostic names the container whose minima are infeasible: " + diagnostic);
    Require(Mentions(diagnostic, "192.00"),
            "the required extent is the unshrunk minima plus the gap and padding: " + diagnostic);
    Require(Mentions(diagnostic, "'first'"),
            "the diagnostic reports in declaration order, so the first child past the edge is named: " +
                diagnostic);
    Require(!Mentions(diagnostic, "'second'"),
            "the diagnostic names the first child that does not fit, not every one after it: " + diagnostic);
}

void TestUnabsorbedOverflowFailsWithAnActionableDiagnostic()
{
    // 12 padding, four 30-high children and three 8 gaps need 168; the stack
    // has 100. The first child past the 88 content edge is the third, so the
    // message that names the second or the fourth is the wrong message.
    const auto build = [] {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, 100.0f});
        b.Column("stack", StackLayout(synth::ui::Extent::Weight(1.0f)), [](synth::ui::Builder& b) {
            for (const char* id : {"top", "middle", "overflowing", "tail"})
            {
                b.Label(id, id, MainOf(synth::ui::Extent::Px(30.0f)));
            }
        });
        return b.Build({0.0f, 0.0f, 400.0f, 100.0f});
    };
    const std::string diagnostic = ResolutionDiagnostic(build);
    Require(!diagnostic.empty(),
            "a container whose in-flow children cannot fit fails instead of clipping them away");
    Require(Mentions(diagnostic, "'stack'"),
            "the diagnostic names the container that overflowed: " + diagnostic);
    Require(Mentions(diagnostic, "vertical"),
            "the diagnostic names the stacking axis: " + diagnostic);
    Require(Mentions(diagnostic, "100.00"),
            "the diagnostic states the extent available: " + diagnostic);
    Require(Mentions(diagnostic, "168.00"),
            "the diagnostic states the extent required: " + diagnostic);
    Require(Mentions(diagnostic, "'overflowing'"),
            "the diagnostic names the first child that does not fit: " + diagnostic);
    Require(!Mentions(diagnostic, "'tail'"),
            "the diagnostic names the FIRST child that does not fit, not the last: " + diagnostic);
    Require(!Mentions(diagnostic, "'middle'") && !Mentions(diagnostic, "'top'"),
            "the diagnostic does not name the children that did fit: " + diagnostic);
    Require(Mentions(diagnostic, "ScrollArea") && Mentions(diagnostic, "weighted"),
            "the diagnostic names both sanctioned ways to absorb the difference: " + diagnostic);
}

void TestAnOverflowingRowNamesItsOwnStackingAxis()
{
    // Same defect one axis over. A row stacks horizontally, so the axis in the
    // message is a fact about the container, not a constant.
    const auto build = [] {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 100.0f, 200.0f});
        b.Row("strip", StackLayout(synth::ui::Extent::Weight(1.0f)), [](synth::ui::Builder& b) {
            b.Label("first", "first", MainOf(synth::ui::Extent::Px(60.0f)));
            b.Label("second", "second", MainOf(synth::ui::Extent::Px(60.0f)));
        });
        return b.Build({0.0f, 0.0f, 100.0f, 200.0f});
    };
    const std::string diagnostic = ResolutionDiagnostic(build);
    Require(!diagnostic.empty(), "a row that cannot fit its children across fails too");
    Require(Mentions(diagnostic, "horizontal"),
            "a row reports the horizontal axis: " + diagnostic);
    Require(!Mentions(diagnostic, "vertical"),
            "the axis is read off the container, not fixed at vertical: " + diagnostic);
    Require(Mentions(diagnostic, "100.00") && Mentions(diagnostic, "152.00"),
            "the row reports its own width as available and 12 + 60 + 8 + 60 + 12 as required: " +
                diagnostic);
    Require(Mentions(diagnostic, "'second'") && !Mentions(diagnostic, "'first'"),
            "the first child across the edge is the second one declared: " + diagnostic);
}

void TestAScrollAreaAbsorbsAListTallerThanItsViewport()
{
    const auto build = [](float viewportHeight) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, viewportHeight});
        b.ScrollArea("list", StackLayout(synth::ui::Extent::Weight(1.0f)), [](synth::ui::Builder& b) {
            for (const char* id : {"top", "middle", "overflowing", "tail"})
            {
                b.Label(id, id, MainOf(synth::ui::Extent::Px(30.0f)));
            }
        });
        return b.Build({0.0f, 0.0f, 400.0f, viewportHeight});
    };
    Require(ResolutionDiagnostic([&] { return build(100.0f); }).empty(),
            "the same content resolves inside a ScrollArea instead of failing");

    const auto shortViewport = build(100.0f);
    const synth::ui::Node& list = FindNode(shortViewport, "list");
    const synth::ui::Node& tail = FindNode(shortViewport, "tail");
    Require(NearlyEqual(tail.bounds.height, 30.0f),
            "every item keeps its own extent along the scroll axis rather than being squeezed");
    Require(tail.bounds.y + tail.bounds.height > list.bounds.height,
            "the tail really is past the viewport, so this is a scrolling list and not a fitting one");
    Require(NearlyEqual(list.scrollContentHeight, 168.0f),
            "the resolver publishes a content extent that contains the last item and the trailing padding");
    Require(list.scrollContentHeight > list.bounds.height,
            "the published content extent is the content's, not the viewport's");

    // Same declaration, different viewport, no producer change.
    const auto tallViewport = build(300.0f);
    Require(NearlyEqual(FindNode(tallViewport, "tail").bounds.height, 30.0f) &&
                NearlyEqual(FindNode(tallViewport, "tail").bounds.y,
                            FindNode(shortViewport, "tail").bounds.y),
            "the rows land identically at a taller viewport: the ScrollArea absorbed the difference");
    Require(NearlyEqual(FindNode(tallViewport, "list").scrollContentHeight, 168.0f),
            "the content extent is a fact about the rows, not about the viewport");
}

void TestAWeightedChildAbsorbsTheRemainder()
{
    const auto build = [](float containerHeight, float furnitureHeight) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, containerHeight});
        b.Column("page", StackLayout(synth::ui::Extent::Weight(1.0f)),
                 [furnitureHeight](synth::ui::Builder& b) {
                     b.Label("furniture", "furniture", MainOf(synth::ui::Extent::Px(furnitureHeight)));
                     b.Label("absorbing", "absorbing", MainOf(synth::ui::Extent::Weight(1.0f)));
                 });
        return b.Build({0.0f, 0.0f, 400.0f, containerHeight});
    };
    const auto shortPage = build(100.0f, 30.0f);
    const auto tallPage = build(200.0f, 30.0f);
    Require(NearlyEqual(FindNode(shortPage, "absorbing").bounds.height, 38.0f),
            "the weighted child takes 100 less the padding, the furniture and the gap");
    Require(NearlyEqual(FindNode(tallPage, "absorbing").bounds.height, 138.0f),
            "the weighted child takes the WHOLE difference when the container grows");
    Require(NearlyEqual(FindNode(tallPage, "furniture").bounds.height, 30.0f),
            "the furniture keeps its own extent at either container extent");

    // A weighted sibling is not a licence to overspend: it can absorb slack,
    // not debt. Without this the failure could be suppressed by declaring one
    // weighted child anywhere in a container that still cannot fit.
    const std::string diagnostic = ResolutionDiagnostic([&] { return build(100.0f, 200.0f); });
    Require(!diagnostic.empty(),
            "a weighted sibling does not rescue fixed children that already overflow");
    Require(Mentions(diagnostic, "'furniture'"),
            "the fixed child that overspent is the one named: " + diagnostic);
}

void TestInsertingARowShiftsSiblingsByExtentPlusGap()
{
    const auto build = [](bool extra) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
        b.Column("col", {}, [extra](synth::ui::Builder& b) {
            b.Label("first", "first", MainOf(synth::ui::Extent::Px(20.0f)));
            if (extra)
            {
                b.Label("inserted", "ins", MainOf(synth::ui::Extent::Px(20.0f)));
            }
            b.Label("last", "last", MainOf(synth::ui::Extent::Px(20.0f)));
        });
        return b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    };
    Require(NearlyEqual(FindNode(build(true), "last").bounds.y -
                            FindNode(build(false), "last").bounds.y,
                        20.0f + synth::ui::kSpacing.gap),
            "inserting a row moves later siblings by its extent plus one gap");
}

void TestExplicitlyPositionedChildrenAreOutOfFlow()
{
    const auto build = [](bool overlay) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
        b.Column("col", {}, [overlay](synth::ui::Builder& b) {
            b.Label("first", "first", MainOf(synth::ui::Extent::Px(20.0f)));
            if (overlay)
            {
                synth::ui::LayoutOptions o;
                o.explicitBounds = synth::ui::Bounds{5.0f, 5.0f, 50.0f, 50.0f};
                b.Draw("overlay", o, [](synth::ui::Bounds) {
                    return std::vector<synth::ui::DrawCommand>{};
                });
            }
            b.Label("last", "last", MainOf(synth::ui::Extent::Px(20.0f)));
        });
        return b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    };
    const auto with = build(true);
    Require(NearlyEqual(FindNode(with, "last").bounds.y, FindNode(build(false), "last").bounds.y),
            "stacked siblings resolve as if the out-of-flow child were absent");
    Require(NearlyEqual(FindNode(with, "overlay").bounds.x, 5.0f),
            "the out-of-flow child keeps its author-supplied bounds");
}

void TestInFlowDrawFactoryReceivesItsResolvedExtent()
{
    const auto buildAt = [](float width) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, width, 300.0f});
        b.Row("row", {}, [](synth::ui::Builder& b) {
            b.Draw("canvas", LayoutMain(synth::ui::Extent::Weight(1.0f)),
                   [](synth::ui::Bounds extent) {
                       return std::vector<synth::ui::DrawCommand>{
                           synth::ui::DrawCommand::Fill(extent, synth::Color::Rgb(1, 2, 3))};
                   });
        });
        return b.Build({0.0f, 0.0f, width, 300.0f});
    };
    const auto narrow = buildAt(400.0f);
    const auto wide = buildAt(800.0f);
    Require(NearlyEqual(FindNode(narrow, "canvas").drawCommands[0].bounds.x, 0.0f),
            "the factory receives a node-local extent starting at the origin");
    Require(FindNode(wide, "canvas").drawCommands[0].bounds.width >
                FindNode(narrow, "canvas").drawCommands[0].bounds.width,
            "commands fill the extent the layout allocated, at any root extent");
}

void TestFormGridAlignsLabelAndControlColumns()
{
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("form", grid, [](synth::ui::Builder& b) {
        b.Row("r1", {}, [](synth::ui::Builder& b) {
            b.Label("r1.label", "Tempo", {});
            b.ComboBox("r1.control", {}, "", synth::ui::Action::Named("a"), {});
        });
        b.Row("r2", {}, [](synth::ui::Builder& b) {
            b.Label("r2.label", "A considerably longer caption", {});
            b.ComboBox("r2.control", {}, "", synth::ui::Action::Named("b"), {});
        });
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "r1.label").bounds.x, FindNode(tree, "r2.label").bounds.x),
            "every participating label column starts at the same x-offset");
    Require(NearlyEqual(FindNode(tree, "r1.control").bounds.x, FindNode(tree, "r2.control").bounds.x),
            "every participating control column starts at the same x-offset");
    Require(FindNode(tree, "r1.control").bounds.x >=
                FindNode(tree, "r2.label").bounds.x + FindNode(tree, "r2.label").bounds.width,
            "the control column clears the widest label");
}

void TestFormGridUsesRowLocalPadding()
{
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    grid.padding = 30.0f;
    synth::ui::LayoutOptions row;
    row.padding = 4.0f;

    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 420.0f, 300.0f});
    b.Column("form", grid, [&row](synth::ui::Builder& b) {
        b.Row("r1", row, [](synth::ui::Builder& b) {
            b.Label("r1.label", "Short", {});
            b.ComboBox("r1.control", {}, "", synth::ui::Action::Named("a"), {});
        });
        b.Row("r2", row, [](synth::ui::Builder& b) {
            b.Label("r2.label", "Longer label", {});
            b.ComboBox("r2.control", {}, "", synth::ui::Action::Named("b"), {});
        });
    });
    const auto tree = b.Build({0.0f, 0.0f, 420.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "r1.label").bounds.x, 4.0f),
            "form-grid cells use their row's local padding, not the form container padding");
    Require(NearlyEqual(FindNode(tree, "r1.control").bounds.x, FindNode(tree, "r2.control").bounds.x),
            "control columns still align with mixed form and row padding");
}

void TestCrossAxisWeightDoesNotExceedContentExtent()
{
    synth::ui::ControlStyle style;
    style.layout.cross = synth::ui::Extent::Weight(2.0f);
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 300.0f, 160.0f});
    b.Row("row", LayoutMain(synth::ui::Extent::Px(100.0f)), [&style](synth::ui::Builder& b) {
        b.Label("tall", "tall", style);
    });
    const auto tree = b.Build({0.0f, 0.0f, 300.0f, 160.0f});
    Require(NearlyEqual(FindNode(tree, "tall").bounds.height, 76.0f),
            "cross-axis weights are normalized to the available content extent");
}

void TestCaptionedControlRowOccupiesTheParentFlowSlot()
{
    synth::ui::ControlStyle style;
    style.caption = "Device";
    style.layout.main = synth::ui::Extent::Px(34.0f);
    style.layout.padding = 19.0f;
    style.layout.gap = 31.0f;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("form", {}, [&style](synth::ui::Builder& b) {
        b.ComboBox("device", {}, "", synth::ui::Action::Named("pick"), style);
        b.Label("after", "after", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const synth::ui::Node& row = FindNode(tree, "device.row");
    const synth::ui::Node& caption = FindNode(tree, "device.caption");
    const synth::ui::Node& control = FindNode(tree, "device");
    Require(NearlyEqual(FindNode(tree, "device.row").bounds.height, 34.0f),
            "the author's layout applies to the implicit caption row");
    Require(NearlyEqual(caption.bounds.x, 0.0f) && NearlyEqual(caption.bounds.y, 0.0f),
            "the implicit caption row resolves with zero padding");
    Require(NearlyEqual(control.bounds.x - (caption.bounds.x + caption.bounds.width),
                        synth::ui::kSpacing.labelGap),
            "the implicit caption row resolves with the library label gap");
    Require(NearlyEqual(FindNode(tree, "after").bounds.y - row.bounds.y,
                        34.0f + synth::ui::kSpacing.gap),
            "the captioned row, not the inner control, advances parent flow");
}

void TestCaptionedAndUncaptionedControlsHonorTheSameDeclaredExtent()
{
    synth::ui::ControlStyle captioned;
    captioned.caption = "Device";
    captioned.layout.main = synth::ui::Extent::Px(34.0f);
    synth::ui::ControlStyle plain;
    plain.layout.main = synth::ui::Extent::Px(34.0f);

    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 420.0f, 300.0f});
    b.Column("form", {}, [&](synth::ui::Builder& b) {
        b.ComboBox("captioned", {}, "", synth::ui::Action::Named("captioned"), captioned);
        b.ComboBox("plain", {}, "", synth::ui::Action::Named("plain"), plain);
    });
    const auto tree = b.Build({0.0f, 0.0f, 420.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "captioned.row").bounds.height, FindNode(tree, "plain").bounds.height),
            "captioned and uncaptioned controls honor the same declared flow extent");
}

void TestSplicedSubtreeLayoutOptionsAreHonoredWhenResolved()
{
    synth::ui::Builder inner;
    inner.Root("inner.root", {0.0f, 0.0f, 100.0f, 50.0f});
    inner.Label("inner.label", "spliced", MainOf(synth::ui::Extent::Px(44.0f)));

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Column("host", {}, [&inner](synth::ui::Builder& b) {
        b.Splice(inner.BuildSubtree());
    });
    const auto tree = outer.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "inner.label").bounds.height, 44.0f),
            "spliced Subtree layout options are honored during resolution");
}

void TestSplicedSubtreeDrawFactoryStillRuns()
{
    synth::ui::Builder inner;
    inner.Root("inner.root", {0.0f, 0.0f, 100.0f, 50.0f});
    inner.Draw("inner.canvas", LayoutMain(synth::ui::Extent::Px(36.0f)), [](synth::ui::Bounds extent) {
        return std::vector<synth::ui::DrawCommand>{
            synth::ui::DrawCommand::Fill(extent, synth::Color::Rgb(4, 5, 6))};
    });

    synth::ui::Builder outer;
    outer.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    outer.Column("host", {}, [&inner](synth::ui::Builder& b) {
        b.Splice(inner.BuildSubtree());
    });
    const auto tree = outer.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const auto& canvas = FindNode(tree, "inner.canvas");
    Require(canvas.drawCommands.size() == 1, "spliced Subtree carries Draw factories");
    Require(NearlyEqual(canvas.drawCommands[0].bounds.height, 36.0f),
            "the spliced Draw factory fills the resolved extent");
}

void TestComponentResolvesIdenticallyUnderDifferentParents()
{
    const auto emit = [](synth::ui::Builder& b, const std::string& p) {
        b.Row(p + ".row", LayoutMain(synth::ui::Extent::Px(40.0f)), [&p](synth::ui::Builder& b) {
            b.Label(p + ".label", "same", MainOf(synth::ui::Extent::Px(60.0f)));
        });
    };
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("top", LayoutMain(synth::ui::Extent::Px(100.0f)), [&](synth::ui::Builder& b) { emit(b, "top"); });
    b.Column("bottom", LayoutMain(synth::ui::Extent::Px(100.0f)), [&](synth::ui::Builder& b) { emit(b, "bottom"); });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "top.row").bounds.y, FindNode(tree, "bottom.row").bounds.y),
            "the same component resolves identically under different parents");
    Require(FindNode(tree, "top").bounds.y != FindNode(tree, "bottom").bounds.y,
            "the two parents really are at different positions");
}

void TestExtentDrivenRedistribution()
{
    const auto buildAt = [](float w) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, w, 300.0f});
        b.Row("row", {}, [](synth::ui::Builder& b) {
            b.Label("fixed", "f", MainOf(synth::ui::Extent::Px(100.0f)));
            b.Label("weighted", "w", MainOf(synth::ui::Extent::Weight(1.0f)));
        });
        return b.Build({0.0f, 0.0f, w, 300.0f});
    };
    const auto narrow = buildAt(400.0f);
    const auto wide = buildAt(800.0f);
    Require(FindNode(wide, "weighted").bounds.width >
                FindNode(narrow, "weighted").bounds.width + 350.0f,
            "a wider root extent widens the weighted child proportionally");
    Require(NearlyEqual(FindNode(wide, "fixed").bounds.width,
                        FindNode(narrow, "fixed").bounds.width),
            "the fixed child keeps its extent at both root extents");
}

void TestTextReservationIsDeterministicAndBackendFree()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Row("row", {}, [](synth::ui::Builder& b) {
        b.Label("short", "ab", {});
        b.Label("long", "abcdefghij", {});
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "short").bounds.width,
                        synth::ui::metrics::TextWidth("ab", synth::ui::TextStyle{})),
            "an intrinsic label width is its character-count reservation");
    Require(FindNode(tree, "long").bounds.width > FindNode(tree, "short").bounds.width,
            "a longer string reserves more width");
}

void TestWrappingRowFlowsOntoAdditionalLines()
{
    synth::ui::LayoutOptions row;
    row.wrap = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 300.0f});
    b.Row("row", row, [](synth::ui::Builder& b) {
        for (int i = 0; i < 4; ++i)
        {
            b.Label("c" + std::to_string(i), "x", MainOf(synth::ui::Extent::Px(80.0f)));
        }
    });
    const auto tree = b.Build({0.0f, 0.0f, 200.0f, 300.0f});
    Require(FindNode(tree, "c2").bounds.y > FindNode(tree, "c0").bounds.y,
            "overflowing children resolve onto subsequent lines");
    Require(FindNode(tree, "row").bounds.height > 20.0f,
            "the container's extent grows to contain every line");
}

void TestAWrappingRowStillFailsOnAChildWiderThanTheRow()
{
    // Wrapping is not an absorber for a child that cannot wrap. A line only
    // breaks when the cursor has already moved (`mainCursor > opts.padding`),
    // so the FIRST child is placed wherever it lands however wide it is --
    // there is no earlier line to push it onto. Exempting wrapping rows from
    // the gate therefore reopened exactly the hole sru-54 closes: a 200-wide
    // child in a 100-wide row laid out 124 past the content edge and was
    // clipped in silence.
    const auto build = [] {
        synth::ui::LayoutOptions row;
        row.wrap = true;
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 100.0f, 300.0f});
        b.Row("strip", row, [](synth::ui::Builder& b) {
            b.Label("unwrappable", "unwrappable", MainOf(synth::ui::Extent::Px(200.0f)));
        });
        return b.Build({0.0f, 0.0f, 100.0f, 300.0f});
    };
    const std::string diagnostic = ResolutionDiagnostic(build);
    Require(!diagnostic.empty(),
            "a wrapping row whose child cannot wrap fails like any other container");
    Require(Mentions(diagnostic, "'strip'") && Mentions(diagnostic, "horizontal"),
            "the wrapping row names itself and its own stacking axis: " + diagnostic);
    Require(Mentions(diagnostic, "100.00") && Mentions(diagnostic, "224.00"),
            "12 + 200 + 12 is what the unwrappable child costs the row: " + diagnostic);
    Require(Mentions(diagnostic, "'unwrappable'"),
            "the child that could not wrap is the one named: " + diagnostic);
}

void TestWrappingRowReservesGrownExtentInParentFlow()
{
    synth::ui::LayoutOptions row;
    row.wrap = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 300.0f});
    b.Column("col", {}, [row](synth::ui::Builder& b) {
        b.Row("wrapped", row, [](synth::ui::Builder& b) {
            for (int i = 0; i < 4; ++i)
            {
                b.Label("w" + std::to_string(i), "x", MainOf(synth::ui::Extent::Px(80.0f)));
            }
        });
        b.Label("after", "after", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 200.0f, 300.0f});
    const auto& wrapped = FindNode(tree, "wrapped");
    Require(FindNode(tree, "after").bounds.y >= wrapped.bounds.y + wrapped.bounds.height + synth::ui::kSpacing.gap,
            "a trailing sibling clears the wrapping row's grown extent");
}

void TestSectionAndScrollAreaStackChildrenVertically()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Section("section", LayoutMain(synth::ui::Extent::Px(100.0f)), [](synth::ui::Builder& b) {
        b.Label("section.first", "first", MainOf(synth::ui::Extent::Px(20.0f)));
        b.Label("section.second", "second", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    b.ScrollArea("scroll", LayoutMain(synth::ui::Extent::Px(100.0f)), [](synth::ui::Builder& b) {
        b.Label("scroll.first", "first", MainOf(synth::ui::Extent::Px(20.0f)));
        b.Label("scroll.second", "second", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(FindNode(tree, "section.second").bounds.y > FindNode(tree, "section.first").bounds.y,
            "Section stacks children vertically");
    Require(FindNode(tree, "scroll.second").bounds.y > FindNode(tree, "scroll.first").bounds.y,
            "ScrollArea stacks children vertically");
}

void TestWrappingRowLineBreakHeightIsPinned()
{
    // The line-break computation was previously pinned only relationally, so a
    // wrong break would have gone undetected. The column's 12 padding leaves
    // the unpadded row 176 wide, which holds two 80-wide children per line
    // (80 + 8 + 80 = 168 <= 176) and breaks before the third. Two lines of
    // 22-high labels separated by one 8 gap.
    synth::ui::LayoutOptions row;
    row.wrap = true;
    row.padding = 0.0f;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 300.0f});
    b.Column("col", {}, [row](synth::ui::Builder& b) {
        b.Row("wrapped", row, [](synth::ui::Builder& b) {
            for (int i = 0; i < 4; ++i)
            {
                b.Label("w" + std::to_string(i), "x", MainOf(synth::ui::Extent::Px(80.0f)));
            }
        });
    });
    const auto tree = b.Build({0.0f, 0.0f, 200.0f, 300.0f});
    Require(NearlyEqual(FindNode(tree, "wrapped").bounds.height, 22.0f + 8.0f + 22.0f),
            "a wrapping row reserves exactly its computed line count");
    Require(NearlyEqual(FindNode(tree, "w1").bounds.y, FindNode(tree, "w0").bounds.y),
            "the first two children share the first line");
    Require(NearlyEqual(FindNode(tree, "w2").bounds.y, FindNode(tree, "w0").bounds.y + 22.0f + 8.0f),
            "the third child starts the second line one gap below the first");
}

void TestIntrinsicColumnReservesAWrappingRowsGrownExtent()
{
    // A wrapping row measured from two levels up: the intrinsic column must
    // measure it at the width it will actually resolve to, not at its
    // unwrapped natural width, or the grandparent under-reserves.
    synth::ui::LayoutOptions wrapping;
    wrapping.wrap = true;
    wrapping.padding = 0.0f;
    synth::ui::LayoutOptions intrinsicColumn;
    intrinsicColumn.main = synth::ui::Extent::Intrinsic();
    intrinsicColumn.padding = 0.0f;

    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 300.0f});
    b.Column("outer", {}, [&](synth::ui::Builder& b) {
        b.Column("bay", intrinsicColumn, [&](synth::ui::Builder& b) {
            b.Row("wrapped", wrapping, [](synth::ui::Builder& b) {
                for (int i = 0; i < 4; ++i)
                {
                    b.Label("w" + std::to_string(i), "x", MainOf(synth::ui::Extent::Px(80.0f)));
                }
            });
        });
        b.Label("after", "after", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 200.0f, 300.0f});
    const auto& bay = FindNode(tree, "bay");
    const auto& wrapped = FindNode(tree, "wrapped");
    Require(NearlyEqual(bay.bounds.height, wrapped.bounds.height),
            "the intrinsic column reserves the wrapping row's grown extent");
    Require(FindNode(tree, "after").bounds.y >= bay.bounds.y + bay.bounds.height + synth::ui::kSpacing.gap,
            "a trailing sibling of the column clears every wrapped line");
}

void TestOverlayChildTakesItsTargetsResolvedBounds()
{
    const auto build = [](bool overlay) {
        synth::ui::Builder b;
        b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
        b.Row("row", {}, [overlay](synth::ui::Builder& b) {
            b.Label("first", "first", MainOf(synth::ui::Extent::Px(60.0f)));
            if (overlay)
            {
                synth::ui::LayoutOptions o;
                o.overlayOf = "second";
                b.Draw("underlay", o, [](synth::ui::Bounds extent) {
                    return std::vector<synth::ui::DrawCommand>{
                        synth::ui::DrawCommand::Fill(extent, synth::Color::Rgb(7, 8, 9))};
                });
            }
            b.Label("second", "second", MainOf(synth::ui::Extent::Px(90.0f)));
        });
        return b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    };
    const auto with = build(true);
    const auto& underlay = FindNode(with, "underlay");
    const auto& second = FindNode(with, "second");
    Require(NearlyEqual(underlay.bounds.x, second.bounds.x) &&
                NearlyEqual(underlay.bounds.y, second.bounds.y) &&
                NearlyEqual(underlay.bounds.width, second.bounds.width) &&
                NearlyEqual(underlay.bounds.height, second.bounds.height),
            "an overlay child resolves to exactly its target sibling's bounds");
    const auto without = build(false);
    for (const char* sibling : {"row", "first", "second"})
    {
        Require(SameBounds(FindNode(with, sibling).bounds, FindNode(without, sibling).bounds),
                std::string("'") + sibling +
                    "' resolves identically with and without the overlay, so the overlay "
                    "consumes no stacking space");
    }
    Require(underlay.drawCommands.size() == 1 &&
                NearlyEqual(underlay.drawCommands[0].bounds.width, second.bounds.width),
            "the overlay's draw factory receives its resolved node-local extent");
}

void TestOverlayRejectsATargetThatIsNotInFlow()
{
    // sru-44 anchors an overlay to an IN-FLOW sibling. An out-of-flow target
    // was never placed by the flow, so there is no slot to cover and the
    // overlay collapses rather than copying a position it was not promised.
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Row("row", {}, [](synth::ui::Builder& b) {
        synth::ui::LayoutOptions anchored;
        anchored.explicitBounds = synth::ui::Bounds{5.0f, 6.0f, 70.0f, 40.0f};
        b.Draw("anchored", anchored, [](synth::ui::Bounds) {
            return std::vector<synth::ui::DrawCommand>{};
        });

        synth::ui::LayoutOptions ontoAnchored;
        ontoAnchored.overlayOf = "anchored";
        b.Draw("onto.anchored", ontoAnchored, [](synth::ui::Bounds) {
            return std::vector<synth::ui::DrawCommand>{};
        });

        synth::ui::LayoutOptions ontoOverlay;
        ontoOverlay.overlayOf = "onto.anchored";
        b.Draw("onto.overlay", ontoOverlay, [](synth::ui::Bounds) {
            return std::vector<synth::ui::DrawCommand>{};
        });

        // In flow, but under a different parent: not a sibling.
        synth::ui::LayoutOptions ontoStranger;
        ontoStranger.overlayOf = "elsewhere";
        b.Draw("onto.stranger", ontoStranger, [](synth::ui::Bounds) {
            return std::vector<synth::ui::DrawCommand>{};
        });
    });
    b.Label("elsewhere", "e", MainOf(synth::ui::Extent::Px(50.0f)));
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    Require(SameBounds(FindNode(tree, "anchored").bounds, {5.0f, 6.0f, 70.0f, 40.0f}),
            "the explicitly positioned sibling keeps its author-supplied bounds");
    for (const char* rejected : {"onto.anchored", "onto.overlay", "onto.stranger"})
    {
        Require(SameBounds(FindNode(tree, rejected).bounds, {}),
                std::string("overlay '") + rejected + "' collapses to nothing");
    }
}

void TestOverlayInsideAnOverlayContainerResolves()
{
    // An overlay may be a container, and resolving it discovers an overlay of
    // its own. That inner one is found only while the deferred overlays are
    // already being walked, so it has to be picked up by the same walk.
    synth::ui::LayoutOptions panelOverlay;
    panelOverlay.overlayOf = "target";
    panelOverlay.padding = 0.0f;

    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("col", {}, [&panelOverlay](synth::ui::Builder& b) {
        b.Section("panel", panelOverlay, [](synth::ui::Builder& b) {
            b.Label("inner.target", "inner", MainOf(synth::ui::Extent::Px(20.0f)));
            synth::ui::LayoutOptions inner;
            inner.overlayOf = "inner.target";
            b.Draw("inner.overlay", inner, [](synth::ui::Bounds extent) {
                return std::vector<synth::ui::DrawCommand>{
                    synth::ui::DrawCommand::Fill(extent, synth::Color::Rgb(3, 4, 5))};
            });
        });
        b.Label("target", "target", MainOf(synth::ui::Extent::Px(40.0f)));
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});

    const auto& panel = FindNode(tree, "panel");
    const auto& target = FindNode(tree, "target");
    Require(SameBounds(panel.bounds, target.bounds),
            "the overlay container takes its own target's bounds");
    Require(panel.bounds.width > 0.0f && panel.bounds.height > 0.0f,
            "the overlay container really was given an extent to resolve into");

    const auto& innerTarget = FindNode(tree, "inner.target");
    const auto& innerOverlay = FindNode(tree, "inner.overlay");
    Require(SameBounds(innerTarget.bounds, {0.0f, 0.0f, panel.bounds.width, 20.0f}),
            "the overlay container lays its own children out");
    Require(SameBounds(innerOverlay.bounds, innerTarget.bounds),
            "an overlay nested inside an overlay container still reaches its target");
    Require(innerOverlay.drawCommands.size() == 1 &&
                NearlyEqual(innerOverlay.drawCommands[0].bounds.width, innerTarget.bounds.width) &&
                NearlyEqual(innerOverlay.drawCommands[0].bounds.height, innerTarget.bounds.height),
            "the nested overlay's draw factory runs against its resolved extent");
}

void TestOverlayTracksATargetTheFormGridMoves()
{
    // A form grid moves and resizes its cells after their own row has placed
    // them. An overlay on such a cell must land on where the cell ended up,
    // not on where the row first put it.
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("form", grid, [](synth::ui::Builder& b) {
        b.Row("r1", {}, [](synth::ui::Builder& b) {
            b.Label("r1.label", "Tempo", {});
            synth::ui::LayoutOptions o;
            o.overlayOf = "r1.control";
            b.Draw("r1.underlay", o, [](synth::ui::Bounds extent) {
                return std::vector<synth::ui::DrawCommand>{
                    synth::ui::DrawCommand::Fill(extent, synth::Color::Rgb(1, 2, 3))};
            });
            b.ComboBox("r1.control", {}, "", synth::ui::Action::Named("a"), {});
        });
        b.Row("r2", {}, [](synth::ui::Builder& b) {
            b.Label("r2.label", "A considerably longer caption", {});
            b.ComboBox("r2.control", {}, "", synth::ui::Action::Named("b"), {});
        });
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});
    const auto& control = FindNode(tree, "r1.control");
    const auto& underlay = FindNode(tree, "r1.underlay");
    Require(control.bounds.x > FindNode(tree, "r1.label").bounds.x,
            "the form grid really did move the control into the shared column");
    Require(SameBounds(underlay.bounds, control.bounds),
            "an overlay lands on its target's post-form-grid bounds");
    Require(underlay.drawCommands.size() == 1 &&
                NearlyEqual(underlay.drawCommands[0].bounds.width, control.bounds.width),
            "the overlay's draw factory runs against the post-form-grid extent");
}

synth::ui::NodeTree BuildStandardLayoutWith(float width,
                                            float height,
                                            synth::ui::Builder::Children upper,
                                            synth::ui::Builder::Children bay)
{
    synth::ui::StandardAppLayout layout;
    layout.idPrefix = "app";
    layout.title = "App";
    layout.upperVisualizer = std::move(upper);
    layout.widgetBay = std::move(bay);

    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, width, height});
    layout.Emit(b);
    return b.Build({0.0f, 0.0f, width, height});
}

synth::ui::Builder::Children StandardBayContent()
{
    return [](synth::ui::Builder& b) {
        b.Button("app.bay.one", "One", synth::ui::Action::Named("one"), {});
        b.Button("app.bay.two", "Two", synth::ui::Action::Named("two"), {});
    };
}

synth::ui::NodeTree BuildStandardLayout(float width, float height, synth::ui::Builder::Children upper)
{
    return BuildStandardLayoutWith(width, height, std::move(upper), StandardBayContent());
}

synth::ui::NodeTree BuildStandardLayoutAt(float width, float height)
{
    return BuildStandardLayoutWith(width, height, {}, StandardBayContent());
}

synth::ui::NodeTree BuildStandardLayoutWithBay()
{
    return BuildStandardLayoutWith(900.0f, 560.0f, {}, StandardBayContent());
}

synth::ui::NodeTree BuildStandardLayoutWithoutBay()
{
    return BuildStandardLayoutWith(900.0f, 560.0f, {}, {});
}

void TestSlotsAcceptArbitraryComponents()
{
    const auto fill = [](const char* id) {
        return [id](synth::ui::Builder& b) {
            b.Draw(id, LayoutMain(synth::ui::Extent::Weight(1.0f)), [](synth::ui::Bounds e) {
                return std::vector<synth::ui::DrawCommand>{
                    synth::ui::DrawCommand::Fill(e, synth::Color::Rgb(1, 1, 1))};
            });
        };
    };
    const auto grid = [&fill](synth::ui::Builder& b) {
        b.Row("cells", LayoutMain(synth::ui::Extent::Weight(1.0f)), [&fill](synth::ui::Builder& b) {
            fill("cell0")(b);
            fill("cell1")(b);
        });
    };
    for (const auto& upper : {synth::ui::Builder::Children(grid),
                              synth::ui::Builder::Children(fill("wave"))})
    {
        const auto tree = BuildStandardLayout(900.0f, 560.0f, upper);
        Require(FindNode(tree, "app.slot.upper").bounds.width > 0.0f,
                "the upper slot resolves whatever component it is given");
    }
    // The slot names are part of the declared interface; what must not appear
    // is any knowledge of what a slot is filled with.
    for (const char* forbidden : {"cellCount", "cellWidth", "kEncoderCount", "kScopeCount",
                                  "BoundsForIndex"})
    {
        Require(!SourceContains("projects/synth/include/synth/PortableUIStandardLayout.hpp", forbidden),
                std::string("the standard layout contains no '") + forbidden + "' logic");
    }
}

void TestStandardLayoutProportionsMatchBothApps()
{
    const auto tree = BuildStandardLayoutAt(900.0f, 560.0f);
    // contentWidth = 900 - 2*16 = 868; 868 * 0.46 = 399.28, capped at 390.
    Require(NearlyEqual(FindNode(tree, "app.visualizers").bounds.width, 390.0f),
            "the visualizer stack takes min(390, contentWidth * 0.46)");
    Require(NearlyEqual(FindNode(tree, "app.encoders").bounds.width, 462.0f),
            "the encoder region takes min(462, remainder)");
    Require(NearlyEqual(FindNode(tree, "app.title").bounds.height, 30.0f),
            "the title row is 30 high");
    Require(FindNode(tree, "app.visualizers").bounds.x < FindNode(tree, "app.encoders").bounds.x,
            "the visualizer stack is on the LEFT, encoders to its right");
    Require(FindNode(tree, "app.slot.upper").bounds.y < FindNode(tree, "app.slot.lower").bounds.y,
            "the visualizer column stacks the upper slot above the lower");
}

void TestEmptyWidgetBayCollapses()
{
    const auto with = BuildStandardLayoutWithBay();
    const auto without = BuildStandardLayoutWithoutBay();
    Require(SameBounds(FindNode(without, "app.bay").bounds, {}),
            "an unsupplied widget bay occupies no space and renders no chrome");
    Require(FindNode(without, "app.bay").children.empty(),
            "an unsupplied widget bay renders no placeholder content");

    // 560 root less the 16 margin twice is 528 of page content. Supplied, this
    // bay stacks two 28-high buttons over one 14 gap — 70 — and costs a second
    // 14 separating it from the body: 528 - 30 title - 14 - 70 - 14 = 400.
    // Unsupplied, it costs NEITHER extent nor gap, so the body takes all 84
    // back: 528 - 30 - 14 = 484.
    Require(NearlyEqual(FindNode(with, "app.bay").bounds.height, 70.0f),
            "a supplied bay is exactly as high as the controls it was given");
    Require(NearlyEqual(FindNode(with, "app.visualizers").bounds.height, 400.0f),
            "the regions above give up the bay's extent and its gap");
    Require(NearlyEqual(FindNode(without, "app.visualizers").bounds.height, 484.0f),
            "a collapsed bay gives back its extent AND the gap it would have cost");
}

void TestStandardLayoutRedistributesAtDifferentExtents()
{
    const auto narrow = BuildStandardLayoutAt(700.0f, 560.0f);
    const auto wide = BuildStandardLayoutAt(1400.0f, 560.0f);
    Require(FindNode(wide, "app.encoders").bounds.width >=
                FindNode(narrow, "app.encoders").bounds.width,
            "regions redistribute through the ordinary resolver at a wider extent");
    Require(NearlyEqual(FindNode(wide, "app.visualizers").bounds.width, 390.0f),
            "the capped stack stays at its maximum inside a real composition");
    // 700 - 2*16 = 668 content; 668 * 0.46 = 307.28 is below the cap, and the
    // encoder region takes what is left after one 14 gap.
    Require(NearlyEqual(FindNode(narrow, "app.visualizers").bounds.width, 307.28f),
            "below the cap the stack is exactly the content-width fraction");
    Require(NearlyEqual(FindNode(narrow, "app.encoders").bounds.width, 668.0f - 14.0f - 307.28f),
            "the encoder region takes the remainder after the gap, as both apps did");
}

// ---------------------------------------------------------------------------
// sru-48 named visual criteria: the checks themselves (task 6.1-6.3).
//
// `tests/support/VisualCriteria.hpp` is the headless half of the criteria, and
// `portable_ui_tests.cpp` runs it over every real page and app. What it cannot
// do there is prove a check would ever FAIL: every first-party surface is
// expected to conform, so a predicate that returned an empty list
// unconditionally would sit green over all of them. These tests are the
// mutation evidence, in the suite rather than in a report: each one builds a
// tree that violates exactly one criterion and requires the corresponding
// check to name it, and requires the conforming twin to come back clean.
// ---------------------------------------------------------------------------

namespace criteria = synth::ui::criteria;

// The stacked column every negative case below mutates: three 20-high labels in
// a 200x200 container on the library's own 12 padding and 8 gap.
synth::ui::NodeTree BuildConformingColumn()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 200.0f});
    synth::ui::LayoutOptions column = StackLayout(synth::ui::Extent::Px(140.0f));
    b.Column("column", column, [](synth::ui::Builder& b) {
        b.Label("first", "first", MainOf(synth::ui::Extent::Px(20.0f)));
        b.Label("second", "second", MainOf(synth::ui::Extent::Px(20.0f)));
        b.Label("third", "third", MainOf(synth::ui::Extent::Px(20.0f)));
    });
    return b.Build({0.0f, 0.0f, 200.0f, 200.0f});
}

synth::ui::Node& MutableNode(synth::ui::NodeTree& tree, const char* id)
{
    for (synth::ui::Node& node : tree.nodes)
    {
        if (node.id == synth::ui::NodeId(id))
        {
            return node;
        }
    }
    throw std::runtime_error(std::string("missing node: ") + id);
}

const std::vector<float>& LibrarySpacingValues()
{
    static const std::vector<float> values{0.0f,
                                           synth::ui::kSpacing.gap,
                                           synth::ui::kSpacing.padding,
                                           synth::ui::kSpacing.labelGap};
    return values;
}

void TestContainmentCheckCatchesAChildPushedOutOfItsParent()
{
    const synth::ui::NodeTree conforming = BuildConformingColumn();
    Require(criteria::ContainmentViolations(conforming).empty(),
            "a well-formed column has every child inside its parent");

    synth::ui::NodeTree overflowing = conforming;
    // One pixel past the column's bottom inside edge, which is the smallest
    // difference the check is allowed to miss and does not.
    synth::ui::Node& third = MutableNode(overflowing, "third");
    third.bounds.y = FindNode(conforming, "column").bounds.height - third.bounds.height + 1.0f;
    const std::vector<std::string> violations = criteria::ContainmentViolations(overflowing);
    Require(violations.size() == 1, "a child pushed one pixel past its parent is one violation");
    Require(Mentions(violations.front(), "third") && Mentions(violations.front(), "column"),
            "the containment violation names both the child and the parent it left");

    // The same child moved one pixel the other way is still inside, so the
    // check is measuring the edge and not merely "the last child moved".
    synth::ui::NodeTree contained = conforming;
    MutableNode(contained, "third").bounds.y =
        FindNode(conforming, "column").bounds.height - third.bounds.height - 1.0f;
    Require(criteria::ContainmentViolations(contained).empty(),
            "a child one pixel inside the parent edge is contained");
}

void TestContainmentUsesTheScrollContentRectangleNotTheViewport()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 200.0f});
    b.ScrollArea("scroll", LayoutMain(synth::ui::Extent::Px(60.0f)), [](synth::ui::Builder& b) {
        for (int ix = 0; ix < 6; ++ix)
        {
            b.Label("row." + std::to_string(ix), "row", MainOf(synth::ui::Extent::Px(30.0f)));
        }
    });
    const synth::ui::NodeTree tree = b.Build({0.0f, 0.0f, 200.0f, 200.0f});
    const synth::ui::Node& scroll = FindNode(tree, "scroll");
    const synth::ui::Node& last = FindNode(tree, "row.5");
    Require(last.bounds.y + last.bounds.height > scroll.bounds.height,
            "the fixture really does put its tail below the visible viewport");
    Require(criteria::ContainmentViolations(tree).empty(),
            "a row below the viewport is contained by the scroll-content rectangle, not overflowing");

    // ... and the content rectangle is a real bound, not an escape hatch: a row
    // past the published content extent is still a violation.
    synth::ui::NodeTree beyond = tree;
    MutableNode(beyond, "row.5").bounds.y = scroll.scrollContentHeight + 1.0f;
    Require(criteria::ContainmentViolations(beyond).size() == 1,
            "a row past the published scroll-content extent overflows");
}

void TestOverlapCheckCatchesTwoSiblingsSharingSpace()
{
    const synth::ui::NodeTree conforming = BuildConformingColumn();
    Require(criteria::SiblingOverlapViolations(conforming).empty(),
            "stacked siblings separated by a gap do not intersect");

    synth::ui::NodeTree overlapping = conforming;
    MutableNode(overlapping, "second").bounds.y = FindNode(conforming, "first").bounds.y + 1.0f;
    const std::vector<std::string> violations = criteria::SiblingOverlapViolations(overlapping);
    Require(violations.size() == 1, "two siblings sharing space are one violation");
    Require(Mentions(violations.front(), "first") && Mentions(violations.front(), "second"),
            "the overlap violation names both siblings");

    // Abutting is not overlapping: a zero gap between two stacked children is a
    // layout choice the pages make, and reading it as an intersection would
    // make the criterion unusable.
    synth::ui::NodeTree abutting = conforming;
    MutableNode(abutting, "second").bounds.y =
        FindNode(conforming, "first").bounds.y + FindNode(conforming, "first").bounds.height;
    Require(criteria::SiblingOverlapViolations(abutting).empty(),
            "two children that abut exactly do not intersect");
}

void TestAnUnderlayIsPinnedToItsTargetRatherThanExemptedFromOverlap()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 200.0f});
    b.Row("row", LayoutMain(synth::ui::Extent::Px(60.0f)), [](synth::ui::Builder& b) {
        synth::ui::ControlStyle underlay;
        underlay.layout.overlayOf = "cell.1";
        b.Draw("cell.1.visualizer", [](synth::ui::Bounds) { return std::vector<synth::ui::DrawCommand>{}; },
               underlay);
        b.Draw("cell.0", LayoutMain(synth::ui::Extent::Weight(1.0f)),
               [](synth::ui::Bounds) { return std::vector<synth::ui::DrawCommand>{}; });
        b.Draw("cell.1", LayoutMain(synth::ui::Extent::Weight(1.0f)),
               [](synth::ui::Bounds) { return std::vector<synth::ui::DrawCommand>{}; });
    });
    const synth::ui::NodeTree tree = b.Build({0.0f, 0.0f, 200.0f, 200.0f});
    Require(criteria::SameRectangle(FindNode(tree, "cell.1.visualizer").bounds,
                                    FindNode(tree, "cell.1").bounds),
            "the fixture really is an sru-25 underlay resolved onto its encoder");
    Require(criteria::SiblingOverlapViolations(tree).empty(),
            "an underlay congruent with the one cell it names is not an overlap violation");
    Require(criteria::UnderlayViolations(tree).empty(), "and it satisfies the underlay pin");

    // The exemption is not a licence to sit anywhere: an underlay dragged onto
    // its neighbour fails the underlay pin AND the overlap check, because it
    // now intersects a sibling it does not name.
    synth::ui::NodeTree drifted = tree;
    MutableNode(drifted, "cell.1.visualizer").bounds = FindNode(tree, "cell.0").bounds;
    Require(criteria::UnderlayViolations(drifted).size() == 1,
            "an underlay that is not congruent with its target is named");
    Require(criteria::SiblingOverlapViolations(drifted).size() == 1,
            "and intersecting the wrong sibling is still an overlap violation");
}

void TestSpacingCheckCatchesAGapOutsideTheSharedMetrics()
{
    const synth::ui::NodeTree conforming = BuildConformingColumn();
    const criteria::SpacingReport clean =
        criteria::SpacingConformance(conforming, LibrarySpacingValues());
    Require(clean.violations.empty(), "a column on the library gap conforms");
    Require(clean.observed.count(synth::ui::kSpacing.gap) == 1,
            "the conforming fixture really was measured, and its gap really is the shared one");

    synth::ui::NodeTree drifted = conforming;
    MutableNode(drifted, "third").bounds.y += 3.0f;
    const criteria::SpacingReport report =
        criteria::SpacingConformance(drifted, LibrarySpacingValues());
    Require(report.violations.size() == 1, "one hand-inserted three-pixel gap is one violation");
    Require(Mentions(report.violations.front(), "third") &&
                Mentions(report.violations.front(), "11.00"),
            "the spacing violation names the offending child and the measured gap");
}

void TestColumnAlignmentCheckCatchesAControlLeavingItsColumn()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 300.0f, 200.0f});
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    b.Column("form", grid, [](synth::ui::Builder& b) {
        for (int ix = 0; ix < 3; ++ix)
        {
            synth::ui::ControlStyle style;
            style.caption = ix == 1 ? "A much longer caption" : "Short";
            b.Toggle("toggle." + std::to_string(ix), "", false,
                     synth::ui::Action::Named("toggle"), style);
        }
    });
    const synth::ui::NodeTree tree = b.Build({0.0f, 0.0f, 300.0f, 200.0f});
    const criteria::ColumnReport clean = criteria::ColumnAlignment(tree, "form");
    Require(clean.violations.empty(), "a form grid aligns its rows' columns");
    Require(clean.comparedRows == 3 && clean.comparedColumns == 2,
            "the alignment check really compared three rows across two columns");

    synth::ui::NodeTree misaligned = tree;
    MutableNode(misaligned, "toggle.2").bounds.x += 4.0f;
    const criteria::ColumnReport report = criteria::ColumnAlignment(misaligned, "form");
    Require(report.violations.size() == 1, "one control leaving its column is one violation");
    Require(Mentions(report.violations.front(), "toggle.2") &&
                Mentions(report.violations.front(), "toggle.0"),
            "the alignment violation names the stray control and the column it left");
}

void TestCaptionCheckSeesThroughAnUnrenderedLabel()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 300.0f, 200.0f});
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    b.Column("form", grid, [](synth::ui::Builder& b) {
        synth::ui::ControlStyle captioned;
        captioned.caption = "Output device";
        b.ComboBox("captioned", {{"a", "A"}}, "a", synth::ui::Action::Named("pick"), captioned);
        // design.md OQ5: a combo box's own label renders nothing in either
        // backend, so this control is unlabelled on screen however the string
        // reads in the tree.
        b.ComboBox("labelled_only", {{"a", "A"}}, "a", synth::ui::Action::Named("pick"), {});
    });
    synth::ui::NodeTree tree = b.Build({0.0f, 0.0f, 300.0f, 200.0f});
    MutableNode(tree, "labelled_only").label = "Input device";

    const criteria::CaptionReport report = criteria::UncaptionedFormControls(tree);
    Require(report.violations.size() == 1,
            "the captioned combo passes and the label-only combo does not");
    Require(Mentions(report.violations.front(), "labelled_only"),
            "the caption violation names the control the user cannot identify");
    Require(report.examined == 2 && report.residualsMatched == 0,
            "both combos were examined and neither was excused");

    // The exception map excuses exactly the control it names, and says so: with
    // the offender listed the check comes back clean AND reports one residual
    // matched, so a caller can tell "excused" apart from "nothing to see".
    const criteria::CaptionReport excused =
        criteria::UncaptionedFormControls(tree, {{"labelled_only", "a product decision for 6.5"}});
    Require(excused.violations.empty() && excused.residualsMatched == 1 && excused.examined == 1,
            "a named exception excuses its own control and nothing else");

    // A toggle DOES render its own label, so a non-empty one identifies it.
    synth::ui::Builder t;
    t.Root("root", {0.0f, 0.0f, 300.0f, 200.0f});
    t.Toggle("toggle", "Send clock", false, synth::ui::Action::Named("toggle"), {});
    Require(criteria::UncaptionedFormControls(t.Build({0.0f, 0.0f, 300.0f, 200.0f}))
                .violations.empty(),
            "a toggle rendering its own label needs no separate caption node");
}

void TestEmptyTextCheckCatchesAReservedBandThatSaysNothing()
{
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 200.0f, 200.0f});
    b.Label("speaks", "Current BPM: 120.00", MainOf(synth::ui::Extent::Px(20.0f)));
    b.StatusText("silent", "", MainOf(synth::ui::Extent::Px(20.0f)));
    const synth::ui::NodeTree tree = b.Build({0.0f, 0.0f, 200.0f, 200.0f});
    const std::vector<std::string> violations = criteria::EmptyTextNodes(tree);
    Require(violations.size() == 1, "a reserved band rendering no text is one violation");
    Require(Mentions(violations.front(), "silent"), "the empty-text violation names the silent node");
}

// A captioned control declaring ControlStyle::controlWidth = Intrinsic is
// pinned to the control column's left edge but sized to its own content, not
// stretched to fill the column -- the button described in the plan. A
// captioned control that leaves controlWidth at its default keeps filling the
// column exactly as before.
void TestCaptionedButtonIsSizedToItsOwnCaptionInsteadOfTheColumn()
{
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("form", grid, [](synth::ui::Builder& b) {
        synth::ui::ControlStyle field;
        field.caption = "Output device";
        b.ComboBox("output", {}, "", synth::ui::Action::Named("pick"), field);

        synth::ui::ControlStyle button;
        button.caption = "Input capture";
        button.controlWidth = synth::ui::Extent::Intrinsic();
        b.Button("retry", "Retry Input", synth::ui::Action::Named("retry"), button);

        synth::ui::ControlStyle textField;
        textField.caption = "Note";
        b.TextField("note", "", "", synth::ui::Action::Named("edit"), textField);
    });
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});

    const synth::ui::Node& output = FindNode(tree, "output");
    const synth::ui::Node& retry = FindNode(tree, "retry");
    const synth::ui::Node& note = FindNode(tree, "note");

    const float expectedIntrinsic =
        std::max(72.0f, synth::ui::metrics::TextWidth("Retry Input", synth::ui::TextStyle{}));
    Require(NearlyEqual(retry.bounds.width, expectedIntrinsic),
            "a captioned button declaring Intrinsic controlWidth sizes to its own label, not the column");
    Require(retry.bounds.width < output.bounds.width,
            "the intrinsic button is narrower than the column a full-width control occupies");
    Require(NearlyEqual(retry.bounds.x, output.bounds.x),
            "the intrinsic button's left edge matches a full-width control's left edge in the same grid");
    Require(NearlyEqual(note.bounds.width, output.bounds.width),
            "a captioned control with no controlWidth override still spans the full control column");
}

// The control-cell width assignment does two jobs (PortableUILayout.hpp
// ApplyFormGrid): size a weighted control to fill the column, and clamp a
// non-weighted control that would otherwise overrun it. This exercises the
// second job directly: a button whose own content is wider than what the
// column has left must be held to the column, and the row it lives in must
// still hold its children (RequireContainerHoldsItsChildren judges FINAL
// bounds, after this clamp runs).
void TestContentWiderThanTheColumnIsClampedAndTheRowStillHoldsItsChildren()
{
    synth::ui::LayoutOptions grid;
    grid.formGrid = true;
    synth::ui::Builder b;
    b.Root("root", {0.0f, 0.0f, 400.0f, 300.0f});
    b.Column("form", grid, [](synth::ui::Builder& b) {
        synth::ui::ControlStyle field;
        field.caption = "A considerably longer caption";
        b.ComboBox("output", {}, "", synth::ui::Action::Named("pick"), field);

        synth::ui::ControlStyle button;
        button.caption = "Short";
        button.controlWidth = synth::ui::Extent::Intrinsic();
        b.Button("wide", "A button label far too long to fit the remaining column width",
                 synth::ui::Action::Named("wide"), button);
    });
    // Must not throw: RequireContainerHoldsItsChildren aborts the build if the
    // clamp fails to keep the oversized button inside its row.
    const auto tree = b.Build({0.0f, 0.0f, 400.0f, 300.0f});

    const synth::ui::Node& row = FindNode(tree, "wide.row");
    const synth::ui::Node& wide = FindNode(tree, "wide");
    const float expectedIntrinsic = std::max(
        72.0f,
        synth::ui::metrics::TextWidth("A button label far too long to fit the remaining column width",
                                      synth::ui::TextStyle{}));
    Require(wide.bounds.width < expectedIntrinsic,
            "the content-sized control's declared width really is wider than what the column can hold");
    Require(wide.bounds.x + wide.bounds.width <= row.bounds.width + 0.01f,
            "a content-sized control wider than its column is clamped to the column, so the row still holds it");
}

}  // namespace

int main()
{
    TestWeightsDivideRemainingSpaceDeterministically();
    TestMaximumClampsAndRedistributesOnce();
    TestClampingRedistributionDoesNotRepeat();
    TestFractionIsOfContentExtentNotRemainingSpace();
    TestUnclampedFractionPinsContentExtentBasis();
    TestInfeasibleMinimaFailLoudlyInDeclarationOrder();
    TestUnabsorbedOverflowFailsWithAnActionableDiagnostic();
    TestAnOverflowingRowNamesItsOwnStackingAxis();
    TestAScrollAreaAbsorbsAListTallerThanItsViewport();
    TestAWeightedChildAbsorbsTheRemainder();
    TestInsertingARowShiftsSiblingsByExtentPlusGap();
    TestExplicitlyPositionedChildrenAreOutOfFlow();
    TestInFlowDrawFactoryReceivesItsResolvedExtent();
    TestFormGridAlignsLabelAndControlColumns();
    TestFormGridUsesRowLocalPadding();
    TestCrossAxisWeightDoesNotExceedContentExtent();
    TestCaptionedControlRowOccupiesTheParentFlowSlot();
    TestCaptionedAndUncaptionedControlsHonorTheSameDeclaredExtent();
    TestSplicedSubtreeLayoutOptionsAreHonoredWhenResolved();
    TestSplicedSubtreeDrawFactoryStillRuns();
    TestComponentResolvesIdenticallyUnderDifferentParents();
    TestExtentDrivenRedistribution();
    TestTextReservationIsDeterministicAndBackendFree();
    TestWrappingRowFlowsOntoAdditionalLines();
    TestAWrappingRowStillFailsOnAChildWiderThanTheRow();
    TestWrappingRowReservesGrownExtentInParentFlow();
    TestWrappingRowLineBreakHeightIsPinned();
    TestIntrinsicColumnReservesAWrappingRowsGrownExtent();
    TestOverlayChildTakesItsTargetsResolvedBounds();
    TestOverlayRejectsATargetThatIsNotInFlow();
    TestOverlayInsideAnOverlayContainerResolves();
    TestOverlayTracksATargetTheFormGridMoves();
    TestSectionAndScrollAreaStackChildrenVertically();
    TestSlotsAcceptArbitraryComponents();
    TestStandardLayoutProportionsMatchBothApps();
    TestEmptyWidgetBayCollapses();
    TestStandardLayoutRedistributesAtDifferentExtents();
    TestContainmentCheckCatchesAChildPushedOutOfItsParent();
    TestContainmentUsesTheScrollContentRectangleNotTheViewport();
    TestOverlapCheckCatchesTwoSiblingsSharingSpace();
    TestAnUnderlayIsPinnedToItsTargetRatherThanExemptedFromOverlap();
    TestSpacingCheckCatchesAGapOutsideTheSharedMetrics();
    TestColumnAlignmentCheckCatchesAControlLeavingItsColumn();
    TestCaptionCheckSeesThroughAnUnrenderedLabel();
    TestEmptyTextCheckCatchesAReservedBandThatSaysNothing();
    TestCaptionedButtonIsSizedToItsOwnCaptionInsteadOfTheColumn();
    TestContentWiderThanTheColumnIsClampedAndTheRowStillHoldsItsChildren();
}
