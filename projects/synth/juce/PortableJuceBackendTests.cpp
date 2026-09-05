#include "PortableJuceBackend.hpp"

#include "synth/PortableUIBuilders.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

juce::Rectangle<int> SurfaceBoundsOf(const juce::Component& surface,
                                     const juce::Component& child)
{
    return surface.getLocalArea(&child, child.getLocalBounds());
}

juce::Image RenderComponent(juce::Component& component)
{
    juce::Image image(juce::Image::ARGB,
                      std::max(1, component.getWidth()),
                      std::max(1, component.getHeight()),
                      true);
    juce::Graphics graphics(image);
    component.paintEntireComponent(graphics, true);
    return image;
}

bool NearlyEqual(float actual, float expected, float tolerance = 0.001f)
{
    return std::fabs(actual - expected) <= tolerance;
}

juce::Colour FillColourOf(synth_juce::PortableComponent& component,
                          const std::string& id)
{
    juce::Component* control = component.FindByNodeId(id);
    Require(control != nullptr, "fill-colour fixture node is rendered");
    if (auto* button = dynamic_cast<juce::TextButton*>(control))
    {
        return button->findColour(juce::TextButton::buttonColourId);
    }
    if (auto* label = dynamic_cast<juce::Label*>(control))
    {
        return label->findColour(juce::Label::backgroundColourId);
    }
    if (auto* editor = dynamic_cast<juce::TextEditor*>(control))
    {
        return editor->findColour(juce::TextEditor::backgroundColourId);
    }
    if (auto* combo = dynamic_cast<juce::ComboBox*>(control))
    {
        return combo->findColour(juce::ComboBox::backgroundColourId);
    }
    if (auto* slider = dynamic_cast<juce::Slider*>(control))
    {
        return slider->findColour(juce::Slider::trackColourId);
    }
    if (auto* toggle = dynamic_cast<juce::ToggleButton*>(control))
    {
        return toggle->findColour(juce::ToggleButton::tickColourId);
    }
    throw std::runtime_error("unsupported fill-colour fixture node");
}

juce::Colour TextColourOf(synth_juce::PortableComponent& component,
                          const std::string& id)
{
    juce::Component* control = component.FindByNodeId(id);
    Require(control != nullptr, "text-colour fixture node is rendered");
    if (auto* label = dynamic_cast<juce::Label*>(control))
    {
        return label->findColour(juce::Label::textColourId);
    }
    if (auto* button = dynamic_cast<juce::TextButton*>(control))
    {
        return button->findColour(juce::TextButton::textColourOffId);
    }
    if (auto* editor = dynamic_cast<juce::TextEditor*>(control))
    {
        return editor->findColour(juce::TextEditor::textColourId);
    }
    throw std::runtime_error("unsupported text-colour fixture node");
}

bool IsDerivedFrom(juce::Colour colour, juce::Colour base)
{
    return colour == base.brighter(0.14f);
}

struct RecordingSurface final : synth::ui::Surface
{
    synth::ui::NodeTree BuildTree() override
    {
        return tree;
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        lastAction = action;
        actions.push_back(action);
        dispatchCount += 1;
    }

    synth::ui::NodeTree tree;
    synth::ui::Action lastAction;
    std::vector<synth::ui::Action> actions;
    int dispatchCount = 0;
    ActionHandler handler_;
};

synth::ui::NodeTree BackendGeometryPropertyTree()
{
    // Keep this representative fixture in sync with backendGeometryPropertyNodes
    // in browser/tests/ui-backend.spec.ts.
    return {.nodes = {
                {.id = synth::ui::NodeId("root"),
                 .kind = synth::ui::NodeKind::Root,
                 .bounds = {0.0f, 0.0f, 500.0f, 260.0f},
                 .children = {synth::ui::NodeId("section"), synth::ui::NodeId("scroll")}},
                {.id = synth::ui::NodeId("section"),
                 .kind = synth::ui::NodeKind::Section,
                 .bounds = {20.0f, 16.0f, 220.0f, 130.0f},
                 .children = {synth::ui::NodeId("row"), synth::ui::NodeId("status")}},
                {.id = synth::ui::NodeId("row"),
                 .kind = synth::ui::NodeKind::Row,
                 .bounds = {7.0f, 11.0f, 190.0f, 70.0f},
                 .children = {synth::ui::NodeId("label"),
                              synth::ui::NodeId("button"),
                              synth::ui::NodeId("toggle"),
                              synth::ui::NodeId("slider"),
                              synth::ui::NodeId("draw"),
                              synth::ui::NodeId("overhang")}},
                {.id = synth::ui::NodeId("label"),
                 .kind = synth::ui::NodeKind::Label,
                 .bounds = {4.0f, 3.0f, 48.0f, 18.0f},
                 .text = "Label"},
                {.id = synth::ui::NodeId("button"),
                 .kind = synth::ui::NodeKind::Button,
                 .bounds = {60.0f, 4.0f, 64.0f, 24.0f},
                 .label = "Go"},
                {.id = synth::ui::NodeId("toggle"),
                 .kind = synth::ui::NodeKind::Toggle,
                 .bounds = {130.0f, 4.0f, 54.0f, 24.0f},
                 .label = "On"},
                {.id = synth::ui::NodeId("slider"),
                 .kind = synth::ui::NodeKind::Slider,
                 .bounds = {60.0f, 36.0f, 96.0f, 24.0f},
                 .value = 0.5f,
                 .minValue = 0.0f,
                 .maxValue = 1.0f,
                 .step = 0.01f},
                {.id = synth::ui::NodeId("draw"),
                 .kind = synth::ui::NodeKind::Draw,
                 .bounds = {160.0f, 34.0f, 24.0f, 24.0f},
                 .drawCommands = {synth::ui::DrawCommand::Fill(
                     {0.0f, 0.0f, 24.0f, 24.0f}, synth::Color::Rgb(1, 2, 3))}},
                {.id = synth::ui::NodeId("overhang"),
                 .kind = synth::ui::NodeKind::Label,
                 .bounds = {180.0f, 50.0f, 40.0f, 24.0f},
                 .text = "Overhang"},
                {.id = synth::ui::NodeId("status"),
                 .kind = synth::ui::NodeKind::StatusText,
                 .bounds = {7.0f, 90.0f, 190.0f, 22.0f},
                 .text = "Status"},
                {.id = synth::ui::NodeId("scroll"),
                 .kind = synth::ui::NodeKind::ScrollArea,
                 .bounds = {260.0f, 20.0f, 120.0f, 90.0f},
                 .scrollContentWidth = 240.0f,
                 .scrollContentHeight = 220.0f,
                 .children = {synth::ui::NodeId("scroll.row"),
                              synth::ui::NodeId("scroll.draw"),
                              synth::ui::NodeId("zero")}},
                {.id = synth::ui::NodeId("scroll.row"),
                 .kind = synth::ui::NodeKind::Row,
                 .bounds = {8.0f, 30.0f, 200.0f, 32.0f},
                 .children = {synth::ui::NodeId("combo"), synth::ui::NodeId("field")}},
                {.id = synth::ui::NodeId("combo"),
                 .kind = synth::ui::NodeKind::ComboBox,
                 .bounds = {4.0f, 4.0f, 75.0f, 24.0f},
                 .options = {{"one", "One"}, {"two", "Two"}},
                 .selectedOption = "one"},
                {.id = synth::ui::NodeId("field"),
                 .kind = synth::ui::NodeKind::TextField,
                 .bounds = {86.0f, 4.0f, 88.0f, 24.0f},
                 .text = "value"},
                {.id = synth::ui::NodeId("scroll.draw"),
                 .kind = synth::ui::NodeKind::Draw,
                 .bounds = {20.0f, 125.0f, 50.0f, 35.0f},
                 .drawCommands = {synth::ui::DrawCommand::Fill(
                     {0.0f, 0.0f, 50.0f, 35.0f}, synth::Color::Rgb(4, 5, 6))}},
                {.id = synth::ui::NodeId("zero"),
                 .kind = synth::ui::NodeKind::Label,
                 .bounds = {150.0f, 10.0f, 0.0f, 0.0f},
                 .text = "unresolved"},
            }};
}

juce::Point<float> FoldAncestorOrigins(
    const synth::ui::NodeTree& tree,
    const std::string& id,
    const std::unordered_map<std::string, juce::Point<int>>& scrollOffsets)
{
    std::unordered_map<std::string, const synth::ui::Node*> nodesById;
    std::unordered_map<std::string, std::string> parentById;
    for (const synth::ui::Node& node : tree.nodes)
    {
        nodesById[node.id.value] = &node;
        for (const synth::ui::NodeId& child : node.children)
        {
            parentById[child.value] = node.id.value;
        }
    }

    const synth::ui::Node* node = nodesById.at(id);
    float x = node->bounds.x;
    float y = node->bounds.y;
    for (auto parent = parentById.find(id); parent != parentById.end();
         parent = parentById.find(parent->second))
    {
        const synth::ui::Node* parentNode = nodesById.at(parent->second);
        x += parentNode->bounds.x;
        y += parentNode->bounds.y;
        if (parentNode->kind == synth::ui::NodeKind::ScrollArea)
        {
            const auto offset = scrollOffsets.find(parentNode->id.value);
            if (offset != scrollOffsets.end())
            {
                x -= static_cast<float>(offset->second.x);
                y -= static_cast<float>(offset->second.y);
            }
        }
    }
    return {x, y};
}

juce::Rectangle<int> RenderedSurfaceBoundsOf(synth_juce::PortableComponent& component,
                                             const std::string& id)
{
    juce::Component* child = component.FindByNodeId(id);
    Require(child != nullptr, "property fixture node is rendered");
    return SurfaceBoundsOf(component, *child);
}

// ---------------------------------------------------------------------------
// sru-52: pointer gestures over Draw and Button nodes.
// ---------------------------------------------------------------------------

// A rendered surface and the recorder behind it. `PortableComponent` holds a
// reference to its surface, so the two have to live and die together.
class GestureFixture
{
public:
    explicit GestureFixture(synth::ui::NodeTree tree)
        : recorder_(std::make_unique<RecordingSurface>())
    {
        recorder_->tree = std::move(tree);
        component_ = std::make_unique<synth_juce::PortableComponent>(*recorder_);
        component_->setSize(200, 200);
        // `getComponentAt` hit-tests only visible components, and hit testing is
        // what decides which node a click over an inert overlay reaches.
        component_->setVisible(true);
        component_->RefreshFromSurface();
    }

    synth_juce::PortableComponent& Component() noexcept { return *component_; }
    std::vector<synth::ui::Action>& Dispatched() noexcept { return recorder_->actions; }

private:
    std::unique_ptr<RecordingSurface> recorder_;
    std::unique_ptr<synth_juce::PortableComponent> component_;
};

std::vector<std::string> DispatchedNames(const std::vector<synth::ui::Action>& actions)
{
    std::vector<std::string> names;
    names.reserve(actions.size());
    for (const synth::ui::Action& action : actions)
    {
        names.push_back(action.name);
    }
    return names;
}

juce::MouseEvent PointerEvent(juce::Component* target,
                              juce::Point<float> position,
                              juce::Point<float> downPosition,
                              int numberOfClicks)
{
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(),
                            position,
                            juce::ModifierKeys::leftButtonModifier,
                            1.0f,
                            1.0f,
                            1.0f,
                            1.0f,
                            1.0f,
                            target,
                            target,
                            juce::Time::getCurrentTime(),
                            downPosition,
                            juce::Time::getCurrentTime(),
                            numberOfClicks,
                            false);
}

// The component a pointer press over the centre of `id` actually reaches. It is
// not always that node's own component: a node that intercepts nothing lets the
// press fall through to whatever is behind it (sru-25).
juce::Component& GestureTargetOver(synth_juce::PortableComponent& surface, const std::string& id)
{
    juce::Component* named = surface.FindByNodeId(id);
    Require(named != nullptr, "gesture fixture node is rendered");
    const juce::Rectangle<int> onSurface = SurfaceBoundsOf(surface, *named);
    // A node resolved to zero extent could never be clicked, which would make
    // every gesture assertion below vacuously true.
    Require(!onSurface.isEmpty(), "gesture fixture node has a clickable extent");
    juce::Component* target = surface.getComponentAt(onSurface.getCentre());
    Require(target != nullptr, "a pointer press over the surface reaches some component");
    return *target;
}

std::vector<std::string> SimulateClick(GestureFixture& fixture, const std::string& id)
{
    fixture.Dispatched().clear();
    juce::Component& target = GestureTargetOver(fixture.Component(), id);
    const juce::Point<float> at = target.getLocalBounds().getCentre().toFloat();
    target.mouseDown(PointerEvent(&target, at, at, 1));
    target.mouseUp(PointerEvent(&target, at, at, 1));
    return DispatchedNames(fixture.Dispatched());
}

// JUCE delivers `mouseDoubleClick` from `Component::internalMouseUp` *after* the
// second press's `mouseUp` (juce_Component.cpp:2248-2265), never in place of it.
// So a double click is two full down/up pairs and then the double-click
// callback, and that is the order this helper replays.
std::vector<std::string> SimulateDoubleClick(GestureFixture& fixture, const std::string& id)
{
    fixture.Dispatched().clear();
    juce::Component& target = GestureTargetOver(fixture.Component(), id);
    const juce::Point<float> at = target.getLocalBounds().getCentre().toFloat();
    target.mouseDown(PointerEvent(&target, at, at, 1));
    target.mouseUp(PointerEvent(&target, at, at, 1));
    target.mouseDown(PointerEvent(&target, at, at, 2));
    target.mouseUp(PointerEvent(&target, at, at, 2));
    target.mouseDoubleClick(PointerEvent(&target, at, at, 2));
    return DispatchedNames(fixture.Dispatched());
}

// 30 px right and 6 px up: `(30 - -6) * kPointerDragSensitivity` is 0.09, well
// past `kPointerDragThreshold`.
std::vector<std::string> SimulateDragPastThreshold(GestureFixture& fixture, const std::string& id)
{
    fixture.Dispatched().clear();
    juce::Component& target = GestureTargetOver(fixture.Component(), id);
    const juce::Point<float> down(10.0f, 10.0f);
    const juce::Point<float> moved(40.0f, 4.0f);
    target.mouseDown(PointerEvent(&target, down, down, 1));
    target.mouseDrag(PointerEvent(&target, moved, down, 1));
    target.mouseUp(PointerEvent(&target, moved, down, 1));
    return DispatchedNames(fixture.Dispatched());
}

// A press on the node released well off it, the gesture every toolkit treats as
// an abandoned click rather than a click.
std::vector<std::string> SimulateReleaseOutside(GestureFixture& fixture, const std::string& id)
{
    fixture.Dispatched().clear();
    juce::Component& target = GestureTargetOver(fixture.Component(), id);
    const juce::Point<float> down = target.getLocalBounds().getCentre().toFloat();
    const juce::Point<float> outside = target.getLocalBounds().getBottomRight().toFloat()
                                     + juce::Point<float>(40.0f, 40.0f);
    target.mouseDown(PointerEvent(&target, down, down, 1));
    target.mouseDrag(PointerEvent(&target, outside, down, 1));
    target.mouseUp(PointerEvent(&target, outside, down, 1));
    return DispatchedNames(fixture.Dispatched());
}

constexpr synth::ui::Bounds kGestureBounds{0.0f, 0.0f, 100.0f, 100.0f};
constexpr synth::ui::Bounds kGestureRootBounds{0.0f, 0.0f, 200.0f, 200.0f};

// One 100x100 canvas under a 200x200 root. Built through the component library
// rather than by hand, so these tests also cover a producer attaching the click
// action when the node is constructed — sru-52's library clause.
synth::ui::NodeTree CanvasTree(synth::ui::ControlStyle style)
{
    synth::ui::Builder builder;
    builder.Root("root", kGestureRootBounds)
        .Draw("canvas", kGestureBounds, std::vector<synth::ui::DrawCommand>{}, std::move(style));
    return builder.Build(kGestureRootBounds);
}

// The Button counterpart of `CanvasTree`, for the parity assertions.
synth::ui::NodeTree ButtonTree(synth::ui::Action action, synth::ui::ControlStyle style)
{
    style.layout.explicitBounds = kGestureBounds;
    synth::ui::Builder builder;
    builder.Root("root", kGestureRootBounds)
        .Button("btn", "Btn", std::move(action), std::move(style));
    return builder.Build(kGestureRootBounds);
}

void TestDrawClickOnlyDispatchesOnce()
{
    GestureFixture fixture(CanvasTree({.action = synth::ui::Action::Named("canvas.click")}));
    Require(SimulateClick(fixture, "canvas") == std::vector<std::string>{"canvas.click"},
            "a click-only Draw node dispatches exactly once on a single click");
}

void TestClickSequenceMatchesButtonExactly()
{
    GestureFixture drawFixture(CanvasTree({.action = synth::ui::Action::Named("click")}));
    GestureFixture buttonFixture(ButtonTree(synth::ui::Action::Named("click"), {}));

    const std::vector<std::string> fromDraw = SimulateClick(drawFixture, "canvas");
    const std::vector<std::string> fromButton = SimulateClick(buttonFixture, "btn");
    Require(fromDraw == fromButton,
            "a Draw node's single-click sequence is identical to a Button's, "
            "in both order and per-action count");
    Require(fromDraw == std::vector<std::string>{"click"},
            "the exact single-click sequence is one click");
}

// The exact ordered list pins both halves of sru-52's drag clause at once: the
// pointer-drag action is dispatched, and no click action is. Deliberately not
// compared against a Button — design.md D10b's parity clause covers click and
// double-click only, because a JUCE Button has no pointer-drag path and no
// producer gives one a drag action.
void TestDragDispatchesNoClick()
{
    GestureFixture fixture(CanvasTree({.action = synth::ui::Action::Named("canvas.click"),
                                       .pointerDragAction = synth::ui::Action::Named("canvas.drag")}));
    Require(SimulateDragPastThreshold(fixture, "canvas")
                == std::vector<std::string>{"canvas.drag"},
            "a drag past the threshold dispatches the drag action and no click");
}

// The drag consumes only its own gesture. Every other case here is one gesture on
// a fresh component, which would still pass if the per-press reset were dropped.
void TestClickAfterADragOnTheSameNodeStillDispatches()
{
    GestureFixture fixture(CanvasTree({.action = synth::ui::Action::Named("canvas.click"),
                                       .pointerDragAction = synth::ui::Action::Named("canvas.drag")}));
    Require(SimulateDragPastThreshold(fixture, "canvas")
                == std::vector<std::string>{"canvas.drag"},
            "the first gesture on the shared fixture is a drag");
    Require(SimulateClick(fixture, "canvas") == std::vector<std::string>{"canvas.click"},
            "a click after a drag on the same node still dispatches");
}

void TestDisabledDrawDispatchesNothing()
{
    GestureFixture fixture(
        CanvasTree({.enabled = false, .action = synth::ui::Action::Named("canvas.click")}));
    juce::Component* canvas = fixture.Component().FindByNodeId("canvas");
    Require(canvas != nullptr, "the disabled canvas is rendered");
    // Disabled is a dispatch rule, not an interception one: the node still takes
    // the press. Without this, an empty action list would also be the result of
    // wrongly clearing interception and letting the press land on a parent that
    // dispatches nothing anyway — a different bug wearing the same test result.
    bool interceptsItself = false;
    bool interceptsChildren = true;
    canvas->getInterceptsMouseClicks(interceptsItself, interceptsChildren);
    Require(interceptsItself && !interceptsChildren,
            "a disabled Draw node carrying a click action still intercepts pointer input");
    Require(&GestureTargetOver(fixture.Component(), "canvas") == canvas,
            "a press over a disabled Draw node still lands on that node");
    Require(SimulateClick(fixture, "canvas").empty(), "a disabled Draw node dispatches nothing");
}

void TestInertDrawInterceptsNothing()
{
    // sru-25: translucent visualizer underlays must keep passing clicks through
    // to the encoders beneath them.
    synth::ui::Builder builder;
    builder.Root("root", kGestureRootBounds)
        .Draw("encoder",
              kGestureBounds,
              std::vector<synth::ui::DrawCommand>{},
              {.action = synth::ui::Action::Named("encoder.click")})
        .Draw("underlay", kGestureBounds, std::vector<synth::ui::DrawCommand>{}, {});
    GestureFixture fixture(builder.Build(kGestureRootBounds));

    juce::Component* underlay = fixture.Component().FindByNodeId("underlay");
    Require(underlay != nullptr, "the inert underlay is rendered");
    Require(underlay->isVisible(), "the inert underlay is rendered visible, not hidden");
    bool interceptsItself = true;
    bool interceptsChildren = true;
    underlay->getInterceptsMouseClicks(interceptsItself, interceptsChildren);
    Require(!interceptsItself && !interceptsChildren,
            "an inert Draw node intercepts no pointer input");
    Require(SimulateClick(fixture, "underlay") == std::vector<std::string>{"encoder.click"},
            "a click over an inert Draw node reaches the interactive node behind it");
}

void TestReleaseOutsideTheNodeIsNoClick()
{
    GestureFixture drawFixture(CanvasTree({.action = synth::ui::Action::Named("click")}));
    GestureFixture buttonFixture(ButtonTree(synth::ui::Action::Named("click"), {}));
    Require(SimulateReleaseOutside(drawFixture, "canvas").empty()
                && SimulateReleaseOutside(buttonFixture, "btn").empty(),
            "a press released off the node is no click, in a Draw node exactly as in a Button");
}

void TestDoubleClickSequenceMatchesButtonExactly()
{
    GestureFixture drawFixture(CanvasTree({.action = synth::ui::Action::Named("click"),
                                           .doubleClickAction = synth::ui::Action::Named("dbl")}));
    GestureFixture buttonFixture(
        ButtonTree(synth::ui::Action::Named("click"),
                   {.doubleClickAction = synth::ui::Action::Named("dbl")}));

    const std::vector<std::string> fromDraw = SimulateDoubleClick(drawFixture, "canvas");
    const std::vector<std::string> fromButton = SimulateDoubleClick(buttonFixture, "btn");
    Require(fromDraw == fromButton,
            "a Draw node's double-click sequence is identical to a Button's, "
            "in both order and per-action count");
    // Pinned as a literal, read off the observed sequence: JUCE derives each
    // click from a `mouseUp`, and both presses of a double click deliver one,
    // so the double-click action lands third and last.
    Require(fromDraw == std::vector<std::string>{"click", "click", "dbl"},
            "the exact double-click sequence is click, click, double-click");
}

void TestGeometryAndColourHelpers()
{

Require(synth_juce::UiToJuceRect(synth::ui::Bounds{1.0f, 2.0f, 3.0f, 4.0f}) == juce::Rectangle<int>(1, 2, 3, 4),
        "UiToJuceRect rounds bounds");
Require(synth_juce::UiToJuceColour(synth::Color::Rgb(10, 20, 30)) == juce::Colour(10, 20, 30),
        "UiToJuceColour maps RGB");
Require(!synth_juce::HasExplicitBounds(synth::ui::Bounds{}), "zero bounds are not explicit");
Require(synth_juce::HasExplicitBounds(synth::ui::Bounds{0.0f, 0.0f, 10.0f, 10.0f}),
        "positive bounds are explicit");
}

void TestRenderedPositionFoldsAncestorOriginsAndScrollOffset()
{
    RecordingSurface propertySurface;
    propertySurface.tree = BackendGeometryPropertyTree();
    synth_juce::PortableComponent component(propertySurface);
    component.setSize(500, 260);
    component.RefreshFromSurface();
    auto* scroll = component.FindByNodeId("scroll");
    Require(scroll != nullptr, "property scroll area is rendered");
    auto* viewport = dynamic_cast<juce::Viewport*>(scroll->getChildComponent(0));
    Require(viewport != nullptr, "property scroll area owns a viewport");
    viewport->setViewPosition(13, 19);

    const std::unordered_map<std::string, juce::Point<int>> scrollOffsets{
        {"scroll", {13, 19}}};
    for (const synth::ui::Node& node : propertySurface.tree.nodes)
    {
        const juce::Point<float> expected =
            FoldAncestorOrigins(propertySurface.tree, node.id.value, scrollOffsets);
        const juce::Rectangle<int> actual =
            RenderedSurfaceBoundsOf(component, node.id.value);
        Require(NearlyEqual(static_cast<float>(actual.getX()), expected.x)
                    && NearlyEqual(static_cast<float>(actual.getY()), expected.y)
                    && actual.getWidth() == static_cast<int>(std::lround(node.bounds.width))
                    && actual.getHeight() == static_cast<int>(std::lround(node.bounds.height)),
                ("node '" + node.id.value +
                 "' renders exactly at the fold of its ancestor origins with its own wire extent")
                    .c_str());
    }
}

void TestOverhangingChildBoundsFoldWithoutReclassification()
{
    RecordingSurface overhangingSurface;
    overhangingSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 400.0f, 300.0f},
         .children = {synth::ui::NodeId("parent")}},
        {.id = synth::ui::NodeId("parent"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {50.0f, 40.0f, 100.0f, 50.0f},
         .children = {synth::ui::NodeId("child")}},
        {.id = synth::ui::NodeId("child"),
         .kind = synth::ui::NodeKind::Label,
         .bounds = {10.0f, 10.0f, 200.0f, 20.0f},
         .text = "overhang"},
    };
    synth_juce::PortableComponent component(overhangingSurface);
    component.setSize(400, 300);
    component.RefreshFromSurface();
    juce::Component* child = component.FindByNodeId("child");
    Require(child != nullptr, "overhanging child is rendered");
    const juce::Rectangle<int> childBounds = SurfaceBoundsOf(component, *child);
    Require(NearlyEqual(static_cast<float>(childBounds.getX()), 60.0f),
            "surface x is the parent's origin plus the child's own bounds, with no reclassification");
    Require(NearlyEqual(static_cast<float>(childBounds.getY()), 50.0f),
            "and surface y is likewise the simple fold");
}

void TestNodeWithoutResolvedBoundsIsNotRescued()
{
    RecordingSurface zeroBoundsSurface;
    zeroBoundsSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 400.0f, 300.0f},
         .children = {synth::ui::NodeId("parent")}},
        {.id = synth::ui::NodeId("parent"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {50.0f, 40.0f, 200.0f, 100.0f},
         .children = {synth::ui::NodeId("orphan")}},
        {.id = synth::ui::NodeId("orphan"),
         .kind = synth::ui::NodeKind::Label,
         .bounds = {0.0f, 0.0f, 0.0f, 0.0f},
         .text = "unresolved"},
    };
    synth_juce::PortableComponent component(zeroBoundsSurface);
    component.setSize(400, 300);
    component.RefreshFromSurface();
    juce::Component* orphan = component.FindByNodeId("orphan");
    Require(orphan != nullptr, "zero-bounds child is rendered");
    const juce::Rectangle<int> b = SurfaceBoundsOf(component, *orphan);
    Require(NearlyEqual(static_cast<float>(b.getX()), 50.0f)
                && NearlyEqual(static_cast<float>(b.getY()), 40.0f),
            "a node without resolved bounds renders at its parent's origin");
    Require(b.getWidth() == 0 && b.getHeight() == 0,
            "with zero-based extent, never flowed or sized by the backend");
}

void TestCarriedColourDecidesTheButtonFill()
{
    RecordingSurface colourSurface;
    colourSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 400.0f, 300.0f},
         .children = {synth::ui::NodeId("styled")}},
        {.id = synth::ui::NodeId("styled"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {0.0f, 0.0f, 80.0f, 24.0f},
         .label = "Styled",
         .color = synth::Color::Rgb(0, 200, 0)},
    };
    synth_juce::PortableComponent component(colourSurface);
    component.setSize(400, 300);
    component.RefreshFromSurface();
    Require(FillColourOf(component, "styled") == juce::Colour::fromRGB(0, 200, 0),
            "the carried colour decides the button fill, and no per-variant "
            "colour table remains to compete with it");
}

void TestCarriedTextStyleDecidesGlyphColour()
{
    RecordingSurface textStyleSurface;
    textStyleSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 400.0f, 300.0f},
         .children = {synth::ui::NodeId("lbl")}},
        {.id = synth::ui::NodeId("lbl"),
         .kind = synth::ui::NodeKind::Label,
         .bounds = {0.0f, 0.0f, 120.0f, 20.0f},
         .text = "Label",
         .color = synth::Color::Rgb(10, 10, 10),
         .textStyle = synth::ui::TextStyle{14.0f,
                                           synth::Color::Rgb(240, 240, 240),
                                           synth::ui::TextAlign::Left}},
    };
    synth_juce::PortableComponent component(textStyleSurface);
    component.setSize(400, 300);
    component.RefreshFromSurface();
    Require(TextColourOf(component, "lbl") == juce::Colour::fromRGB(240, 240, 240),
            "a label's glyphs take their colour from textStyle, never from node colour");
}

void TestSelectedPresentationDerivesFromTheCarriedColour()
{
    RecordingSurface selectedSurface;
    selectedSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 400.0f, 300.0f},
         .children = {synth::ui::NodeId("plain"), synth::ui::NodeId("sel")}},
        {.id = synth::ui::NodeId("plain"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {0.0f, 0.0f, 80.0f, 24.0f},
         .label = "Plain",
         .color = synth::Color::Rgb(0, 120, 0)},
        {.id = synth::ui::NodeId("sel"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {0.0f, 32.0f, 80.0f, 24.0f},
         .label = "Selected",
         .selected = true,
         .color = synth::Color::Rgb(0, 120, 0)},
    };
    synth_juce::PortableComponent component(selectedSurface);
    component.setSize(400, 300);
    component.RefreshFromSurface();
    Require(FillColourOf(component, "sel") != FillColourOf(component, "plain"),
            "selected presentation differs from unselected");
    Require(IsDerivedFrom(FillColourOf(component, "sel"), juce::Colour::fromRGB(0, 120, 0)),
            "and is derived from the carried colour rather than substituted from a palette");
}

void TestDisabledAndContainerPresentationDeriveFromTheCarriedColour()
{
    RecordingSurface stateColourSurface;
    stateColourSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 200.0f, 140.0f},
         .children = {synth::ui::NodeId("section"),
                      synth::ui::NodeId("disabled")}},
        {.id = synth::ui::NodeId("section"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {8.0f, 8.0f, 80.0f, 40.0f},
         .color = synth::Color::Rgb(80, 40, 120)},
        {.id = synth::ui::NodeId("disabled"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {8.0f, 60.0f, 96.0f, 28.0f},
         .label = "Disabled",
         .enabled = false,
         .color = synth::Color::Rgb(40, 80, 120)},
    };
    synth_juce::PortableComponent component(stateColourSurface);
    component.setSize(200, 140);
    component.RefreshFromSurface();

    const juce::Image image = RenderComponent(component);
    Require(image.getPixelAt(48, 28) == juce::Colour::fromRGB(80, 40, 120),
            "a container's background fill is the carried colour");
    Require(FillColourOf(component, "disabled")
                == juce::Colour::fromRGB(40, 80, 120).darker(0.35f),
            "disabled presentation is derived from the carried colour once");
    Require(NearlyEqual(component.FindByNodeId("disabled")->getAlpha(), 0.58f),
            "disabled component alpha carries the dimming opacity");
}

void TestControlsRegisterRetainAndDispatchAcrossRefresh()
{
RecordingSurface surface;
synth::ui::Builder builder;
builder.Root("root", synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f})
    .Button("start", "Start", synth::ui::Action::Named("start"), {})
    .ComboBox("device",
              {{"a", "Built In"}, {"b", "External"}},
              "a",
              synth::ui::Action::Named("device.select"), {})
    .Draw("scope",
          synth::ui::Bounds{8.0f, 8.0f, 64.0f, 48.0f},
          {synth::ui::DrawCommand::Fill(synth::Color::Rgb(1, 2, 3)),
           synth::ui::DrawCommand::Line({0.0f, 0.0f}, {64.0f, 48.0f}, synth::Color::Rgb(4, 5, 6), 1.0f)}, {});
surface.tree = builder.Build(synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f});

synth_juce::PortableComponent component(surface);
component.setSize(320, 240);
component.RefreshFromSurface();
Require(surface.dispatchCount == 0, "refresh does not dispatch actions");
Require(component.FindByNodeId("start") != nullptr, "button control is registered by node id");
Require(component.FindByNodeId("device") != nullptr, "combo control is registered by node id");
Require(component.FindByNodeId("missing") == nullptr, "missing node id returns null");

RecordingSurface statusSurface;
statusSurface.tree.nodes = {
    {.id = synth::ui::NodeId("status.root"),
     .kind = synth::ui::NodeKind::Root,
     .bounds = {0.0f, 0.0f, 160.0f, 48.0f},
     .children = {synth::ui::NodeId("status.marker")}},
    {.id = synth::ui::NodeId("status.marker"),
     .kind = synth::ui::NodeKind::StatusText,
     .bounds = {0.0f, 0.0f, 160.0f, 48.0f},
     .text = "!"},
};
synth_juce::PortableComponent statusComponent(statusSurface);
statusComponent.setSize(160, 48);
statusComponent.RefreshFromSurface();
auto* marker = statusComponent.FindByNodeId("status.marker");
Require(marker != nullptr, "status marker is retained");
bool markerIntercepts = true;
bool markerChildrenIntercept = true;
marker->getInterceptsMouseClicks(markerIntercepts, markerChildrenIntercept);
Require(!markerIntercepts && !markerChildrenIntercept,
        "status text is pointer-transparent and cannot block an overlapping button");

auto* button = dynamic_cast<juce::TextButton*>(component.FindByNodeId("start"));
Require(button != nullptr, "start node is a TextButton");
juce::Component* retainedButtonComponent = button;
button->onClick();
Require(surface.dispatchCount == 1, "button click dispatches through surface");
Require(surface.lastAction.name == "start", "dispatched action name");

{
    synth::ui::Builder changedBuilder;
    changedBuilder.Root("root", synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f})
        .Button("start", "Launch", synth::ui::Action::Named("start.changed"), {})
        .ComboBox("device",
                  {{"a2", "Built In 2"}, {"b2", "External 2"}},
                  "a2",
                  synth::ui::Action::Named("device.select.changed"), {})
        .Draw("scope",
              synth::ui::Bounds{8.0f, 8.0f, 64.0f, 48.0f},
              {synth::ui::DrawCommand::Fill(synth::Color::Rgb(7, 8, 9))}, {});
    surface.tree = changedBuilder.Build(synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f});
}
component.RefreshFromSurface();
Require(surface.dispatchCount == 1, "refresh after mutation does not dispatch actions");
Require(component.FindByNodeId("start") == retainedButtonComponent, "button component is retained by stable id");
button = dynamic_cast<juce::TextButton*>(component.FindByNodeId("start"));
Require(button != nullptr, "retained start node is still a TextButton");
button->onClick();
Require(surface.dispatchCount == 2, "retained button dispatches after refresh");
Require(surface.lastAction.name == "start.changed", "retained button dispatches current action");

auto* combo = dynamic_cast<juce::ComboBox*>(component.FindByNodeId("device"));
Require(combo != nullptr, "device node is a ComboBox");
Require(combo->getItemText(1) == juce::String("External 2"), "combo same-count label refreshes");
combo->setSelectedId(2, juce::dontSendNotification);
combo->onChange();
Require(surface.dispatchCount == 3, "combo dispatches current action");
Require(surface.lastAction.name == "device.select.changed", "combo dispatches refreshed action name");
Require(surface.lastAction.value == "b2", "combo dispatches refreshed option id");

{
    RecordingSurface valueSurface;
    synth::ui::Builder valueBuilder;
    valueBuilder.Root("value.root", synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f})
        .ComboBox("value.combo",
                  {{"a2", "Built In 2"}, {"b2", "External 2"}},
                  "a2",
                  synth::ui::Action::WithValue("controller.input", "controller:input"), {})
        .TextField("value.text",
                   "Mapping",
                   "63",
                   synth::ui::Action::WithValue("controller.mapping", "controller:mapping:0"), {})
        .Toggle("value.toggle",
                "Enabled",
                false,
                synth::ui::Action::WithValue("controller.mapping", "controller:mapping:1"), {})
        .Slider("value.slider",
                "Depth",
                0.0f,
                0.0f,
                1.0f,
                0.01f,
                synth::ui::Action::WithValue("controller.depth", "controller:depth"), {});
    valueSurface.tree = valueBuilder.Build(synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f});

    synth_juce::PortableComponent valueComponent(valueSurface);
    valueComponent.setSize(320, 240);
    valueComponent.RefreshFromSurface();

    auto* valueCombo = dynamic_cast<juce::ComboBox*>(valueComponent.FindByNodeId("value.combo"));
    Require(valueCombo != nullptr, "prefixed combo node is a ComboBox");
    valueCombo->setSelectedId(2, juce::sendNotificationSync);
    Require(valueSurface.lastAction.value == "controller:input:b2",
            "combo appends the selected option id to the current action prefix");

    auto* valueText = dynamic_cast<juce::TextEditor*>(valueComponent.FindByNodeId("value.text"));
    Require(valueText != nullptr, "prefixed text node is a TextEditor");
    valueText->setText("64", false);
    const int dispatchesBeforeTextCommit = valueSurface.dispatchCount;
    valueText->onReturnKey();
    Require(valueSurface.lastAction.value == "controller:mapping:0:64",
            "text field appends committed text to its action prefix");
    valueText->onFocusLost();
    Require(valueSurface.dispatchCount == dispatchesBeforeTextCommit + 1,
            "text field dispatches once when Return is followed by unchanged focus loss");

    auto* valueToggle = dynamic_cast<juce::ToggleButton*>(valueComponent.FindByNodeId("value.toggle"));
    Require(valueToggle != nullptr, "prefixed toggle node is a ToggleButton");
    valueToggle->setToggleState(true, juce::dontSendNotification);
    valueToggle->onClick();
    Require(valueSurface.lastAction.value == "controller:mapping:1:1",
            "toggle appends its checked state to its action prefix");

    auto* valueSlider = dynamic_cast<juce::Slider*>(valueComponent.FindByNodeId("value.slider"));
    Require(valueSlider != nullptr, "prefixed slider node is a Slider");
    valueSlider->setValue(0.75, juce::sendNotificationSync);
    Require(valueSurface.lastAction.value.starts_with("controller:depth:"),
            "slider appends its value to its action prefix");
}
}

void TestComposedSubtreeRootsFoldWithOneOffset()
{
    RecordingSurface compositeSurface;
    synth::ui::Builder compositeBuilder;
    compositeBuilder.Root("runtime.main.root", synth::ui::Bounds{0.0f, 0.0f, 996.0f, 240.0f})
        .Root("app.root", synth::ui::Bounds{0.0f, 0.0f, 900.0f, 240.0f})
        .Draw("app.draw",
              synth::ui::Bounds{0.0f, 0.0f, 900.0f, 40.0f},
              {synth::ui::DrawCommand::Fill(synth::Color::Rgb(1, 2, 3))}, {});
    for (int index = 0; index < 12; ++index)
    {
        compositeBuilder.Button("app.control." + std::to_string(index),
                                "App",
                                synth::ui::Action::Named("app.control"), {});
    }
    compositeBuilder.Root("runtime.sidebar.root", synth::ui::Bounds{900.0f, 0.0f, 96.0f, 240.0f})
        .Draw("runtime.sidebar.draw",
              synth::ui::Bounds{0.0f, 0.0f, 96.0f, 120.0f},
              {synth::ui::DrawCommand::Fill(synth::Color::Rgb(4, 5, 6))}, {})
        .Button("runtime.sidebar.control", "Side", synth::ui::Action::Named("runtime.sidebar"), {});
    compositeSurface.tree = compositeBuilder.Build(synth::ui::Bounds{900.0f, 0.0f, 96.0f, 240.0f});
    compositeSurface.tree.nodes.front().children = {
        synth::ui::NodeId("app.root"), synth::ui::NodeId("runtime.sidebar.root")};

    synth_juce::PortableComponent compositeComponent(compositeSurface);
    compositeComponent.setSize(996, 240);
    compositeComponent.RefreshFromSurface();

    const juce::Component* firstAppControl = compositeComponent.FindByNodeId("app.control.0");
    const juce::Component* wrappedAppControl = compositeComponent.FindByNodeId("app.control.11");
    const juce::Component* sidebarControl = compositeComponent.FindByNodeId("runtime.sidebar.control");
    Require(firstAppControl != nullptr && wrappedAppControl != nullptr && sidebarControl != nullptr,
            "composite controls are hosted");
    Require(SurfaceBoundsOf(compositeComponent, *firstAppControl)
                == juce::Rectangle<int>(0, 0, 0, 0),
            "unbounded app controls are not rescued by backend flow");
    Require(SurfaceBoundsOf(compositeComponent, *wrappedAppControl)
                == juce::Rectangle<int>(0, 0, 0, 0),
            "unbounded app controls are not wrapped by a backend cursor");
    Require(SurfaceBoundsOf(compositeComponent, *sidebarControl)
                == juce::Rectangle<int>(900, 0, 96, 28),
            "producer-resolved sidebar control folds through its subtree root with one sidebar offset");
}

void TestRetainedControlsFollowSemanticReparenting()
{
    RecordingSurface hierarchySurface;
    hierarchySurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 996.0f, 300.0f},
         .children = {synth::ui::NodeId("section"), synth::ui::NodeId("sidebar.root")}},
        {.id = synth::ui::NodeId("section"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {20.0f, 20.0f, 840.0f, 220.0f},
         .children = {synth::ui::NodeId("row.a"), synth::ui::NodeId("row.b")}},
        {.id = synth::ui::NodeId("row.a"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {10.0f, 10.0f, 800.0f, 60.0f},
         .children = {synth::ui::NodeId("field.a"),
                      synth::ui::NodeId("flow"),
                      synth::ui::NodeId("draw"),
                      synth::ui::NodeId("paint")}},
        {.id = synth::ui::NodeId("row.b"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {10.0f, 90.0f, 800.0f, 60.0f},
         .children = {synth::ui::NodeId("field.b")}},
        {.id = synth::ui::NodeId("field.a"),
         .kind = synth::ui::NodeKind::TextField,
         .bounds = {8.0f, 8.0f, 120.0f, 28.0f},
         .label = "A",
         .text = "saved",
         .action = synth::ui::Action::Named("field.a")},
        {.id = synth::ui::NodeId("field.b"),
         .kind = synth::ui::NodeKind::TextField,
         .bounds = {8.0f, 8.0f, 120.0f, 28.0f},
         .label = "B",
         .text = "other",
         .action = synth::ui::Action::Named("field.b")},
        {.id = synth::ui::NodeId("flow"),
         .kind = synth::ui::NodeKind::Button,
         .label = "Flow",
         .action = synth::ui::Action::Named("flow")},
        {.id = synth::ui::NodeId("draw"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {300.0f, 8.0f, 80.0f, 40.0f},
         .pointerDragAction = synth::ui::Action::Named("draw.drag"),
         .drawCommands = {synth::ui::DrawCommand::Fill(synth::Color::Rgb(10, 20, 30))}},
        {.id = synth::ui::NodeId("paint"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {400.0f, 8.0f, 80.0f, 40.0f},
         .drawCommands = {synth::ui::DrawCommand::Fill(synth::Color::Rgb(30, 20, 10))}},
        {.id = synth::ui::NodeId("sidebar.root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {900.0f, 0.0f, 96.0f, 240.0f},
         .children = {synth::ui::NodeId("sidebar.button")}},
        {.id = synth::ui::NodeId("sidebar.button"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {12.0f, 128.0f, 72.0f, 28.0f},
         .label = "Side",
         .action = synth::ui::Action::Named("sidebar")},
    };

    synth_juce::PortableComponent hierarchyComponent(hierarchySurface);
    hierarchyComponent.setSize(996, 300);
    hierarchyComponent.addToDesktop(0);
    hierarchyComponent.setVisible(true);
    hierarchyComponent.RefreshFromSurface();

    auto* section = hierarchyComponent.FindByNodeId("section");
    auto* rowA = hierarchyComponent.FindByNodeId("row.a");
    auto* rowB = hierarchyComponent.FindByNodeId("row.b");
    auto* fieldA = dynamic_cast<juce::TextEditor*>(hierarchyComponent.FindByNodeId("field.a"));
    auto* fieldB = dynamic_cast<juce::TextEditor*>(hierarchyComponent.FindByNodeId("field.b"));
    auto* flowButton = hierarchyComponent.FindByNodeId("flow");
    auto* draw = hierarchyComponent.FindByNodeId("draw");
    auto* paint = hierarchyComponent.FindByNodeId("paint");
    auto* sidebarButton = hierarchyComponent.FindByNodeId("sidebar.button");
    Require(section != nullptr && rowA != nullptr && rowB != nullptr && fieldA != nullptr
                && fieldB != nullptr && flowButton != nullptr && draw != nullptr && paint != nullptr
                && sidebarButton != nullptr,
            "hierarchy fixture nodes are all retained");
    Require(rowA->getParentComponent() == section, "row A is hosted by its semantic section");
    Require(fieldA->getParentComponent() == rowA, "field A is hosted by its semantic row");
    Require(SurfaceBoundsOf(hierarchyComponent, *fieldA).getY()
                != SurfaceBoundsOf(hierarchyComponent, *fieldB).getY(),
            "parent-local row fields occupy distinct surface rows");
    Require(SurfaceBoundsOf(hierarchyComponent, *sidebarButton).getX() == 912,
            "absolute sidebar offset is applied exactly once");
    Require(SurfaceBoundsOf(hierarchyComponent, *flowButton)
                == juce::Rectangle<int>(30, 30, 0, 0),
            "zero-bounds child renders at its parent origin with zero extent");
    Require(draw->getParentComponent() == rowA, "draw node is hosted by its semantic row");
    Require(paint->getParentComponent() == rowA, "non-interactive draw node is hosted by its semantic row");

    juce::Component* retainedFieldA = fieldA;
    fieldA->grabKeyboardFocus();
    fieldA->setText("draft", false);
    hierarchyComponent.RefreshFromSurface();
    fieldA = dynamic_cast<juce::TextEditor*>(hierarchyComponent.FindByNodeId("field.a"));
    Require(fieldA == retainedFieldA, "focused text field is retained with the same parent");
    Require(fieldA->hasKeyboardFocus(true), "focused text field keeps focus with the same parent");
    Require(fieldA->getText() == juce::String("draft"), "focused text field keeps its draft with the same parent");

    hierarchySurface.tree.nodes[2].children.erase(hierarchySurface.tree.nodes[2].children.begin());
    hierarchySurface.tree.nodes[3].children.push_back(synth::ui::NodeId("field.a"));
    hierarchyComponent.RefreshFromSurface();
    fieldA = dynamic_cast<juce::TextEditor*>(hierarchyComponent.FindByNodeId("field.a"));
    rowB = hierarchyComponent.FindByNodeId("row.b");
    Require(fieldA == retainedFieldA, "focused text field is retained after moving semantic parents");
    Require(fieldA->getParentComponent() == rowB, "moved text field is hosted by its new semantic row");
    Require(fieldA->hasKeyboardFocus(true), "focused text field keeps focus after moving semantic parents");
    Require(fieldA->getText() == juce::String("draft"), "focused text field keeps its draft after moving semantic parents");
}

void TestScrollPositionSurvivesRefreshAndClampsOnShrink()
{
    RecordingSurface scrollSurface;
    scrollSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 640.0f, 480.0f},
         .children = {synth::ui::NodeId("scroll")}},
        {.id = synth::ui::NodeId("scroll"),
         .kind = synth::ui::NodeKind::ScrollArea,
         .bounds = {20.0f, 30.0f, 180.0f, 90.0f},
         .scrollContentWidth = 420.0f,
         .scrollContentHeight = 360.0f,
         .children = {synth::ui::NodeId("row.a"),
                      synth::ui::NodeId("row.b"),
                      synth::ui::NodeId("final")}},
        {.id = synth::ui::NodeId("row.a"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {0.0f, 0.0f, 420.0f, 60.0f}},
        {.id = synth::ui::NodeId("row.b"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {0.0f, 80.0f, 420.0f, 60.0f}},
        {.id = synth::ui::NodeId("final"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {330.0f, 300.0f, 72.0f, 28.0f},
         .label = "Final",
         .action = synth::ui::Action::Named("final")},
    };

    synth_juce::PortableComponent component(scrollSurface);
    component.setSize(640, 480);
    component.RefreshFromSurface();

    auto* scroll = component.FindByNodeId("scroll");
    auto* rowA = component.FindByNodeId("row.a");
    auto* rowB = component.FindByNodeId("row.b");
    auto* finalButton = component.FindByNodeId("final");
    Require(scroll != nullptr && rowA != nullptr && rowB != nullptr && finalButton != nullptr,
            "scroll fixture nodes are all retained");
    auto* viewport = dynamic_cast<juce::Viewport*>(scroll->getChildComponent(0));
    Require(viewport != nullptr, "scroll area owns a JUCE viewport");
    Require(rowA->getParentComponent() == viewport->getViewedComponent()
                && rowB->getParentComponent() == viewport->getViewedComponent(),
            "scroll rows are hosted by the viewed content component");
    Require(SurfaceBoundsOf(component, *scroll) == juce::Rectangle<int>(20, 30, 180, 90),
            "viewport keeps declared visible bounds");
    Require(viewport->getViewedComponent()->getWidth() == 420
                && viewport->getViewedComponent()->getHeight() == 360,
            "scroll content uses declared two-axis extent");
    Require(!SurfaceBoundsOf(component, *viewport).intersects(
                SurfaceBoundsOf(component, *finalButton)),
            "final button begins outside the visible viewport");
    viewport->setViewPosition(240, 270);
    Require(viewport->getViewPositionX() > 0 && viewport->getViewPositionY() > 0,
            "viewport scrolls on both axes");
    Require(SurfaceBoundsOf(component, *viewport).intersects(
                SurfaceBoundsOf(component, *finalButton)),
            "final button is reachable after scrolling");

    const int retainedViewX = viewport->getViewPositionX();
    const int retainedViewY = viewport->getViewPositionY();
    component.RefreshFromSurface();
    Require(viewport->getViewPositionX() == retainedViewX
                && viewport->getViewPositionY() == retainedViewY,
            "viewport position survives refresh for a stable scroll node");
    Require(SurfaceBoundsOf(component, *viewport).intersects(
                SurfaceBoundsOf(component, *finalButton)),
            "scrolled descendant remains reachable after refresh");

    scrollSurface.tree.nodes[1].scrollContentWidth = 180.0f;
    scrollSurface.tree.nodes[1].scrollContentHeight = 90.0f;
    component.RefreshFromSurface();
    Require(viewport->getViewPositionX() == 0 && viewport->getViewPositionY() == 0,
            "viewport position is clamped when content shrinks");
}

void TestDrawCommandsPaintNodeLocal()
{
    RecordingSurface drawCoordinateSurface;
    drawCoordinateSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 240.0f, 230.0f},
         .children = {synth::ui::NodeId("row")}},
        {.id = synth::ui::NodeId("row"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {10.0f, 10.0f, 220.0f, 210.0f},
         .children = {synth::ui::NodeId("overflow.draw"),
                      synth::ui::NodeId("local.draw"),
                      synth::ui::NodeId("local.line"),
                      synth::ui::NodeId("fractional.local")}},
        {.id = synth::ui::NodeId("overflow.draw"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {10.0f, 10.0f, 40.0f, 30.0f},
         .drawCommands = {synth::ui::DrawCommand::Fill(
             {20.0f, 20.0f, 40.0f, 30.0f}, synth::Color::Rgb(220, 40, 30))}},
        {.id = synth::ui::NodeId("local.draw"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {70.0f, 10.0f, 40.0f, 30.0f},
         .drawCommands = {
             synth::ui::DrawCommand::FillEllipse({}, synth::Color::Rgb(10, 20, 30)),
             synth::ui::DrawCommand::Fill(
                 {0.0f, 0.0f, 40.0f, 30.0f}, synth::Color::Rgb(30, 180, 70))}},
        {.id = synth::ui::NodeId("local.line"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {20.0f, 40.0f, 160.0f, 100.0f},
         .drawCommands = {
             synth::ui::DrawCommand::Line(
                 {35.0f, 65.0f},
                 {100.0f, 65.0f},
                 synth::Color::Rgb(230, 170, 30),
                 3.0f),
             synth::ui::DrawCommand::Line(
                 {35.0f, 75.0f},
                 {185.0f, 75.0f},
                 synth::Color::Rgb(40, 100, 230),
                 3.0f)}},
        {.id = synth::ui::NodeId("fractional.local"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {20.0f, 160.0f, 40.4f, 20.4f},
         .drawCommands = {synth::ui::DrawCommand::Fill(
             {0.0f, 0.0f, 40.25f, 20.25f}, synth::Color::Rgb(150, 70, 210))}},
    };

    synth_juce::PortableComponent drawCoordinateComponent(drawCoordinateSurface);
    drawCoordinateComponent.setSize(240, 230);
    drawCoordinateComponent.RefreshFromSurface();
    const juce::Image image = RenderComponent(drawCoordinateComponent);

    Require(image.getPixelAt(40, 40) == juce::Colour(220, 40, 30),
            "draw command bounds are node-local inside a nested hosted component");
    Require(image.getPixelAt(65, 45) == synth_juce::UiToJuceColour(synth::kSurfaceBackground),
            "draw command overflow is clipped to the draw node bounds");
    Require(image.getPixelAt(100, 35) == juce::Colour(30, 180, 70),
            "node-local draw commands paint once inside a nested hosted component");
    Require(image.getPixelAt(65, 125) == juce::Colour(40, 100, 230),
            "a node-local line is folded through the draw-node origin");
    Require(image.getPixelAt(65, 115) == juce::Colour(230, 170, 30),
            "every command in a draw-node buffer uses node-local geometry");
    Require(image.getPixelAt(30, 170) == juce::Colour(150, 70, 210),
            "fractional node dimensions preserve node-local draw geometry");
}

void TestScrollContentFloorsAtTheVisibleBounds()
{
    RecordingSurface visibleFloorSurface;
    visibleFloorSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 320.0f, 240.0f},
         .children = {synth::ui::NodeId("scroll")}},
        {.id = synth::ui::NodeId("scroll"),
         .kind = synth::ui::NodeKind::ScrollArea,
         .bounds = {20.0f, 30.0f, 180.0f, 90.0f},
         .scrollContentWidth = 40.0f,
         .scrollContentHeight = 30.0f},
    };

    synth_juce::PortableComponent component(visibleFloorSurface);
    component.setSize(320, 240);
    component.RefreshFromSurface();

    auto* scroll = component.FindByNodeId("scroll");
    Require(scroll != nullptr, "visible-floor scroll node is retained");
    auto* viewport = dynamic_cast<juce::Viewport*>(scroll->getChildComponent(0));
    Require(viewport != nullptr && viewport->getViewedComponent() != nullptr,
            "visible-floor scroll area owns viewed content");
    Require(viewport->getViewedComponent()->getWidth() == 180
                && viewport->getViewedComponent()->getHeight() == 90,
            "first refresh floors undersized declared content at visible bounds");

    scroll->setSize(240, 150);
    Require(viewport->getViewedComponent()->getWidth() == 240
                && viewport->getViewedComponent()->getHeight() == 150,
            "pure scroll host resize recomputes the visible content floor");
}

void TestInteractiveDrawDragDispatchesDeltas()
{
    RecordingSurface dragSurface;
    synth::ui::Builder dragBuilder;
    dragBuilder.Root("root", synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f})
        .DrawInteractive("encoder",
                         synth::ui::Bounds{16.0f, 24.0f, 80.0f, 80.0f},
                         {},
                         synth::ui::Action::WithValue("encoder.drag", "stale"),
                         std::nullopt,
                         {});
    dragSurface.tree = dragBuilder.Build(synth::ui::Bounds{0.0f, 0.0f, 320.0f, 240.0f});

    synth_juce::PortableComponent dragComponent(dragSurface);
    dragComponent.setSize(320, 240);
    dragComponent.RefreshFromSurface();

    auto* overlay = dragComponent.FindByNodeId("encoder");
    Require(overlay != nullptr, "interactive draw overlay is hosted");
    const juce::MouseInputSource mouseSource = juce::Desktop::getInstance().getMainMouseSource();
    const juce::Point<float> downPoint(20.0f, 20.0f);
    const juce::Point<float> dragPoint(30.0f, 10.0f);
    juce::MouseEvent downEvent(mouseSource,
                               downPoint,
                               juce::ModifierKeys::leftButtonModifier,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               overlay,
                               overlay,
                               juce::Time::getCurrentTime(),
                               downPoint,
                               juce::Time::getCurrentTime(),
                               1,
                               false);
    overlay->mouseDown(downEvent);
    juce::MouseEvent dragEvent(mouseSource,
                               dragPoint,
                               juce::ModifierKeys::leftButtonModifier,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               overlay,
                               overlay,
                               juce::Time::getCurrentTime(),
                               downPoint,
                               juce::Time::getCurrentTime(),
                               1,
                               false);
    overlay->mouseDrag(dragEvent);

    Require(dragSurface.dispatchCount == 1, "interactive draw drag dispatches action");
    Require(dragSurface.lastAction.name == "encoder.drag", "interactive draw drag action name");
    Require(dragSurface.lastAction.value == std::to_string(0.05f),
            "interactive draw drag replaces colon-free action value with delta");
}

void TestDisabledSemanticControlsRenderDisabledAndKeepTheirState()
{
    // sru-34: a disabled semantic control renders disabled, keeps its
    // current option/value/text, and never dispatches its user action.
    RecordingSurface disabledSurface;
    disabledSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 480.0f, 320.0f},
         .children = {synth::ui::NodeId("button"),
                      synth::ui::NodeId("combo"),
                      synth::ui::NodeId("field"),
                      synth::ui::NodeId("toggle"),
                      synth::ui::NodeId("slider"),
                      synth::ui::NodeId("draw")}},
        {.id = synth::ui::NodeId("button"),
         .kind = synth::ui::NodeKind::Button,
         .bounds = {8.0f, 8.0f, 96.0f, 28.0f},
         .label = "Submit",
         .enabled = false,
         .action = synth::ui::Action::Named("submit"),
         .doubleClickAction = synth::ui::Action::Named("submit.double")},
        {.id = synth::ui::NodeId("combo"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {8.0f, 44.0f, 160.0f, 28.0f},
         .label = "Message",
         .enabled = false,
         .options = {{"first", "First"}, {"second", "Second"}},
         .selectedOption = "second",
         .action = synth::ui::Action::WithValue("message", "0")},
        {.id = synth::ui::NodeId("field"),
         .kind = synth::ui::NodeKind::TextField,
         .bounds = {8.0f, 80.0f, 120.0f, 28.0f},
         .label = "Argument",
         .text = "7",
         .enabled = false,
         .action = synth::ui::Action::WithValue("argument", "0")},
        {.id = synth::ui::NodeId("toggle"),
         .kind = synth::ui::NodeKind::Toggle,
         .bounds = {8.0f, 116.0f, 120.0f, 28.0f},
         .label = "Enabled",
         .checked = true,
         .enabled = false,
         .action = synth::ui::Action::WithValue("toggle", "0")},
        {.id = synth::ui::NodeId("slider"),
         .kind = synth::ui::NodeKind::Slider,
         .bounds = {8.0f, 152.0f, 140.0f, 28.0f},
         .label = "Depth",
         .enabled = false,
         .value = 0.25f,
         .action = synth::ui::Action::WithValue("depth", "0")},
        {.id = synth::ui::NodeId("draw"),
         .kind = synth::ui::NodeKind::Draw,
         .bounds = {8.0f, 188.0f, 80.0f, 40.0f},
         .enabled = false,
         .pointerDragAction = synth::ui::Action::WithValue("draw.drag", "0"),
         .doubleClickAction = synth::ui::Action::Named("draw.double")},
    };

    synth_juce::PortableComponent disabledComponent(disabledSurface);
    disabledComponent.setSize(480, 320);
    disabledComponent.RefreshFromSurface();

    auto* disabledButton =
        dynamic_cast<juce::TextButton*>(disabledComponent.FindByNodeId("button"));
    auto* disabledCombo =
        dynamic_cast<juce::ComboBox*>(disabledComponent.FindByNodeId("combo"));
    auto* disabledField =
        dynamic_cast<juce::TextEditor*>(disabledComponent.FindByNodeId("field"));
    auto* disabledToggle =
        dynamic_cast<juce::ToggleButton*>(disabledComponent.FindByNodeId("toggle"));
    auto* disabledSlider =
        dynamic_cast<juce::Slider*>(disabledComponent.FindByNodeId("slider"));
    juce::Component* disabledDraw = disabledComponent.FindByNodeId("draw");
    Require(disabledButton != nullptr && disabledCombo != nullptr && disabledField != nullptr
                && disabledToggle != nullptr && disabledSlider != nullptr
                && disabledDraw != nullptr,
            "disabled fixture nodes are all rendered");
    Require(!disabledButton->isEnabled() && !disabledCombo->isEnabled()
                && !disabledField->isEnabled() && !disabledToggle->isEnabled()
                && !disabledSlider->isEnabled() && !disabledDraw->isEnabled(),
            "disabled portable nodes render as disabled JUCE controls");

    Require(disabledButton->getButtonText() == juce::String("Submit"),
            "disabled button keeps its portable label");
    Require(disabledCombo->getNumItems() == 2
                && disabledCombo->getItemText(0) == juce::String("First")
                && disabledCombo->getItemText(1) == juce::String("Second")
                && disabledCombo->getSelectedItemIndex() == 1,
            "disabled combo keeps its options and selected option");
    Require(disabledField->getText() == juce::String("7"),
            "disabled text field keeps its portable value");
    Require(disabledToggle->getToggleState(), "disabled toggle keeps its checked state");
    Require(std::abs(disabledSlider->getValue() - 0.25) < 1e-6,
            "disabled slider keeps its portable value");

    disabledButton->onClick();
    disabledCombo->setSelectedId(1, juce::dontSendNotification);
    disabledCombo->onChange();
    disabledField->setText("9", false);
    disabledField->onReturnKey();
    disabledField->onFocusLost();
    disabledToggle->setToggleState(false, juce::dontSendNotification);
    disabledToggle->onClick();
    disabledSlider->setValue(0.75, juce::sendNotificationSync);
    Require(disabledSurface.dispatchCount == 0,
            "disabled semantic controls dispatch no portable action");

    const juce::MouseInputSource disabledSource =
        juce::Desktop::getInstance().getMainMouseSource();
    const juce::Point<float> disabledDown(10.0f, 10.0f);
    const juce::Point<float> disabledMove(40.0f, 4.0f);
    const auto makeEvent = [&](juce::Component* target, juce::Point<float> position) {
        return juce::MouseEvent(disabledSource,
                                position,
                                juce::ModifierKeys::leftButtonModifier,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                target,
                                target,
                                juce::Time::getCurrentTime(),
                                disabledDown,
                                juce::Time::getCurrentTime(),
                                1,
                                false);
    };
    disabledButton->mouseDoubleClick(makeEvent(disabledButton, disabledDown));
    disabledDraw->mouseDown(makeEvent(disabledDraw, disabledDown));
    disabledDraw->mouseDrag(makeEvent(disabledDraw, disabledMove));
    disabledDraw->mouseDoubleClick(makeEvent(disabledDraw, disabledDown));
    Require(disabledSurface.dispatchCount == 0,
            "disabled button and draw nodes dispatch neither drag nor double click");

    disabledSurface.tree.nodes[1].enabled = true;
    disabledSurface.tree.nodes[6].enabled = true;
    disabledComponent.RefreshFromSurface();
    Require(disabledComponent.FindByNodeId("button")->isEnabled(),
            "re-enabled portable node renders as an enabled JUCE control");
    dynamic_cast<juce::TextButton*>(disabledComponent.FindByNodeId("button"))->onClick();
    Require(disabledSurface.dispatchCount == 1 && disabledSurface.lastAction.name == "submit",
            "re-enabled control dispatches its portable action again");
    disabledDraw = disabledComponent.FindByNodeId("draw");
    disabledDraw->mouseDown(makeEvent(disabledDraw, disabledDown));
    disabledDraw->mouseDrag(makeEvent(disabledDraw, disabledMove));
    Require(disabledSurface.dispatchCount == 2
                && disabledSurface.lastAction.name == "draw.drag",
            "re-enabled draw node dispatches its drag action again");
}

void TestRetainedControlsFollowThePortableTreeNotTheirOwnState()
{
    // sru-33: a retained control follows the portable tree, not its own
    // last user input, when the surface changes a selection or value
    // out-of-band (a refused edit reverting, or reconciliation choosing a
    // different device).
    RecordingSurface retainedSurface;
    retainedSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 320.0f, 160.0f},
         .children = {synth::ui::NodeId("combo"),
                      synth::ui::NodeId("field"),
                      synth::ui::NodeId("toggle")}},
        {.id = synth::ui::NodeId("combo"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {8.0f, 8.0f, 160.0f, 28.0f},
         .options = {{"first", "First"}, {"second", "Second"}},
         .selectedOption = "first",
         .action = synth::ui::Action::Named("combo")},
        {.id = synth::ui::NodeId("field"),
         .kind = synth::ui::NodeKind::TextField,
         .bounds = {8.0f, 44.0f, 120.0f, 28.0f},
         .text = "0",
         .action = synth::ui::Action::Named("field")},
        {.id = synth::ui::NodeId("toggle"),
         .kind = synth::ui::NodeKind::Toggle,
         .bounds = {8.0f, 80.0f, 120.0f, 28.0f},
         .label = "On",
         .action = synth::ui::Action::Named("toggle")},
    };

    synth_juce::PortableComponent retainedComponent(retainedSurface);
    retainedComponent.setSize(320, 160);
    retainedComponent.RefreshFromSurface();
    auto* retainedCombo = dynamic_cast<juce::ComboBox*>(retainedComponent.FindByNodeId("combo"));
    auto* retainedField = dynamic_cast<juce::TextEditor*>(retainedComponent.FindByNodeId("field"));
    auto* retainedToggle =
        dynamic_cast<juce::ToggleButton*>(retainedComponent.FindByNodeId("toggle"));
    Require(retainedCombo != nullptr && retainedField != nullptr && retainedToggle != nullptr,
            "retained fixture nodes are rendered");

    retainedCombo->setSelectedId(2, juce::dontSendNotification);
    retainedField->setText("draft", false);
    retainedToggle->setToggleState(true, juce::dontSendNotification);
    retainedSurface.tree.nodes[1].selectedOption = "second";
    retainedSurface.tree.nodes[2].text = "9";
    retainedSurface.tree.nodes[3].checked = true;
    retainedComponent.RefreshFromSurface();
    retainedSurface.tree.nodes[1].selectedOption = "first";
    retainedSurface.tree.nodes[2].text = "0";
    retainedSurface.tree.nodes[3].checked = false;
    retainedComponent.RefreshFromSurface();
    Require(retainedComponent.FindByNodeId("combo") == retainedCombo
                && retainedComponent.FindByNodeId("field") == retainedField
                && retainedComponent.FindByNodeId("toggle") == retainedToggle,
            "controls are retained across refreshes by stable node id");
    Require(retainedCombo->getSelectedItemIndex() == 0,
            "a retained combo follows the portable selected option");
    Require(retainedField->getText() == juce::String("0"),
            "an unfocused retained text field follows the portable value");
    Require(!retainedToggle->getToggleState(),
            "a retained toggle follows the portable checked state");
    Require(retainedSurface.dispatchCount == 0,
            "following the portable tree dispatches no action");
}

void TestContainerNodesRenderAsPanelsAndPaintNoLabel()
{
    // sru-33 host parity: a Row's or Section's own label paints nothing in
    // either host. The browser backend writes textContent only for
    // Button/Label/StatusText, and this pins JUCE to the same rule so the
    // two hosts cannot drift.
    //
    // This is a statement about renderers, not a way to convey content: a
    // container label is invisible to the user, so any text a page needs to
    // show -- a heading, a row's identity -- belongs in an explicit Label or
    // StatusText child node. Making a renderer paint container labels would
    // change generic layout on every page and is deliberately not done.
    RecordingSurface containerSurface;
    containerSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 240.0f, 120.0f},
         .children = {synth::ui::NodeId("section")}},
        {.id = synth::ui::NodeId("section"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {0.0f, 0.0f, 240.0f, 120.0f},
         .label = "Section Label",
         .children = {synth::ui::NodeId("row")}},
        {.id = synth::ui::NodeId("row"),
         .kind = synth::ui::NodeKind::Row,
         .bounds = {0.0f, 0.0f, 240.0f, 60.0f},
         .label = "Row Label",
         .children = {synth::ui::NodeId("child")}},
        {.id = synth::ui::NodeId("child"),
         .kind = synth::ui::NodeKind::Label,
         .bounds = {0.0f, 0.0f, 240.0f, 22.0f},
         .text = "Child"},
    };

    synth_juce::PortableComponent containerComponent(containerSurface);
    containerComponent.setSize(240, 120);
    containerComponent.RefreshFromSurface();
    juce::Component* section = containerComponent.FindByNodeId("section");
    juce::Component* row = containerComponent.FindByNodeId("row");
    Require(section != nullptr && row != nullptr, "container fixture nodes are rendered");
    Require(dynamic_cast<juce::Label*>(section) == nullptr
                && dynamic_cast<juce::Label*>(row) == nullptr
                && dynamic_cast<juce::Button*>(section) == nullptr
                && dynamic_cast<juce::Button*>(row) == nullptr,
            "container nodes render as panels rather than text controls");
    const juce::Image labelledContainers = RenderComponent(containerComponent);

    containerSurface.tree.nodes[1].label.clear();
    containerSurface.tree.nodes[2].label.clear();
    containerComponent.RefreshFromSurface();
    const juce::Image unlabelledContainers = RenderComponent(containerComponent);
    bool containerPixelsMatch = true;
    for (int y = 0; y < labelledContainers.getHeight(); ++y)
    {
        for (int x = 0; x < labelledContainers.getWidth(); ++x)
        {
            containerPixelsMatch = containerPixelsMatch
                                   && labelledContainers.getPixelAt(x, y)
                                          == unlabelledContainers.getPixelAt(x, y);
        }
    }
    Require(containerPixelsMatch, "a container node's own label paints nothing");
}

void TestDeclaredColumnBoundsResolveWithoutReflow()
{
    // scw-3 / sru-33: a form that declares explicit parent-local column
    // bounds keeps them in JUCE instead of being reflowed into the
    // renderer's own wrapping cursor.
    RecordingSurface columnSurface;
    columnSurface.tree.nodes = {
        {.id = synth::ui::NodeId("root"),
         .kind = synth::ui::NodeKind::Root,
         .bounds = {0.0f, 0.0f, 200.0f, 200.0f},
         .children = {synth::ui::NodeId("column.0"), synth::ui::NodeId("column.1")}},
        {.id = synth::ui::NodeId("column.0"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {8.0f, 40.0f, 80.0f, 120.0f},
         .children = {synth::ui::NodeId("column.0.row.0"), synth::ui::NodeId("column.0.row.1")}},
        {.id = synth::ui::NodeId("column.1"),
         .kind = synth::ui::NodeKind::Section,
         .bounds = {104.0f, 40.0f, 80.0f, 120.0f},
         .children = {synth::ui::NodeId("column.1.row.0"), synth::ui::NodeId("column.1.row.1")}},
        {.id = synth::ui::NodeId("column.0.row.0"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {0.0f, 20.0f, 80.0f, 28.0f},
         .options = {{"a", "A"}},
         .selectedOption = "a"},
        {.id = synth::ui::NodeId("column.0.row.1"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {0.0f, 60.0f, 80.0f, 28.0f},
         .options = {{"a", "A"}},
         .selectedOption = "a"},
        {.id = synth::ui::NodeId("column.1.row.0"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {0.0f, 20.0f, 80.0f, 28.0f},
         .options = {{"a", "A"}},
         .selectedOption = "a"},
        {.id = synth::ui::NodeId("column.1.row.1"),
         .kind = synth::ui::NodeKind::ComboBox,
         .bounds = {0.0f, 60.0f, 80.0f, 28.0f},
         .options = {{"a", "A"}},
         .selectedOption = "a"},
    };

    synth_juce::PortableComponent columnComponent(columnSurface);
    columnComponent.setSize(200, 200);
    columnComponent.RefreshFromSurface();
    const auto columnBounds = [&](const char* id) {
        juce::Component* control = columnComponent.FindByNodeId(id);
        Require(control != nullptr, "declared column control is rendered");
        return SurfaceBoundsOf(columnComponent, *control);
    };
    Require(columnBounds("column.0.row.0") == juce::Rectangle<int>(8, 60, 80, 28)
                && columnBounds("column.0.row.1") == juce::Rectangle<int>(8, 100, 80, 28)
                && columnBounds("column.1.row.0") == juce::Rectangle<int>(104, 60, 80, 28)
                && columnBounds("column.1.row.1") == juce::Rectangle<int>(104, 100, 80, 28),
            "declared two-column bounds resolve exactly, without renderer reflow");
}

int failureCount = 0;

void Run(const char* name, void (*body)())
{
    try
    {
        body();
        std::cout << "[PASS] " << name << "\n";
    }
    catch (const std::exception& error)
    {
        ++failureCount;
        std::cout << "[FAIL] " << name << ": " << error.what() << "\n";
    }
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    Run("TestGeometryAndColourHelpers", TestGeometryAndColourHelpers);
    Run("TestRenderedPositionFoldsAncestorOriginsAndScrollOffset", TestRenderedPositionFoldsAncestorOriginsAndScrollOffset);
    Run("TestOverhangingChildBoundsFoldWithoutReclassification", TestOverhangingChildBoundsFoldWithoutReclassification);
    Run("TestNodeWithoutResolvedBoundsIsNotRescued", TestNodeWithoutResolvedBoundsIsNotRescued);
    Run("TestCarriedColourDecidesTheButtonFill", TestCarriedColourDecidesTheButtonFill);
    Run("TestCarriedTextStyleDecidesGlyphColour", TestCarriedTextStyleDecidesGlyphColour);
    Run("TestSelectedPresentationDerivesFromTheCarriedColour", TestSelectedPresentationDerivesFromTheCarriedColour);
    Run("TestDisabledAndContainerPresentationDeriveFromTheCarriedColour", TestDisabledAndContainerPresentationDeriveFromTheCarriedColour);
    Run("TestControlsRegisterRetainAndDispatchAcrossRefresh", TestControlsRegisterRetainAndDispatchAcrossRefresh);
    Run("TestComposedSubtreeRootsFoldWithOneOffset", TestComposedSubtreeRootsFoldWithOneOffset);
    Run("TestRetainedControlsFollowSemanticReparenting", TestRetainedControlsFollowSemanticReparenting);
    Run("TestScrollPositionSurvivesRefreshAndClampsOnShrink", TestScrollPositionSurvivesRefreshAndClampsOnShrink);
    Run("TestDrawCommandsPaintNodeLocal", TestDrawCommandsPaintNodeLocal);
    Run("TestScrollContentFloorsAtTheVisibleBounds", TestScrollContentFloorsAtTheVisibleBounds);
    Run("TestInteractiveDrawDragDispatchesDeltas", TestInteractiveDrawDragDispatchesDeltas);
    Run("TestDisabledSemanticControlsRenderDisabledAndKeepTheirState", TestDisabledSemanticControlsRenderDisabledAndKeepTheirState);
    Run("TestRetainedControlsFollowThePortableTreeNotTheirOwnState", TestRetainedControlsFollowThePortableTreeNotTheirOwnState);
    Run("TestContainerNodesRenderAsPanelsAndPaintNoLabel", TestContainerNodesRenderAsPanelsAndPaintNoLabel);
    Run("TestDeclaredColumnBoundsResolveWithoutReflow", TestDeclaredColumnBoundsResolveWithoutReflow);

    // sru-52: Draw nodes dispatch a plain click, and their gesture sequences
    // match a Button's exactly.
    Run("TestDrawClickOnlyDispatchesOnce", TestDrawClickOnlyDispatchesOnce);
    Run("TestClickSequenceMatchesButtonExactly", TestClickSequenceMatchesButtonExactly);
    Run("TestDragDispatchesNoClick", TestDragDispatchesNoClick);
    Run("TestClickAfterADragOnTheSameNodeStillDispatches",
        TestClickAfterADragOnTheSameNodeStillDispatches);
    Run("TestDisabledDrawDispatchesNothing", TestDisabledDrawDispatchesNothing);
    Run("TestInertDrawInterceptsNothing", TestInertDrawInterceptsNothing);
    Run("TestReleaseOutsideTheNodeIsNoClick", TestReleaseOutsideTheNodeIsNoClick);
    Run("TestDoubleClickSequenceMatchesButtonExactly", TestDoubleClickSequenceMatchesButtonExactly);

    if (failureCount != 0)
    {
        std::cout << failureCount << " PortableJuceBackendTests case(s) failed\n";
        return 1;
    }
    std::cout << "PortableJuceBackendTests passed\n";
    return 0;
}
