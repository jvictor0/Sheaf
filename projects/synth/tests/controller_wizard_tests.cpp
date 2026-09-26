#include "synth/ControllerWizard.hpp"

#include "synth/MidiAppCatalog.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "controller wizard contracts must not see JUCE headers"
#endif

#include <iostream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*fn)()) {
        Registry().push_back({name, fn});
    }
};

#define TEST_CASE(name) \
    void name(); \
    Register reg_##name(#name, &name); \
    void name()

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            throw std::runtime_error(std::string("requirement failed: ") + #expr); \
        } \
    } while (false)

class FirstForm final : public synth::ControllerConfigForm {
public:
    ~FirstForm() override {
        ++destroyedCount;
    }

    std::string_view WizardId() const override { return "test.first"; }

    bool Validate(std::string& error) const override {
        if (name_.empty()) {
            error = "name is required";
            return false;
        }
        error.clear();
        return true;
    }

    synth::ui::NodeTree BuildTree() override { return {}; }

    void SetActionHandler(ActionHandler handler) override {
        actionHandler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override {
        if (action.name == "set-name") {
            name_ = action.value;
        }
        if (actionHandler_) {
            actionHandler_(action);
        }
    }

    std::string_view Name() const { return name_; }

    static int destroyedCount;

private:
    std::string name_;
    ActionHandler actionHandler_;
};

int FirstForm::destroyedCount = 0;

class SecondForm final : public synth::ControllerConfigForm {
public:
    std::string_view WizardId() const override { return "test.second"; }

    bool Validate(std::string& error) const override {
        error.clear();
        return true;
    }

    synth::ui::NodeTree BuildTree() override { return {}; }
    void SetActionHandler(ActionHandler) override {}
    void DispatchAction(const synth::ui::Action&) override {}
};

class FirstWizard final : public synth::TypedControllerWizard<FirstForm> {
public:
    std::string_view Id() const override { return "test.first"; }

    std::unique_ptr<synth::ControllerConfigForm>
    ConfigForm(const std::optional<synth::MidiControllerSlot>&) const override {
        return std::make_unique<FirstForm>();
    }

protected:
    synth::WizardGenerationResult GenerateTypedProfile(
        const FirstForm& form, const synth::WizardGenerationContext& context) const override {
        ++generationCount;
        synth::MidiControllerSlot slot;
        slot.name = std::string(form.Name());
        slot.input = context.input;
        slot.output = context.output;
        return {.controller = std::move(slot)};
    }

public:
    mutable int generationCount = 0;
};

class SecondWizard final : public synth::TypedControllerWizard<SecondForm> {
public:
    std::string_view Id() const override { return "test.second"; }

    std::unique_ptr<synth::ControllerConfigForm>
    ConfigForm(const std::optional<synth::MidiControllerSlot>&) const override {
        return std::make_unique<SecondForm>();
    }

protected:
    synth::WizardGenerationResult GenerateTypedProfile(
        const SecondForm&, const synth::WizardGenerationContext&) const override {
        ++generationCount;
        return {};
    }

public:
    mutable int generationCount = 0;
};

synth::WizardGenerationContext Context() {
    return {.name = "ignored-by-form", .input = {.identifier = "in-id", .name = "Input"},
            .output = {.identifier = "out-id", .name = "Output"}};
}

synth::MidiDeviceInfoRef Device(std::string identifier, std::string name) {
    return {.identifier = std::move(identifier), .name = std::move(name)};
}

synth::MidiEndpointRef Endpoint(std::string identifier, std::string name) {
    return {.identifier = std::move(identifier), .name = std::move(name)};
}

synth::MidiDeviceList Devices(std::initializer_list<synth::MidiDeviceInfoRef> inputs,
                              std::initializer_list<synth::MidiDeviceInfoRef> outputs) {
    return {.inputs = inputs, .outputs = outputs};
}

synth::ControllerWizardDescriptor Descriptor(
    std::string id, std::string displayName, synth::MidiProfileKind kind,
    std::initializer_list<std::string> inputAliases,
    std::initializer_list<std::string> outputAliases) {
    return {.id = std::move(id),
            .displayName = std::move(displayName),
            .kind = kind,
            .inputAliases = inputAliases,
            .outputAliases = outputAliases,
            .factory = [] { return std::unique_ptr<synth::ControllerWizard>{}; }};
}

std::vector<synth::ControllerWizardDescriptor> TestTwisterRegistry() {
    return {Descriptor("com.sheaf.midi-fighter-twister", "MIDI Fighter Twister",
                       synth::MidiProfileKind::MfTwister, {"Midi Fighter Twister"},
                       {"Midi Fighter Twister"})};
}

// The real (non-stub) registry an app with no device defaults gets: the
// library's single Twister descriptor, with a working factory.
std::vector<synth::ControllerWizardDescriptor> LibraryTwisterRegistry() {
    return synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{});
}

synth::MidiAppDeviceDefault AppDefault(std::string id, std::string displayName,
                                       synth::MidiProfileKind kind,
                                       std::initializer_list<std::string> inputAliases,
                                       std::initializer_list<std::string> outputAliases,
                                       synth::MidiControllerProfileConfig config) {
    synth::MidiAppDeviceDefault deviceDefault;
    deviceDefault.id = std::move(id);
    deviceDefault.displayName = std::move(displayName);
    deviceDefault.kind = kind;
    deviceDefault.inputAliases = inputAliases;
    deviceDefault.outputAliases = outputAliases;
    deviceDefault.config = std::move(config);
    return deviceDefault;
}

synth::MidiControllerSlot StoredController(std::string name,
                                           synth::MidiEndpointRef input,
                                           synth::MidiEndpointRef output) {
    synth::MidiControllerSlot slot;
    slot.name = std::move(name);
    slot.input = std::move(input);
    slot.output = std::move(output);
    return slot;
}

void RequireCandidate(const synth::WizardCandidate& candidate,
                      std::string_view wizardId,
                      std::string_view displayName,
                      synth::MidiProfileKind kind,
                      std::string_view inputId,
                      std::string_view outputId) {
    REQUIRE_TRUE(candidate.wizardId == wizardId);
    REQUIRE_TRUE(candidate.displayName == displayName);
    REQUIRE_TRUE(candidate.kind == kind);
    REQUIRE_TRUE(candidate.input.identifier == inputId);
    REQUIRE_TRUE(candidate.output.identifier == outputId);
}

void RequireDeviceIds(const std::vector<synth::MidiDeviceInfoRef>& devices,
                      std::initializer_list<std::string_view> ids) {
    REQUIRE_TRUE(devices.size() == ids.size());
    std::size_t ix = 0;
    for (std::string_view id : ids) {
        REQUIRE_TRUE(devices[ix].identifier == id);
        ++ix;
    }
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, std::string_view id) {
    for (const synth::ui::Node& node : tree.nodes) {
        if (node.id.value == id) {
            return &node;
        }
    }
    return nullptr;
}

const synth::ui::Node* FindParentOf(const synth::ui::NodeTree& tree, std::string_view childId) {
    for (const synth::ui::Node& node : tree.nodes) {
        for (const synth::ui::NodeId& child : node.children) {
            if (child.value == childId) {
                return &node;
            }
        }
    }
    return nullptr;
}

// Resolves a form node's position in the form's own coordinate space the way a
// host does: every child's bounds are parent-local, so absolute position is the
// sum of its ancestors' origins.
synth::ui::Bounds FormBounds(const synth::ui::NodeTree& tree, const std::string& id) {
    const synth::ui::Node* node = FindNodeById(tree, id);
    if (node == nullptr) {
        throw std::runtime_error("form node missing: " + id);
    }
    synth::ui::Bounds bounds = node->bounds;
    std::string childId = id;
    bool climbing = true;
    while (climbing) {
        climbing = false;
        for (const synth::ui::Node& candidate : tree.nodes) {
            for (const synth::ui::NodeId& child : candidate.children) {
                if (child.value != childId) {
                    continue;
                }
                bounds.x += candidate.bounds.x;
                bounds.y += candidate.bounds.y;
                childId = candidate.id.value;
                climbing = true;
                break;
            }
            if (climbing) {
                break;
            }
        }
    }
    return bounds;
}

std::string TwisterButtonField(std::size_t buttonIx, std::string_view field) {
    return "controller-wizard.twister.button." + std::to_string(buttonIx) + "." + std::string(field);
}

std::vector<const synth::ui::Node*> NodesOfKind(const synth::ui::NodeTree& tree,
                                                synth::ui::NodeKind kind) {
    std::vector<const synth::ui::Node*> result;
    for (const synth::ui::Node& node : tree.nodes) {
        if (node.kind == kind) {
            result.push_back(&node);
        }
    }
    return result;
}

TEST_CASE(MfTwisterConfigFormPlacesSixButtonsInTwoColumnsOfThree) {
    synth::MfTwisterConfigForm form;
    const synth::ui::NodeTree tree = form.BuildTree();

    const synth::ui::Bounds slot = FormBounds(tree, "controller-wizard.twister.encoder-slot");
    REQUIRE_TRUE(slot.width == 160.0f && slot.height > 0.0f);

    std::vector<synth::ui::Bounds> message;
    for (std::size_t buttonIx = 0; buttonIx < synth::MfTwisterConfigForm::kButtonCount; ++buttonIx) {
        const synth::ui::Bounds labelBounds = FormBounds(tree, TwisterButtonField(buttonIx, "label"));
        const synth::ui::Bounds messageCaptionBounds =
            FormBounds(tree, TwisterButtonField(buttonIx, "message") + ".caption");
        const synth::ui::Bounds messageBounds = FormBounds(tree, TwisterButtonField(buttonIx, "message"));
        const synth::ui::Bounds argumentCaptionBounds =
            FormBounds(tree, TwisterButtonField(buttonIx, "argument") + ".caption");
        const synth::ui::Bounds argumentBounds = FormBounds(tree, TwisterButtonField(buttonIx, "argument"));
        REQUIRE_TRUE(labelBounds.width > 0.0f && labelBounds.height > 0.0f);
        REQUIRE_TRUE(messageCaptionBounds.width > 0.0f && messageCaptionBounds.height > 0.0f);
        REQUIRE_TRUE(messageBounds.width > 0.0f && messageBounds.height > 0.0f);
        REQUIRE_TRUE(argumentCaptionBounds.width > 0.0f && argumentCaptionBounds.height > 0.0f);
        REQUIRE_TRUE(argumentBounds.width > 0.0f && argumentBounds.height > 0.0f);
        REQUIRE_TRUE(messageBounds.x >= labelBounds.x + labelBounds.width);
        REQUIRE_TRUE(argumentBounds.x >= messageBounds.x + messageBounds.width);
        REQUIRE_TRUE(messageCaptionBounds.x == messageBounds.x);
        REQUIRE_TRUE(argumentCaptionBounds.x == argumentBounds.x);
        REQUIRE_TRUE(argumentCaptionBounds.y == messageCaptionBounds.y);
        REQUIRE_TRUE(labelBounds.y == messageCaptionBounds.y);
        REQUIRE_TRUE(messageBounds.y > messageCaptionBounds.y);
        REQUIRE_TRUE(argumentBounds.y == messageBounds.y);
        message.push_back(messageBounds);
    }

    // Each column names itself above its first button row.
    for (std::size_t column = 0; column < 2; ++column) {
        const synth::ui::Bounds heading = FormBounds(
            tree, "controller-wizard.twister.column." + std::to_string(column) + ".heading");
        REQUIRE_TRUE(heading.width > 0.0f && heading.height > 0.0f);
        REQUIRE_TRUE(heading.y + heading.height <= message[column * 3].y);
        REQUIRE_TRUE(heading.x == FormBounds(tree, TwisterButtonField(column * 3, "label")).x);
    }

    // Buttons 0-2 are the first column, buttons 3-5 the second, in CC order.
    for (std::size_t row = 0; row < 3; ++row) {
        REQUIRE_TRUE(message[row].x == message[0].x);
        REQUIRE_TRUE(message[row + 3].x == message[3].x);
        REQUIRE_TRUE(message[row + 3].y == message[row].y);
    }
    REQUIRE_TRUE(message[1].y > message[0].y);
    REQUIRE_TRUE(message[2].y > message[1].y);
    REQUIRE_TRUE(message[3].x > message[0].x + message[0].width);

    // The one controller-wide Encoder Slot precedes both columns, and the form
    // reports an intrinsic height that covers everything it laid out.
    REQUIRE_TRUE(message[0].y >= slot.y + slot.height);
    REQUIRE_TRUE(tree.nodes.front().bounds.height >= message[2].y + message[2].height);
}

// Task 7.1 replaced every container extent this form used to compute with
// `Extent::Intrinsic()`. These are the numbers that arithmetic produced, pinned
// as literals so the swap is provably a no-op on screen rather than a claim
// that it is one -- and so a later change to a declared leaf extent has to be a
// decision rather than a silent reflow of the product owner's signed-off
// appearance.
TEST_CASE(MfTwisterConfigFormResolvesItsExtentsFromItsDeclarationsAlone) {
    synth::MfTwisterConfigForm form;
    const synth::ui::NodeTree tree = form.BuildTree();

    const synth::ui::Node* body = FindNodeById(tree, "controller-wizard.twister.body");
    REQUIRE_TRUE(body != nullptr);
    REQUIRE_TRUE(body->bounds.width == 684.0f);
    REQUIRE_TRUE(body->bounds.height == 294.0f);

    const synth::ui::Node* columns = FindNodeById(tree, "controller-wizard.twister.columns");
    REQUIRE_TRUE(columns != nullptr);
    REQUIRE_TRUE(columns->bounds.width == 668.0f);
    REQUIRE_TRUE(columns->bounds.height == 244.0f);

    for (const char* columnId : {"controller-wizard.twister.column.0",
                                 "controller-wizard.twister.column.1"}) {
        const synth::ui::Node* column = FindNodeById(tree, columnId);
        REQUIRE_TRUE(column != nullptr);
        REQUIRE_TRUE(column->bounds.width == 326.0f);
        REQUIRE_TRUE(column->bounds.height == 244.0f);
    }

    const synth::ui::Node* slotRow = FindNodeById(tree, "controller-wizard.twister.slot");
    REQUIRE_TRUE(slotRow != nullptr);
    REQUIRE_TRUE(slotRow->bounds.width == 258.0f);

    const synth::ui::Node* fields = FindNodeById(tree, "controller-wizard.twister.button.0.fields");
    REQUIRE_TRUE(fields != nullptr);
    REQUIRE_TRUE(fields->bounds.width == 248.0f);
    REQUIRE_TRUE(fields->bounds.height == 46.0f);

    // The standalone preview surface is a surface the form is rendered INTO,
    // never a size it derives: the body keeps its own measurements inside it,
    // and the leftover is the preview's, not the form's.
    REQUIRE_TRUE(tree.nodes.front().bounds.width == 1024.0f);
    REQUIRE_TRUE(tree.nodes.front().bounds.height == 768.0f);
    REQUIRE_TRUE(body->bounds.width < tree.nodes.front().bounds.width);

    // THE WARNED STATE, which is the whole reason `kColumnWidth` and
    // `kButtonRowHeight` survived task 7.1's cleanup. The inline argument error
    // is out of flow and its row reserves a fixed band for it. Make it in-flow
    // and its intrinsic cross extent propagates into every ancestor's: the
    // message reserves 424px against a 326px column, so the column would widen
    // by a third and shove the second column sideways the moment a field went
    // invalid. Nothing above this point would notice, because everything above
    // it renders a valid form.
    //
    // So: show the error, and require every extent to be the number it was.
    // The argument field is only enabled -- and therefore only validated -- for
    // the messages that take one, so the message is switched first; with the
    // default `HoldReset` there is no error node to show and this pin would
    // have measured the clean form twice.
    form.buttons[0].message = synth::UISystemMessage::SceneSelect;
    form.buttons[0].argumentText = "1x";
    const synth::ui::NodeTree warned = form.BuildTree();

    const synth::ui::Node* warnedError =
        FindNodeById(warned, "controller-wizard.twister.button.0.argument.error");
    REQUIRE_TRUE(warnedError != nullptr);
    REQUIRE_TRUE(warnedError->bounds.width > 0.0f && warnedError->bounds.height > 0.0f);

    const synth::ui::Node* warnedBody = FindNodeById(warned, "controller-wizard.twister.body");
    REQUIRE_TRUE(warnedBody != nullptr);
    REQUIRE_TRUE(warnedBody->bounds.width == 684.0f);
    REQUIRE_TRUE(warnedBody->bounds.height == 294.0f);

    const synth::ui::Node* warnedColumns = FindNodeById(warned, "controller-wizard.twister.columns");
    REQUIRE_TRUE(warnedColumns != nullptr);
    REQUIRE_TRUE(warnedColumns->bounds.width == 668.0f);

    for (const char* columnId : {"controller-wizard.twister.column.0",
                                 "controller-wizard.twister.column.1"}) {
        const synth::ui::Node* warnedColumn = FindNodeById(warned, columnId);
        REQUIRE_TRUE(warnedColumn != nullptr);
        REQUIRE_TRUE(warnedColumn->bounds.width == 326.0f);
        REQUIRE_TRUE(warnedColumn->bounds.height == 244.0f);
    }

    // The second column has not moved, which is the visible symptom an in-flow
    // error would produce first.
    REQUIRE_TRUE(FindNodeById(warned, "controller-wizard.twister.column.1")->bounds.x ==
                 FindNodeById(tree, "controller-wizard.twister.column.1")->bounds.x);

    // And the reserved band is doing its job: the error is inside the row that
    // reserved it, rather than overhanging into the row below.
    const synth::ui::Node* warnedRow = FindNodeById(warned, "controller-wizard.twister.button.0");
    REQUIRE_TRUE(warnedRow != nullptr);
    REQUIRE_TRUE(warnedRow->bounds.height == 68.0f);
    REQUIRE_TRUE(warnedError->bounds.y + warnedError->bounds.height <= warnedRow->bounds.height);
}

TEST_CASE(MfTwisterConfigFormBuildsRootlessSubtreeForWizardHosts) {
    synth::MfTwisterConfigForm form;
    const synth::ui::Subtree subtree = form.BuildSubtree();
    REQUIRE_TRUE(FindNodeById(subtree.tree, "controller-wizard.twister") == nullptr);
    const synth::ui::Node* body = FindNodeById(subtree.tree, "controller-wizard.twister.body");
    REQUIRE_TRUE(body != nullptr && body->kind == synth::ui::NodeKind::Section);
    REQUIRE_TRUE(FindParentOf(subtree.tree, body->id.value) == nullptr);
    REQUIRE_TRUE(subtree.layout.at("controller-wizard.twister.body").formGrid);
    REQUIRE_TRUE(subtree.layout.at("controller-wizard.twister.column.0").formGrid);
    REQUIRE_TRUE(subtree.layout.at("controller-wizard.twister.column.1").formGrid);
    REQUIRE_TRUE(FindNodeById(subtree.tree, "controller-wizard.twister.encoder-slot") != nullptr);
    REQUIRE_TRUE(FindNodeById(subtree.tree, "controller-wizard.twister.columns") != nullptr);
    REQUIRE_TRUE(FindParentOf(subtree.tree, "controller-wizard.twister.column.0")->id.value ==
                 "controller-wizard.twister.columns");

    synth::ui::Builder host;
    host.Root("host", {0.0f, 0.0f, 640.0f, 420.0f});
    host.Splice(form.BuildSubtree());
    const synth::ui::NodeTree hosted = host.Build({0.0f, 0.0f, 640.0f, 420.0f});
    const synth::ui::Node* root = FindNodeById(hosted, "host");
    REQUIRE_TRUE(root != nullptr);
    REQUIRE_TRUE(root->children.size() == 1);
    REQUIRE_TRUE(root->children.front().value == "controller-wizard.twister.body");
}

TEST_CASE(MfTwisterConfigFormBuildsClosedSixButtonSurfaceAndRoutesPortableActions) {
    synth::MfTwisterConfigForm form;
    const synth::ui::NodeTree initialTree = form.BuildTree();

    const synth::ui::Node* slot =
        FindNodeById(initialTree, "controller-wizard.twister.encoder-slot");
    REQUIRE_TRUE(slot != nullptr);
    REQUIRE_TRUE(slot->kind == synth::ui::NodeKind::TextField);
    REQUIRE_TRUE(slot->text == "0");

    const std::vector<const synth::ui::Node*> combos =
        NodesOfKind(initialTree, synth::ui::NodeKind::ComboBox);
    const std::vector<const synth::ui::Node*> arguments =
        NodesOfKind(initialTree, synth::ui::NodeKind::TextField);
    REQUIRE_TRUE(combos.size() == synth::MfTwisterConfigForm::kButtonCount);
    REQUIRE_TRUE(arguments.size() == synth::MfTwisterConfigForm::kButtonCount + 1);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.button.0.message") != nullptr);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.button.5.message") != nullptr);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.button.0.argument") != nullptr);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.button.5.argument") != nullptr);
    // No backend paints a container node's own label, so the column names and
    // the side-button names are rendered Label children, not Section/Row labels.
    for (const char* columnId : {"controller-wizard.twister.column.0",
                                 "controller-wizard.twister.column.1"}) {
        REQUIRE_TRUE(FindNodeById(initialTree, columnId)->label.empty());
    }
    const synth::ui::Node* leftHeading =
        FindNodeById(initialTree, "controller-wizard.twister.column.0.heading");
    const synth::ui::Node* rightHeading =
        FindNodeById(initialTree, "controller-wizard.twister.column.1.heading");
    REQUIRE_TRUE(leftHeading != nullptr && leftHeading->kind == synth::ui::NodeKind::Label &&
                 leftHeading->text == "Left (CC 8-10)");
    REQUIRE_TRUE(rightHeading != nullptr && rightHeading->kind == synth::ui::NodeKind::Label &&
                 rightHeading->text == "Right (CC 11-13)");
    for (std::size_t buttonIx = 0; buttonIx < synth::MfTwisterConfigForm::kButtonCount; ++buttonIx) {
        const synth::ui::Node* buttonRow =
            FindNodeById(initialTree, "controller-wizard.twister.button." + std::to_string(buttonIx));
        REQUIRE_TRUE(buttonRow != nullptr && buttonRow->label.empty());
        const synth::ui::Node* buttonLabel =
            FindNodeById(initialTree, TwisterButtonField(buttonIx, "label"));
        // One-based, so a "Button N" refusal names a row the user can see.
        REQUIRE_TRUE(buttonLabel != nullptr && buttonLabel->kind == synth::ui::NodeKind::Label &&
                     buttonLabel->text == "Button " + std::to_string(buttonIx + 1));
    }
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.0")->children.size() == 4);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.1")->children.size() == 4);
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.0")->children[0].value ==
                 "controller-wizard.twister.column.0.heading");
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.0")->children[1].value ==
                 "controller-wizard.twister.button.0");
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.0")->children[3].value ==
                 "controller-wizard.twister.button.2");
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.1")->children[1].value ==
                 "controller-wizard.twister.button.3");
    REQUIRE_TRUE(FindNodeById(initialTree, "controller-wizard.twister.column.1")->children[3].value ==
                 "controller-wizard.twister.button.5");

    const std::vector<std::string> expectedLabels = {
        "Toggle Reset", "Hold Reset", "Toggle Random", "Hold Random",
        "Toggle Random Mod", "Hold Random Mod", "Toggle Gesture Select",
        "Hold Gesture Select", "Bank Select", "Next Bank", "Previous Bank",
        "Start", "Continue", "Stop", "Clock", "Scene Select"};
    REQUIRE_TRUE(combos.front()->options.size() == expectedLabels.size());
    for (std::size_t ix = 0; ix < expectedLabels.size(); ++ix) {
        REQUIRE_TRUE(combos.front()->options[ix].label == expectedLabels[ix]);
    }
    REQUIRE_TRUE(combos.front()->selectedOption == "hold-reset");
    REQUIRE_TRUE(combos[1]->selectedOption == "hold-random");
    REQUIRE_TRUE(combos[2]->selectedOption == "hold-random-mod");
    REQUIRE_TRUE(combos[3]->selectedOption == "next-bank");
    REQUIRE_TRUE(combos[4]->selectedOption == "start");
    REQUIRE_TRUE(combos[5]->selectedOption == "previous-bank");
    REQUIRE_TRUE(!FindNodeById(initialTree, "controller-wizard.twister.button.3.argument")->enabled);

    form.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.encoder-slot", "17"));
    form.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.button.3.message", "scene-select"));
    form.DispatchAction(synth::ui::Action::WithValue(
        "controller-wizard.twister.button.3.argument", "6"));
    const synth::ui::NodeTree editedTree = form.BuildTree();
    REQUIRE_TRUE(FindNodeById(editedTree, "controller-wizard.twister.encoder-slot")->text == "17");
    REQUIRE_TRUE(FindNodeById(editedTree, "controller-wizard.twister.button.3.message")->selectedOption ==
                 "scene-select");
    REQUIRE_TRUE(FindNodeById(editedTree, "controller-wizard.twister.button.3.argument")->enabled);
    REQUIRE_TRUE(FindNodeById(editedTree, "controller-wizard.twister.button.3.argument")->text == "6");

    const std::vector<std::pair<std::string, bool>> argumentEnabled = {
        {"toggle-reset", false}, {"hold-reset", false},
        {"toggle-random", false}, {"hold-random", false},
        {"toggle-random-mod", false}, {"hold-random-mod", false},
        {"toggle-gesture-select", true}, {"hold-gesture-select", true},
        {"bank-select", true}, {"next-bank", false}, {"previous-bank", false},
        {"start", false}, {"continue", false}, {"stop", false},
        {"clock", false}, {"scene-select", true}};
    for (const auto& [messageId, enabled] : argumentEnabled) {
        form.DispatchAction(synth::ui::Action::WithValue(
            "controller-wizard.twister.button.0.message", messageId));
        REQUIRE_TRUE(FindNodeById(form.BuildTree(), "controller-wizard.twister.button.0.argument")->enabled ==
                     enabled);
    }
}

TEST_CASE(MfTwisterConfigFormValidatesExactSizeTIntegerTextAndIgnoresDisabledArguments) {
    synth::MfTwisterConfigForm form;
    std::string error;

    form.encoderSlotText = "0";
    REQUIRE_TRUE(form.Validate(error));
    form.encoderSlotText = std::to_string(std::numeric_limits<std::size_t>::max());
    REQUIRE_TRUE(form.Validate(error));

    for (const std::string& invalid : std::vector<std::string>{
             {}, "-1", "1x", "   ", "184467440737095516160"}) {
        form.encoderSlotText = invalid;
        REQUIRE_TRUE(!form.Validate(error));
        REQUIRE_TRUE(!error.empty());
    }

    form.encoderSlotText = "0";
    form.buttons[0].message = synth::UISystemMessage::Start;
    form.buttons[0].argumentText = "not-a-number";
    REQUIRE_TRUE(form.Validate(error));
    form.buttons[0].message = synth::UISystemMessage::SceneSelect;
    REQUIRE_TRUE(!form.Validate(error));
    form.buttons[0].argumentText = std::to_string(std::numeric_limits<std::size_t>::max());
    REQUIRE_TRUE(form.Validate(error));
    for (const std::string& invalid : std::vector<std::string>{
             {}, "-1", "1x", "   ", "184467440737095516160"}) {
        form.buttons[0].argumentText = invalid;
        REQUIRE_TRUE(!form.Validate(error));
        REQUIRE_TRUE(!error.empty());
    }
    form.buttons[0].argumentText = "1x";
    const synth::ui::NodeTree invalidTree = form.BuildTree();
    const synth::ui::Node* argumentError =
        FindNodeById(invalidTree, "controller-wizard.twister.button.0.argument.error");
    REQUIRE_TRUE(argumentError != nullptr);
    REQUIRE_TRUE(FindParentOf(invalidTree, argumentError->id.value)->id.value ==
                 "controller-wizard.twister.button.0");
    const synth::ui::Bounds errorBounds = FormBounds(invalidTree, argumentError->id.value);
    const synth::ui::Bounds buttonBounds = FormBounds(invalidTree, "controller-wizard.twister.button.0");
    REQUIRE_TRUE(errorBounds.y + errorBounds.height <= buttonBounds.y + buttonBounds.height);
    form.buttons[0].message = synth::UISystemMessage::NextParamBank;
    REQUIRE_TRUE(form.Validate(error));
    const synth::ui::NodeTree disabledTree = form.BuildTree();
    REQUIRE_TRUE(FindNodeById(disabledTree, "controller-wizard.twister.button.0.argument.error") == nullptr);

    form.buttons[0].message = synth::UISystemMessage::ParamIncDec;
    REQUIRE_TRUE(!form.Validate(error));
    REQUIRE_TRUE(!error.empty());
}

TEST_CASE(UISystemMessageHelpersExposeCatalogLabelsAndPreserveBankSlotArguments) {
    const synth::UISystemMessageChoice* holdReset =
        synth::FindUISystemMessageChoice(synth::UISystemMessage::HoldReset);
    REQUIRE_TRUE(holdReset != nullptr);
    REQUIRE_TRUE(holdReset->label == "Hold Reset");
    REQUIRE_TRUE(synth::FindUISystemMessageChoice(synth::UISystemMessage::SceneSelect) != nullptr);

    const synth::MidiControllerSystemMessageAssociation next =
        synth::MakeUISystemMessageAssociation(
            *synth::FindUISystemMessageChoice(synth::UISystemMessage::NextParamBank), 23);
    REQUIRE_TRUE(next.press.type == synth::MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(next.press.slotIx == 23);
    REQUIRE_TRUE(next.feedback.type == synth::MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(next.feedback.slotIx == 23);
}

TEST_CASE(ConfigFormOwnsStateAndDispatchActionMutatesIt) {
    FirstForm::destroyedCount = 0;
    FirstWizard wizard;
    {
        std::unique_ptr<synth::ControllerConfigForm> form = wizard.ConfigForm(std::nullopt);
        REQUIRE_TRUE(form != nullptr);
        REQUIRE_TRUE(dynamic_cast<FirstForm*>(form.get()) != nullptr);
        REQUIRE_TRUE(dynamic_cast<FirstForm*>(form.get())->Name().empty());

        form->DispatchAction(synth::ui::Action::WithValue("set-name", "Controller One"));
        REQUIRE_TRUE(dynamic_cast<FirstForm*>(form.get())->Name() == "Controller One");
    }
    REQUIRE_TRUE(FirstForm::destroyedCount == 1);
}

TEST_CASE(TypedWizardRejectsInvalidFormBeforeGeneration) {
    FirstWizard wizard;
    std::unique_ptr<synth::ControllerConfigForm> form = wizard.ConfigForm(std::nullopt);
    const synth::ControllerWizard& baseWizard = wizard;

    const synth::WizardGenerationResult result = baseWizard.GenerateProfile(*form, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(wizard.generationCount == 0);
}

TEST_CASE(TypedWizardGeneratesProfileFromItsConcreteForm) {
    FirstWizard wizard;
    std::unique_ptr<synth::ControllerConfigForm> form = wizard.ConfigForm(std::nullopt);
    form->DispatchAction(synth::ui::Action::WithValue("set-name", "Controller One"));
    const synth::ControllerWizard& baseWizard = wizard;

    const synth::WizardGenerationResult result = baseWizard.GenerateProfile(*form, Context());

    REQUIRE_TRUE(result);
    REQUIRE_TRUE(result.controller->name == "Controller One");
    REQUIRE_TRUE(result.controller->input.identifier == "in-id");
    REQUIRE_TRUE(result.controller->output.identifier == "out-id");
    REQUIRE_TRUE(wizard.generationCount == 1);
}

TEST_CASE(TypedWizardRejectsDifferentConcreteFormWithoutGeneration) {
    FirstWizard first;
    SecondWizard second;
    std::unique_ptr<synth::ControllerConfigForm> secondForm = second.ConfigForm(std::nullopt);
    const synth::ControllerWizard& firstBase = first;

    const synth::WizardGenerationResult result = firstBase.GenerateProfile(*secondForm, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(first.generationCount == 0);
    REQUIRE_TRUE(second.generationCount == 0);
}

TEST_CASE(MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm) {
    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(LibraryTwisterRegistry(), "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(wizard != nullptr);
    REQUIRE_TRUE(wizard->Id() == "com.sheaf.midi-fighter-twister");

    std::unique_ptr<synth::ControllerConfigForm> baseForm = wizard->ConfigForm(std::nullopt);
    auto* form = dynamic_cast<synth::MfTwisterConfigForm*>(baseForm.get());
    REQUIRE_TRUE(form != nullptr);
    form->encoderSlotText = "4";
    form->buttons[0] = {.message = synth::UISystemMessage::HoldReset,
                        .argumentText = "disabled-reset-argument"};
    form->buttons[1] = {.message = synth::UISystemMessage::HoldRandom,
                        .argumentText = "disabled-random-argument"};
    form->buttons[2] = {.message = synth::UISystemMessage::HoldRandomMod,
                        .argumentText = "disabled-random-mod-argument"};
    form->buttons[3] = {.message = synth::UISystemMessage::SelectParamBank, .argumentText = "7"};
    form->buttons[4] = {.message = synth::UISystemMessage::NextParamBank,
                        .argumentText = "disabled-next-argument"};
    form->buttons[5] = {.message = synth::UISystemMessage::PrevParamBank,
                        .argumentText = "disabled-previous-argument"};

    const synth::WizardGenerationResult result = wizard->GenerateProfile(*form, Context());

    REQUIRE_TRUE(result);
    REQUIRE_TRUE(result.controller->name == "ignored-by-form");
    REQUIRE_TRUE(result.controller->kind == synth::MidiProfileKind::MfTwister);
    REQUIRE_TRUE(result.controller->disposition == synth::MidiControllerDisposition::Active);
    REQUIRE_TRUE(result.controller->wizardId == "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(result.controller->input.identifier == "in-id");
    REQUIRE_TRUE(result.controller->input.name == "Input");
    REQUIRE_TRUE(result.controller->output.identifier == "out-id");
    REQUIRE_TRUE(result.controller->output.name == "Output");

    const synth::MidiControllerProfileConfig& profile = result.controller->config;
    REQUIRE_TRUE(profile.encoderInput.has_value());
    REQUIRE_TRUE(profile.encoderInput->turns.size() == 16);
    REQUIRE_TRUE(profile.encoderInput->pushes.size() == 16);
    for (std::size_t position = 0; position < profile.encoderInput->turns.size(); ++position) {
        const synth::EncoderMidiMapping& mapping = profile.encoderInput->turns[position];
        REQUIRE_TRUE(mapping.slotIx == 4);
        REQUIRE_TRUE(mapping.position == position);
    }
    for (std::size_t position = 0; position < profile.encoderInput->pushes.size(); ++position) {
        const synth::EncoderMidiMapping& mapping = profile.encoderInput->pushes[position];
        REQUIRE_TRUE(mapping.slotIx == 4);
        REQUIRE_TRUE(mapping.position == position);
    }
    REQUIRE_TRUE(profile.encoderOutput.has_value());
    REQUIRE_TRUE(profile.encoderOutput->mappings.size() == 16);
    for (std::size_t position = 0; position < profile.encoderOutput->mappings.size(); ++position) {
        const synth::EncoderMidiOutMapping& mapping = profile.encoderOutput->mappings[position];
        REQUIRE_TRUE(mapping.slotIx == 4);
        REQUIRE_TRUE(mapping.position == position);
    }

    REQUIRE_TRUE(profile.systemMessages.size() == synth::MfTwisterConfigForm::kButtonCount);
    for (std::size_t buttonIx = 0; buttonIx < profile.systemMessages.size(); ++buttonIx) {
        const synth::MidiControllerSystemMessageAssociation& association =
            profile.systemMessages[buttonIx];
        REQUIRE_TRUE(association.control.has_value());
        REQUIRE_TRUE(association.control->channel == 3);
        REQUIRE_TRUE(association.control->cc == 8 + buttonIx);
        REQUIRE_TRUE(!association.outputFeedback);
    }
    REQUIRE_TRUE(profile.systemMessages[0].press.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(profile.systemMessages[0].press.hasBoolValue);
    REQUIRE_TRUE(profile.systemMessages[0].press.boolValue);
    REQUIRE_TRUE(profile.systemMessages[0].release.has_value());
    REQUIRE_TRUE(profile.systemMessages[0].release->type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(profile.systemMessages[0].release->hasBoolValue);
    REQUIRE_TRUE(!profile.systemMessages[0].release->boolValue);
    for (std::size_t buttonIx = 1; buttonIx <= 2; ++buttonIx) {
        REQUIRE_TRUE(profile.systemMessages[buttonIx].press.hasBoolValue);
        REQUIRE_TRUE(profile.systemMessages[buttonIx].press.boolValue);
        REQUIRE_TRUE(profile.systemMessages[buttonIx].release.has_value());
        REQUIRE_TRUE(profile.systemMessages[buttonIx].release->hasBoolValue);
        REQUIRE_TRUE(!profile.systemMessages[buttonIx].release->boolValue);
    }
    REQUIRE_TRUE(profile.systemMessages[1].press.type == synth::MessageIn::Type::ToggleRandom);
    REQUIRE_TRUE(profile.systemMessages[2].press.type == synth::MessageIn::Type::ToggleRandomMod);
    REQUIRE_TRUE(profile.systemMessages[3].press.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(profile.systemMessages[3].press.slotIx == 4);
    REQUIRE_TRUE(profile.systemMessages[3].press.bankIx == 7);
    REQUIRE_TRUE(profile.systemMessages[4].press.type == synth::MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(profile.systemMessages[4].press.slotIx == 4);
    REQUIRE_TRUE(profile.systemMessages[5].press.type == synth::MessageIn::Type::PrevParamBank);
    REQUIRE_TRUE(profile.systemMessages[5].press.slotIx == 4);
}

TEST_CASE(MfTwisterSeedExtractionRequiresOneExactRepresentableProfileShape) {
    synth::MfTwisterControllerWizard wizard;
    synth::MfTwisterConfigForm source;
    source.encoderSlotText = "4";
    source.buttons[0] = {.message = synth::UISystemMessage::ToggleGestureSelect, .argumentText = "1"};
    source.buttons[1] = {.message = synth::UISystemMessage::HoldGestureSelect, .argumentText = "2"};
    source.buttons[2] = {.message = synth::UISystemMessage::SelectParamBank, .argumentText = "3"};
    source.buttons[3] = {.message = synth::UISystemMessage::SelectParamBank, .argumentText = "7"};
    source.buttons[4] = {.message = synth::UISystemMessage::SceneSelect, .argumentText = "5"};
    source.buttons[5] = {.message = synth::UISystemMessage::PrevParamBank, .argumentText = "ignored"};
    const synth::WizardGenerationResult generated = wizard.GenerateProfile(source, Context());
    REQUIRE_TRUE(generated);

    const auto seeded = synth::ExtractMfTwisterWizardSeed(generated.controller->config);
    REQUIRE_TRUE(seeded.has_value());
    REQUIRE_TRUE(seeded->encoderSlotText == "4");
    REQUIRE_TRUE(seeded->buttons[0].message == synth::UISystemMessage::ToggleGestureSelect &&
                 seeded->buttons[0].argumentText == "1");
    REQUIRE_TRUE(seeded->buttons[1].message == synth::UISystemMessage::HoldGestureSelect &&
                 seeded->buttons[1].argumentText == "2");
    REQUIRE_TRUE(seeded->buttons[2].message == synth::UISystemMessage::SelectParamBank &&
                 seeded->buttons[2].argumentText == "3");
    REQUIRE_TRUE(seeded->buttons[3].message == synth::UISystemMessage::SelectParamBank &&
                 seeded->buttons[3].argumentText == "7");
    REQUIRE_TRUE(seeded->buttons[4].message == synth::UISystemMessage::SceneSelect &&
                 seeded->buttons[4].argumentText == "5");
    REQUIRE_TRUE(seeded->buttons[5].message == synth::UISystemMessage::PrevParamBank &&
                 seeded->buttons[5].argumentText == "0");

    const auto rejects = [&](auto mutate) {
        synth::MidiControllerProfileConfig incompatible = generated.controller->config;
        mutate(incompatible);
        REQUIRE_TRUE(!synth::ExtractMfTwisterWizardSeed(incompatible).has_value());
    };
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.analogInput = synth::AnalogMidiInConfig{};
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.pressureInput = synth::PolyphonicPressureMidiInConfig{};
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->turns.pop_back();
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->turns.push_back(config.encoderInput->turns.front());
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->pushes[0].control.cc += 1;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->pushes.pop_back();
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->turns[0].slotIx = 8;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderInput->pushes[0].slotIx = 8;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderOutput->mappings[0].position = 15;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderOutput->mappings.pop_back();
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderOutput->mappings.push_back(config.encoderOutput->mappings.front());
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.encoderOutput->mappings[0].slotIx = 8;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages.pop_back();
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages.push_back(config.systemMessages.front());
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[0].control->cc = 99;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[1].control->cc = config.systemMessages[0].control->cc;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[0].outputFeedback = true;
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[1].release.reset();
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[0].feedback = synth::MessageIn::Clock(0);
    });
    rejects([](synth::MidiControllerProfileConfig& config) {
        config.systemMessages[3].press.slotIx = 2;
        config.systemMessages[3].feedback.slotIx = 2;
    });
}

TEST_CASE(MfTwisterWizardRefusesInvalidFormsAtomically) {
    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(LibraryTwisterRegistry(), "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(wizard != nullptr);
    std::unique_ptr<synth::ControllerConfigForm> baseForm = wizard->ConfigForm(std::nullopt);
    auto* form = dynamic_cast<synth::MfTwisterConfigForm*>(baseForm.get());
    REQUIRE_TRUE(form != nullptr);
    form->encoderSlotText = "not-a-slot";

    const synth::WizardGenerationResult result = wizard->GenerateProfile(*form, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.controller.has_value());
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(form->encoderSlotText == "not-a-slot");
}

TEST_CASE(MakeControllerWizardRegistryWithEmptyCatalogReturnsTheOneTwisterDescriptor) {
    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{});

    REQUIRE_TRUE(registry.size() == 1);
    REQUIRE_TRUE(registry.front().id == "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(registry.front().displayName == "MIDI Fighter Twister");
    REQUIRE_TRUE(registry.front().kind == synth::MidiProfileKind::MfTwister);
    REQUIRE_TRUE(registry.front().inputAliases.size() == 1);
    REQUIRE_TRUE(registry.front().inputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(registry.front().outputAliases.size() == 1);
    REQUIRE_TRUE(registry.front().outputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(synth::MakeControllerWizard(registry, "missing.wizard") == nullptr);
    REQUIRE_TRUE(synth::MakeControllerWizard(registry, "com.sheaf.midi-fighter-twister") != nullptr);
}

TEST_CASE(MakeControllerWizardRegistryWithAppDefaultsReturnsOneDescriptorPerDefault) {
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.twister", "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
        {"Midi Fighter Twister"}, {"Midi Fighter Twister"}, synth::MidiControllerProfileConfig{}));
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.apc40.generic", "Akai APC40 mkII (Generic)", synth::MidiProfileKind::Generic,
        {"APC40 mkII"}, {"APC40 mkII"}, synth::MidiControllerProfileConfig{}));

    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);

    REQUIRE_TRUE(registry.size() == 2);
    REQUIRE_TRUE(registry[0].id == "froggers.twister");
    REQUIRE_TRUE(registry[0].displayName == "MIDI Fighter Twister");
    REQUIRE_TRUE(registry[0].kind == synth::MidiProfileKind::MfTwister);
    REQUIRE_TRUE(registry[0].inputAliases.size() == 1);
    REQUIRE_TRUE(registry[0].inputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(registry[0].outputAliases.size() == 1);
    REQUIRE_TRUE(registry[0].outputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(registry[1].id == "froggers.apc40.generic");
    REQUIRE_TRUE(registry[1].displayName == "Akai APC40 mkII (Generic)");
    REQUIRE_TRUE(registry[1].kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(registry[1].inputAliases.size() == 1);
    REQUIRE_TRUE(registry[1].inputAliases[0] == "APC40 mkII");
    REQUIRE_TRUE(registry[1].outputAliases.size() == 1);
    REQUIRE_TRUE(registry[1].outputAliases[0] == "APC40 mkII");
}

TEST_CASE(AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig) {
    synth::MidiControllerProfileConfig storedConfig;
    storedConfig.analogInput =
        synth::AnalogMidiInConfig{.sceneBlend = synth::MidiControlAddress{.channel = 5, .cc = 9}};

    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.apc40.generic", "Akai APC40 mkII (Generic)", synth::MidiProfileKind::Generic,
        {"APC40 mkII"}, {"APC40 mkII"}, storedConfig));

    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);
    REQUIRE_TRUE(registry.size() == 1);

    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(registry, "froggers.apc40.generic");
    REQUIRE_TRUE(wizard != nullptr);
    REQUIRE_TRUE(wizard->Id() == "froggers.apc40.generic");

    std::unique_ptr<synth::ControllerConfigForm> form = wizard->ConfigForm(std::nullopt);
    REQUIRE_TRUE(form != nullptr);
    std::string error = "not-yet-cleared";
    REQUIRE_TRUE(form->Validate(error));
    REQUIRE_TRUE(error.empty());

    const synth::WizardGenerationResult result = wizard->GenerateProfile(*form, Context());
    REQUIRE_TRUE(result);
    REQUIRE_TRUE(result.controller->name == "ignored-by-form");
    REQUIRE_TRUE(result.controller->kind == synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(result.controller->disposition == synth::MidiControllerDisposition::Active);
    REQUIRE_TRUE(result.controller->wizardId == "froggers.apc40.generic");
    REQUIRE_TRUE(result.controller->input.identifier == "in-id");
    REQUIRE_TRUE(result.controller->output.identifier == "out-id");
    REQUIRE_TRUE(result.controller->config.analogInput.has_value());
    REQUIRE_TRUE(result.controller->config.analogInput->sceneBlend.has_value());
    REQUIRE_TRUE(result.controller->config.analogInput->sceneBlend->channel == 5);
    REQUIRE_TRUE(result.controller->config.analogInput->sceneBlend->cc == 9);
}

TEST_CASE(DiscoveryWithAppRegistryClassifiesDeviceByFirstDefaultsInputAlias) {
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.twister", "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
        {"Midi Fighter Twister"}, {"Midi Fighter Twister"}, synth::MidiControllerProfileConfig{}));
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.apc40.generic", "Akai APC40 mkII (Generic)", synth::MidiProfileKind::Generic,
        {"APC40 mkII"}, {"APC40 mkII"}, synth::MidiControllerProfileConfig{}));
    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);

    const synth::MidiDeviceList devices = Devices(
        {Device("in-1", "Midi Fighter Twister")}, {Device("out-1", "Midi Fighter Twister")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{}, registry);

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "froggers.twister", "MIDI Fighter Twister",
                     synth::MidiProfileKind::MfTwister, "in-1", "out-1");
}

TEST_CASE(DiscoveryMatchesMidiFighterTwisterByCaseInsensitiveExactAlias) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-1", "mIDI fIGHTER tWISTER")},
        {Device("out-1", "MIDI FIGHTER TWISTER")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{},
                                         TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister, "in-1", "out-1");
    REQUIRE_TRUE(discovery.unmatchedInputs.empty());
    REQUIRE_TRUE(discovery.unmatchedOutputs.empty());
}

TEST_CASE(DiscoveryRejectsPrefixSuffixAndImplicitNumberVariants) {
    const synth::MidiDeviceList devices = Devices(
        {Device("prefix-in", "USB Midi Fighter Twister"),
         Device("suffix-in", "Midi Fighter Twister Port 1"),
         Device("number-in", "Midi Fighter Twister 2")},
        {Device("prefix-out", "USB Midi Fighter Twister"),
         Device("suffix-out", "Midi Fighter Twister Port 1"),
         Device("number-out", "Midi Fighter Twister 2")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{},
                                         TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.empty());
    RequireDeviceIds(discovery.unmatchedInputs, {"prefix-in", "suffix-in", "number-in"});
    RequireDeviceIds(discovery.unmatchedOutputs, {"prefix-out", "suffix-out", "number-out"});
}

TEST_CASE(DiscoveryReportsUnmatchedNamesAndHalfPairs) {
    const synth::MidiDeviceList devices = Devices(
        {Device("twister-in", "Midi Fighter Twister"),
         Device("keyboard-in", "Keyboard")},
        {Device("drum-out", "Drum Rack")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{},
                                         TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.empty());
    RequireDeviceIds(discovery.unmatchedInputs, {"twister-in", "keyboard-in"});
    RequireDeviceIds(discovery.unmatchedOutputs, {"drum-out"});
}

TEST_CASE(DiscoveryReturnsCandidatesInRegistryOrderAndUsesEndpointOnce) {
    const std::vector<synth::ControllerWizardDescriptor> registry = {
        Descriptor("wizard.alpha", "Alpha", synth::MidiProfileKind::Generic, {"Alpha"},
                   {"Alpha"}),
        Descriptor("wizard.beta", "Beta", synth::MidiProfileKind::Launchpad, {"Beta"},
                   {"Beta"}),
        Descriptor("wizard.alpha-shadow", "Alpha Shadow", synth::MidiProfileKind::WrldBldr,
                   {"Alpha"}, {"Alpha"})};
    const synth::MidiDeviceList devices = Devices(
        {Device("beta-in", "Beta"), Device("alpha-in", "Alpha")},
        {Device("beta-out", "Beta"), Device("alpha-out", "Alpha")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{}, registry);

    REQUIRE_TRUE(discovery.available.size() == 2);
    RequireCandidate(discovery.available[0], "wizard.alpha", "Alpha",
                     synth::MidiProfileKind::Generic, "alpha-in", "alpha-out");
    RequireCandidate(discovery.available[1], "wizard.beta", "Beta",
                     synth::MidiProfileKind::Launchpad, "beta-in", "beta-out");
    REQUIRE_TRUE(discovery.unmatchedInputs.empty());
    REQUIRE_TRUE(discovery.unmatchedOutputs.empty());
}

TEST_CASE(DiscoveryPairsDuplicateDevicesByEnumerationOrder) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-1", "Midi Fighter Twister"), Device("in-2", "Midi Fighter Twister")},
        {Device("out-1", "Midi Fighter Twister"), Device("out-2", "Midi Fighter Twister")});

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, synth::MidiInstrumentConfig{},
                                         TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 2);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister, "in-1", "out-1");
    RequireCandidate(discovery.available[1], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister, "in-2", "out-2");
}

TEST_CASE(DiscoveryClaimsStoredEndpointsByExactIdBeforeNameFallback) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-name-only", "Midi Fighter Twister"),
         Device("in-exact", "Midi Fighter Twister")},
        {Device("out-name-fallback", "Midi Fighter Twister"),
         Device("out-free", "Midi Fighter Twister")});
    synth::MidiInstrumentConfig instrument;
    instrument.controllers.push_back(StoredController(
        "claimed", Endpoint("in-exact", "Midi Fighter Twister"),
        Endpoint("missing-output-id", "Midi Fighter Twister")));

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
                     "in-name-only", "out-free");
}

TEST_CASE(DiscoveryDoesNotFallbackByNameWhenExactIdLostContention) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-1", "Midi Fighter Twister"), Device("in-2", "Midi Fighter Twister")},
        {Device("out-1", "Midi Fighter Twister"), Device("out-2", "Midi Fighter Twister")});
    synth::MidiInstrumentConfig instrument;
    instrument.controllers.push_back(StoredController(
        "first", Endpoint("missing-input-id", "Midi Fighter Twister"),
        Endpoint("missing-output-id", "Midi Fighter Twister")));
    instrument.controllers.push_back(StoredController(
        "second", Endpoint("in-1", "Midi Fighter Twister"),
        Endpoint("out-1", "Midi Fighter Twister")));

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
                     "in-2", "out-2");
}

TEST_CASE(DiscoveryTreatsHalfConfiguredStoredRefsAsEndpointClaims) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-claimed", "Midi Fighter Twister"),
         Device("in-free", "Midi Fighter Twister")},
        {Device("out-free-a", "Midi Fighter Twister"),
         Device("out-free-b", "Midi Fighter Twister")});
    synth::MidiInstrumentConfig instrument;
    instrument.controllers.push_back(
        StoredController("input-only", Endpoint("in-claimed", "Midi Fighter Twister"), {}));

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
                     "in-free", "out-free-a");
    RequireDeviceIds(discovery.unmatchedOutputs, {"out-free-b"});
}

TEST_CASE(DiscoverySuppressesPairsClaimedByActiveAndBlacklistedRecords) {
    const synth::MidiDeviceList devices = Devices(
        {Device("active-in", "Midi Fighter Twister"),
         Device("blacklisted-in", "Midi Fighter Twister"),
         Device("free-in", "Midi Fighter Twister")},
        {Device("active-out", "Midi Fighter Twister"),
         Device("blacklisted-out", "Midi Fighter Twister"),
         Device("free-out", "Midi Fighter Twister")});
    synth::MidiInstrumentConfig instrument;
    instrument.controllers.push_back(
        StoredController("active", Endpoint("active-in", "Midi Fighter Twister"),
                         Endpoint("active-out", "Midi Fighter Twister")));
    synth::MidiControllerSlot blacklisted =
        StoredController("blacklisted", Endpoint("blacklisted-in", "Midi Fighter Twister"),
                         Endpoint("blacklisted-out", "Midi Fighter Twister"));
    blacklisted.disposition = synth::MidiControllerDisposition::Blacklisted;
    blacklisted.wizardId = "future.vendor/unknown";
    instrument.controllers.push_back(std::move(blacklisted));

    const synth::WizardDiscovery discovery =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());

    REQUIRE_TRUE(discovery.available.size() == 1);
    RequireCandidate(discovery.available[0], "com.sheaf.midi-fighter-twister",
                     "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister, "free-in", "free-out");
    REQUIRE_TRUE(discovery.unmatchedInputs.empty());
    REQUIRE_TRUE(discovery.unmatchedOutputs.empty());
}

TEST_CASE(DiscoveryResultsAreStableAndInputsRemainUnchanged) {
    const synth::MidiDeviceList devices = Devices(
        {Device("in-1", "Midi Fighter Twister"), Device("in-2", "Keyboard")},
        {Device("out-1", "Midi Fighter Twister"), Device("out-2", "Drum Rack")});
    synth::MidiInstrumentConfig instrument;
    instrument.controllers.push_back(StoredController(
        "manual", Endpoint("missing", "Missing Device"), Endpoint("", "")));

    const synth::WizardDiscovery first =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());
    const synth::WizardDiscovery second =
        synth::DiscoverControllerWizards(devices, instrument, TestTwisterRegistry());

    REQUIRE_TRUE(first.available.size() == second.available.size());
    REQUIRE_TRUE(first.unmatchedInputs == second.unmatchedInputs);
    REQUIRE_TRUE(first.unmatchedOutputs == second.unmatchedOutputs);
    REQUIRE_TRUE(first.available.size() == 1);
    RequireCandidate(first.available[0], second.available[0].wizardId,
                     second.available[0].displayName, second.available[0].kind,
                     second.available[0].input.identifier, second.available[0].output.identifier);
    REQUIRE_TRUE(devices.inputs[0].identifier == "in-1");
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "missing");
}

int Main() {
    int failed = 0;
    for (const TestCase& test : Registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
        }
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int main() {
    return Main();
}
