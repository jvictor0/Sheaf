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

    void SetName(std::string name) { name_ = std::move(name); }
    std::string_view Name() const { return name_; }

    static int destroyedCount;

private:
    std::string name_;
};

int FirstForm::destroyedCount = 0;

class SecondForm final : public synth::ControllerConfigForm {
public:
    std::string_view WizardId() const override { return "test.second"; }

    bool Validate(std::string& error) const override {
        error.clear();
        return true;
    }
};

class FirstWizard final : public synth::TypedControllerWizard<FirstForm> {
public:
    std::string_view Id() const override { return "test.first"; }

    std::unique_ptr<synth::ControllerConfigForm> ConfigForm() const override {
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

    std::unique_ptr<synth::ControllerConfigForm> ConfigForm() const override {
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
// library's own MfTwister, Launchpad and WRLD.Bldr descriptors, each with a
// working factory.
std::vector<synth::ControllerWizardDescriptor> EmptyCatalogLibraryRegistry() {
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

    form.buttons[0].message = synth::UISystemMessage::NextParamBank;
    REQUIRE_TRUE(form.Validate(error));

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

TEST_CASE(TypedWizardRejectsInvalidFormBeforeGeneration) {
    FirstWizard wizard;
    std::unique_ptr<synth::ControllerConfigForm> form = wizard.ConfigForm();
    const synth::ControllerWizard& baseWizard = wizard;

    const synth::WizardGenerationResult result = baseWizard.GenerateProfile(*form, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(wizard.generationCount == 0);
}

TEST_CASE(TypedWizardGeneratesProfileFromItsConcreteForm) {
    FirstWizard wizard;
    std::unique_ptr<synth::ControllerConfigForm> form = wizard.ConfigForm();
    dynamic_cast<FirstForm&>(*form).SetName("Controller One");
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
    std::unique_ptr<synth::ControllerConfigForm> secondForm = second.ConfigForm();
    const synth::ControllerWizard& firstBase = first;

    const synth::WizardGenerationResult result = firstBase.GenerateProfile(*secondForm, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(first.generationCount == 0);
    REQUIRE_TRUE(second.generationCount == 0);
}

TEST_CASE(MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm) {
    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(EmptyCatalogLibraryRegistry(), "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(wizard != nullptr);
    REQUIRE_TRUE(wizard->Id() == "com.sheaf.midi-fighter-twister");

    std::unique_ptr<synth::ControllerConfigForm> baseForm = wizard->ConfigForm();
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

TEST_CASE(MfTwisterWizardRefusesInvalidFormsAtomically) {
    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(EmptyCatalogLibraryRegistry(), "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(wizard != nullptr);
    std::unique_ptr<synth::ControllerConfigForm> baseForm = wizard->ConfigForm();
    auto* form = dynamic_cast<synth::MfTwisterConfigForm*>(baseForm.get());
    REQUIRE_TRUE(form != nullptr);
    form->encoderSlotText = "not-a-slot";

    const synth::WizardGenerationResult result = wizard->GenerateProfile(*form, Context());

    REQUIRE_TRUE(!result);
    REQUIRE_TRUE(!result.controller.has_value());
    REQUIRE_TRUE(!result.error.empty());
    REQUIRE_TRUE(form->encoderSlotText == "not-a-slot");
}

TEST_CASE(MakeControllerWizardRegistryWithEmptyCatalogReturnsTwisterLaunchpadAndWrldBldr) {
    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{});

    REQUIRE_TRUE(registry.size() == 3);
    REQUIRE_TRUE(registry[0].id == "com.sheaf.midi-fighter-twister");
    REQUIRE_TRUE(registry[0].displayName == "MIDI Fighter Twister");
    REQUIRE_TRUE(registry[0].kind == synth::MidiProfileKind::MfTwister);
    REQUIRE_TRUE(registry[0].inputAliases.size() == 1);
    REQUIRE_TRUE(registry[0].inputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(registry[0].outputAliases.size() == 1);
    REQUIRE_TRUE(registry[0].outputAliases[0] == "Midi Fighter Twister");
    REQUIRE_TRUE(synth::MakeControllerWizard(registry, "missing.wizard") == nullptr);
    REQUIRE_TRUE(synth::MakeControllerWizard(registry, "com.sheaf.midi-fighter-twister") != nullptr);

    REQUIRE_TRUE(registry[1].id == "library.launchpad");
    REQUIRE_TRUE(registry[1].displayName == "Launchpad");
    REQUIRE_TRUE(registry[1].kind == synth::MidiProfileKind::Launchpad);
    REQUIRE_TRUE(registry[1].inputAliases.empty());
    REQUIRE_TRUE(registry[1].outputAliases.empty());

    REQUIRE_TRUE(registry[2].id == "library.wrldbldr");
    REQUIRE_TRUE(registry[2].displayName == "WRLD.Bldr");
    REQUIRE_TRUE(registry[2].kind == synth::MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(registry[2].inputAliases.empty());
    REQUIRE_TRUE(registry[2].outputAliases.empty());

    std::unique_ptr<synth::ControllerWizard> launchpadWizard =
        synth::MakeControllerWizard(registry, "library.launchpad");
    REQUIRE_TRUE(launchpadWizard != nullptr);
    std::unique_ptr<synth::ControllerConfigForm> launchpadForm = launchpadWizard->ConfigForm();
    REQUIRE_TRUE(launchpadForm != nullptr);
    const synth::WizardGenerationResult launchpadResult =
        launchpadWizard->GenerateProfile(*launchpadForm, Context());
    REQUIRE_TRUE(launchpadResult);
    REQUIRE_TRUE(launchpadResult.controller->kind == synth::MidiProfileKind::Launchpad);
    REQUIRE_TRUE(launchpadResult.controller->wizardId == "library.launchpad");
    const synth::MidiControllerProfileConfig expectedLaunchpad = synth::LaunchpadDefaultProfileConfig();
    {
        synth::JsonArena arena(1024 * 1024);
        const std::string actualJson =
            synth::ToJSON(arena, launchpadResult.controller->config).Dumps(0);
        const std::string expectedJson = synth::ToJSON(arena, expectedLaunchpad).Dumps(0);
        REQUIRE_TRUE(actualJson == expectedJson);
    }

    std::unique_ptr<synth::ControllerWizard> wrldbldrWizard =
        synth::MakeControllerWizard(registry, "library.wrldbldr");
    REQUIRE_TRUE(wrldbldrWizard != nullptr);
    std::unique_ptr<synth::ControllerConfigForm> wrldbldrForm = wrldbldrWizard->ConfigForm();
    REQUIRE_TRUE(wrldbldrForm != nullptr);
    const synth::WizardGenerationResult wrldbldrResult =
        wrldbldrWizard->GenerateProfile(*wrldbldrForm, Context());
    REQUIRE_TRUE(wrldbldrResult);
    REQUIRE_TRUE(wrldbldrResult.controller->kind == synth::MidiProfileKind::WrldBldr);
    REQUIRE_TRUE(wrldbldrResult.controller->wizardId == "library.wrldbldr");
    const synth::MidiControllerProfileConfig expectedWrldBldr = synth::WrldBldrDefaultProfileConfig();
    {
        synth::JsonArena arena(1024 * 1024);
        const std::string actualJson =
            synth::ToJSON(arena, wrldbldrResult.controller->config).Dumps(0);
        const std::string expectedJson = synth::ToJSON(arena, expectedWrldBldr).Dumps(0);
        REQUIRE_TRUE(actualJson == expectedJson);
    }
}

TEST_CASE(MakeControllerWizardRegistryReturnsCatalogDefaultsThenLibraryForUncoveredKinds) {
    // The catalog covers MfTwister and Generic; Launchpad and WRLD.Bldr have
    // no catalog device, so a device of each stays reachable as a starting
    // point via the library descriptors, appended after the catalog's own
    // (frogg3rs' real shape: its six devices cover MfTwister/Generic/
    // Launchpad, so only WRLD.Bldr gets a library descriptor appended).
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.twister", "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
        {"Midi Fighter Twister"}, {"Midi Fighter Twister"}, synth::MidiControllerProfileConfig{}));
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.apc40.generic", "Akai APC40 mkII (Generic)", synth::MidiProfileKind::Generic,
        {"APC40 mkII"}, {"APC40 mkII"}, synth::MidiControllerProfileConfig{}));

    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);

    REQUIRE_TRUE(registry.size() == 4);
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
    REQUIRE_TRUE(registry[2].id == "library.launchpad");
    REQUIRE_TRUE(registry[2].displayName == "Launchpad");
    REQUIRE_TRUE(registry[2].kind == synth::MidiProfileKind::Launchpad);
    REQUIRE_TRUE(registry[3].id == "library.wrldbldr");
    REQUIRE_TRUE(registry[3].displayName == "WRLD.Bldr");
    REQUIRE_TRUE(registry[3].kind == synth::MidiProfileKind::WrldBldr);
}

TEST_CASE(MakeControllerWizardRegistryOmitsLibraryDevicesForKindsTheCatalogAlreadyCovers) {
    // frogg3rs' real shape: MfTwister, Generic and Launchpad are all covered
    // by catalog devices (two Launchpad-kind entries here stand in for its
    // three), so only WRLD.Bldr -- the one kind with no catalog device --
    // gets a library descriptor appended.
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.twister", "MIDI Fighter Twister", synth::MidiProfileKind::MfTwister,
        {"Midi Fighter Twister"}, {"Midi Fighter Twister"}, synth::MidiControllerProfileConfig{}));
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.apc40.generic", "Akai APC40 mkII (Generic)", synth::MidiProfileKind::Generic,
        {"APC40 mkII"}, {"APC40 mkII"}, synth::MidiControllerProfileConfig{}));
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.launchpad.x", "Launchpad X", synth::MidiProfileKind::Launchpad,
        {"Launchpad X"}, {"Launchpad X"}, synth::MidiControllerProfileConfig{}));

    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);

    REQUIRE_TRUE(registry.size() == 4);
    REQUIRE_TRUE(registry[0].id == "froggers.twister");
    REQUIRE_TRUE(registry[1].id == "froggers.apc40.generic");
    REQUIRE_TRUE(registry[2].id == "froggers.launchpad.x");
    REQUIRE_TRUE(registry[3].id == "library.wrldbldr");
    REQUIRE_TRUE(registry[3].displayName == "WRLD.Bldr");
    REQUIRE_TRUE(registry[3].kind == synth::MidiProfileKind::WrldBldr);
}

// The suppression rule is symmetric across the two library kinds: a catalog
// covering WRLD.Bldr must omit exactly that library entry (and no other),
// while a catalog covering neither still gets both. Without this, a check
// that always appended the WRLD.Bldr library entry regardless of catalog
// coverage would pass every other registry test in this file, since none of
// them give WRLD.Bldr its own catalog device to be covered by.
TEST_CASE(MakeControllerWizardRegistryOmitsTheLibraryWrldBldrEntryWhenTheCatalogAlreadyCoversWrldBldr) {
    synth::MidiAppCatalog catalog;
    catalog.deviceDefaults.push_back(AppDefault(
        "froggers.wrld", "WRLD.Bldr", synth::MidiProfileKind::WrldBldr, {"WRLD.Bldr"}, {"WRLD.Bldr"},
        synth::MidiControllerProfileConfig{}));

    const std::vector<synth::ControllerWizardDescriptor> registry =
        synth::MakeControllerWizardRegistry(catalog);

    // The catalog's own WRLD.Bldr device, plus library entries for the two
    // kinds it does not cover (MfTwister, Launchpad) -- not a third entry
    // for WRLD.Bldr.
    REQUIRE_TRUE(registry.size() == 3);
    REQUIRE_TRUE(registry[0].id == "froggers.wrld");
    bool sawLibraryWrldBldr = false;
    bool sawMfTwister = false;
    bool sawLibraryLaunchpad = false;
    for (const synth::ControllerWizardDescriptor& descriptor : registry) {
        sawLibraryWrldBldr = sawLibraryWrldBldr || descriptor.id == "library.wrldbldr";
        sawMfTwister = sawMfTwister || descriptor.kind == synth::MidiProfileKind::MfTwister;
        sawLibraryLaunchpad = sawLibraryLaunchpad || descriptor.id == "library.launchpad";
    }
    // A catalog-covered WRLD.Bldr gets no library.wrldbldr entry, while
    // MfTwister and Launchpad -- kinds this catalog does not cover -- still
    // get theirs.
    REQUIRE_TRUE(!sawLibraryWrldBldr);
    REQUIRE_TRUE(sawMfTwister);
    REQUIRE_TRUE(sawLibraryLaunchpad);
}

// An app whose catalog sets an empty libraryDeviceKinds (Frogg3rs) offers
// only its own presets: no library descriptor is appended for a kind its
// device defaults do not cover, even though the default libraryDeviceKinds
// would otherwise offer one.
TEST_CASE(MakeControllerWizardRegistryWithEmptyLibraryDeviceKindsReturnsOnlyCatalogDefaults) {
    synth::MidiAppCatalog catalog;
    catalog.libraryDeviceKinds.clear();
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
    REQUIRE_TRUE(registry[1].id == "froggers.apc40.generic");
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
    // The one catalog device plus a library descriptor for each of the three
    // kinds this Generic-only catalog does not cover (MfTwister, Launchpad,
    // WRLD.Bldr); this test is about AppDefaultControllerWizard's own
    // descriptor, unaffected by the other three being present too.
    REQUIRE_TRUE(registry.size() == 4);

    std::unique_ptr<synth::ControllerWizard> wizard =
        synth::MakeControllerWizard(registry, "froggers.apc40.generic");
    REQUIRE_TRUE(wizard != nullptr);
    REQUIRE_TRUE(wizard->Id() == "froggers.apc40.generic");

    std::unique_ptr<synth::ControllerConfigForm> form = wizard->ConfigForm();
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
