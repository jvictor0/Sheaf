#pragma once

#include "synth/MidiController.hpp"
#include "synth/MidiConfigViewModel.hpp"
#include "synth/MidiReconcile.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"

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

// Portable wizard-owned form state. Renderers only render its ui::Surface
// tree and dispatch actions back to it; all configuration policy remains here.
class ControllerConfigForm : public ui::Surface {
public:
    ~ControllerConfigForm() override = default;
    virtual std::string_view WizardId() const = 0;
    virtual ui::Subtree BuildSubtree() {
        ui::NodeTree tree = BuildTree();
        const auto root =
            std::find_if(tree.nodes.begin(), tree.nodes.end(), [](const ui::Node& node) {
                return node.kind == ui::NodeKind::Root;
            });
        if (root != tree.nodes.end()) {
            tree.nodes.erase(root);
        }
        return ui::Subtree{std::move(tree), {}, {}};
    }
    virtual bool Validate(std::string& error) const = 0;
    virtual std::string_view ReconfigureWarning() const { return {}; }
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
    ui::Subtree BuildSubtree() override;
    ui::NodeTree BuildTree() override;
    void SetActionHandler(ActionHandler handler) override;
    void DispatchAction(const ui::Action&) override;
    bool Validate(std::string& error) const override;
    std::string_view ReconfigureWarning() const override;

    // A non-empty warning is displayed only for an existing profile that
    // cannot be losslessly represented by this form.
    std::string reconfigureWarning;

private:
    ActionHandler actionHandler_;
};

// Returns a complete form seed only for an exactly representable Twister
// profile. A missing result deliberately prevents partial extraction.
std::optional<MfTwisterConfigForm>
ExtractMfTwisterWizardSeed(const MidiControllerProfileConfig& profile);

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
    virtual std::unique_ptr<ControllerConfigForm>
    ConfigForm(const std::optional<MidiControllerSlot>& seed) const = 0;
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
    std::unique_ptr<ControllerConfigForm>
    ConfigForm(const std::optional<MidiControllerSlot>& seed) const override;

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

// The registry the Controllers page's Layout combo and discovery draw from:
// one descriptor per app device default when the catalog supplies any,
// else the library's single Twister descriptor.
std::vector<ControllerWizardDescriptor> MakeControllerWizardRegistry(const MidiAppCatalog& catalog);
WizardDiscovery DiscoverControllerWizards(
    const MidiDeviceList&, const MidiInstrumentConfig&,
    const std::vector<ControllerWizardDescriptor>&);
std::unique_ptr<ControllerWizard> MakeControllerWizard(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id);

}  // namespace synth
