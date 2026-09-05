#pragma once

// JUCE-free Controllers page presentation and action layer (OpenSpec tasks
// 5.1–5.3). Derives a semantic tree from MidiConfigViewModel and MIDI
// connection state; routes every user action through existing view-model APIs
// and commits accepted edits through a host-provided callback (typically
// engine.EditInstrument).

#include "synth/MidiConfigViewModel.hpp"
#include "synth/ControllerWizard.hpp"
#include "synth/MidiReconcile.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"
#include "synth/PortableUIMetrics.hpp"
#include "synth/RuntimePageStyle.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <variant>

namespace synth::runtime_ui {

inline constexpr const char* kEndpointNoneOptionId = "none";
inline constexpr const char* kEndpointOfflineOptionId = "keep_offline";

namespace NodeIds {

inline constexpr const char* kRoot = "runtime.controllers.root";
inline constexpr const char* kBack = "runtime.controllers.back";
inline constexpr const char* kStatus = "runtime.controllers.status";
inline constexpr const char* kScroll = "runtime.controllers.scroll";
inline constexpr const char* kAddRow = "runtime.controllers.add_row";
inline constexpr const char* kAddPreset = "runtime.controllers.add_preset";
inline constexpr const char* kAddButton = "runtime.controllers.add_button";
inline constexpr const char* kAvailable = "runtime.controllers.available";
inline constexpr const char* kAvailableHeading = "runtime.controllers.available.heading";
inline constexpr const char* kAvailableEmpty = "runtime.controllers.available.empty";
inline constexpr const char* kAvailableUnmatchedInputs = "runtime.controllers.available.unmatched_inputs";
inline constexpr const char* kAvailableUnmatchedOutputs = "runtime.controllers.available.unmatched_outputs";
inline constexpr const char* kStatusLegend = "runtime.controllers.status_legend";
inline constexpr const char* kWizardLaunch = "runtime.controllers.wizard.launch";
inline constexpr const char* kWizardChooser = "runtime.controllers.wizard.chooser";
inline constexpr const char* kWizardChooserEmpty = "runtime.controllers.wizard.chooser.empty";
inline constexpr const char* kWizardForm = "runtime.controllers.wizard.form";
inline constexpr const char* kWizardBack = "runtime.controllers.wizard.back";
inline constexpr const char* kWizardCancel = "runtime.controllers.wizard.cancel";
inline constexpr const char* kWizardSubmit = "runtime.controllers.wizard.submit";
inline constexpr const char* kWizardIgnore = "runtime.controllers.wizard.ignore";
inline constexpr const char* kWizardWarning = "runtime.controllers.wizard.warning";
inline constexpr const char* kWizardStatus = "runtime.controllers.wizard.status";

inline std::string WizardCandidateToken(const WizardCandidate& candidate)
{
    const auto hex = [](std::string_view value) {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (unsigned char byte : value)
        {
            encoded += kHex[byte >> 4U];
            encoded += kHex[byte & 0x0fU];
        }
        return encoded;
    };
    return hex(candidate.wizardId) + "_" + hex(candidate.displayName) + "_" +
           std::to_string(static_cast<int>(candidate.kind)) + "_" +
           hex(candidate.input.identifier) + "_" + hex(candidate.input.name) +
           "_" + hex(candidate.output.identifier) + "_" +
           hex(candidate.output.name);
}

inline std::optional<WizardCandidate> WizardCandidateFromToken(
    std::string_view token)
{
    std::vector<std::string_view> parts;
    while (true)
    {
        const std::size_t delimiter = token.find('_');
        parts.push_back(token.substr(0, delimiter));
        if (delimiter == std::string_view::npos)
        {
            break;
        }
        token.remove_prefix(delimiter + 1);
    }
    if (parts.size() != 7)
    {
        return std::nullopt;
    }

    const auto unhex = [](std::string_view value)
        -> std::optional<std::string> {
        if (value.size() % 2 != 0)
        {
            return std::nullopt;
        }
        const auto nibble = [](char character) -> std::optional<unsigned char> {
            if (character >= '0' && character <= '9')
            {
                return static_cast<unsigned char>(character - '0');
            }
            if (character >= 'a' && character <= 'f')
            {
                return static_cast<unsigned char>(character - 'a' + 10);
            }
            return std::nullopt;
        };
        std::string decoded;
        decoded.reserve(value.size() / 2);
        for (std::size_t ix = 0; ix < value.size(); ix += 2)
        {
            const std::optional<unsigned char> high = nibble(value[ix]);
            const std::optional<unsigned char> low = nibble(value[ix + 1]);
            if (!high.has_value() || !low.has_value())
            {
                return std::nullopt;
            }
            decoded.push_back(static_cast<char>((*high << 4U) | *low));
        }
        return decoded;
    };

    MidiProfileKind kind;
    if (parts[2] == "0")
    {
        kind = MidiProfileKind::WrldBldr;
    }
    else if (parts[2] == "1")
    {
        kind = MidiProfileKind::MfTwister;
    }
    else if (parts[2] == "2")
    {
        kind = MidiProfileKind::Launchpad;
    }
    else if (parts[2] == "3")
    {
        kind = MidiProfileKind::Generic;
    }
    else
    {
        return std::nullopt;
    }

    const std::optional<std::string> wizardId = unhex(parts[0]);
    const std::optional<std::string> displayName = unhex(parts[1]);
    const std::optional<std::string> inputIdentifier = unhex(parts[3]);
    const std::optional<std::string> inputName = unhex(parts[4]);
    const std::optional<std::string> outputIdentifier = unhex(parts[5]);
    const std::optional<std::string> outputName = unhex(parts[6]);
    if (!wizardId.has_value() || !displayName.has_value() ||
        !inputIdentifier.has_value() || !inputName.has_value() ||
        !outputIdentifier.has_value() || !outputName.has_value())
    {
        return std::nullopt;
    }
    return WizardCandidate{.wizardId = *wizardId,
                           .displayName = *displayName,
                           .kind = kind,
                           .input = {.identifier = *inputIdentifier,
                                     .name = *inputName},
                           .output = {.identifier = *outputIdentifier,
                                      .name = *outputName}};
}

inline std::string WizardChooserCandidate(const WizardCandidate& candidate)
{
    return std::string(kWizardChooser) + ".candidate." + WizardCandidateToken(candidate);
}

inline std::string AvailableRow(std::size_t candidateIx)
{
    return "runtime.controllers.available." + std::to_string(candidateIx);
}

inline std::string AvailableName(std::size_t candidateIx)
{
    return AvailableRow(candidateIx) + ".name";
}

inline std::string AvailableConfigure(std::size_t candidateIx)
{
    return AvailableRow(candidateIx) + ".configure";
}

inline std::string AvailableIgnore(std::size_t candidateIx)
{
    return AvailableRow(candidateIx) + ".ignore";
}

inline std::string ControllerRow(std::size_t controllerIx)
{
    return "runtime.controllers.row." + std::to_string(controllerIx);
}

inline std::string ControllerActionToken(std::size_t controllerIx,
                                         std::string_view name)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(name.size() * 2 + 24);
    encoded = std::to_string(controllerIx);
    encoded += ':';
    for (unsigned char byte : name)
    {
        encoded += kHex[byte >> 4U];
        encoded += kHex[byte & 0x0fU];
    }
    return encoded;
}

inline std::optional<std::pair<std::size_t, std::string>>
ControllerActionIdentityFromToken(std::string_view token)
{
    const std::size_t delimiter = token.find(':');
    if (delimiter == std::string_view::npos)
    {
        return std::nullopt;
    }
    std::size_t controllerIx = 0;
    const char* const begin = token.data();
    const char* const end = begin + delimiter;
    const auto [parsedEnd, error] = std::from_chars(begin, end, controllerIx);
    if (error != std::errc{} || parsedEnd != end)
    {
        return std::nullopt;
    }
    const std::string_view encodedName = token.substr(delimiter + 1);
    if (encodedName.size() % 2 != 0)
    {
        return std::nullopt;
    }
    const auto nibble = [](char character) -> std::optional<unsigned char> {
        if (character >= '0' && character <= '9')
        {
            return static_cast<unsigned char>(character - '0');
        }
        if (character >= 'a' && character <= 'f')
        {
            return static_cast<unsigned char>(character - 'a' + 10);
        }
        return std::nullopt;
    };
    std::string name;
    name.reserve(encodedName.size() / 2);
    for (std::size_t ix = 0; ix < encodedName.size(); ix += 2)
    {
        const std::optional<unsigned char> high = nibble(encodedName[ix]);
        const std::optional<unsigned char> low = nibble(encodedName[ix + 1]);
        if (!high.has_value() || !low.has_value())
        {
            return std::nullopt;
        }
        name.push_back(static_cast<char>((*high << 4U) | *low));
    }
    return std::pair{controllerIx, std::move(name)};
}

inline std::string ControllerDisclosure(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".disclosure";
}

inline std::string ControllerName(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".name";
}

inline std::string ControllerKind(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".kind";
}

inline std::string ControllerBadge(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".badge";
}

inline std::string ControllerRename(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".rename";
}

inline std::string ControllerRenameDraft(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".rename_draft";
}

inline std::string ControllerDelete(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".delete";
}

inline std::string ControllerBlacklist(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".blacklist";
}

inline std::string ControllerConfigure(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".configure";
}

inline std::string ControllerRemoveBlacklist(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".remove_blacklist";
}

inline std::string ControllerRestore(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".restore";
}

inline std::string ControllerInputLabel(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".input_label";
}

inline std::string ControllerOutputLabel(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".output_label";
}

inline std::string ControllerInput(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".input";
}

inline std::string ControllerOutput(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".output";
}

inline std::string ControllerInputStatus(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".input_status";
}

inline std::string ControllerOutputStatus(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".output_status";
}

inline std::string SectionToggle(std::size_t controllerIx, MidiConfigSection section)
{
    return ControllerRow(controllerIx) + ".section." + std::to_string(static_cast<int>(section)) + ".toggle";
}

inline std::string SectionBody(std::size_t controllerIx, MidiConfigSection section)
{
    return ControllerRow(controllerIx) + ".section." + std::to_string(static_cast<int>(section)) + ".body";
}

inline std::string GroupHeader(std::size_t controllerIx, MidiConfigSection section, std::size_t headerIx)
{
    return SectionBody(controllerIx, section) + ".header." + std::to_string(headerIx);
}

inline std::string GroupColumnLabel(std::size_t controllerIx, MidiConfigSection section, std::size_t headerIx,
                                    std::size_t fieldIx)
{
    return GroupHeader(controllerIx, section, headerIx) + ".column." + std::to_string(fieldIx);
}

inline std::string MappingRow(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx)
{
    return SectionBody(controllerIx, section) + ".mapping." + std::to_string(rowIx);
}

inline std::string MappingField(std::size_t controllerIx,
                              MidiConfigSection section,
                              std::size_t rowIx,
                              MidiMappingRowVM::Field field)
{
    return MappingRow(controllerIx, section, rowIx) + ".field." +
           std::to_string(static_cast<int>(field));
}

inline std::string MappingDelete(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx)
{
    return MappingRow(controllerIx, section, rowIx) + ".delete";
}

inline std::string GroupAddSingle(std::size_t controllerIx, MidiConfigSection section, std::size_t headerIx)
{
    return GroupHeader(controllerIx, section, headerIx) + ".add_single";
}

inline std::string GroupAddBlock(std::size_t controllerIx, MidiConfigSection section, std::size_t headerIx)
{
    return GroupHeader(controllerIx, section, headerIx) + ".add_block";
}

}  // namespace NodeIds

namespace Actions {

inline constexpr const char* kBack = "runtime.controllers.back";
inline constexpr const char* kToggleConfig = "runtime.controllers.toggle_config";
inline constexpr const char* kToggleSection = "runtime.controllers.toggle_section";
inline constexpr const char* kEndpointSelect = "runtime.controllers.endpoint_select";
inline constexpr const char* kMappingFieldCommit = "runtime.controllers.mapping_field_commit";
inline constexpr const char* kDeleteRow = "runtime.controllers.delete_row";
inline constexpr const char* kAddSingle = "runtime.controllers.add_single";
inline constexpr const char* kAddBlock = "runtime.controllers.add_block";
inline constexpr const char* kAddPresetDraft = "runtime.controllers.add_preset_draft";
inline constexpr const char* kAddController = "runtime.controllers.add_controller";
inline constexpr const char* kAvailableConfigure = "runtime.controllers.available.configure";
inline constexpr const char* kAvailableIgnore = "runtime.controllers.available.ignore";
inline constexpr const char* kWizardOpen = "runtime.controllers.wizard.open";
inline constexpr const char* kWizardChoose = "runtime.controllers.wizard.choose";
inline constexpr const char* kWizardBack = "runtime.controllers.wizard.back";
inline constexpr const char* kWizardCancel = "runtime.controllers.wizard.cancel";
inline constexpr const char* kWizardSubmit = "runtime.controllers.wizard.submit";
inline constexpr const char* kWizardIgnore = "runtime.controllers.wizard.ignore";
inline constexpr const char* kControllerRename = "runtime.controllers.controller.rename";
inline constexpr const char* kControllerRenameDraft = "runtime.controllers.controller.rename_draft";
inline constexpr const char* kControllerDelete = "runtime.controllers.controller.delete";
inline constexpr const char* kControllerBlacklist = "runtime.controllers.controller.blacklist";
inline constexpr const char* kControllerRemoveBlacklist = "runtime.controllers.controller.remove_blacklist";
inline constexpr const char* kControllerConfigure = "runtime.controllers.controller.configure";
inline constexpr const char* kControllerRestore = "runtime.controllers.controller.restore";

// The fixed part of what the Controllers page emits. The per-controller and
// wizard-step actions above are not listed: they are matched by prefix, because
// their names carry a controller index the page composes at build time and no
// fixed set can hold them.
inline constexpr std::string_view kControllersActions[] = {
    kBack,
    kToggleConfig,
    kToggleSection,
    kEndpointSelect,
    kMappingFieldCommit,
    kDeleteRow,
    kAddSingle,
    kAddBlock,
    kAddPresetDraft,
    kAddController,
    kAvailableConfigure,
    kAvailableIgnore,
    kWizardOpen,
    kWizardChoose,
    kWizardBack,
    kWizardCancel,
    kWizardSubmit,
    kWizardIgnore,
};

}  // namespace Actions

namespace ControllersLayout {

inline constexpr float kPageMargin = 4.0f;
inline constexpr float kBackRowHeight = 32.0f;
inline constexpr float kBackButtonWidth = 80.0f;
inline constexpr float kWizardIgnoreWidth = 160.0f;
inline constexpr float kRowGap = 6.0f;
inline constexpr float kStatusRowHeight = 24.0f;
inline constexpr float kControllerHeaderLineHeight = 36.0f;
inline constexpr float kControllerHeaderHeight = 2.0f * kControllerHeaderLineHeight;
inline constexpr float kSectionHeaderHeight = 28.0f;
inline constexpr float kMappingRowHeight = 30.0f;
inline constexpr float kGroupHeaderHeight = 42.0f;
// Separates the mapping editor's columns from each other and from the Add and
// Block buttons that follow them. The group header and the mapping rows below
// it draw the same gap, so a header label stays over the field it names.
inline constexpr float kEditorColumnGap = 4.0f;
inline constexpr float kAddRowHeight = 40.0f;
inline constexpr float kBaseEditorWidth = 90.0f;
inline constexpr float kDeleteButtonWidth = 22.0f;
inline constexpr float kAddButtonWidth = 62.0f;
// The section heading's legend: three in-flow dot/word pairs (online, offline,
// not set). Each dot sits in its own cell, sized for the 8px dot it centres;
// the word beside it takes the word's own intrinsic width, so the two never
// drift apart the way character-advance arithmetic over a shared string did.
// The same cell width also sizes the two per-port status dots on a
// controller row's line two, immediately before each port's combo.
inline constexpr float kStatusDotWidth = 16.0f;
inline constexpr float kStatusLegendPairGap = 16.0f;
inline constexpr float kEndpointFieldWidth = 220.0f;
inline constexpr float kEndpointBoxGap = 8.0f;
// Available-controller row columns: the recognized controller's descriptor
// display name, then its paired endpoint names, then the two lifecycle actions.
inline constexpr float kAvailableNameWidth = 200.0f;
inline constexpr float kAvailableEndpointsWidth = 260.0f;
inline constexpr float kAvailableConfigureWidth = 92.0f;
inline constexpr float kAvailableIgnoreWidth = 72.0f;
inline constexpr float kAvailableControlGap = 8.0f;
inline constexpr float kControllerNameWidth = 200.0f;
inline constexpr float kControllerKindWidth = 100.0f;
inline constexpr float kControllerDisclosureWidth = 24.0f;
// The draft column is wide enough to hold the "Name" caption plus a usable
// field for a short name.
inline constexpr float kLifecycleDraftWidth = 160.0f;
inline constexpr float kLifecycleRenameWidth = 72.0f;
inline constexpr float kLifecycleDeleteWidth = 66.0f;
inline constexpr float kLifecycleBlacklistWidth = 78.0f;
inline constexpr float kLifecycleRestoreWidth = 76.0f;
inline constexpr float kLifecycleConfigureWidth = 86.0f;
inline constexpr float kLifecycleRemoveWidth = 72.0f;
inline constexpr float kLifecycleControlGap = 4.0f;
// The name draft and Rename button live in the expanded editor, so line two's
// lifecycle block is Delete plus Restore and Release, each shown only when its
// own row condition holds. Restore and Release can both show at once, so the
// width below is their worst case, not their typical case.
inline constexpr float kActiveLifecycleWidth =
    kLifecycleDeleteWidth + kLifecycleControlGap + kLifecycleRestoreWidth +
    kLifecycleControlGap + kLifecycleBlacklistWidth;
inline constexpr float kBlacklistedEndpointLabelWidth = 240.0f;
inline constexpr float kBlacklistedBadgeWidth = 84.0f;
// A blacklisted record has no expanded editor to hold a moved rename field,
// and no story for renaming a row the operator is ignoring, so the name
// draft and Rename button are simply gone here, not moved.
inline constexpr float kBlacklistedLifecycleWidth =
    kLifecycleConfigureWidth + kLifecycleControlGap + kLifecycleRemoveWidth;
// Line one: disclosure, name, kind. The status dots are on line two now,
// beside the ports they describe.
inline constexpr float kActiveHeaderLine1Width =
    kControllerDisclosureWidth + kLifecycleControlGap + kControllerNameWidth +
    kLifecycleControlGap + kControllerKindWidth;
// Line two: a status dot immediately before each port's combo, then the
// lifecycle controls.
inline constexpr float kActiveHeaderLine2Width =
    kStatusDotWidth + kLifecycleControlGap + kEndpointFieldWidth + kEndpointBoxGap +
    kStatusDotWidth + kLifecycleControlGap + kEndpointFieldWidth + kLifecycleControlGap +
    kActiveLifecycleWidth + kLifecycleControlGap;
inline constexpr float kActiveControllerHeaderWidth =
    std::max(kActiveHeaderLine1Width, kActiveHeaderLine2Width);
// Line one: name, kind, Released badge.
inline constexpr float kBlacklistedHeaderLine1Width =
    kControllerNameWidth + kLifecycleControlGap + kControllerKindWidth +
    kLifecycleControlGap + kBlacklistedBadgeWidth;
// Line two: the two stored-endpoint labels followed by the lifecycle controls.
inline constexpr float kBlacklistedHeaderLine2Width =
    kBlacklistedEndpointLabelWidth + kLifecycleControlGap + kBlacklistedEndpointLabelWidth +
    kLifecycleControlGap + kBlacklistedLifecycleWidth;
inline constexpr float kBlacklistedControllerHeaderWidth =
    std::max(kBlacklistedHeaderLine1Width, kBlacklistedHeaderLine2Width);
inline constexpr float kControllerHeaderMinWidth =
    std::max(kActiveControllerHeaderWidth, kBlacklistedControllerHeaderWidth);
inline constexpr float kSectionMaxHeight = 220.0f;

inline int FieldEditorWidth(MidiMappingRowVM::Field field)
{
    using Field = MidiMappingRowVM::Field;
    switch (field)
    {
        case Field::MessageKind:
        case Field::AppAction:
            return 150;
        case Field::MessageArg:
            return 74;
        case Field::EncoderMode:
        case Field::BlockMessageType:
            return 132;
        case Field::AddressType:
            return 90;
        case Field::TurnStep:
            return 74;
        case Field::Channel:
        case Field::Cc:
        case Field::SlotIx:
        case Field::Position:
        case Field::LaunchpadX:
        case Field::LaunchpadY:
        case Field::WrldBldrX:
        case Field::WrldBldrY:
        case Field::Button:
        case Field::BlockStartCc:
        case Field::BlockEndCc:
        case Field::BlockStartPos:
        case Field::BlockStartArg:
        case Field::BlockBankSlotIx:
        case Field::BlockStartX:
        case Field::BlockStartY:
        case Field::BlockEndX:
        case Field::BlockEndY:
        case Field::GridSlotIx:
        case Field::GridXMin:
        case Field::GridXMax:
        case Field::GridYMin:
        case Field::GridYMax:
            return 66;
        case Field::GestureIx:
            return 72;
        case Field::SceneBlend:
            return 84;
        case Field::BlockRowMajor:
        case Field::BlockOutputFeedback:
            return 82;
        default:
            return static_cast<int>(kBaseEditorWidth);
    }
}

inline std::string FormatFieldValue(MidiMappingRowVM::Field field, double value)
{
    if (FieldIsInteger(field))
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(std::llround(value)));
        return buffer;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    return buffer;
}

inline const char* SectionName(MidiConfigSection section)
{
    switch (section)
    {
        case MidiConfigSection::Encoders:
            return "Encoders";
        case MidiConfigSection::SystemMessages:
            return "System Messages";
        case MidiConfigSection::Analogs:
            return "Analogs";
    }
    return "";
}

inline const char* RowGroupCaption(MidiMappingRowVM::RowGroup group,
                                   MidiMappingRowVM::Kind kind = MidiMappingRowVM::Kind::Individual)
{
    switch (group)
    {
        case MidiMappingRowVM::RowGroup::EncoderTurn:
            return "Turn";
        case MidiMappingRowVM::RowGroup::EncoderPush:
            return "Push";
        case MidiMappingRowVM::RowGroup::EncoderMode:
            return "Mode";
        case MidiMappingRowVM::RowGroup::EncoderStep:
            return "Step (relative modes only)";
        case MidiMappingRowVM::RowGroup::AnalogGesture:
            return "Gestures";
        case MidiMappingRowVM::RowGroup::AnalogAppAction:
            return "App actions";
        case MidiMappingRowVM::RowGroup::AnalogSceneBlend:
            return "Scene blend";
        case MidiMappingRowVM::RowGroup::System:
            return "System";
        case MidiMappingRowVM::RowGroup::Grid:
            return kind == MidiMappingRowVM::Kind::Block ? "Grid Block" : "Grid Button";
    }
    return "";
}

inline std::string RowGroupToken(MidiMappingRowVM::RowGroup group)
{
    switch (group)
    {
        case MidiMappingRowVM::RowGroup::EncoderTurn:
            return "encoder_turn";
        case MidiMappingRowVM::RowGroup::EncoderPush:
            return "encoder_push";
        case MidiMappingRowVM::RowGroup::EncoderMode:
            return "encoder_mode";
        case MidiMappingRowVM::RowGroup::EncoderStep:
            return "encoder_step";
        case MidiMappingRowVM::RowGroup::AnalogGesture:
            return "analog_gesture";
        case MidiMappingRowVM::RowGroup::AnalogAppAction:
            return "analog_app_action";
        case MidiMappingRowVM::RowGroup::AnalogSceneBlend:
            return "analog_scene_blend";
        case MidiMappingRowVM::RowGroup::System:
            return "system";
        case MidiMappingRowVM::RowGroup::Grid:
            return "grid";
    }
    return "unknown";
}

inline std::optional<MidiMappingRowVM::RowGroup> ParseRowGroupToken(const std::string& token)
{
    if (token == "encoder_turn")
    {
        return MidiMappingRowVM::RowGroup::EncoderTurn;
    }
    if (token == "encoder_push")
    {
        return MidiMappingRowVM::RowGroup::EncoderPush;
    }
    if (token == "encoder_mode")
    {
        return MidiMappingRowVM::RowGroup::EncoderMode;
    }
    if (token == "encoder_step")
    {
        return MidiMappingRowVM::RowGroup::EncoderStep;
    }
    if (token == "analog_gesture")
    {
        return MidiMappingRowVM::RowGroup::AnalogGesture;
    }
    if (token == "analog_app_action")
    {
        return MidiMappingRowVM::RowGroup::AnalogAppAction;
    }
    if (token == "analog_scene_blend")
    {
        return MidiMappingRowVM::RowGroup::AnalogSceneBlend;
    }
    if (token == "system")
    {
        return MidiMappingRowVM::RowGroup::System;
    }
    if (token == "grid")
    {
        return MidiMappingRowVM::RowGroup::Grid;
    }
    return std::nullopt;
}

inline std::string SectionToken(MidiConfigSection section)
{
    switch (section)
    {
        case MidiConfigSection::Encoders:
            return "encoders";
        case MidiConfigSection::SystemMessages:
            return "system_messages";
        case MidiConfigSection::Analogs:
            return "analogs";
    }
    return "unknown";
}

inline std::optional<MidiConfigSection> ParseSectionToken(const std::string& token)
{
    if (token == "encoders")
    {
        return MidiConfigSection::Encoders;
    }
    if (token == "system_messages")
    {
        return MidiConfigSection::SystemMessages;
    }
    if (token == "analogs")
    {
        return MidiConfigSection::Analogs;
    }
    return std::nullopt;
}

inline std::string FieldToken(MidiMappingRowVM::Field field)
{
    return std::to_string(static_cast<int>(field));
}

inline std::optional<MidiMappingRowVM::Field> ParseFieldToken(const std::string& token)
{
    try
    {
        const int value = std::stoi(token);
        return static_cast<MidiMappingRowVM::Field>(value);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

inline Color EndpointStatusColor(MidiEndpointStatus status)
{
    switch (status)
    {
        case MidiEndpointStatus::Online:
            return Color::Rgb(50, 205, 50);
        case MidiEndpointStatus::Offline:
            return Color::Rgb(220, 20, 60);
        case MidiEndpointStatus::Unconfigured:
            break;
    }
    return Color::Rgb(128, 128, 128);
}

// Draws one status dot: a controller row's per-port indicator and the status
// legend's swatch are the same mark, so both callers share this to keep the
// dot's size and colour from drifting apart between the two places it appears.
inline void EmitStatusDot(ui::Builder& parent, const std::string& id, MidiEndpointStatus status)
{
    ui::LayoutOptions dotLayout;
    dotLayout.main = ui::Extent::Px(kStatusDotWidth);
    parent.Draw(id, dotLayout, [status](ui::Bounds nodeExtent) {
        constexpr float kDotSize = 8.0f;
        return std::vector<ui::DrawCommand>{ui::DrawCommand::FillEllipse(
            {(nodeExtent.width - kDotSize) / 2.0f,
             (nodeExtent.height - kDotSize) / 2.0f, kDotSize, kDotSize},
            EndpointStatusColor(status))};
    });
}

// Stored endpoint identity, independent of connection status, so a
// deliberately inert Blacklisted record still shows which endpoints it holds.
inline std::string StoredEndpointLabel(const MidiEndpointRef& ref)
{
    if (!ref.IsConfigured())
    {
        return "(none)";
    }
    if (ref.name.empty())
    {
        return ref.identifier;
    }
    if (ref.identifier.empty())
    {
        return ref.name;
    }
    return ref.name + " (" + ref.identifier + ")";
}

inline std::vector<ui::ControlOption> BuildEndpointOptions(const std::vector<MidiDeviceInfoRef>& devices,
                                                           MidiEndpointStatus status,
                                                           const MidiEndpointRef& stored,
                                                           const std::string& storedLabel,
                                                           std::string& selectedOptionId)
{
    std::vector<ui::ControlOption> options;
    options.push_back({kEndpointNoneOptionId, "(none)"});
    selectedOptionId = kEndpointNoneOptionId;

    // Follow reconciliation identity semantics: the exact stored identifier
    // wins, and the stored name is only a fallback. Duplicate same-name units
    // are otherwise indistinguishable in this picker.
    bool selectedByIdentifier = false;
    for (const MidiDeviceInfoRef& device : devices)
    {
        options.push_back({device.identifier, device.name});
        if (status != MidiEndpointStatus::Online)
        {
            continue;
        }
        if (!stored.identifier.empty() && device.identifier == stored.identifier)
        {
            selectedOptionId = device.identifier;
            selectedByIdentifier = true;
        }
        else if (!selectedByIdentifier && device.name == storedLabel)
        {
            selectedOptionId = device.identifier;
        }
    }

    if (status == MidiEndpointStatus::Offline)
    {
        options.push_back({kEndpointOfflineOptionId, storedLabel});
        selectedOptionId = kEndpointOfflineOptionId;
    }

    return options;
}

// Installs a descriptor's default profile onto `slot`: opens the descriptor's
// wizard, opens a blank form (ConfigForm(nullopt)), generates a profile from
// it, then copies the generated kind, config and wizardId onto `slot`. On
// failure `reason` (when non-null) gets wording the caller can format as its
// own "Refused: " + *reason. Three sites install a descriptor from a blank
// form this way, and all three call this one definition: the add row (adding
// a new row from a preset, below), and MidiConfigViewModel.cpp's
// SlotMatchesWizardProfile (regenerating a row's wizard profile to compare it
// against the row's current config) and RestoreController (reinstalling a
// row's preset over a diverged config).
inline bool InstallDescriptorProfile(const std::vector<ControllerWizardDescriptor>& layouts,
                                     const ControllerWizardDescriptor& descriptor,
                                     MidiControllerSlot& slot,
                                     std::string* reason)
{
    std::unique_ptr<ControllerWizard> wizard = MakeControllerWizard(layouts, descriptor.id);
    if (!wizard)
    {
        if (reason)
        {
            *reason = "controller wizard is unavailable";
        }
        return false;
    }
    std::unique_ptr<ControllerConfigForm> form = wizard->ConfigForm(std::nullopt);
    if (!form)
    {
        if (reason)
        {
            *reason = "controller wizard could not open a form";
        }
        return false;
    }
    WizardGenerationResult generated = wizard->GenerateProfile(
        *form, {.name = slot.name, .input = slot.input, .output = slot.output});
    if (!generated)
    {
        if (reason)
        {
            *reason = generated.error.empty() ? "controller profile generation failed" : generated.error;
        }
        return false;
    }
    slot.kind = generated.controller->kind;
    slot.config = std::move(generated.controller->config);
    slot.wizardId = generated.controller->wizardId;
    return true;
}

// The add row's Preset combo: every registry descriptor's display name
// (option id = descriptor id), then one "Custom (<kind>)" entry per
// MidiProfileKind in this fixed order, option id `custom.<kind token>` using
// the existing MidiProfileKindName (the add handler parses it back with the
// existing MidiProfileKindFromName -- no second token switch).
inline std::vector<ui::ControlOption> BuildAddPresetOptions(
    const std::vector<ControllerWizardDescriptor>& layouts)
{
    std::vector<ui::ControlOption> options;
    options.reserve(layouts.size() + 4);
    for (const ControllerWizardDescriptor& descriptor : layouts)
    {
        options.push_back({descriptor.id, descriptor.displayName});
    }
    constexpr MidiProfileKind kCustomKinds[] = {MidiProfileKind::Generic, MidiProfileKind::MfTwister,
                                                MidiProfileKind::Launchpad, MidiProfileKind::WrldBldr};
    for (MidiProfileKind kind : kCustomKinds)
    {
        options.push_back({std::string("custom.") + MidiProfileKindName(kind),
                           std::string("Custom (") + MidiProfileKindDisplayName(kind) + ")"});
    }
    return options;
}

// The add row's Preset combo defaults to its first option when no draft has
// been recorded yet. Derived from BuildAddPresetOptions itself -- not a
// second "first option" rule -- so the row's displayed default and the id
// HandleAddController installs from cannot drift apart.
inline std::string EffectiveAddPresetId(const std::vector<ControllerWizardDescriptor>& layouts,
                                        const std::string& draft)
{
    if (!draft.empty())
    {
        return draft;
    }
    return BuildAddPresetOptions(layouts).front().id;
}

}  // namespace ControllersLayout

struct ControllersPageCallbacks
{
    std::function<MidiInstrumentConfig()> instrumentSnapshot;
    std::function<MidiConnectionState()> connectionState;
    std::function<MidiDeviceList()> enumerateDevices;
    std::function<bool(MidiInstrumentConfig)> commitInstrument;
    std::function<bool()> saveRuntimeConfiguration;
    std::function<void(std::string)> setStatus;
    std::function<void()> onBack;
    // The message-kind combo's offered list. Empty means the library
    // default (UISystemMessageCatalog(), MidiConfigViewModel's own
    // default) -- a host with an app catalog fills this from
    // MakeUISystemMessageChoices(engine.MidiCatalog()).
    std::vector<UISystemMessageChoice> messageCatalog;
    // The analog app-action row's target combo. Empty means no analog-range
    // app actions (MidiConfigViewModel's own default) -- a host with an app
    // catalog fills this from MakeAnalogAppActionChoices(engine.MidiCatalog()).
    std::vector<UISystemMessageChoice> analogActionCatalog;
    // The add row's Preset combo options, and the registry every wizard
    // lookup on this page resolves against. Empty means the library default
    // (the Twister-only registry, MidiConfigViewModel's own default) -- a
    // host with an app catalog fills this from
    // MakeControllerWizardRegistry(engine.MidiCatalog()).
    std::vector<ControllerWizardDescriptor> layouts;
};

struct ExistingWizardTarget {
    std::size_t index = 0;
    std::string name;
    MidiProfileKind kind = MidiProfileKind::Generic;
    MidiEndpointRef input;
    MidiEndpointRef output;
    std::optional<std::string> wizardId;
    MidiControllerDisposition disposition = MidiControllerDisposition::Active;
};

struct WizardSession {
    std::variant<WizardCandidate, ExistingWizardTarget> target;
    std::unique_ptr<ControllerWizard> wizard;
    std::unique_ptr<ControllerConfigForm> form;
    std::string warning;
    std::string status;
};

inline bool WizardDiscoveryEqual(const WizardDiscovery& lhs, const WizardDiscovery& rhs)
{
    if (lhs.unmatchedInputs != rhs.unmatchedInputs || lhs.unmatchedOutputs != rhs.unmatchedOutputs ||
        lhs.available.size() != rhs.available.size())
    {
        return false;
    }
    for (std::size_t ix = 0; ix < lhs.available.size(); ++ix)
    {
        const WizardCandidate& left = lhs.available[ix];
        const WizardCandidate& right = rhs.available[ix];
        if (left.wizardId != right.wizardId || left.displayName != right.displayName ||
            left.kind != right.kind || left.input != right.input || left.output != right.output)
        {
            return false;
        }
    }
    return true;
}

class ControllersPageSurface final : public ui::Surface
{
public:
    explicit ControllersPageSurface(ControllersPageCallbacks callbacks)
        : m_callbacks(std::move(callbacks))
    {
        if (!m_callbacks.messageCatalog.empty())
        {
            m_vm.SetMessageCatalog(m_callbacks.messageCatalog);
        }
        if (!m_callbacks.analogActionCatalog.empty())
        {
            m_vm.SetAnalogActionCatalog(m_callbacks.analogActionCatalog);
        }
        if (!m_callbacks.layouts.empty())
        {
            m_vm.SetLayouts(m_callbacks.layouts);
        }
        m_dirty = true;
    }

    ui::NodeTree BuildTree() override
    {
        if (m_wizardSession.has_value())
        {
            return BuildWizardFormTree();
        }
        if (m_wizardChooserOpen)
        {
            return BuildWizardChooserTree();
        }
        return BuildControllersPageTree(m_vm,
                                        m_devices,
                                        m_discovery,
                                        m_contentBounds,
                                        m_statusText,
                                        m_addPresetId,
                                        m_renameDrafts);
    }

    void SetActionHandler(ActionHandler handler) override
    {
        m_outerHandler_ = std::move(handler);
    }

    void DispatchAction(const ui::Action& action) override
    {
        HandleAction(action);
        RefreshOnTick(/*respectFocusGuard=*/false);
        ++m_treeRevision;
        if (m_outerHandler_)
        {
            m_outerHandler_(action);
        }
    }

    void SetContentBounds(ui::Bounds bounds)
    {
        if (m_contentBounds.x == bounds.x && m_contentBounds.y == bounds.y &&
            m_contentBounds.width == bounds.width && m_contentBounds.height == bounds.height)
        {
            return;
        }
        m_contentBounds = bounds;
        ++m_treeRevision;
    }

    void SetEnumerateDevices(MidiDeviceList devices)
    {
        if (m_devices == devices)
        {
            return;
        }
        m_devices = std::move(devices);
        ++m_treeRevision;
    }

    void SetDiscovery(WizardDiscovery discovery)
    {
        if (WizardDiscoveryEqual(m_discovery, discovery))
        {
            return;
        }
        m_discovery = std::move(discovery);
        ++m_treeRevision;
    }

    const WizardDiscovery& Discovery() const
    {
        return m_discovery;
    }

    const WizardSession* ActiveWizardSession() const
    {
        return m_wizardSession ? &*m_wizardSession : nullptr;
    }

    bool OpenCandidate(std::size_t candidateIx)
    {
        if (m_wizardSession || candidateIx >= m_discovery.available.size())
        {
            return false;
        }

        const WizardCandidate candidate = m_discovery.available[candidateIx];
        std::unique_ptr<ControllerWizard> wizard = MakeControllerWizard(m_vm.Layouts(), candidate.wizardId);
        if (!wizard)
        {
            SetStatus("Refused: controller wizard is unavailable");
            return false;
        }
        std::unique_ptr<ControllerConfigForm> form = wizard->ConfigForm(std::nullopt);
        if (!form)
        {
            SetStatus("Refused: controller wizard could not open a form");
            return false;
        }

        m_wizardSession.emplace(WizardSession{.target = candidate,
                                              .wizard = std::move(wizard),
                                              .form = std::move(form)});
        m_wizardChooserOpen = false;
        ++m_treeRevision;
        return true;
    }

    bool OpenExisting(std::size_t controllerIx)
    {
        if (m_wizardSession || !m_callbacks.instrumentSnapshot)
        {
            return false;
        }
        const MidiInstrumentConfig instrument = m_callbacks.instrumentSnapshot();
        if (controllerIx >= instrument.controllers.size())
        {
            return false;
        }
        return OpenExistingFromSnapshot(instrument, controllerIx);
    }

    void MarkDirty()
    {
        m_dirty = true;
    }

    void SetFocusGuard(std::function<bool()> guard)
    {
        m_focusGuard = std::move(guard);
    }

    void RefreshOnTick()
    {
        RefreshOnTick(/*respectFocusGuard=*/true);
    }

    void RefreshOnTick(bool respectFocusGuard)
    {
        if (!m_callbacks.connectionState || !m_callbacks.instrumentSnapshot)
        {
            return;
        }

        const std::string fingerprint = ConnectionFingerprint(m_callbacks.connectionState());
        if (fingerprint != m_lastFingerprint)
        {
            m_dirty = true;
            m_lastFingerprint = fingerprint;
        }

        if (!m_dirty)
        {
            return;
        }

        if (respectFocusGuard && m_focusGuard && m_focusGuard())
        {
            return;
        }

        m_vm.Rebuild(m_callbacks.instrumentSnapshot(), m_callbacks.connectionState());
        m_dirty = false;
        ++m_treeRevision;
    }

    MidiConfigViewModel& ViewModel()
    {
        return m_vm;
    }

    const MidiConfigViewModel& ViewModel() const
    {
        return m_vm;
    }

    const std::string& StatusText() const
    {
        return m_statusText;
    }

    std::uint64_t TreeRevision() const
    {
        return m_treeRevision;
    }

    void SetAddPresetDraft(std::string presetId)
    {
        if (m_addPresetId == presetId)
        {
            return;
        }
        m_addPresetId = std::move(presetId);
        ++m_treeRevision;
    }

    bool NeedsDeferredDispatch(const ui::Action& action) const
    {
        return action.name == Actions::kBack || action.name == Actions::kToggleConfig ||
               action.name == Actions::kToggleSection ||
               action.name == Actions::kDeleteRow || action.name == Actions::kAddSingle ||
               action.name == Actions::kAddBlock || action.name == Actions::kEndpointSelect ||
               action.name == Actions::kMappingFieldCommit ||
               action.name == Actions::kAddController || action.name == Actions::kWizardOpen ||
               action.name == Actions::kAvailableConfigure ||
               action.name == Actions::kAvailableIgnore ||
               action.name == Actions::kWizardChoose ||
               action.name == Actions::kWizardBack ||
               action.name == Actions::kWizardCancel ||
               action.name == Actions::kWizardSubmit ||
               action.name == Actions::kWizardIgnore ||
               action.name == Actions::kControllerRenameDraft ||
               action.name == Actions::kControllerRename ||
               action.name == Actions::kControllerDelete ||
               action.name == Actions::kControllerBlacklist ||
               action.name == Actions::kControllerRemoveBlacklist ||
               action.name == Actions::kControllerRestore ||
               action.name == Actions::kControllerConfigure;
    }

private:
    bool OpenExistingFromSnapshot(const MidiInstrumentConfig& instrument,
                                  std::size_t controllerIx)
    {
        if (m_wizardSession || controllerIx >= instrument.controllers.size())
        {
            return false;
        }
        const MidiControllerSlot& controller = instrument.controllers[controllerIx];
        if (!controller.wizardId.has_value())
        {
            return false;
        }
        std::unique_ptr<ControllerWizard> wizard = MakeControllerWizard(m_vm.Layouts(), *controller.wizardId);
        if (!wizard)
        {
            return false;
        }
        std::unique_ptr<ControllerConfigForm> form = wizard->ConfigForm(controller);
        if (!form)
        {
            return false;
        }

        const std::string warning(form->ReconfigureWarning());
        ExistingWizardTarget target{.index = controllerIx,
                                    .name = controller.name,
                                    .kind = controller.kind,
                                    .input = controller.input,
                                    .output = controller.output,
                                    .wizardId = controller.wizardId,
                                    .disposition = controller.disposition};
        m_wizardSession.emplace(WizardSession{.target = std::move(target),
                                              .wizard = std::move(wizard),
                                              .form = std::move(form),
                                              .warning = warning});
        m_wizardChooserOpen = false;
        ++m_treeRevision;
        return true;
    }

    static std::string ConnectionFingerprint(const MidiConnectionState& state)
    {
        std::string fp;
        fp.reserve(state.controllers.size() * 2 + 8);
        fp += std::to_string(state.controllers.size());
        for (const auto& controller : state.controllers)
        {
            fp += ':';
            fp += std::to_string(static_cast<int>(controller.input.status));
            fp += ',';
            fp += std::to_string(static_cast<int>(controller.output.status));
        }
        return fp;
    }

    bool Commit(MidiInstrumentConfig out)
    {
        if (!m_callbacks.commitInstrument ||
            !m_callbacks.commitInstrument(std::move(out)))
        {
            return false;
        }
        m_dirty = true;
        return true;
    }

    void SetStatus(std::string text)
    {
        if (m_statusText == text)
        {
            return;
        }
        m_statusText = std::move(text);
        ++m_treeRevision;
        if (m_callbacks.setStatus)
        {
            m_callbacks.setStatus(m_statusText);
        }
    }

    void HandleAction(const ui::Action& action)
    {
        if (m_wizardSession.has_value())
        {
            if (action.name == Actions::kWizardBack || action.name == Actions::kWizardCancel)
            {
                CloseWizardSession();
                return;
            }

            if (action.name == Actions::kWizardSubmit)
            {
                HandleWizardSubmit();
                return;
            }
            if (action.name == Actions::kWizardIgnore)
            {
                HandleWizardIgnore();
                return;
            }

            m_wizardSession->form->DispatchAction(action);
            return;
        }

        if (m_wizardChooserOpen)
        {
            if (action.name == Actions::kWizardBack || action.name == Actions::kWizardCancel)
            {
                m_wizardChooserOpen = false;
                ++m_treeRevision;
            }
            else if (action.name == Actions::kWizardChoose)
            {
                OpenChooserCandidate(action.value);
            }
            return;
        }

        if (action.name == Actions::kBack)
        {
            if (m_callbacks.onBack)
            {
                m_callbacks.onBack();
            }
            return;
        }

        if (action.name == Actions::kToggleConfig)
        {
            const std::size_t controllerIx = ParseIndex(action.value);
            m_vm.ToggleConfig(controllerIx);
            return;
        }

        if (action.name == Actions::kToggleSection)
        {
            const auto parts = Split(action.value, ':');
            if (parts.size() != 2)
            {
                return;
            }
            const std::size_t controllerIx = ParseIndex(parts[0]);
            const std::optional<MidiConfigSection> section = ControllersLayout::ParseSectionToken(parts[1]);
            if (!section.has_value())
            {
                return;
            }
            m_vm.ToggleSection(controllerIx, *section);
            return;
        }

        if (action.name == Actions::kEndpointSelect)
        {
            HandleEndpointSelect(action.value);
            return;
        }

        if (action.name == Actions::kMappingFieldCommit)
        {
            HandleMappingFieldCommit(action.value);
            return;
        }

        if (action.name == Actions::kDeleteRow)
        {
            HandleDeleteRow(action.value);
            return;
        }

        if (action.name == Actions::kAddSingle)
        {
            HandleAdd(action.value, /*asBlock=*/false);
            return;
        }

        if (action.name == Actions::kAddBlock)
        {
            HandleAdd(action.value, /*asBlock=*/true);
            return;
        }

        if (action.name == Actions::kAddPresetDraft)
        {
            SetAddPresetDraft(action.value);
            return;
        }

        if (action.name == Actions::kAddController)
        {
            HandleAddController(action.value);
            return;
        }

        if (action.name == Actions::kWizardOpen)
        {
            if (m_discovery.available.size() == 1)
            {
                OpenCandidate(0);
            }
            else if (m_discovery.available.size() > 1)
            {
                m_wizardChooserOpen = true;
                ++m_treeRevision;
            }
            return;
        }

        if (action.name == Actions::kAvailableConfigure)
        {
            OpenCandidate(ParseIndex(action.value));
            return;
        }

        if (action.name == Actions::kAvailableIgnore)
        {
            const std::optional<WizardCandidate> candidate =
                NodeIds::WizardCandidateFromToken(action.value);
            if (!candidate.has_value())
            {
                SetStatus("Refused: invalid controller identity");
                return;
            }
            HandleIgnoreCandidate(*candidate, /*sessionStatus=*/false);
            return;
        }

        if (action.name == Actions::kControllerRenameDraft)
        {
            const std::size_t firstSeparator = action.value.find(':');
            const std::size_t separator = firstSeparator == std::string::npos
                ? std::string::npos
                : action.value.find(':', firstSeparator + 1);
            if (separator == std::string::npos)
            {
                SetStatus("Refused: invalid rename request");
                return;
            }
            const std::optional<std::pair<std::size_t, std::string>> identity =
                NodeIds::ControllerActionIdentityFromToken(action.value.substr(0, separator));
            if (!identity.has_value())
            {
                SetStatus("Refused: invalid controller identity");
                return;
            }
            m_renameDrafts[identity->second] = action.value.substr(separator + 1);
            ++m_treeRevision;
            return;
        }

        if (action.name == Actions::kControllerRename)
        {
            HandleRenameController(action.value);
            return;
        }

        if (action.name == Actions::kControllerDelete)
        {
            HandleDeleteController(action.value);
            return;
        }

        if (action.name == Actions::kControllerBlacklist)
        {
            HandleBlacklistController(action.value);
            return;
        }

        if (action.name == Actions::kControllerRemoveBlacklist)
        {
            HandleRemoveFromBlacklist(action.value);
            return;
        }

        if (action.name == Actions::kControllerRestore)
        {
            HandleRestoreController(action.value);
            return;
        }

        if (action.name == Actions::kControllerConfigure)
        {
            const std::optional<std::pair<std::size_t, std::string>> identity =
                NodeIds::ControllerActionIdentityFromToken(action.value);
            MidiInstrumentConfig instrument;
            if (!SnapshotForLifecycleIdentity(identity, instrument))
            {
                return;
            }
            OpenExistingFromSnapshot(instrument, identity->first);
        }
    }

    static bool CandidateIdentityEqual(const WizardCandidate& lhs,
                                       const WizardCandidate& rhs)
    {
        return lhs.wizardId == rhs.wizardId &&
               lhs.displayName == rhs.displayName &&
               lhs.kind == rhs.kind &&
               lhs.input == rhs.input &&
               lhs.output == rhs.output;
    }

    static bool ExactEndpointPresent(const std::vector<MidiDeviceInfoRef>& devices,
                                     const MidiDeviceInfoRef& endpoint)
    {
        return std::find(devices.begin(), devices.end(), endpoint) != devices.end();
    }

    static std::string AvailableControllerName(const MidiInstrumentConfig& instrument,
                                               const std::string& displayName)
    {
        if (instrument.FindController(displayName) == nullptr)
        {
            return displayName;
        }
        for (std::size_t suffix = 2;; ++suffix)
        {
            const std::string candidate =
                displayName + " " + std::to_string(suffix);
            if (instrument.FindController(candidate) == nullptr)
            {
                return candidate;
            }
        }
    }

    void SetWizardStatus(std::string text)
    {
        if (!m_wizardSession.has_value() ||
            m_wizardSession->status == text)
        {
            return;
        }
        m_wizardSession->status = std::move(text);
        ++m_treeRevision;
        if (m_callbacks.setStatus)
        {
            m_callbacks.setStatus(m_wizardSession->status);
        }
    }

    bool RevalidateCandidate(const WizardCandidate& expected,
                             MidiInstrumentConfig& instrument,
                             MidiDeviceList& devices,
                             bool sessionStatus)
    {
        const auto report = [&](std::string text) {
            if (sessionStatus)
            {
                SetWizardStatus(std::move(text));
            }
            else
            {
                SetStatus(std::move(text));
            }
        };
        if (!m_callbacks.instrumentSnapshot || !m_callbacks.enumerateDevices)
        {
            report("Refused: current controller state is unavailable");
            return false;
        }

        devices = m_callbacks.enumerateDevices();
        instrument = m_callbacks.instrumentSnapshot();
        const WizardDiscovery current = DiscoverControllerWizards(
            devices, instrument, m_vm.Layouts());
        const auto match = std::find_if(
            current.available.begin(), current.available.end(),
            [&](const WizardCandidate& candidate) {
                return CandidateIdentityEqual(candidate, expected);
            });
        if (match != current.available.end())
        {
            return true;
        }

        const bool endpointsPresent =
            ExactEndpointPresent(devices.inputs, expected.input) &&
            ExactEndpointPresent(devices.outputs, expected.output);
        report(
            endpointsPresent
                ? "Refused: this controller is no longer available or its endpoints are claimed"
                : "Refused: reconnect both controller endpoints and try again");
        return false;
    }

    void RefreshDiscoveryFromCallbacks()
    {
        if (!m_callbacks.instrumentSnapshot || !m_callbacks.enumerateDevices)
        {
            return;
        }
        MidiDeviceList devices = m_callbacks.enumerateDevices();
        MidiInstrumentConfig instrument = m_callbacks.instrumentSnapshot();
        SetEnumerateDevices(devices);
        SetDiscovery(DiscoverControllerWizards(
            devices, instrument, m_vm.Layouts()));
    }

    bool SaveCommittedWizardAction(bool sessionStatus)
    {
        if (!m_callbacks.saveRuntimeConfiguration ||
            !m_callbacks.saveRuntimeConfiguration())
        {
            if (sessionStatus)
            {
                SetWizardStatus(
                    "The controller was committed, but runtime configuration save failed");
            }
            else
            {
                SetStatus(
                    "The controller was committed, but runtime configuration save failed");
            }
            return false;
        }
        return true;
    }

    bool CommitNewCandidate(MidiInstrumentConfig instrument,
                            MidiControllerSlot controller,
                            bool sessionStatus)
    {
        if (!instrument.AddController(std::move(controller)))
        {
            if (sessionStatus)
            {
                SetWizardStatus("Refused: generated controller record is invalid");
            }
            else
            {
                SetStatus("Refused: generated controller record is invalid");
            }
            return false;
        }
        if (!Commit(std::move(instrument)))
        {
            if (sessionStatus)
            {
                SetWizardStatus("Refused: host rejected the instrument commit");
            }
            else
            {
                SetStatus("Refused: host rejected the instrument commit");
            }
            return false;
        }

        RefreshDiscoveryFromCallbacks();
        return SaveCommittedWizardAction(sessionStatus);
    }

    bool SnapshotForLifecycleIdentity(
        const std::optional<std::pair<std::size_t, std::string>>& identity,
        MidiInstrumentConfig& instrument)
    {
        if (!identity.has_value() || !m_callbacks.instrumentSnapshot)
        {
            SetStatus("Refused: invalid controller identity");
            return false;
        }
        instrument = m_callbacks.instrumentSnapshot();
        if (identity->first >= instrument.controllers.size() ||
            instrument.controllers[identity->first].name != identity->second)
        {
            SetStatus("Refused: controller record changed; refresh and try again");
            return false;
        }
        return true;
    }

    bool CommitLifecycleAction(const std::string& token,
                               const std::function<bool(MidiConfigViewModel&, std::size_t,
                                                        MidiInstrumentConfig&, std::string*)>& mutate,
                               std::string success)
    {
        const std::optional<std::pair<std::size_t, std::string>> identity =
            NodeIds::ControllerActionIdentityFromToken(token);
        MidiInstrumentConfig instrument;
        if (!SnapshotForLifecycleIdentity(identity, instrument))
        {
            return false;
        }
        MidiConfigViewModel mutationViewModel;
        mutationViewModel.Rebuild(instrument, MidiConnectionState{});
        MidiInstrumentConfig out;
        std::string reason;
        if (!mutate(mutationViewModel, identity->first, out, &reason))
        {
            SetStatus("Refused: " + reason);
            return false;
        }
        if (!Commit(std::move(out)))
        {
            SetStatus("Refused: host rejected the instrument commit");
            return false;
        }
        RefreshDiscoveryFromCallbacks();
        if (!SaveCommittedWizardAction(/*sessionStatus=*/false))
        {
            return false;
        }
        SetStatus(std::move(success));
        return true;
    }

    void HandleRenameController(const std::string& token)
    {
        const std::optional<std::pair<std::size_t, std::string>> identity =
            NodeIds::ControllerActionIdentityFromToken(token);
        if (!identity.has_value())
        {
            SetStatus("Refused: invalid controller identity");
            return;
        }
        const auto draft = m_renameDrafts.find(identity->second);
        const std::string name = draft != m_renameDrafts.end() ? draft->second : identity->second;
        if (CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.RenameController(controllerIx, name, out, reason);
            },
            "Renamed " + name))
        {
            // Re-key m_vm's per-row UI caches now, before RefreshOnTick's
            // next Rebuild() runs -- Rebuild() erases cache entries for
            // names no longer present, and by then the old name is gone.
            m_vm.NoteControllerRenamed(identity->second, name);
            m_renameDrafts.erase(identity->second);
        }
    }

    void HandleDeleteController(const std::string& token)
    {
        CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.DeleteController(controllerIx, out, reason);
            },
            "Deleted controller");
    }

    void HandleBlacklistController(const std::string& token)
    {
        CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.BlacklistController(controllerIx, out, reason);
            },
            "Released controller");
    }

    void HandleRemoveFromBlacklist(const std::string& token)
    {
        CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.RemoveFromBlacklist(controllerIx, out, reason);
            },
            "Reclaimed controller");
    }

    void HandleRestoreController(const std::string& token)
    {
        CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.RestoreController(controllerIx, out, reason);
            },
            "Restored controller");
    }

    void HandleWizardSubmit()
    {
        if (!m_wizardSession.has_value())
        {
            return;
        }
        if (!std::holds_alternative<WizardCandidate>(m_wizardSession->target))
        {
            HandleExistingWizardSubmit();
            return;
        }

        const WizardCandidate candidate =
            std::get<WizardCandidate>(m_wizardSession->target);
        MidiInstrumentConfig instrument;
        MidiDeviceList devices;
        if (!RevalidateCandidate(candidate, instrument, devices,
                                 /*sessionStatus=*/true))
        {
            return;
        }

        const std::string name =
            AvailableControllerName(instrument, candidate.displayName);
        WizardGenerationResult generated =
            m_wizardSession->wizard->GenerateProfile(
                *m_wizardSession->form,
                {.name = name,
                 .input = {.identifier = candidate.input.identifier,
                           .name = candidate.input.name},
                 .output = {.identifier = candidate.output.identifier,
                            .name = candidate.output.name}});
        if (!generated)
        {
            SetWizardStatus(
                "Refused: " +
                (generated.error.empty()
                     ? std::string("controller profile generation failed")
                     : generated.error));
            return;
        }

        MidiControllerSlot controller = std::move(*generated.controller);
        controller.name = name;
        controller.kind = candidate.kind;
        controller.disposition = MidiControllerDisposition::Active;
        controller.wizardId = candidate.wizardId;
        controller.input = {.identifier = candidate.input.identifier,
                            .name = candidate.input.name};
        controller.output = {.identifier = candidate.output.identifier,
                             .name = candidate.output.name};
        controller.dormantConfig.reset();

        if (!CommitNewCandidate(std::move(instrument),
                                std::move(controller),
                                /*sessionStatus=*/true))
        {
            return;
        }
        CloseWizardSession();
        SetStatus("Configured " + name);
    }

    bool RevalidateExistingWizardTarget(const ExistingWizardTarget& expected,
                                        MidiInstrumentConfig& instrument)
    {
        if (!m_callbacks.instrumentSnapshot)
        {
            SetWizardStatus("Refused: current controller state is unavailable");
            return false;
        }
        instrument = m_callbacks.instrumentSnapshot();
        if (expected.index >= instrument.controllers.size())
        {
            SetWizardStatus("Refused: controller record changed; refresh and try again");
            return false;
        }
        const MidiControllerSlot& current = instrument.controllers[expected.index];
        if (current.name != expected.name || current.kind != expected.kind ||
            current.input.identifier != expected.input.identifier ||
            current.input.name != expected.input.name ||
            current.output.identifier != expected.output.identifier ||
            current.output.name != expected.output.name ||
            current.wizardId != expected.wizardId || current.disposition != expected.disposition)
        {
            SetWizardStatus("Refused: controller record changed; refresh and try again");
            return false;
        }
        return true;
    }

    void HandleExistingWizardSubmit()
    {
        if (!m_wizardSession.has_value() ||
            !std::holds_alternative<ExistingWizardTarget>(m_wizardSession->target))
        {
            return;
        }
        const ExistingWizardTarget expected =
            std::get<ExistingWizardTarget>(m_wizardSession->target);
        MidiInstrumentConfig instrument;
        if (!RevalidateExistingWizardTarget(expected, instrument))
        {
            return;
        }

        const MidiControllerSlot& current = instrument.controllers[expected.index];
        WizardGenerationResult generated = m_wizardSession->wizard->GenerateProfile(
            *m_wizardSession->form,
            {.name = current.name, .input = current.input, .output = current.output});
        if (!generated)
        {
            SetWizardStatus(
                "Refused: " +
                (generated.error.empty()
                     ? std::string("controller profile generation failed")
                     : generated.error));
            return;
        }

        MidiControllerSlot replacement = current;
        replacement.kind = generated.controller->kind;
        replacement.config = std::move(generated.controller->config);
        replacement.dormantConfig.reset();
        replacement.disposition = MidiControllerDisposition::Active;
        if (!instrument.ReplaceController(expected.index, std::move(replacement)))
        {
            SetWizardStatus("Refused: generated controller record is invalid");
            return;
        }
        if (!Commit(std::move(instrument)))
        {
            SetWizardStatus("Refused: host rejected the instrument commit");
            return;
        }
        RefreshDiscoveryFromCallbacks();
        if (!SaveCommittedWizardAction(/*sessionStatus=*/true))
        {
            return;
        }
        const std::string name = expected.name;
        CloseWizardSession();
        SetStatus("Reconfigured " + name);
    }

    void HandleWizardIgnore()
    {
        if (!m_wizardSession.has_value() ||
            !std::holds_alternative<WizardCandidate>(
                m_wizardSession->target))
        {
            return;
        }
        HandleIgnoreCandidate(
            std::get<WizardCandidate>(m_wizardSession->target),
            /*sessionStatus=*/true);
    }

    void HandleIgnoreCandidate(const WizardCandidate& candidate,
                               bool sessionStatus)
    {
        MidiInstrumentConfig instrument;
        MidiDeviceList devices;
        if (!RevalidateCandidate(candidate, instrument, devices,
                                 sessionStatus))
        {
            return;
        }

        const std::string name =
            AvailableControllerName(instrument, candidate.displayName);
        MidiControllerSlot controller;
        controller.name = name;
        controller.kind = candidate.kind;
        controller.disposition = MidiControllerDisposition::Blacklisted;
        controller.wizardId = candidate.wizardId;
        controller.input = {.identifier = candidate.input.identifier,
                            .name = candidate.input.name};
        controller.output = {.identifier = candidate.output.identifier,
                             .name = candidate.output.name};

        if (!CommitNewCandidate(std::move(instrument),
                                std::move(controller),
                                sessionStatus))
        {
            return;
        }
        if (sessionStatus)
        {
            CloseWizardSession();
        }
        SetStatus("Ignored " + name);
    }

    static std::size_t ParseIndex(const std::string& text)
    {
        try
        {
            return static_cast<std::size_t>(std::stoull(text));
        }
        catch (...)
        {
            return 0;
        }
    }

    static std::vector<std::string> Split(const std::string& text, char delimiter)
    {
        std::vector<std::string> parts;
        std::stringstream stream(text);
        std::string part;
        while (std::getline(stream, part, delimiter))
        {
            parts.push_back(part);
        }
        return parts;
    }

    void HandleEndpointSelect(const std::string& value)
    {
        const std::size_t firstSeparator = value.find(':');
        if (firstSeparator == std::string::npos)
        {
            return;
        }
        const std::size_t secondSeparator = value.find(':', firstSeparator + 1);
        if (secondSeparator == std::string::npos)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(value.substr(0, firstSeparator));
        const bool output = value.substr(firstSeparator + 1, secondSeparator - firstSeparator - 1) == "output";
        const std::string optionId = value.substr(secondSeparator + 1);

        if (optionId == kEndpointOfflineOptionId)
        {
            return;
        }

        if (optionId == kEndpointNoneOptionId)
        {
            MidiInstrumentConfig out;
            if (m_vm.SetEndpointRef(controllerIx, output, MidiEndpointRef{}, out))
            {
                Commit(std::move(out));
                SetStatus("Cleared device");
            }
            return;
        }

        if (!m_callbacks.enumerateDevices)
        {
            return;
        }

        const MidiDeviceList devices = m_callbacks.enumerateDevices();
        const std::vector<MidiDeviceInfoRef>& list = output ? devices.outputs : devices.inputs;
        for (const MidiDeviceInfoRef& device : list)
        {
            if (device.identifier != optionId)
            {
                continue;
            }
            MidiEndpointRef ref;
            ref.identifier = device.identifier;
            ref.name = device.name;
            MidiInstrumentConfig out;
            if (m_vm.SetEndpointRef(controllerIx, output, ref, out))
            {
                Commit(std::move(out));
                SetStatus("Selected " + device.name);
            }
            return;
        }
    }

    void HandleMappingFieldCommit(const std::string& value)
    {
        const auto parts = Split(value, ':');
        if (parts.size() < 4)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::optional<MidiConfigSection> section = ControllersLayout::ParseSectionToken(parts[1]);
        const std::size_t rowIx = ParseIndex(parts[2]);
        const std::optional<MidiMappingRowVM::Field> field = ControllersLayout::ParseFieldToken(parts[3]);
        if (!section.has_value() || !field.has_value())
        {
            return;
        }

        std::string rawValue;
        for (std::size_t ix = 4; ix < parts.size(); ++ix)
        {
            if (ix > 4)
            {
                rawValue += ':';
            }
            rawValue += parts[ix];
        }

        double numericValue = 0.0;
        try
        {
            std::size_t consumed = 0;
            numericValue = std::stod(rawValue, &consumed);
            if (consumed != rawValue.size() || !std::isfinite(numericValue))
            {
                SetStatus("Refused: value must be a finite number");
                return;
            }
        }
        catch (...)
        {
            SetStatus("Refused: value must be a finite number");
            return;
        }
        MidiInstrumentConfig out;
        std::string reason;
        bool presentationChanged = false;
        if (m_vm.ApplyMappingEdit(controllerIx, *section, rowIx, *field, numericValue, out, &reason,
                                  &presentationChanged))
        {
            Commit(std::move(out));
            SetStatus("OK");
        }
        else if (presentationChanged)
        {
            SetStatus("Warning: " + reason);
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    void HandleDeleteRow(const std::string& value)
    {
        const auto parts = Split(value, ':');
        if (parts.size() != 3)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::optional<MidiConfigSection> section = ControllersLayout::ParseSectionToken(parts[1]);
        const std::size_t rowIx = ParseIndex(parts[2]);
        if (!section.has_value())
        {
            return;
        }
        MidiInstrumentConfig out;
        std::string reason;
        if (m_vm.DeleteRow(controllerIx, *section, rowIx, out, &reason))
        {
            Commit(std::move(out));
            SetStatus("Deleted");
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    void HandleAdd(const std::string& value, bool asBlock)
    {
        const auto parts = Split(value, ':');
        if (parts.size() != 3)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::optional<MidiConfigSection> section = ControllersLayout::ParseSectionToken(parts[1]);
        const std::optional<MidiMappingRowVM::RowGroup> group = ControllersLayout::ParseRowGroupToken(parts[2]);
        if (!section.has_value() || !group.has_value())
        {
            return;
        }
        MidiInstrumentConfig out;
        std::string reason;
        const bool ok = asBlock ? m_vm.AddBlock(controllerIx, *section, *group, out, &reason)
                                : m_vm.AddSingle(controllerIx, *section, *group, out, &reason);
        if (ok)
        {
            Commit(std::move(out));
            SetStatus(asBlock ? "Added block" : "Added");
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    void HandleAddController(const std::string& /*value*/)
    {
        if (!m_callbacks.instrumentSnapshot)
        {
            return;
        }
        const MidiInstrumentConfig instrument = m_callbacks.instrumentSnapshot();
        const std::vector<ControllerWizardDescriptor>& layouts = m_vm.Layouts();
        // The combo defaults to its first option when no draft was recorded
        // (e.g. Add pressed on a freshly opened page with the combo never
        // touched); this must match what the row actually displayed.
        const std::string presetId = ControllersLayout::EffectiveAddPresetId(layouts, m_addPresetId);
        constexpr std::string_view kCustomPrefix = "custom.";

        if (presetId.starts_with(kCustomPrefix))
        {
            // BuildAddPresetOptions is the only source of a "custom." id, and it
            // builds the token with MidiProfileKindName over exactly the four
            // kinds MidiProfileKindFromName parses -- the parse cannot fail, so
            // there is no refusal branch for it.
            MidiProfileKind kind = MidiProfileKind::Generic;
            MidiProfileKindFromName(presetId.substr(kCustomPrefix.size()), kind);
            const std::string name = AvailableControllerName(instrument, MidiProfileKindDisplayName(kind));
            MidiInstrumentConfig out;
            std::string reason;
            if (m_vm.AddController(name, kind, out, &reason))
            {
                Commit(std::move(out));
                SetStatus("Added " + name);
            }
            else
            {
                SetStatus("Refused: " + reason);
            }
            return;
        }

        const ControllerWizardDescriptor* descriptor = nullptr;
        for (const ControllerWizardDescriptor& candidate : layouts)
        {
            if (candidate.id == presetId)
            {
                descriptor = &candidate;
                break;
            }
        }
        if (descriptor == nullptr)
        {
            SetStatus("Refused: unknown controller preset");
            return;
        }

        MidiControllerSlot slot;
        slot.name = AvailableControllerName(instrument, descriptor->displayName);

        // A candidate needs an unclaimed input AND output matching the preset's
        // aliases; with only one side connected there is no candidate, and both
        // ports are left unset so their combos read "(none)".
        const WizardDiscovery addDiscovery = DiscoverControllerWizards(m_devices, instrument, layouts);
        for (const WizardCandidate& candidate : addDiscovery.available)
        {
            if (candidate.wizardId == descriptor->id)
            {
                slot.input = {.identifier = candidate.input.identifier, .name = candidate.input.name};
                slot.output = {.identifier = candidate.output.identifier, .name = candidate.output.name};
                break;
            }
        }

        std::string reason;
        if (!ControllersLayout::InstallDescriptorProfile(layouts, *descriptor, slot, &reason))
        {
            SetStatus("Refused: " + reason);
            return;
        }

        MidiInstrumentConfig out = instrument;
        if (!out.AddController(slot))
        {
            SetStatus("Refused: generated controller record is invalid");
            return;
        }
        if (!Commit(std::move(out)))
        {
            SetStatus("Refused: host rejected the instrument commit");
            return;
        }
        SetStatus("Added " + slot.name);
    }

    void CloseWizardSession()
    {
        m_wizardSession.reset();
        ++m_treeRevision;
    }

    void OpenChooserCandidate(const std::string& token)
    {
        for (std::size_t candidateIx = 0; candidateIx < m_discovery.available.size(); ++candidateIx)
        {
            if (NodeIds::WizardCandidateToken(m_discovery.available[candidateIx]) == token)
            {
                m_wizardChooserStatus.clear();
                OpenCandidate(candidateIx);
                return;
            }
        }
        m_wizardChooserStatus = "That controller is no longer available. Refresh and choose another controller.";
        ++m_treeRevision;
    }

    // The chooser page is built on the component library: one Column stacking
    // the Back action, the heading, an optional status line, and one button per
    // candidate. Nothing here carries bounds — every extent is declared and the
    // resolver derives the geometry from the page extent it is handed, so no
    // backend has to flow or size any of it.
    //
    // Before this it set bounds on the root and on nothing else, and the
    // auto-flow cursor both backends have since deleted was the only thing
    // positioning its children. The Controllers page is built
    // this way; the chooser is here early because the deletions landed first.
    ui::NodeTree BuildWizardChooserTree() const
    {
        ui::LayoutOptions body;
        body.main = ui::Extent::Weight(1.0f);
        body.padding = ControllersLayout::kPageMargin;
        body.gap = ControllersLayout::kRowGap;

        // The Back action keeps its own width instead of stretching over the
        // page, which is the one thing the chooser needs a Row for.
        ui::LayoutOptions actionRow;
        actionRow.main = ui::Extent::Px(ControllersLayout::kBackRowHeight);
        actionRow.padding = 0.0f;
        actionRow.gap = ControllersLayout::kRowGap;

        ui::ControlStyle backButton;
        backButton.layout.main = ui::Extent::Px(ControllersLayout::kBackButtonWidth);

        ui::ControlStyle textRow;
        textRow.layout.main = ui::Extent::Px(ControllersLayout::kStatusRowHeight);

        // A candidate's label names two endpoints, so it takes the page width
        // and a full row's height rather than an intrinsic button extent.
        ui::ControlStyle candidateRow;
        candidateRow.layout.main = ui::Extent::Px(ControllersLayout::kBackRowHeight);

        ui::Builder builder;
        builder.Root(NodeIds::kWizardChooser, m_contentBounds);
        builder.Column(std::string(NodeIds::kWizardChooser) + ".body", body, [&](ui::Builder& page) {
            page.Row(std::string(NodeIds::kWizardChooser) + ".actions", actionRow, [&](ui::Builder& row) {
                row.Button(NodeIds::kWizardBack, "Back", ui::Action::Named(Actions::kWizardBack), backButton);
            });
            page.Label(std::string(NodeIds::kWizardChooser) + ".heading",
                       "Choose a controller to configure",
                       textRow);

            if (m_discovery.available.empty())
            {
                page.StatusText(NodeIds::kWizardChooserEmpty,
                                "No recognized unconfigured controller pair is present",
                                textRow);
                return;
            }

            if (!m_wizardChooserStatus.empty())
            {
                page.StatusText(std::string(NodeIds::kWizardChooser) + ".status",
                                m_wizardChooserStatus,
                                textRow);
            }

            for (const WizardCandidate& candidate : m_discovery.available)
            {
                page.Button(NodeIds::WizardChooserCandidate(candidate),
                            candidate.displayName + " — " + candidate.input.name + " (" +
                                candidate.input.identifier + ") / " + candidate.output.name + " (" +
                                candidate.output.identifier + ")",
                            ui::Action::WithValue(Actions::kWizardChoose,
                                                  NodeIds::WizardCandidateToken(candidate)),
                            candidateRow);
            }
        });
        return builder.Build(m_contentBounds);
    }

    ui::NodeTree BuildWizardFormTree()
    {
        ui::LayoutOptions body;
        body.main = ui::Extent::Weight(1.0f);
        body.padding = ControllersLayout::kPageMargin;
        body.gap = ControllersLayout::kRowGap;

        ui::LayoutOptions actionsRow;
        actionsRow.main = ui::Extent::Px(ControllersLayout::kBackRowHeight);
        actionsRow.padding = 0.0f;
        actionsRow.gap = ControllersLayout::kRowGap;

        const auto buttonStyle = [](float width) {
            ui::ControlStyle style;
            style.color = pagestyle::kDefaultButton;
            style.textStyle = pagestyle::kDefaultTextStyle;
            style.layout.main = ui::Extent::Px(width);
            style.layout.cross = ui::Extent::Px(ControllersLayout::kBackRowHeight);
            return style;
        };

        ui::ControlStyle messageStyle;
        messageStyle.textStyle = pagestyle::kMutedTextStyle;
        messageStyle.layout.main = ui::Extent::Px(ControllersLayout::kStatusRowHeight);

        // The spliced form declares its own width, and a wizard is free to
        // declare one wider than the host surface. `TwisterFormLayout` asked
        // for 664 against this page's 640-wide body, so before this region
        // existed the form overhung its parent by 28px and both backends
        // clipped the right column's argument fields away with no diagnostic.
        // This overflow did not register because the gate only checks the
        // stacking axis, not cross-axis overruns of fixed-extent children. (The
        // form is 684 wide now -- widening `kMessageWidth` to stop the message
        // selectors clipping their own text made the overhang larger, not
        // smaller, which is exactly why the host cannot rely on a form's
        // declared width being one it can show.)
        //
        // A `ScrollArea` is one of two sanctioned absorbing
        // mechanisms, and the resolver publishes its content extent from the
        // children it just placed -- on both axes -- so the whole form stays
        // reachable at any surface a host declares, including one narrower than
        // 640. This is the host's obligation and it holds for third-party
        // wizards too, which the page cannot re-measure. Removing
        // `TwisterFormLayout`'s arithmetic separately stops the Twister form
        // needing to scroll at all; this region is what makes any form safe
        // meanwhile.
        ui::LayoutOptions formScroll;
        formScroll.main = ui::Extent::Weight(1.0f);
        formScroll.padding = 0.0f;
        formScroll.gap = 0.0f;

        ui::Builder builder;
        builder.Root(NodeIds::kWizardForm, m_contentBounds);
        builder.Column(std::string(NodeIds::kWizardForm) + ".body", body, [&](ui::Builder& page) {
            page.ScrollArea(std::string(NodeIds::kWizardForm) + ".scroll",
                            formScroll,
                            [&](ui::Builder& scroll) {
                                scroll.Splice(m_wizardSession->form->BuildSubtree());
                            });
            page.Row(std::string(NodeIds::kWizardForm) + ".actions", actionsRow, [&](ui::Builder& row) {
                row.Button(NodeIds::kWizardBack,
                           "Back",
                           ui::Action::Named(Actions::kWizardBack),
                           buttonStyle(ControllersLayout::kBackButtonWidth));
                row.Button(NodeIds::kWizardCancel,
                           "Cancel",
                           ui::Action::Named(Actions::kWizardCancel),
                           buttonStyle(ControllersLayout::kBackButtonWidth));
                row.Button(NodeIds::kWizardSubmit,
                           "Submit",
                           ui::Action::Named(Actions::kWizardSubmit),
                           buttonStyle(ControllersLayout::kBackButtonWidth));
                if (std::holds_alternative<WizardCandidate>(m_wizardSession->target))
                {
                    row.Button(NodeIds::kWizardIgnore,
                               "Ignore this controller",
                               ui::Action::Named(Actions::kWizardIgnore),
                               buttonStyle(ControllersLayout::kWizardIgnoreWidth));
                }
            });
            if (!m_wizardSession->warning.empty())
            {
                page.StatusText(NodeIds::kWizardWarning,
                                m_wizardSession->warning,
                                messageStyle);
            }
            if (!m_wizardSession->status.empty())
            {
                page.StatusText(NodeIds::kWizardStatus,
                                m_wizardSession->status,
                                messageStyle);
            }
        });
        return builder.Build(m_contentBounds);
    }

    static ui::NodeTree BuildControllersPageTree(const MidiConfigViewModel& vm,
                                                  const MidiDeviceList& devices,
                                                  const WizardDiscovery& discovery,
                                                  ui::Bounds area,
                                                  const std::string& statusText,
                                                  const std::string& addPresetId,
                                                  const std::map<std::string, std::string>& renameDrafts)
    {
        const auto renameDraftFor = [&](const std::string& name) {
            const auto it = renameDrafts.find(name);
            return it != renameDrafts.end() ? it->second : name;
        };
        const float contentWidth =
            std::max(0.0f, area.width - ControllersLayout::kPageMargin * 2.0f);
        const float scrollWidth =
            std::max(contentWidth, ControllersLayout::kControllerHeaderMinWidth);

        const auto layout = [](ui::Extent main, ui::Extent cross, float gap, float padding = 0.0f) {
            ui::LayoutOptions out;
            out.main = main;
            out.cross = cross;
            out.padding = padding;
            out.gap = gap;
            return out;
        };
        const auto columnLayout = [&](ui::Extent main, float gap, float padding = 0.0f) {
            ui::LayoutOptions out = layout(main, ui::Extent::Weight(1.0f), gap, padding);
            return out;
        };
        const auto rowLayout = [&](float height, float width, float gap) {
            return layout(ui::Extent::Px(height), ui::Extent::Px(width), gap);
        };
        const auto style = [](float main, float cross, Color color) {
            ui::ControlStyle out;
            out.color = color;
            out.textStyle = pagestyle::kDefaultTextStyle;
            out.layout.main = ui::Extent::Px(main);
            out.layout.cross = ui::Extent::Px(cross);
            return out;
        };
        const auto labelStyle = [](float main) {
            ui::ControlStyle out;
            out.textStyle = pagestyle::kDefaultTextStyle;
            out.layout.main = ui::Extent::Px(main);
            return out;
        };
        const auto statusStyle = [](float main) {
            ui::ControlStyle out;
            out.textStyle = pagestyle::kMutedTextStyle;
            out.layout.main = ui::Extent::Px(main);
            return out;
        };
        const auto button = [&](float width,
                                float height = ControllersLayout::kControllerHeaderLineHeight) {
            return style(width, height, pagestyle::kDefaultButton);
        };
        const auto fieldControl = [&](float width,
                                      float height = ControllersLayout::kControllerHeaderLineHeight) {
            return style(width, height, pagestyle::kDefaultPanel);
        };
        const auto columnControl = [](float width, float height) {
            ui::ControlStyle out;
            out.color = pagestyle::kDefaultButton;
            out.textStyle = pagestyle::kDefaultTextStyle;
            out.layout.main = ui::Extent::Px(height);
            out.layout.cross = ui::Extent::Px(width);
            return out;
        };

        const auto diagnosticText = [](const char* label, const std::vector<MidiDeviceInfoRef>& devices) {
            std::string text = label;
            for (const MidiDeviceInfoRef& device : devices)
            {
                if (text.size() > std::string_view(label).size())
                {
                    text += ", ";
                }
                text += device.name;
            }
            return text;
        };

        const auto emitAvailable = [&](ui::Builder& scroll) {
            ui::LayoutOptions availableLayout = columnLayout(ui::Extent::Intrinsic(), 0.0f);
            availableLayout.cross = ui::Extent::Px(scrollWidth);
            scroll.Section(NodeIds::kAvailable,
                           availableLayout,
                           [&](ui::Builder& available) {
                               available.Label(NodeIds::kAvailableHeading,
                                               "Available controllers",
                                               labelStyle(ControllersLayout::kStatusRowHeight));
                               if (discovery.available.empty())
                               {
                                   available.StatusText(
                                       NodeIds::kAvailableEmpty,
                                       "No recognized unconfigured controller pair is present",
                                       statusStyle(ControllersLayout::kStatusRowHeight));
                               }
                               else
                               {
                                   for (std::size_t candidateIx = 0;
                                        candidateIx < discovery.available.size();
                                        ++candidateIx)
                                   {
                                       const WizardCandidate& candidate =
                                           discovery.available[candidateIx];
                                       available.Row(
                                           NodeIds::AvailableRow(candidateIx),
                                           rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                                     scrollWidth,
                                                     ControllersLayout::kAvailableControlGap),
                                           [&](ui::Builder& row) {
                                               row.Label(NodeIds::AvailableName(candidateIx),
                                                         candidate.displayName,
                                                         labelStyle(ControllersLayout::kAvailableNameWidth));
                                               row.Label(NodeIds::AvailableRow(candidateIx) + ".endpoints",
                                                         candidate.input.name + " / " +
                                                             candidate.output.name,
                                                         labelStyle(ControllersLayout::kAvailableEndpointsWidth));
                                               row.Button(NodeIds::AvailableConfigure(candidateIx),
                                                          "Configure",
                                                          ui::Action::WithValue(
                                                              Actions::kAvailableConfigure,
                                                              std::to_string(candidateIx)),
                                                          button(ControllersLayout::kAvailableConfigureWidth));
                                               row.Button(NodeIds::AvailableIgnore(candidateIx),
                                                          "Ignore",
                                                          ui::Action::WithValue(
                                                              Actions::kAvailableIgnore,
                                                              NodeIds::WizardCandidateToken(candidate)),
                                                          button(ControllersLayout::kAvailableIgnoreWidth));
                                           });
                                   }
                               }
                               if (!discovery.unmatchedInputs.empty())
                               {
                                   available.StatusText(
                                       NodeIds::kAvailableUnmatchedInputs,
                                       diagnosticText("Unmatched input: ", discovery.unmatchedInputs),
                                       statusStyle(ControllersLayout::kStatusRowHeight));
                               }
                               if (!discovery.unmatchedOutputs.empty())
                               {
                                   available.StatusText(
                                       NodeIds::kAvailableUnmatchedOutputs,
                                       diagnosticText("Unmatched output: ", discovery.unmatchedOutputs),
                                       statusStyle(ControllersLayout::kStatusRowHeight));
                               }
                           });
        };

        const auto emitMappingField = [&](ui::Builder& mappingRow,
                                          std::size_t controllerIx,
                                          MidiConfigSection section,
                                          std::size_t mappingRowIx,
                                          MidiMappingRowVM::Field field) {
            const float fieldWidth = static_cast<float>(ControllersLayout::FieldEditorWidth(field));
            ui::ControlStyle fieldStyle =
                fieldControl(fieldWidth, ControllersLayout::kMappingRowHeight);
            ui::ControlStyle toggleStyle = button(fieldWidth, ControllersLayout::kMappingRowHeight);
            if (field == MidiMappingRowVM::Field::MessageKind)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = vm.MessageCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
                }
                const int current = vm.UISystemMessageIndex(controllerIx, section, mappingRowIx);
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    current >= 0 ? std::to_string(current) : "0",
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
                return;
            }
            if (field == MidiMappingRowVM::Field::AppAction)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = vm.AnalogActionCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
                }
                double current = 0.0;
                std::string selected = "0";
                if (vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current))
                {
                    selected = std::to_string(static_cast<int>(current));
                }
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    selected,
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
                return;
            }
            if (field == MidiMappingRowVM::Field::EncoderMode)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = EncoderModeCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)]});
                }
                double current = 0.0;
                std::string selected = "0";
                if (vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current))
                {
                    selected = std::to_string(static_cast<int>(current));
                }
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    selected,
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
                return;
            }
            if (field == MidiMappingRowVM::Field::AddressType)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = ControlAddressTypeCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)]});
                }
                double current = 0.0;
                std::string selected = "0";
                if (vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current))
                {
                    const int currentIx = static_cast<int>(current);
                    if (current == static_cast<double>(currentIx) && currentIx >= 0 &&
                        currentIx < static_cast<int>(catalog.size()))
                    {
                        selected = std::to_string(currentIx);
                    }
                }
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    selected,
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
                return;
            }
            if (field == MidiMappingRowVM::Field::BlockMessageType)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = BlockableMessageCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)]});
                }
                const int current = vm.BlockMessageTypeIndex(controllerIx, section, mappingRowIx);
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    current >= 0 ? std::to_string(current) : "0",
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
                return;
            }
            if (field == MidiMappingRowVM::Field::BlockRowMajor ||
                field == MidiMappingRowVM::Field::BlockOutputFeedback)
            {
                double current = 0.0;
                if (vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current))
                {
                    mappingRow.Toggle(
                        NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                        FieldShortLabel(field),
                        current != 0.0,
                        ui::Action::WithValue(
                            Actions::kMappingFieldCommit,
                            std::to_string(controllerIx) + ":" +
                                ControllersLayout::SectionToken(section) + ":" +
                                std::to_string(mappingRowIx) + ":" +
                                ControllersLayout::FieldToken(field)),
                        toggleStyle);
                }
                return;
            }

            double initial = 0.0;
            if (!vm.RowFieldValue(controllerIx, section, mappingRowIx, field, initial))
            {
                return;
            }
            mappingRow.TextField(
                NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                FieldShortLabel(field),
                ControllersLayout::FormatFieldValue(field, initial),
                ui::Action::WithValue(
                    Actions::kMappingFieldCommit,
                    std::to_string(controllerIx) + ":" + ControllersLayout::SectionToken(section) +
                        ":" + std::to_string(mappingRowIx) + ":" +
                        ControllersLayout::FieldToken(field)),
                fieldStyle);
        };

        const auto emitSection = [&](ui::Builder& scroll,
                                     std::size_t controllerIx,
                                     MidiConfigSection section) {
            const bool expanded = vm.SectionExpanded(controllerIx, section);
            ui::ControlStyle sectionToggleStyle =
                columnControl(220.0f, ControllersLayout::kSectionHeaderHeight);
            scroll.Button(NodeIds::SectionToggle(controllerIx, section),
                          std::string(ControllersLayout::SectionName(section)) +
                              (expanded ? " v" : " >"),
                          ui::Action::WithValue(
                              Actions::kToggleSection,
                              std::to_string(controllerIx) + ":" +
                                  ControllersLayout::SectionToken(section)),
                          sectionToggleStyle);
            if (!expanded)
            {
                return;
            }

            const std::vector<MidiMappingRowVM> rows = vm.SectionRows(controllerIx, section);
            // The fields plus the gaps drawn between them, which is what the
            // rows below actually occupy.
            auto fieldsWidth = [](const std::vector<MidiMappingRowVM::Field>& fields) {
                float width = 0.0f;
                for (MidiMappingRowVM::Field field : fields)
                {
                    width += static_cast<float>(ControllersLayout::FieldEditorWidth(field));
                }
                if (!fields.empty())
                {
                    width += static_cast<float>(fields.size() - 1) *
                             ControllersLayout::kEditorColumnGap;
                }
                return width;
            };
            float desiredSectionWidth = 420.0f;
            for (const MidiMappingRowVM& row : rows)
            {
                float headerControlsWidth = 0.0f;
                if (vm.GroupSupportsAdd(controllerIx, section, row.group))
                {
                    headerControlsWidth += ControllersLayout::kAddButtonWidth;
                    if (vm.GroupSupportsBlocks(controllerIx, section, row.group))
                    {
                        headerControlsWidth += ControllersLayout::kEditorColumnGap +
                                               ControllersLayout::kAddButtonWidth;
                    }
                }
                desiredSectionWidth = std::max(
                    desiredSectionWidth,
                    fieldsWidth(row.editableFields) + ControllersLayout::kEditorColumnGap +
                        std::max(headerControlsWidth,
                                 row.deletable ? ControllersLayout::kDeleteButtonWidth : 0.0f));
            }
            for (MidiMappingRowVM::RowGroup group : vm.AddableGroups(controllerIx, section))
            {
                desiredSectionWidth = std::max(
                    desiredSectionWidth,
                    fieldsWidth(vm.GroupColumnFields(controllerIx, section, group)) +
                        ControllersLayout::kEditorColumnGap * 2.0f +
                        ControllersLayout::kAddButtonWidth * 2.0f + 16.0f);
            }
            const float sectionWidth = desiredSectionWidth + 16.0f;
            ui::LayoutOptions sectionLayout =
                columnLayout(ui::Extent::Intrinsic(), 0.0f);
            sectionLayout.cross = ui::Extent::Px(sectionWidth);
            scroll.Section(NodeIds::SectionBody(controllerIx, section), sectionLayout, [&](ui::Builder& body) {
                std::size_t headerIx = 0;
                std::size_t mappingRowIx = 0;
                std::optional<MidiMappingRowVM::RowGroup> previousGroup;
                std::optional<std::vector<MidiMappingRowVM::Field>> previousFields;
                std::set<MidiMappingRowVM::RowGroup> seenGroups;

                const auto emitGroupHeader = [&](MidiMappingRowVM::RowGroup group,
                                                 const std::vector<MidiMappingRowVM::Field>& fields,
                                                 bool isFirstHeaderForGroup,
                                                 MidiMappingRowVM::Kind kind) {
                    const std::string headerId = NodeIds::GroupHeader(controllerIx, section, headerIx);
                    const bool showColumnLabels = fields.size() > 1;
                    const bool showAddControls =
                        isFirstHeaderForGroup && vm.GroupSupportsAdd(controllerIx, section, group);
                    const bool showBlockControl =
                        showAddControls && vm.GroupSupportsBlocks(controllerIx, section, group);
                    body.Label(headerId + ".caption",
                               ControllersLayout::RowGroupCaption(group, kind),
                               labelStyle(ControllersLayout::kStatusRowHeight));
                    if (showColumnLabels || showAddControls)
                    {
                        body.Row(headerId,
                                 rowLayout(ControllersLayout::kGroupHeaderHeight,
                                           sectionWidth,
                                           ControllersLayout::kEditorColumnGap),
                                 [&](ui::Builder& header) {
                                     for (std::size_t fieldIx = 0; fieldIx < fields.size(); ++fieldIx)
                                     {
                                         if (!showColumnLabels)
                                         {
                                             break;
                                         }
                                         const MidiMappingRowVM::Field field = fields[fieldIx];
                                         header.Label(
                                             NodeIds::GroupColumnLabel(
                                                 controllerIx, section, headerIx, fieldIx),
                                             FieldShortLabel(field),
                                             labelStyle(static_cast<float>(
                                                 ControllersLayout::FieldEditorWidth(field))));
                                     }
                                     if (showAddControls)
                                     {
                                         header.Button(
                                             NodeIds::GroupAddSingle(controllerIx, section, headerIx),
                                             "Add",
                                             ui::Action::WithValue(
                                                 Actions::kAddSingle,
                                                 std::to_string(controllerIx) + ":" +
                                                     ControllersLayout::SectionToken(section) + ":" +
                                                     ControllersLayout::RowGroupToken(group)),
                                             button(ControllersLayout::kAddButtonWidth, 28.0f));
                                         if (showBlockControl)
                                         {
                                             header.Button(
                                                 NodeIds::GroupAddBlock(controllerIx, section, headerIx),
                                                 "Block",
                                                 ui::Action::WithValue(
                                                     Actions::kAddBlock,
                                                     std::to_string(controllerIx) + ":" +
                                                         ControllersLayout::SectionToken(section) + ":" +
                                                         ControllersLayout::RowGroupToken(group)),
                                                 button(ControllersLayout::kAddButtonWidth, 28.0f));
                                         }
                                     }
                                 });
                    }
                    ++headerIx;
                };

                const auto emitMappingRow = [&](const MidiMappingRowVM& rowVmRow) {
                    body.Row(NodeIds::MappingRow(controllerIx, section, mappingRowIx),
                             rowLayout(ControllersLayout::kMappingRowHeight,
                                       sectionWidth,
                                       ControllersLayout::kEditorColumnGap),
                             [&](ui::Builder& row) {
                                 for (MidiMappingRowVM::Field field : rowVmRow.editableFields)
                                 {
                                     emitMappingField(row, controllerIx, section, mappingRowIx, field);
                                 }
                                 if (rowVmRow.deletable)
                                 {
                                     row.Button(
                                         NodeIds::MappingDelete(controllerIx, section, mappingRowIx),
                                         "x",
                                         ui::Action::WithValue(
                                             Actions::kDeleteRow,
                                             std::to_string(controllerIx) + ":" +
                                                 ControllersLayout::SectionToken(section) + ":" +
                                                 std::to_string(mappingRowIx)),
                                         button(ControllersLayout::kDeleteButtonWidth,
                                                ControllersLayout::kMappingRowHeight));
                                 }
                             });
                    ++mappingRowIx;
                };

                for (std::size_t rowIx = 0; rowIx < rows.size(); ++rowIx)
                {
                    const MidiMappingRowVM::RowGroup group = rows[rowIx].group;
                    if (!previousGroup.has_value() || *previousGroup != group ||
                        *previousFields != rows[rowIx].editableFields)
                    {
                        const bool isFirstHeaderForGroup = seenGroups.insert(group).second;
                        emitGroupHeader(group, rows[rowIx].editableFields, isFirstHeaderForGroup,
                                        rows[rowIx].kind);
                        previousGroup = group;
                        previousFields = rows[rowIx].editableFields;
                    }
                    emitMappingRow(rows[rowIx]);
                }

                for (MidiMappingRowVM::RowGroup group : vm.AddableGroups(controllerIx, section))
                {
                    if (seenGroups.count(group) != 0)
                    {
                        continue;
                    }
                    emitGroupHeader(group,
                                    vm.GroupColumnFields(controllerIx, section, group),
                                    true,
                                    MidiMappingRowVM::Kind::Individual);
                }
            });
        };

        const auto emitControllerRow = [&](ui::Builder& scroll,
                                           const MidiControllerRowVM& rowVm,
                                           std::size_t controllerIx) {
            scroll.Column(
                NodeIds::ControllerRow(controllerIx),
                rowLayout(ControllersLayout::kControllerHeaderHeight, scrollWidth, 0.0f),
                [&](ui::Builder& section) {
                    if (rowVm.disposition == MidiControllerDisposition::Blacklisted)
                    {
                        section.Row(
                            NodeIds::ControllerRow(controllerIx) + ".line1",
                            rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                     scrollWidth,
                                     ControllersLayout::kLifecycleControlGap),
                            [&](ui::Builder& row) {
                                row.Label(NodeIds::ControllerName(controllerIx),
                                         rowVm.name,
                                         labelStyle(ControllersLayout::kControllerNameWidth));
                                row.Label(NodeIds::ControllerKind(controllerIx),
                                         MidiProfileKindDisplayName(rowVm.kind),
                                         labelStyle(ControllersLayout::kControllerKindWidth));
                                row.Label(NodeIds::ControllerBadge(controllerIx),
                                         "Released",
                                         labelStyle(ControllersLayout::kBlacklistedBadgeWidth));
                            });
                        section.Row(
                            NodeIds::ControllerRow(controllerIx) + ".line2",
                            rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                     scrollWidth,
                                     ControllersLayout::kLifecycleControlGap),
                            [&](ui::Builder& row) {
                                row.Label(NodeIds::ControllerInputLabel(controllerIx),
                                         "MIDI in: " +
                                             ControllersLayout::StoredEndpointLabel(rowVm.storedInput),
                                         labelStyle(ControllersLayout::kBlacklistedEndpointLabelWidth));
                                row.Label(NodeIds::ControllerOutputLabel(controllerIx),
                                         "MIDI out: " +
                                             ControllersLayout::StoredEndpointLabel(rowVm.storedOutput),
                                         labelStyle(ControllersLayout::kBlacklistedEndpointLabelWidth));
                                if (rowVm.hasResolvedWizard)
                                {
                                    row.Button(
                                        NodeIds::ControllerConfigure(controllerIx),
                                        "Configure",
                                        ui::Action::WithValue(
                                            Actions::kControllerConfigure,
                                            NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                        button(ControllersLayout::kLifecycleConfigureWidth));
                                }
                                row.Button(
                                    NodeIds::ControllerRemoveBlacklist(controllerIx),
                                    "Reclaim",
                                    ui::Action::WithValue(
                                        Actions::kControllerRemoveBlacklist,
                                        NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                    button(ControllersLayout::kLifecycleRemoveWidth));
                            });
                        return;
                    }

                    section.Row(
                        NodeIds::ControllerRow(controllerIx) + ".line1",
                        rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                 scrollWidth,
                                 ControllersLayout::kLifecycleControlGap),
                        [&](ui::Builder& row) {
                            row.Button(NodeIds::ControllerDisclosure(controllerIx),
                                      rowVm.configExpanded ? "v" : ">",
                                      ui::Action::WithValue(Actions::kToggleConfig,
                                                            std::to_string(controllerIx)),
                                      button(ControllersLayout::kControllerDisclosureWidth));
                            row.Label(NodeIds::ControllerName(controllerIx),
                                     rowVm.name,
                                     labelStyle(ControllersLayout::kControllerNameWidth));
                            row.Label(NodeIds::ControllerKind(controllerIx),
                                     MidiProfileKindDisplayName(rowVm.kind),
                                     labelStyle(ControllersLayout::kControllerKindWidth));
                        });

                    section.Row(
                        NodeIds::ControllerRow(controllerIx) + ".line2",
                        rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                 scrollWidth,
                                 ControllersLayout::kLifecycleControlGap),
                        [&](ui::Builder& row) {
                            const float portWidth = ControllersLayout::kStatusDotWidth +
                                                    ControllersLayout::kLifecycleControlGap +
                                                    ControllersLayout::kEndpointFieldWidth;
                            const float endpointClusterWidth =
                                portWidth + ControllersLayout::kEndpointBoxGap + portWidth;
                            ui::LayoutOptions portLayout =
                                layout(ui::Extent::Px(portWidth),
                                      ui::Extent::Px(ControllersLayout::kControllerHeaderLineHeight),
                                      ControllersLayout::kLifecycleControlGap);
                            row.Row(NodeIds::ControllerRow(controllerIx) + ".endpoints",
                                   layout(ui::Extent::Px(endpointClusterWidth),
                                          ui::Extent::Px(ControllersLayout::kControllerHeaderLineHeight),
                                          ControllersLayout::kEndpointBoxGap),
                                   [&](ui::Builder& endpoints) {
                                       endpoints.Row(
                                           NodeIds::ControllerRow(controllerIx) + ".input_port",
                                           portLayout,
                                           [&](ui::Builder& inputPort) {
                                               ControllersLayout::EmitStatusDot(inputPort,
                                                          NodeIds::ControllerInputStatus(controllerIx),
                                                          rowVm.inputStatus);
                                               std::string selectedInput;
                                               ui::ControlStyle inputStyle =
                                                   fieldControl(ControllersLayout::kEndpointFieldWidth);
                                               inputStyle.caption = "MIDI in";
                                               inputPort.ComboBox(
                                                   NodeIds::ControllerInput(controllerIx),
                                                   ControllersLayout::BuildEndpointOptions(
                                                       devices.inputs,
                                                       rowVm.inputStatus,
                                                       rowVm.storedInput,
                                                       rowVm.inputDeviceLabel,
                                                       selectedInput),
                                                   selectedInput,
                                                   ui::Action::WithValue(
                                                       Actions::kEndpointSelect,
                                                       std::to_string(controllerIx) + ":input"),
                                                   inputStyle);
                                           });
                                       endpoints.Row(
                                           NodeIds::ControllerRow(controllerIx) + ".output_port",
                                           portLayout,
                                           [&](ui::Builder& outputPort) {
                                               ControllersLayout::EmitStatusDot(outputPort,
                                                          NodeIds::ControllerOutputStatus(controllerIx),
                                                          rowVm.outputStatus);
                                               std::string selectedOutput;
                                               ui::ControlStyle outputStyle =
                                                   fieldControl(ControllersLayout::kEndpointFieldWidth);
                                               outputStyle.caption = "MIDI out";
                                               outputPort.ComboBox(
                                                   NodeIds::ControllerOutput(controllerIx),
                                                   ControllersLayout::BuildEndpointOptions(
                                                       devices.outputs,
                                                       rowVm.outputStatus,
                                                       rowVm.storedOutput,
                                                       rowVm.outputDeviceLabel,
                                                       selectedOutput),
                                                   selectedOutput,
                                                   ui::Action::WithValue(
                                                       Actions::kEndpointSelect,
                                                       std::to_string(controllerIx) + ":output"),
                                                   outputStyle);
                                           });
                                   });
                            row.Button(NodeIds::ControllerDelete(controllerIx),
                                      "Delete",
                                      ui::Action::WithValue(
                                          Actions::kControllerDelete,
                                          NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                      button(ControllersLayout::kLifecycleDeleteWidth));
                            if (rowVm.hasResolvedWizard && !rowVm.matchesWizardProfile)
                            {
                                row.Button(
                                    NodeIds::ControllerRestore(controllerIx),
                                    "Restore",
                                    ui::Action::WithValue(
                                        Actions::kControllerRestore,
                                        NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                    button(ControllersLayout::kLifecycleRestoreWidth));
                            }
                            if (rowVm.hasResolvedWizard && rowVm.hasCompleteEndpointPair)
                            {
                                row.Button(
                                    NodeIds::ControllerBlacklist(controllerIx),
                                    "Release",
                                    ui::Action::WithValue(
                                        Actions::kControllerBlacklist,
                                        NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                    button(ControllersLayout::kLifecycleBlacklistWidth));
                            }
                        });
                });
        };

        ui::Builder builder;
        builder.Root(NodeIds::kRoot, area);

        ui::LayoutOptions pageLayout;
        pageLayout.main = ui::Extent::Weight(1.0f);
        pageLayout.cross = ui::Extent::Weight(1.0f);
        pageLayout.padding = ControllersLayout::kPageMargin;
        pageLayout.gap = ControllersLayout::kRowGap;

        ui::LayoutOptions actionsLayout =
            rowLayout(ControllersLayout::kBackRowHeight, contentWidth, ControllersLayout::kRowGap);
        ui::LayoutOptions scrollLayout;
        scrollLayout.main = ui::Extent::Weight(1.0f);
        scrollLayout.cross = ui::Extent::Weight(1.0f);
        scrollLayout.padding = 0.0f;
        scrollLayout.gap = ControllersLayout::kRowGap;

        builder.Column(std::string(NodeIds::kRoot) + ".page", pageLayout, [&](ui::Builder& page) {
            page.Row(std::string(NodeIds::kRoot) + ".actions", actionsLayout, [&](ui::Builder& actions) {
                actions.Button(NodeIds::kBack,
                               "Back",
                               ui::Action::Named(Actions::kBack),
                               button(ControllersLayout::kBackButtonWidth,
                                      ControllersLayout::kBackRowHeight));
                ui::ControlStyle wizard = button(180.0f, ControllersLayout::kBackRowHeight);
                wizard.enabled = !discovery.available.empty();
                actions.Button(NodeIds::kWizardLaunch,
                               "Configuration Wizard",
                               ui::Action::Named(Actions::kWizardOpen),
                               wizard);
            });
            page.ScrollArea(NodeIds::kScroll, scrollLayout, [&](ui::Builder& scroll) {
                emitAvailable(scroll);
                // The status dots on each controller row carry no legend of their
                // own (a Label cannot carry three colours), so this row lays out
                // three in-flow dot/word pairs, each pair its own small Row so the
                // tight gap inside a pair and the wider gap between pairs can
                // differ without either being a hand-placed offset.
                scroll.Row(
                    std::string(NodeIds::kStatusLegend) + ".row",
                    rowLayout(ControllersLayout::kStatusRowHeight,
                             scrollWidth,
                             ControllersLayout::kStatusLegendPairGap),
                    [&](ui::Builder& legend) {
                        struct LegendEntry {
                            const char* suffix;
                            const char* word;
                            MidiEndpointStatus status;
                        };
                        const LegendEntry entries[] = {
                            {".online", "online", MidiEndpointStatus::Online},
                            {".offline", "offline", MidiEndpointStatus::Offline},
                            {".not_set", "not set", MidiEndpointStatus::Unconfigured},
                        };
                        for (const LegendEntry& entry : entries)
                        {
                            const std::string dotId =
                                std::string(NodeIds::kStatusLegend) + entry.suffix;
                            const std::string pairId = dotId + ".pair";
                            const std::string labelId = dotId + ".label";
                            ui::LayoutOptions pairLayout;
                            pairLayout.main = ui::Extent::Intrinsic();
                            pairLayout.padding = 0.0f;
                            pairLayout.gap = ControllersLayout::kLifecycleControlGap;
                            legend.Row(pairId, pairLayout, [&](ui::Builder& pair) {
                                ControllersLayout::EmitStatusDot(pair, dotId, entry.status);
                                ui::ControlStyle labelAutoStyle;
                                labelAutoStyle.textStyle = pagestyle::kDefaultTextStyle;
                                pair.Label(labelId, entry.word, labelAutoStyle);
                            });
                        }
                    });
                const auto& controllers = vm.Controllers();
                for (std::size_t controllerIx = 0; controllerIx < controllers.size(); ++controllerIx)
                {
                    const MidiControllerRowVM& rowVm = controllers[controllerIx];
                    emitControllerRow(scroll, rowVm, controllerIx);
                    if (rowVm.disposition == MidiControllerDisposition::Blacklisted ||
                        !rowVm.configExpanded)
                    {
                        continue;
                    }
                    // The expanded editor's first row, above the three section
                    // toggles: the record's name, moved here from the header.
                    scroll.Row(
                        NodeIds::ControllerRow(controllerIx) + ".name_row",
                        rowLayout(ControllersLayout::kControllerHeaderLineHeight, scrollWidth,
                                 ControllersLayout::kLifecycleControlGap),
                        [&](ui::Builder& row) {
                            ui::ControlStyle nameDraftStyle =
                                fieldControl(ControllersLayout::kLifecycleDraftWidth);
                            nameDraftStyle.caption = "Name";
                            row.TextField(
                                NodeIds::ControllerRenameDraft(controllerIx),
                                "Rename",
                                renameDraftFor(rowVm.name),
                                ui::Action::WithValue(
                                    Actions::kControllerRenameDraft,
                                    NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                nameDraftStyle);
                            row.Button(
                                NodeIds::ControllerRename(controllerIx),
                                "Rename",
                                ui::Action::WithValue(
                                    Actions::kControllerRename,
                                    NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                button(ControllersLayout::kLifecycleRenameWidth));
                        });
                    for (MidiConfigSection section : rowVm.sections)
                    {
                        emitSection(scroll, controllerIx, section);
                    }
                }
                scroll.Row(NodeIds::kAddRow,
                           rowLayout(ControllersLayout::kAddRowHeight,
                                     scrollWidth,
                                     ControllersLayout::kAvailableControlGap),
                           [&](ui::Builder& row) {
                               std::vector<ui::ControlOption> presetOptions =
                                   ControllersLayout::BuildAddPresetOptions(vm.Layouts());
                               const std::string selectedPreset =
                                   ControllersLayout::EffectiveAddPresetId(vm.Layouts(), addPresetId);
                               ui::ControlStyle addPresetStyle =
                                   fieldControl(260.0f, ControllersLayout::kAddRowHeight);
                               addPresetStyle.caption = "Preset";
                               row.ComboBox(NodeIds::kAddPreset,
                                            std::move(presetOptions),
                                            selectedPreset,
                                            ui::Action::Named(Actions::kAddPresetDraft),
                                            addPresetStyle);
                               row.Button(NodeIds::kAddButton,
                                          "Add",
                                          ui::Action::Named(Actions::kAddController),
                                          button(72.0f, ControllersLayout::kAddRowHeight));
                           });
            });
            page.StatusText(NodeIds::kStatus,
                            statusText.empty() ? "Ready" : statusText,
                            statusStyle(ControllersLayout::kStatusRowHeight));
        });

        return builder.Build(area);
    }

    ControllersPageCallbacks m_callbacks;
    MidiConfigViewModel m_vm;
    MidiDeviceList m_devices;
    WizardDiscovery m_discovery;
    std::optional<WizardSession> m_wizardSession;
    bool m_wizardChooserOpen = false;
    std::string m_wizardChooserStatus;
    ui::Bounds m_contentBounds{0.0f, 0.0f, 640.0f, 480.0f};
    std::string m_statusText = "Ready";
    std::string m_addPresetId;
    std::map<std::string, std::string> m_renameDrafts;
    bool m_dirty = true;
    std::string m_lastFingerprint;
    std::uint64_t m_treeRevision = 1;
    ActionHandler m_outerHandler_;
    std::function<bool()> m_focusGuard;
};

}  // namespace synth::runtime_ui
