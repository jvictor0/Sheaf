#include "synth/ControllerWizard.hpp"
#include "synth/MidiAppCatalog.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace synth {

// The contract is intentionally header-defined: TypedControllerWizard must be
// instantiated by each concrete portable form type.

namespace {

constexpr std::string_view kMfTwisterWizardId = "com.sheaf.midi-fighter-twister";
constexpr std::string_view kMfTwisterDisplayName = "MIDI Fighter Twister";
constexpr std::string_view kMfTwisterAlias = "Midi Fighter Twister";
constexpr std::string_view kLibraryLaunchpadWizardId = "library.launchpad";
constexpr std::string_view kLibraryWrldBldrWizardId = "library.wrldbldr";

struct TwisterMessageChoice {
    std::string_view id;
    UISystemMessage message;
};

constexpr std::array<TwisterMessageChoice, 16> kTwisterMessageChoices = {{
    {"toggle-reset", UISystemMessage::ToggleReset},
    {"hold-reset", UISystemMessage::HoldReset},
    {"toggle-random", UISystemMessage::ToggleRandom},
    {"hold-random", UISystemMessage::HoldRandom},
    {"toggle-random-mod", UISystemMessage::ToggleRandomMod},
    {"hold-random-mod", UISystemMessage::HoldRandomMod},
    {"toggle-gesture-select", UISystemMessage::ToggleGestureSelect},
    {"hold-gesture-select", UISystemMessage::HoldGestureSelect},
    {"bank-select", UISystemMessage::SelectParamBank},
    {"next-bank", UISystemMessage::NextParamBank},
    {"previous-bank", UISystemMessage::PrevParamBank},
    {"start", UISystemMessage::Start},
    {"continue", UISystemMessage::Continue},
    {"stop", UISystemMessage::Stop},
    {"clock", UISystemMessage::Clock},
    {"scene-select", UISystemMessage::SceneSelect},
}};

std::string MessageOptionId(UISystemMessage message) {
    for (const TwisterMessageChoice& choice : kTwisterMessageChoices) {
        if (choice.message == message) {
            return std::string(choice.id);
        }
    }
    return {};
}

bool TwisterMessageAllowed(UISystemMessage message) {
    return !MessageOptionId(message).empty();
}

// This is form policy, deliberately narrower than UISystemMessageHasArg().
// In particular, Next/Previous Bank take their slot from Encoder Slot rather
// than from a per-button argument field.
bool TwisterArgumentEnabled(UISystemMessage message) {
    switch (message) {
        case UISystemMessage::ToggleGestureSelect:
        case UISystemMessage::HoldGestureSelect:
        case UISystemMessage::SelectParamBank:
        case UISystemMessage::SceneSelect:
            return true;
        case UISystemMessage::ParamIncDec:
        case UISystemMessage::ParamSetAbsolute:
        case UISystemMessage::ParamPush:
        case UISystemMessage::ToggleReset:
        case UISystemMessage::HoldReset:
        case UISystemMessage::ToggleRandom:
        case UISystemMessage::HoldRandom:
        case UISystemMessage::ToggleRandomMod:
        case UISystemMessage::HoldRandomMod:
        case UISystemMessage::Start:
        case UISystemMessage::Continue:
        case UISystemMessage::Stop:
        case UISystemMessage::Clock:
        case UISystemMessage::SetGestureValue:
        case UISystemMessage::SetSceneBlend:
        case UISystemMessage::NextParamBank:
        case UISystemMessage::PrevParamBank:
        case UISystemMessage::AppAction:
        case UISystemMessage::HoldDrill:
        case UISystemMessage::Shift:
            return false;
    }
    return false;
}

bool ParseSizeT(std::string_view text, std::size_t& result) {
    if (text.empty()) {
        return false;
    }
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto parsed = std::from_chars(first, last, result, 10);
    return parsed.ec == std::errc{} && parsed.ptr == last;
}

std::size_t ParseSizeTOrAssert(std::string_view text) {
    std::size_t result = 0;
    [[maybe_unused]] const bool parsed = ParseSizeT(text, result);
    assert(parsed);
    return result;
}

bool CaseInsensitiveEquals(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t ix = 0; ix < lhs.size(); ++ix) {
        const auto left = static_cast<unsigned char>(lhs[ix]);
        const auto right = static_cast<unsigned char>(rhs[ix]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

void ClaimEndpoint(const MidiEndpointRef& ref,
                   const std::vector<MidiDeviceInfoRef>& devices,
                   std::vector<bool>& claimed) {
    if (!ref.IsConfigured()) {
        return;
    }

    if (!ref.identifier.empty()) {
        for (std::size_t ix = 0; ix < devices.size(); ++ix) {
            if (devices[ix].identifier == ref.identifier) {
                if (!claimed[ix]) {
                    claimed[ix] = true;
                }
                return;
            }
        }
    }

    if (!ref.name.empty()) {
        for (std::size_t ix = 0; ix < devices.size(); ++ix) {
            if (!claimed[ix] && devices[ix].name == ref.name) {
                claimed[ix] = true;
                return;
            }
        }
    }
}

std::vector<bool> ClaimedInputs(const MidiDeviceList& devices,
                                const MidiInstrumentConfig& instrument) {
    std::vector<bool> claimed(devices.inputs.size(), false);
    for (const MidiControllerSlot& slot : instrument.controllers) {
        ClaimEndpoint(slot.input, devices.inputs, claimed);
    }
    return claimed;
}

std::vector<bool> ClaimedOutputs(const MidiDeviceList& devices,
                                 const MidiInstrumentConfig& instrument) {
    std::vector<bool> claimed(devices.outputs.size(), false);
    for (const MidiControllerSlot& slot : instrument.controllers) {
        ClaimEndpoint(slot.output, devices.outputs, claimed);
    }
    return claimed;
}

std::vector<std::size_t> MatchingUnclaimedEndpoints(
    const std::vector<MidiDeviceInfoRef>& devices,
    const std::vector<bool>& claimed,
    const std::vector<bool>& assigned,
    const std::vector<std::string>& aliases) {
    std::vector<std::size_t> matches;
    for (std::size_t ix = 0; ix < devices.size(); ++ix) {
        if (!claimed[ix] && !assigned[ix] && MatchesAnyAlias(devices[ix].name, aliases)) {
            matches.push_back(ix);
        }
    }
    return matches;
}

std::vector<MidiDeviceInfoRef> UnmatchedEndpoints(
    const std::vector<MidiDeviceInfoRef>& devices,
    const std::vector<bool>& claimed,
    const std::vector<bool>& assigned) {
    std::vector<MidiDeviceInfoRef> unmatched;
    for (std::size_t ix = 0; ix < devices.size(); ++ix) {
        if (!claimed[ix] && !assigned[ix]) {
            unmatched.push_back(devices[ix]);
        }
    }
    return unmatched;
}

}  // namespace

bool MatchesAnyAlias(std::string_view name, const std::vector<std::string>& aliases) {
    return std::any_of(aliases.begin(), aliases.end(), [name](const std::string& alias) {
        return CaseInsensitiveEquals(name, alias);
    });
}

MfTwisterConfigForm::MfTwisterConfigForm() {
    buttons[0].message = UISystemMessage::HoldReset;
    buttons[1].message = UISystemMessage::HoldRandom;
    buttons[2].message = UISystemMessage::HoldRandomMod;
    buttons[3].message = UISystemMessage::NextParamBank;
    buttons[4].message = UISystemMessage::Start;
    buttons[5].message = UISystemMessage::PrevParamBank;
}

std::string_view MfTwisterConfigForm::WizardId() const {
    return kMfTwisterWizardId;
}

bool MfTwisterConfigForm::Validate(std::string& error) const {
    std::size_t parsed = 0;
    if (!ParseSizeT(encoderSlotText, parsed)) {
        error = "Encoder Slot must be a non-negative base-10 integer";
        return false;
    }
    for (std::size_t buttonIx = 0; buttonIx < buttons.size(); ++buttonIx) {
        const MfTwisterButtonConfig& button = buttons[buttonIx];
        if (!TwisterMessageAllowed(button.message)) {
            error = "Button " + std::to_string(buttonIx + 1) + " has an unsupported Twister message";
            return false;
        }
        if (TwisterArgumentEnabled(button.message) && !ParseSizeT(button.argumentText, parsed)) {
            error = "Button " + std::to_string(buttonIx + 1) +
                    " argument must be a non-negative base-10 integer";
            return false;
        }
    }
    error.clear();
    return true;
}

std::string_view MfTwisterControllerWizard::Id() const {
    return kMfTwisterWizardId;
}

std::unique_ptr<ControllerConfigForm> MfTwisterControllerWizard::ConfigForm() const {
    return std::make_unique<MfTwisterConfigForm>();
}

WizardGenerationResult MfTwisterControllerWizard::GenerateTypedProfile(
    const MfTwisterConfigForm& form, const WizardGenerationContext& context) const {
    const std::size_t encoderSlot = ParseSizeTOrAssert(form.encoderSlotText);
    MfTwisterDefaultProfileOptions options;
    options.slotIx = encoderSlot;

    for (std::size_t buttonIx = 0; buttonIx < form.buttons.size(); ++buttonIx) {
        const MfTwisterButtonConfig& button = form.buttons[buttonIx];
        const std::size_t argument =
            TwisterArgumentEnabled(button.message) ? ParseSizeTOrAssert(button.argumentText) : 0;
        MidiControllerSystemMessageAssociation association =
            MakeUISystemMessageAssociation(*FindUISystemMessageChoice(button.message), argument);

        if (button.message == UISystemMessage::SelectParamBank ||
            button.message == UISystemMessage::NextParamBank ||
            button.message == UISystemMessage::PrevParamBank) {
            association.press.slotIx = encoderSlot;
            association.feedback.slotIx = encoderSlot;
            if (association.release.has_value()) {
                association.release->slotIx = encoderSlot;
            }
        }
        options.sideButtons[buttonIx] = std::move(association);
    }

    MidiControllerSlot controller;
    controller.name = context.name;
    controller.kind = MidiProfileKind::MfTwister;
    controller.disposition = MidiControllerDisposition::Active;
    controller.wizardId = std::string(Id());
    controller.config = MfTwisterDefaultProfileConfig(std::move(options));
    controller.input = context.input;
    controller.output = context.output;
    return {.controller = std::move(controller)};
}

namespace {

// An app default's wizard form: no fields, since the default's config is
// fixed and nothing on it is user-editable. No form is presented to the
// player; this is generation-policy state only, read by GenerateProfile.
class AppDefaultConfigForm final : public ControllerConfigForm {
public:
    explicit AppDefaultConfigForm(std::string wizardId) : wizardId_(std::move(wizardId)) {}

    std::string_view WizardId() const override { return wizardId_; }

    bool Validate(std::string& error) const override {
        error.clear();
        return true;
    }

private:
    std::string wizardId_;
};

// The wizard behind an app default's add-row preset entry: it does not derive
// its result from any form input, only from the default's own config, so it
// implements ControllerWizard directly rather than through
// TypedControllerWizard.
class AppDefaultControllerWizard final : public ControllerWizard {
public:
    AppDefaultControllerWizard(std::string id, MidiProfileKind kind, MidiControllerProfileConfig config)
        : id_(std::move(id)), kind_(kind), config_(std::move(config)) {}

    std::string_view Id() const override { return id_; }

    std::unique_ptr<ControllerConfigForm> ConfigForm() const override {
        return std::make_unique<AppDefaultConfigForm>(id_);
    }

    WizardGenerationResult GenerateProfile(
        const ControllerConfigForm&, const WizardGenerationContext& context) const override {
        MidiControllerSlot controller;
        controller.name = context.name;
        controller.kind = kind_;
        controller.disposition = MidiControllerDisposition::Active;
        controller.wizardId = id_;
        controller.config = config_;
        controller.input = context.input;
        controller.output = context.output;
        return {.controller = std::move(controller)};
    }

private:
    std::string id_;
    MidiProfileKind kind_;
    MidiControllerProfileConfig config_;
};

}  // namespace

// A device must stay reachable as a starting point in every app: the
// catalog's own devices come first, then one library descriptor for each
// kind in catalog.libraryDeviceKinds the catalog has no device of, so an app
// that lists a kind (or names no device of it) still offers it. An app whose
// catalog already covers a kind keeps only its own device(s) for it -- the
// library entry would otherwise duplicate a kind the operator already sees.
// A kind absent from libraryDeviceKinds gets no library descriptor even when
// the catalog has no device of it, so an app can offer only its own presets.
std::vector<ControllerWizardDescriptor> MakeControllerWizardRegistry(const MidiAppCatalog& catalog) {
    std::vector<ControllerWizardDescriptor> registry;
    registry.reserve(catalog.deviceDefaults.size() + catalog.libraryDeviceKinds.size());
    for (const MidiAppDeviceDefault& deviceDefault : catalog.deviceDefaults) {
        registry.push_back(ControllerWizardDescriptor{
            .id = deviceDefault.id,
            .displayName = deviceDefault.displayName,
            .kind = deviceDefault.kind,
            .inputAliases = deviceDefault.inputAliases,
            .outputAliases = deviceDefault.outputAliases,
            .factory = [deviceDefault] {
                return std::make_unique<AppDefaultControllerWizard>(
                    deviceDefault.id, deviceDefault.kind, deviceDefault.config);
            }});
    }

    const auto catalogCovers = [&catalog](MidiProfileKind kind) {
        return std::any_of(catalog.deviceDefaults.begin(), catalog.deviceDefaults.end(),
                           [kind](const MidiAppDeviceDefault& deviceDefault) {
                               return deviceDefault.kind == kind;
                           });
    };

    const auto libraryOffers = [&catalog](MidiProfileKind kind) {
        return std::find(catalog.libraryDeviceKinds.begin(), catalog.libraryDeviceKinds.end(), kind) !=
               catalog.libraryDeviceKinds.end();
    };

    if (libraryOffers(MidiProfileKind::MfTwister) && !catalogCovers(MidiProfileKind::MfTwister)) {
        registry.push_back(ControllerWizardDescriptor{
            .id = std::string(kMfTwisterWizardId),
            .displayName = std::string(kMfTwisterDisplayName),
            .kind = MidiProfileKind::MfTwister,
            .inputAliases = {std::string(kMfTwisterAlias)},
            .outputAliases = {std::string(kMfTwisterAlias)},
            .factory = [] { return std::make_unique<MfTwisterControllerWizard>(); }});
    }

    // Every non-MfTwister library device shares the same descriptor shape --
    // an id, the kind's own display name, no aliases (it names no specific
    // physical device), and a factory that installs that kind's default
    // profile (DefaultProfileConfigForKind, the same one place
    // MidiConfigViewModel::AddController chooses a kind's default from)
    // through AppDefaultControllerWizard -- so one loop over their (kind,
    // id) pairs replaces a copy-pasted block per kind.
    struct LibraryDevice {
        MidiProfileKind kind;
        std::string_view id;
    };
    const std::array<LibraryDevice, 2> libraryDevices = {{
        {MidiProfileKind::Launchpad, kLibraryLaunchpadWizardId},
        {MidiProfileKind::WrldBldr, kLibraryWrldBldrWizardId},
    }};
    for (const LibraryDevice& library : libraryDevices) {
        if (!libraryOffers(library.kind) || catalogCovers(library.kind)) {
            continue;
        }
        std::string id(library.id);
        registry.push_back(ControllerWizardDescriptor{
            .id = id,
            .displayName = MidiProfileKindDisplayName(library.kind),
            .kind = library.kind,
            .inputAliases = {},
            .outputAliases = {},
            .factory = [id, kind = library.kind] {
                return std::make_unique<AppDefaultControllerWizard>(id, kind, DefaultProfileConfigForKind(kind));
            }});
    }
    return registry;
}

WizardDiscovery DiscoverControllerWizards(
    const MidiDeviceList& devices, const MidiInstrumentConfig& instrument,
    const std::vector<ControllerWizardDescriptor>& registry) {
    WizardDiscovery discovery;
    std::vector<bool> claimedInputs = ClaimedInputs(devices, instrument);
    std::vector<bool> claimedOutputs = ClaimedOutputs(devices, instrument);
    std::vector<bool> assignedInputs(devices.inputs.size(), false);
    std::vector<bool> assignedOutputs(devices.outputs.size(), false);

    for (const ControllerWizardDescriptor& descriptor : registry) {
        std::vector<std::size_t> inputMatches = MatchingUnclaimedEndpoints(
            devices.inputs, claimedInputs, assignedInputs, descriptor.inputAliases);
        std::vector<std::size_t> outputMatches = MatchingUnclaimedEndpoints(
            devices.outputs, claimedOutputs, assignedOutputs, descriptor.outputAliases);
        const std::size_t pairCount = std::min(inputMatches.size(), outputMatches.size());

        for (std::size_t pairIx = 0; pairIx < pairCount; ++pairIx) {
            const std::size_t inputIx = inputMatches[pairIx];
            const std::size_t outputIx = outputMatches[pairIx];
            assignedInputs[inputIx] = true;
            assignedOutputs[outputIx] = true;
            discovery.available.push_back({
                .wizardId = descriptor.id,
                .displayName = descriptor.displayName,
                .kind = descriptor.kind,
                .input = devices.inputs[inputIx],
                .output = devices.outputs[outputIx],
            });
        }
    }

    discovery.unmatchedInputs =
        UnmatchedEndpoints(devices.inputs, claimedInputs, assignedInputs);
    discovery.unmatchedOutputs =
        UnmatchedEndpoints(devices.outputs, claimedOutputs, assignedOutputs);
    return discovery;
}

const ControllerWizardDescriptor* FindControllerWizardDescriptor(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id) {
    for (const ControllerWizardDescriptor& descriptor : registry) {
        if (descriptor.id == id) {
            return &descriptor;
        }
    }
    return nullptr;
}

std::unique_ptr<ControllerWizard> MakeControllerWizard(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id) {
    const ControllerWizardDescriptor* descriptor = FindControllerWizardDescriptor(registry, id);
    if (descriptor == nullptr || !descriptor->factory) {
        return nullptr;
    }
    return descriptor->factory();
}

}  // namespace synth
