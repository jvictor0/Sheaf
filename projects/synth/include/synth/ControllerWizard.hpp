#pragma once

#include "synth/MidiController.hpp"
#include "synth/MidiConfigViewModel.hpp"
#include "synth/MidiReconcile.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth {

// Portable wizard-owned form state: generation policy only, read by
// GenerateProfile below. It carries no rendering or action-dispatch surface.
class ControllerConfigForm {
public:
    virtual ~ControllerConfigForm() = default;
    virtual std::string_view WizardId() const = 0;
    virtual bool Validate(std::string& error) const = 0;
};

struct MfTwisterButtonConfig {
    UISystemMessage message;
    std::string argumentText = "0";
};

class MfTwisterConfigForm final : public ControllerConfigForm {
public:
    static constexpr std::size_t kButtonCount = 6;

    MfTwisterConfigForm();

    std::string encoderSlotText = "0";
    std::array<MfTwisterButtonConfig, kButtonCount> buttons;

    std::string_view WizardId() const override;
    bool Validate(std::string& error) const override;
};

struct WizardGenerationContext {
    std::string name;
    MidiEndpointRef input;
    MidiEndpointRef output;
};

struct WizardGenerationResult {
    std::optional<MidiControllerSlot> controller;
    std::string error;
    explicit operator bool() const { return controller.has_value(); }
};

class ControllerWizard {
public:
    virtual ~ControllerWizard() = default;
    virtual std::string_view Id() const = 0;
    virtual std::unique_ptr<ControllerConfigForm> ConfigForm() const = 0;
    virtual WizardGenerationResult GenerateProfile(
        const ControllerConfigForm&, const WizardGenerationContext&) const = 0;
};

template <class Form>
class TypedControllerWizard : public ControllerWizard {
    static_assert(std::derived_from<Form, ControllerConfigForm>);

public:
    WizardGenerationResult GenerateProfile(
        const ControllerConfigForm& form, const WizardGenerationContext& context) const final {
        const auto* typedForm = dynamic_cast<const Form*>(&form);
        assert(typedForm != nullptr);
        if (typedForm == nullptr) {
            return {.error = "controller wizard form type mismatch"};
        }

        std::string error;
        if (!typedForm->Validate(error)) {
            if (error.empty()) {
                error = "controller wizard form is invalid";
            }
            return {.error = std::move(error)};
        }
        return GenerateTypedProfile(*typedForm, context);
    }

protected:
    virtual WizardGenerationResult GenerateTypedProfile(
        const Form&, const WizardGenerationContext&) const = 0;
};

class MfTwisterControllerWizard final : public TypedControllerWizard<MfTwisterConfigForm> {
public:
    std::string_view Id() const override;
    std::unique_ptr<ControllerConfigForm> ConfigForm() const override;

protected:
    WizardGenerationResult GenerateTypedProfile(
        const MfTwisterConfigForm&, const WizardGenerationContext&) const override;
};

struct WizardCandidate {
    std::string wizardId;
    std::string displayName;
    MidiProfileKind kind;
    MidiDeviceInfoRef input;
    MidiDeviceInfoRef output;
};

struct WizardDiscovery {
    std::vector<WizardCandidate> available;
    std::vector<MidiDeviceInfoRef> unmatchedInputs;
    std::vector<MidiDeviceInfoRef> unmatchedOutputs;
};

struct ControllerWizardDescriptor {
    std::string id;
    std::string displayName;
    MidiProfileKind kind;
    std::vector<std::string> inputAliases;
    std::vector<std::string> outputAliases;
    std::function<std::unique_ptr<ControllerWizard>()> factory;
};

// The registry the Controllers page's add row and discovery draw from, and
// that a row's stored wizard id resolves against: one descriptor per app
// device default, then one library descriptor for each kind in the
// catalog's libraryDeviceKinds that the catalog has no device default of --
// so a device stays reachable as a starting point in every app whose
// catalog lists that kind.
std::vector<ControllerWizardDescriptor> MakeControllerWizardRegistry(const MidiAppCatalog& catalog);
WizardDiscovery DiscoverControllerWizards(
    const MidiDeviceList&, const MidiInstrumentConfig&,
    const std::vector<ControllerWizardDescriptor>&);

// Case-insensitive membership test against a descriptor's own input or output
// aliases: the one match rule discovery uses to bind a connected port to a
// preset. Shared with the Controllers page so it can list every preset a
// waiting device matches, not only the one discovery bound the pair to.
bool MatchesAnyAlias(std::string_view name, const std::vector<std::string>& aliases);

// The one lookup every caller that needs "does this stored wizard id name a
// registry descriptor, and if so which" shares: MakeControllerWizard below,
// MidiConfigViewModel's hasResolvedWizard/matchesWizardProfile/
// RestoreController, and the page's device label (ControllerDeviceLabel).
// Returns nullptr for an id no descriptor carries.
const ControllerWizardDescriptor* FindControllerWizardDescriptor(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id);

std::unique_ptr<ControllerWizard> MakeControllerWizard(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id);

}  // namespace synth
