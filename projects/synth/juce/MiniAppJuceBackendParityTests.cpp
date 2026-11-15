#include "PortableJuceBackend.hpp"

#include "../apps/miniapp/MiniApp.hpp"
#include "../apps/miniapp/MiniAppUiModel.hpp"

#include "synth/PortableUI.hpp"
#include "synth/GangedRandomLfoVisualizer.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

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
        throw std::runtime_error(std::string(label) + " expected " + std::to_string(expected) + " got " +
                                 std::to_string(actual));
    }
}

void RequireExactBounds(synth::ui::Bounds actual, synth::ui::Bounds expected, const char* label)
{
    RequireNear(actual.x, expected.x, 0.0001f, (std::string(label) + " x").c_str());
    RequireNear(actual.y, expected.y, 0.0001f, (std::string(label) + " y").c_str());
    RequireNear(actual.width, expected.width, 0.0001f, (std::string(label) + " width").c_str());
    RequireNear(actual.height, expected.height, 0.0001f, (std::string(label) + " height").c_str());
}

void RequireExactColour(juce::Colour actual, juce::Colour expected, const char* label)
{
    if (actual != expected)
    {
        throw std::runtime_error(std::string(label) + " expected rgba(" +
                                 std::to_string(expected.getRed()) + "," +
                                 std::to_string(expected.getGreen()) + "," +
                                 std::to_string(expected.getBlue()) + "," +
                                 std::to_string(expected.getAlpha()) + ") got rgba(" +
                                 std::to_string(actual.getRed()) + "," +
                                 std::to_string(actual.getGreen()) + "," +
                                 std::to_string(actual.getBlue()) + "," +
                                 std::to_string(actual.getAlpha()) + ")");
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

juce::Colour FillColourOf(synth_juce::PortableComponent& component,
                          const std::string& id)
{
    juce::Component* control = component.FindByNodeId(id);
    Require(control != nullptr, "fill-colour parity node is rendered");
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
    throw std::runtime_error("unsupported fill-colour parity node");
}

juce::Colour TextColourOf(synth_juce::PortableComponent& component,
                          const std::string& id)
{
    juce::Component* control = component.FindByNodeId(id);
    Require(control != nullptr, "text-colour parity node is rendered");
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
    throw std::runtime_error("unsupported text-colour parity node");
}

// Node bounds are parent-relative, so a claim about where two nodes sit
// relative to each other is a claim about their bounds folded over their
// ancestor origins.
synth::ui::Bounds SurfaceBoundsOf(const synth::ui::NodeTree& tree, const std::string& id)
{
    const auto parentOf = [&tree](const std::string& wanted) -> std::string {
        for (const synth::ui::Node& node : tree.nodes)
        {
            for (const synth::ui::NodeId& child : node.children)
            {
                if (child.value == wanted)
                {
                    return node.id.value;
                }
            }
        }
        return {};
    };
    const synth::ui::Node* node = FindNodeById(tree, id.c_str());
    Require(node != nullptr, "surface bounds for a node that exists");
    synth::ui::Bounds bounds = node->bounds;
    for (std::string parent = parentOf(id); !parent.empty(); parent = parentOf(parent))
    {
        const synth::ui::Node* parentNode = FindNodeById(tree, parent.c_str());
        Require(parentNode != nullptr, "surface bounds walks a complete ancestor chain");
        bounds.x += parentNode->bounds.x;
        bounds.y += parentNode->bounds.y;
    }
    return bounds;
}

synth::ui::NodeTree BackendStyleParityTree()
{
    // Keep this parity fixture in sync with backendStyleParityNodes in
    // browser/tests/ui-backend.spec.ts. Boundless FillEllipse is deliberately
    // excluded; see the Task 3.12 note in openspec/.../tasks.md.
    return {.nodes = {
                {.id = synth::ui::NodeId("root"),
                 .kind = synth::ui::NodeKind::Root,
                 .bounds = {0.0f, 0.0f, 500.0f, 280.0f},
                 .color = synth::Color::Rgb(8, 9, 10),
                 .children = {synth::ui::NodeId("section"),
                              synth::ui::NodeId("scroll"),
                              synth::ui::NodeId("square")}},
                {.id = synth::ui::NodeId("section"),
                 .kind = synth::ui::NodeKind::Section,
                 .bounds = {20.0f, 16.0f, 220.0f, 160.0f},
                 .color = synth::Color::Rgb(20, 30, 40),
                 .borderColor = synth::Color::Rgb(200, 210, 220),
                 .borderWidth = 4.0f,
                 .cornerRadius = 8.0f,
                 .children = {synth::ui::NodeId("row"),
                              synth::ui::NodeId("status"),
                              synth::ui::NodeId("disabled")}},
                {.id = synth::ui::NodeId("row"),
                 .kind = synth::ui::NodeKind::Row,
                 .bounds = {7.0f, 11.0f, 190.0f, 70.0f},
                 .children = {synth::ui::NodeId("label"),
                              synth::ui::NodeId("button"),
                              synth::ui::NodeId("toggle"),
                              synth::ui::NodeId("slider"),
                              synth::ui::NodeId("draw")}},
                {.id = synth::ui::NodeId("label"),
                 .kind = synth::ui::NodeKind::Label,
                 .bounds = {4.0f, 3.0f, 48.0f, 18.0f},
                 .text = "Label",
                 .color = synth::Color::Rgb(10, 10, 10),
                 .textStyle = synth::ui::TextStyle{14.0f,
                                                   synth::Color::Rgb(240, 240, 240),
                                                   synth::ui::TextAlign::Left}},
                {.id = synth::ui::NodeId("button"),
                 .kind = synth::ui::NodeKind::Button,
                 .bounds = {60.0f, 4.0f, 64.0f, 24.0f},
                 .label = "Go",
                 .color = synth::Color::Rgb(0, 120, 0),
                 .textStyle = synth::ui::TextStyle{13.0f,
                                                   synth::Color::Rgb(244, 245, 246),
                                                   synth::ui::TextAlign::Center}},
                {.id = synth::ui::NodeId("toggle"),
                 .kind = synth::ui::NodeKind::Toggle,
                 .bounds = {130.0f, 4.0f, 54.0f, 24.0f},
                 .label = "On",
                 .checked = true,
                 .color = synth::Color::Rgb(0, 120, 0)},
                {.id = synth::ui::NodeId("slider"),
                 .kind = synth::ui::NodeKind::Slider,
                 .bounds = {60.0f, 36.0f, 96.0f, 24.0f},
                 .value = 0.5f,
                 .minValue = 0.0f,
                 .maxValue = 1.0f,
                 .step = 0.01f,
                 .color = synth::Color::Rgb(10, 80, 160)},
                {.id = synth::ui::NodeId("draw"),
                 .kind = synth::ui::NodeKind::Draw,
                 .bounds = {160.0f, 34.0f, 24.0f, 24.0f},
                 .color = synth::Color::Rgb(250, 0, 0),
                 .drawCommands = {synth::ui::DrawCommand::Fill(
                     {0.0f, 0.0f, 24.0f, 24.0f}, synth::Color::Rgb(1, 2, 3))}},
                {.id = synth::ui::NodeId("status"),
                 .kind = synth::ui::NodeKind::StatusText,
                 .bounds = {7.0f, 90.0f, 190.0f, 22.0f},
                 .text = "Status",
                 .textStyle = synth::ui::TextStyle{15.0f,
                                                   synth::Color::Rgb(180, 200, 220),
                                                   synth::ui::TextAlign::Left}},
                {.id = synth::ui::NodeId("disabled"),
                 .kind = synth::ui::NodeKind::Button,
                 .bounds = {7.0f, 118.0f, 96.0f, 24.0f},
                 .label = "Disabled",
                 .enabled = false,
                 .color = synth::Color::Rgb(40, 80, 120)},
                {.id = synth::ui::NodeId("scroll"),
                 .kind = synth::ui::NodeKind::ScrollArea,
                 .bounds = {260.0f, 20.0f, 120.0f, 90.0f},
                 .color = synth::Color::Rgb(45, 55, 65),
                 .borderColor = synth::Color::Rgb(180, 190, 200),
                 .borderWidth = 4.0f,
                 .cornerRadius = 10.0f,
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
                 .selectedOption = "one",
                 .color = synth::Color::Rgb(120, 20, 80)},
                {.id = synth::ui::NodeId("field"),
                 .kind = synth::ui::NodeKind::TextField,
                 .bounds = {86.0f, 4.0f, 88.0f, 24.0f},
                 .text = "value",
                 .color = synth::Color::Rgb(30, 70, 90),
                 .textStyle = synth::ui::TextStyle{13.0f,
                                                   synth::Color::Rgb(230, 235, 240),
                                                   synth::ui::TextAlign::Left}},
                {.id = synth::ui::NodeId("scroll.draw"),
                 .kind = synth::ui::NodeKind::Draw,
                 .bounds = {20.0f, 125.0f, 50.0f, 35.0f},
                 .drawCommands = {synth::ui::DrawCommand::Fill(
                     {0.0f, 0.0f, 50.0f, 35.0f}, synth::Color::Rgb(4, 5, 6))}},
                {.id = synth::ui::NodeId("zero"),
                 .kind = synth::ui::NodeKind::Label,
                 .bounds = {150.0f, 10.0f, 0.0f, 0.0f},
                 .text = "unresolved"},
                {.id = synth::ui::NodeId("square"),
                 .kind = synth::ui::NodeKind::Section,
                 .bounds = {400.0f, 20.0f, 40.0f, 40.0f},
                 .color = synth::Color::Rgb(33, 44, 55)},
            }};
}

struct StaticSurface final : synth::ui::Surface
{
    synth::ui::NodeTree BuildTree() override
    {
        return tree;
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action&) override {}

    synth::ui::NodeTree tree;
    ActionHandler handler_;
};

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

void RequireDrawStartsInsideResolvedBounds(const juce::Image& image,
                                           juce::Rectangle<int> bounds,
                                           const char* label)
{
    const juce::Colour rootBackground = synth_juce::UiToJuceColour(synth::kSurfaceBackground);
    const int x = bounds.getX() + 2;
    const int y = bounds.getY() + 2;
    Require(image.getBounds().contains(x, y), label);
    Require(image.getPixelAt(x, y) != rootBackground, label);
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    {
        StaticSurface paritySurface;
        paritySurface.tree = BackendStyleParityTree();
        synth_juce::PortableComponent parityComponent(paritySurface);
        parityComponent.setSize(500, 280);
        parityComponent.RefreshFromSurface();

        for (const synth::ui::Node& node : paritySurface.tree.nodes)
        {
            const juce::Rectangle<int> expected =
                synth_juce::UiToJuceRect(SurfaceBoundsOf(paritySurface.tree, node.id.value));
            Require(parityComponent.SurfaceBoundsForNode(node.id.value) == expected,
                    ("JUCE parity geometry for " + node.id.value + " matches the tree fold").c_str());
        }

        const juce::Image parityImage = RenderComponent(parityComponent);
        RequireExactColour(parityImage.getPixelAt(490, 250),
                           juce::Colour::fromRGB(8, 9, 10),
                           "root carried background matches browser parity fixture");
        RequireExactColour(parityImage.getPixelAt(230, 170),
                           juce::Colour::fromRGB(20, 30, 40),
                           "section carried background matches browser parity fixture");
        RequireExactColour(parityImage.getPixelAt(25, 40),
                           juce::Colour::fromRGB(20, 30, 40),
                           "section carried fill covers its own padding, not only its children");
        RequireExactColour(parityImage.getPixelAt(35, 101),
                           juce::Colour::fromRGB(20, 30, 40),
                           "section carried fill covers the gap between its children");
        RequireExactColour(parityImage.getPixelAt(130, 18),
                           juce::Colour::fromRGB(200, 210, 220),
                           "section border mid-edge uses the carried colour exactly");
        RequireExactColour(parityImage.getPixelAt(130, 19),
                           juce::Colour::fromRGB(200, 210, 220),
                           "section border width covers the declared 4px band");
        RequireExactColour(parityImage.getPixelAt(130, 21),
                           juce::Colour::fromRGB(20, 30, 40),
                           "section fill resumes immediately inside the declared border band");
        RequireExactColour(parityImage.getPixelAt(28, 16),
                           juce::Colour::fromRGB(200, 210, 220),
                           "section rounded border path keeps the declared outer radius");
        RequireExactColour(parityImage.getPixelAt(261, 21),
                           juce::Colour::fromRGB(8, 9, 10),
                           "scroll-area rounded corner leaves the root surface visible");
        RequireExactColour(parityImage.getPixelAt(320, 22),
                           juce::Colour::fromRGB(180, 190, 200),
                           "scroll-area border uses the carried colour exactly");
        RequireExactColour(parityImage.getPixelAt(400, 20),
                           juce::Colour::fromRGB(33, 44, 55),
                           "a filled container with no carried radius uses the shared square default");
        RequireExactColour(FillColourOf(parityComponent, "label"),
                           juce::Colour::fromRGB(10, 10, 10),
                           "label carried colour is its text background");
        RequireExactColour(TextColourOf(parityComponent, "label"),
                           juce::Colour::fromRGB(240, 240, 240),
                           "label glyph colour comes from textStyle");
        auto* label = dynamic_cast<juce::Label*>(parityComponent.FindByNodeId("label"));
        Require(label != nullptr, "label parity node is a JUCE Label");
        RequireNear(label->getFont().getHeight(), 14.0f, 0.001f,
                    "label textStyle size is assigned");
        RequireExactColour(FillColourOf(parityComponent, "button"),
                           juce::Colour::fromRGB(0, 120, 0),
                           "button carried colour is its fill");
        RequireExactColour(TextColourOf(parityComponent, "button"),
                           juce::Colour::fromRGB(244, 245, 246),
                           "button glyph colour is assigned from textStyle");
        RequireExactColour(FillColourOf(parityComponent, "toggle"),
                           juce::Colour::fromRGB(0, 120, 0).brighter(0.14f),
                           "checked toggle derives its carried accent");
        RequireExactColour(FillColourOf(parityComponent, "slider"),
                           juce::Colour::fromRGB(10, 80, 160),
                           "slider carried colour is its track accent");
        RequireExactColour(FillColourOf(parityComponent, "combo"),
                           juce::Colour::fromRGB(120, 20, 80),
                           "combo carried colour is its field background");
        RequireExactColour(FillColourOf(parityComponent, "field"),
                           juce::Colour::fromRGB(30, 70, 90),
                           "text field carried colour is its field background");
        RequireExactColour(TextColourOf(parityComponent, "field"),
                           juce::Colour::fromRGB(230, 235, 240),
                           "text field glyph colour is assigned from textStyle");
        const synth::ui::Bounds drawBounds = SurfaceBoundsOf(paritySurface.tree, "draw");
        RequireExactBounds(drawBounds, {187.0f, 61.0f, 24.0f, 24.0f},
                           "draw parity fixture geometry");
        RequireExactColour(parityImage.getPixelAt(static_cast<int>(std::lround(drawBounds.x + 12.0f)),
                                                  static_cast<int>(std::lround(drawBounds.y + 12.0f))),
                           juce::Colour::fromRGB(1, 2, 3),
                           "draw paints its command colour and ignores carried Node::color");
        RequireExactColour(FillColourOf(parityComponent, "disabled"),
                           juce::Colour::fromRGB(40, 80, 120).darker(0.35f),
                           "disabled button fill is derived from its carried colour");
        RequireNear(parityComponent.FindByNodeId("disabled")->getAlpha(), 0.58f, 0.001f,
                    "disabled parity node carries the backend dim opacity");
    }

    synth::GangedRandomLfoSnapshot<2> predictiveSnapshot;
    predictiveSnapshot.sampleRate = 48000.0;
    predictiveSnapshot.roundElapsedSamples = 3.0;
    predictiveSnapshot.voices[0] = {.source = 0.1f, .target = 0.9f, .output = 0.2f, .shape = 0.0f,
                                    .waitingIncrement = 0.25, .movingIncrement = 0.5,
                                    .color = synth::Color::Cyan};
    predictiveSnapshot.voices[1] = {.source = 0.8f, .target = 0.2f, .output = 0.7f, .shape = 1.0f,
                                    .waitingIncrement = 0.125, .movingIncrement = 0.25,
                                    .color = synth::Color::Orange};
    std::vector<synth::ui::DrawCommand> predictiveCommands;
    synth::ui::BuildGangedRandomLfoCommands(
        predictiveSnapshot, {0.0f, 0.0f, 160.0f, 80.0f}, predictiveCommands);
    juce::Image predictiveImage(juce::Image::ARGB, 160, 80, true);
    juce::Graphics predictiveGraphics(predictiveImage);
    std::size_t predictivePolylines = 0;
    std::size_t predictiveDots = 0;
    const juce::Rectangle<float> predictiveBounds{0.0f, 0.0f, 160.0f, 80.0f};
    for (const auto& command : predictiveCommands)
    {
        synth_juce::PaintDrawCommand(predictiveGraphics, command, predictiveBounds);
        predictivePolylines += command.kind == synth::ui::DrawCommand::Kind::Polyline ? 1u : 0u;
        predictiveDots += command.kind == synth::ui::DrawCommand::Kind::FillEllipse ? 1u : 0u;
    }
    Require(predictivePolylines > 2, "JUCE consumes predictive polyline commands");
    Require(predictiveDots == 2, "JUCE consumes predictive ellipse commands");

    synth::ParameterManager manager;
    synth::GridManager gridManager;
    synth::MessageInBus uiBus(&manager);
    synth::RuntimeConfig config = synth_miniapp::MiniAppCore::Config();
    synth::MidiInstrumentConfig instrument;
    synth::AppContext context;
    context.parameterManager = &manager;
    context.gridManager = &gridManager;
    context.uiBus = &uiBus;
    context.config = &config;
    context.instrument = &instrument;

    std::uint64_t timestamp = 500;
    context.now = [&timestamp]() { return timestamp++; };

    synth_miniapp::MiniApp app;
    app.Init(&context);
    synth::ui::Surface& surface = app.PortableSurface();

    synth_juce::PortableComponent component(surface);
    component.setSize(config.uiWidth, config.uiHeight);
    component.RefreshFromSurface();

    // The JUCE host must mirror the complete portable MiniApp node tree.
    const synth::ui::NodeTree tree = surface.BuildTree();
    Require(FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kRoot) != nullptr, "miniapp root node exists");
    Require(FindNodeById(tree, synth_miniapp::MiniAppNodeIds::Encoder(0).c_str()) != nullptr,
            "encoder zero draw node exists");
    Require(FindNodeById(tree, synth_miniapp::MiniAppNodeIds::Encoder(15).c_str()) != nullptr,
            "encoder fifteen draw node exists");
    Require(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0)) != nullptr,
            "encoder zero is hosted");
    Require(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(15)) != nullptr,
            "encoder fifteen is hosted");
    Require(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::kStart) != nullptr, "start control is hosted");
    const synth::ui::Node* vcoPanel = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kVcoScope);
    const synth::ui::Node* lfoPanel = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kLfoScope);
    Require(vcoPanel != nullptr && lfoPanel != nullptr,
            "JUCE receives both MiniApp waveform panels");
    Require(FindNodeById(tree, "miniapp.ganged_random_lfo.round") == nullptr,
            "removed MiniApp main panel is absent");
    Require(SurfaceBoundsOf(tree, vcoPanel->id.value).y + vcoPanel->bounds.height <=
                SurfaceBoundsOf(tree, lfoPanel->id.value).y,
            "JUCE tree preserves vertical scope ordering");
    juce::Image panelImage(juce::Image::ARGB, config.uiWidth, config.uiHeight, true);
    juce::Graphics panelGraphics(panelImage);
    for (const synth::ui::Node* panel : {vcoPanel, lfoPanel})
    {
        Require(!panel->drawCommands.empty(), "each MiniApp scope supplies portable draw commands");
        const juce::Rectangle<float> surfacePanelBounds =
            synth_juce::UiToJuceRectF(SurfaceBoundsOf(tree, panel->id.value));
        const juce::Rectangle<float> panelBounds{0.0f,
                                                 0.0f,
                                                 panel->bounds.width,
                                                 panel->bounds.height};
        panelGraphics.saveState();
        panelGraphics.addTransform(juce::AffineTransform::translation(
            surfacePanelBounds.getX(), surfacePanelBounds.getY()));
        for (const auto& command : panel->drawCommands)
        {
            synth_juce::PaintDrawCommand(panelGraphics, command, panelBounds);
        }
        panelGraphics.restoreState();
    }

    const synth::ui::Node* encoderNode = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::Encoder(0).c_str());
    const synth::ui::Node* encoderFifteenNode =
        FindNodeById(tree, synth_miniapp::MiniAppNodeIds::Encoder(15).c_str());
    Require(encoderNode != nullptr, "encoder node for layout parity");
    Require(encoderFifteenNode != nullptr, "encoder fifteen node for layout parity");
    Require(SurfaceBoundsOf(tree, vcoPanel->id.value).x + vcoPanel->bounds.width <=
                    SurfaceBoundsOf(tree, encoderNode->id.value).x &&
                SurfaceBoundsOf(tree, lfoPanel->id.value).x + lfoPanel->bounds.width <=
                    SurfaceBoundsOf(tree, encoderFifteenNode->id.value).x,
            "scope stack remains left of encoder grid");

    // The encoder grid's geometry is resolved rather than hand-computed, so
    // these are the new exact numbers, not the old ones and not a relation.
    // At 900x560: the page margin puts content at 16 with 868 across; the
    // title takes 30 and a 14 gap, so the body starts at y=60; the stack takes
    // 390 and the encoder region 462, starting at x = 16 + 390 + 14 = 420;
    // the captioned slider rows in the bay leave the region 406 high, which
    // four rows over three 8 gaps divide into 95.5, and 462 divides the same
    // way into 109.5.
    const synth::ui::Bounds encoderZeroSurface = SurfaceBoundsOf(tree, encoderNode->id.value);
    const synth::ui::Bounds encoderFifteenSurface = SurfaceBoundsOf(tree, encoderFifteenNode->id.value);
    RequireExactBounds(encoderZeroSurface, {420.0f, 60.0f, 109.5f, 95.5f}, "encoder zero surface bounds");
    RequireExactBounds(encoderFifteenSurface, {772.5f, 370.5f, 109.5f, 95.5f},
                       "encoder fifteen surface bounds");
    RequireExactBounds(SurfaceBoundsOf(tree, "miniapp.encoders"), {420.0f, 60.0f, 462.0f, 406.0f},
                       "encoder region surface bounds");
    RequireExactBounds(SurfaceBoundsOf(tree, vcoPanel->id.value), {16.0f, 60.0f, 390.0f, 196.0f},
                       "VCO scope surface bounds");
    RequireExactBounds(SurfaceBoundsOf(tree, lfoPanel->id.value), {16.0f, 270.0f, 390.0f, 196.0f},
                       "LFO scope surface bounds");

    // And the backend must arrive at those same numbers by its own fold, which
    // is the parity claim: pin them against the literals, not against the tree
    // the expectations were read out of.
    Require(component.SurfaceBoundsForNode(synth_miniapp::MiniAppNodeIds::Encoder(0)) ==
                juce::Rectangle<int>(420, 60, 110, 96),
            "the JUCE host folds encoder zero to the same surface bounds");
    Require(component.SurfaceBoundsForNode(synth_miniapp::MiniAppNodeIds::Encoder(15)) ==
                juce::Rectangle<int>(773, 371, 110, 96),
            "the JUCE host folds encoder fifteen to the same surface bounds");

    StaticSurface paintedGridSurface;
    paintedGridSurface.tree = tree;
    for (std::size_t encoderIx = 0; encoderIx < synth_miniapp::EncoderGridLayout::kEncoderCount; ++encoderIx)
    {
        const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(encoderIx);
        for (synth::ui::Node& node : paintedGridSurface.tree.nodes)
        {
            if (node.id == synth::ui::NodeId(encoderId))
            {
                node.drawCommands.push_back(
                    synth::ui::DrawCommand::Fill(
                        {0.0f, 0.0f, node.bounds.width, node.bounds.height},
                        synth::Color::Rgb(90, 120, 160)));
                break;
            }
        }
    }
    synth_juce::PortableComponent paintedGridComponent(paintedGridSurface);
    paintedGridComponent.setSize(config.uiWidth, config.uiHeight);
    paintedGridComponent.RefreshFromSurface();
    const juce::Image paintedGridImage = RenderComponent(paintedGridComponent);

    for (std::size_t encoderIx = 0; encoderIx < synth_miniapp::EncoderGridLayout::kEncoderCount; ++encoderIx)
    {
        const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(encoderIx);
        const synth::ui::Node* node = FindNodeById(tree, encoderId.c_str());
        Require(node != nullptr, "each MiniApp encoder remains in the portable tree");
        const juce::Rectangle<int> expectedBounds =
            synth_juce::UiToJuceRect(SurfaceBoundsOf(tree, encoderId));
        Require(paintedGridComponent.SurfaceBoundsForNode(encoderId) == expectedBounds,
                "each hosted MiniApp encoder retains its pre-change surface bounds");
        RequireDrawStartsInsideResolvedBounds(paintedGridImage,
                                               expectedBounds,
                                               "each MiniApp encoder paints at its resolved origin");
    }
    const juce::Image renderedComponent = RenderComponent(component);
    RequireDrawStartsInsideResolvedBounds(renderedComponent,
                                           synth_juce::UiToJuceRect(SurfaceBoundsOf(tree, vcoPanel->id.value)),
                                           "VCO scope paints at its resolved origin");
    RequireDrawStartsInsideResolvedBounds(renderedComponent,
                                           synth_juce::UiToJuceRect(SurfaceBoundsOf(tree, lfoPanel->id.value)),
                                           "LFO scope paints at its resolved origin");

    auto* startButton = dynamic_cast<juce::TextButton*>(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::kStart));
    Require(startButton != nullptr, "start node is a TextButton");
    const std::size_t queueBefore = uiBus.Size();
    startButton->onClick();
    Require(uiBus.Size() == queueBefore + 1, "start click routes through portable backend");
    synth::MessageIn message;
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()), "start message is queued");
    Require(message.type == synth::MessageIn::Type::Start, "start message type");
    Require(message.timestamp == 500, "start uses runtime timestamp provider");

    auto* resetToggle = dynamic_cast<juce::ToggleButton*>(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::kReset));
    Require(resetToggle != nullptr, "reset modifier is a ToggleButton");
    resetToggle->onClick();
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()), "reset message is queued");
    Require(message.type == synth::MessageIn::Type::ToggleReset, "reset routes modifier action");
    Require(message.timestamp == 501, "reset uses runtime timestamp provider");

    auto* blendSlider = dynamic_cast<juce::Slider*>(component.FindByNodeId(synth_miniapp::MiniAppNodeIds::kSceneBlend));
    Require(blendSlider != nullptr, "scene blend slider is hosted");
    blendSlider->setValue(0.25, juce::dontSendNotification);
    blendSlider->onValueChange();
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()), "blend message is queued");
    Require(message.type == synth::MessageIn::Type::SetSceneBlend, "blend routes slider action");
    RequireNear(message.value, 0.25f, 0.001f, "blend slider value");

    auto* encoder = component.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(0));
    Require(encoder != nullptr, "encoder interactive overlay is hosted");
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
                               encoder,
                               encoder,
                               juce::Time::getCurrentTime(),
                               downPoint,
                               juce::Time::getCurrentTime(),
                               1,
                               false);
    encoder->mouseDown(downEvent);
    juce::MouseEvent dragEvent(mouseSource,
                               dragPoint,
                               juce::ModifierKeys::leftButtonModifier,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               1.0f,
                               encoder,
                               encoder,
                               juce::Time::getCurrentTime(),
                               downPoint,
                               juce::Time::getCurrentTime(),
                               1,
                               false);
    encoder->mouseDrag(dragEvent);
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()), "encoder drag message is queued");
    Require(message.type == synth::MessageIn::Type::ParamIncDec, "encoder drag routes inc/dec");
    Require(message.slotIx == 0, "encoder drag slot");
    Require(message.position == 0, "encoder drag position");
    RequireNear(message.delta, 0.05f, 0.0001f, "encoder drag delta");
    Require(message.timestamp == 503, "encoder drag uses timestamp provider");

    encoder->mouseDoubleClick(downEvent);
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()), "encoder push message is queued");
    Require(message.type == synth::MessageIn::Type::ParamPush, "encoder double-click routes push");
    Require(message.slotIx == 0, "encoder push slot");
    Require(message.position == 0, "encoder push position");
    Require(message.timestamp == 504, "encoder push uses timestamp provider");

    auto* encoderFifteen = component.FindByNodeId(synth_miniapp::MiniAppNodeIds::Encoder(15));
    Require(encoderFifteen != nullptr, "encoder fifteen interactive overlay is hosted");
    juce::MouseEvent encoderFifteenDownEvent(mouseSource,
                                             downPoint,
                                             juce::ModifierKeys::leftButtonModifier,
                                             1.0f,
                                             1.0f,
                                             1.0f,
                                             1.0f,
                                             1.0f,
                                             encoderFifteen,
                                             encoderFifteen,
                                             juce::Time::getCurrentTime(),
                                             downPoint,
                                             juce::Time::getCurrentTime(),
                                             1,
                                             false);
    encoderFifteen->mouseDoubleClick(encoderFifteenDownEvent);
    Require(uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max()),
            "encoder fifteen push message is queued");
    Require(message.type == synth::MessageIn::Type::ParamPush,
            "encoder fifteen double-click routes push");
    Require(message.slotIx == 0, "encoder fifteen push slot");
    Require(message.position == 15, "encoder fifteen push position");
    Require(message.timestamp == 505, "encoder fifteen push uses timestamp provider");

    std::cout << "MiniApp JUCE backend parity tests passed\n";
    return 0;
}
