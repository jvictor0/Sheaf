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

inline std::string AvailableRow(std::size_t candidateIx)
{
    return "runtime.controllers.available." + std::to_string(candidateIx);
}

inline std::string ControllerRow(std::size_t controllerIx)
{
    return "runtime.controllers.row." + std::to_string(controllerIx);
}

inline std::string ControllerActionToken(std::size_t controllerIx,
                                         std::string_view name)
{
    return std::to_string(controllerIx) + ':' + HexEncodeBytes(name, /*uppercase=*/false);
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
    std::string name;
    if (!HexDecodeBytes(encodedName, name, /*separatedByWhitespace=*/false))
    {
        return std::nullopt;
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

inline std::string ControllerDevice(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".device";
}

inline std::string ControllerVariant(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".variant";
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

inline std::string ControllerRestore(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".restore";
}

inline std::string ControllerPresetNotice(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".preset_notice";
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

inline std::string ConnectMessages(std::size_t controllerIx)
{
    return ControllerRow(controllerIx) + ".connect_messages";
}

inline std::string ConnectMessagesHeading(std::size_t controllerIx)
{
    return ConnectMessages(controllerIx) + ".heading";
}

inline std::string ConnectMessageRow(std::size_t controllerIx, std::size_t messageIx)
{
    return ConnectMessages(controllerIx) + ".row." + std::to_string(messageIx);
}

inline std::string ConnectMessageField(std::size_t controllerIx, std::size_t messageIx)
{
    return ConnectMessageRow(controllerIx, messageIx) + ".field";
}

inline std::string ConnectMessageDelete(std::size_t controllerIx, std::size_t messageIx)
{
    return ConnectMessageRow(controllerIx, messageIx) + ".delete";
}

inline std::string ConnectMessageAdd(std::size_t controllerIx)
{
    return ConnectMessages(controllerIx) + ".add";
}

inline std::string ConnectMessageAddRow(std::size_t controllerIx)
{
    return ConnectMessages(controllerIx) + ".add_row";
}

}  // namespace NodeIds

namespace Actions {

inline constexpr const char* kBack = "runtime.controllers.back";
inline constexpr const char* kToggleConfig = "runtime.controllers.toggle_config";
inline constexpr const char* kToggleSection = "runtime.controllers.toggle_section";
inline constexpr const char* kEndpointSelect = "runtime.controllers.endpoint_select";
inline constexpr const char* kVariantSelect = "runtime.controllers.variant_select";
inline constexpr const char* kMappingFieldCommit = "runtime.controllers.mapping_field_commit";
inline constexpr const char* kDeleteRow = "runtime.controllers.delete_row";
inline constexpr const char* kAddSingle = "runtime.controllers.add_single";
inline constexpr const char* kAddBlock = "runtime.controllers.add_block";
inline constexpr const char* kAddPresetDraft = "runtime.controllers.add_preset_draft";
inline constexpr const char* kAddController = "runtime.controllers.add_controller";
inline constexpr const char* kControllerRename = "runtime.controllers.controller.rename";
inline constexpr const char* kControllerRenameDraft = "runtime.controllers.controller.rename_draft";
inline constexpr const char* kControllerDelete = "runtime.controllers.controller.delete";
inline constexpr const char* kControllerRestore = "runtime.controllers.controller.restore";
inline constexpr const char* kConnectMessageCommit = "runtime.controllers.connect_message_commit";
inline constexpr const char* kConnectMessageDelete = "runtime.controllers.connect_message_delete";
inline constexpr const char* kConnectMessageAdd = "runtime.controllers.connect_message_add";

// The fixed part of what the Controllers page emits. The per-controller
// actions above are not listed: they are matched by prefix, because their
// names carry a controller index the page composes at build time and no
// fixed set can hold them.
inline constexpr std::string_view kControllersActions[] = {
    kBack,
    kToggleConfig,
    kToggleSection,
    kEndpointSelect,
    kVariantSelect,
    kMappingFieldCommit,
    kDeleteRow,
    kAddSingle,
    kAddBlock,
    kAddPresetDraft,
    kAddController,
    kConnectMessageCommit,
    kConnectMessageDelete,
    kConnectMessageAdd,
};

}  // namespace Actions

namespace ControllersLayout {

inline constexpr float kPageMargin = 4.0f;
inline constexpr float kBackRowHeight = 32.0f;
inline constexpr float kBackButtonWidth = 80.0f;
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
// The add row's own column gap.
inline constexpr float kAvailableControlGap = 8.0f;
inline constexpr float kControllerNameWidth = 200.0f;
inline constexpr float kControllerDisclosureWidth = 24.0f;
// Line one's model selector, on launchpad rows only: wide enough for
// "Launchpad Mini MK3" plus its caption.
inline constexpr float kVariantFieldWidth = 180.0f;
// The draft column is wide enough to hold the "Name" caption plus a usable
// field for a short name.
inline constexpr float kLifecycleDraftWidth = 160.0f;
inline constexpr float kLifecycleRenameWidth = 72.0f;
inline constexpr float kLifecycleDeleteWidth = 66.0f;
inline constexpr float kLifecycleRestoreWidth = 76.0f;
inline constexpr float kLifecycleControlGap = 4.0f;
// The name draft and Rename button live in the expanded editor, so line
// two's lifecycle block is Delete alone; Restore moves to the third line,
// shown only when its own row condition holds.
inline constexpr float kBlacklistedEndpointLabelWidth = 240.0f;
inline constexpr float kBlacklistedBadgeWidth = 84.0f;
// A blacklisted record has no expanded editor to hold a moved rename field,
// and no story for renaming a row the operator is ignoring, so the name
// draft and Rename button are simply gone here, not moved. Its only
// lifecycle control is the same Delete button an active row offers.
inline constexpr float kBlacklistedLifecycleWidth = kLifecycleDeleteWidth;
// Line two: a status dot immediately before each port's combo, then the
// lifecycle controls.
inline constexpr float kActiveHeaderLine2Width =
    kStatusDotWidth + kLifecycleControlGap + kEndpointFieldWidth + kEndpointBoxGap +
    kStatusDotWidth + kLifecycleControlGap + kEndpointFieldWidth + kLifecycleControlGap +
    kLifecycleDeleteWidth + kLifecycleControlGap;
// 308: every library device name (RunDeviceLabelWidthCheck in the miniapp
// JUCE suite measures each one at the page's default text size) needs this
// much room, whether or not line two's own lifecycle controls happen to
// leave that much free.
inline constexpr float kControllerDeviceWidth = 308.0f;
// Line one: disclosure, name, device, and on a launchpad row the Variant
// selector. The status dots are on line two now, beside the ports they
// describe. The width below is the launchpad case, the wider of the two.
inline constexpr float kActiveHeaderLine1Width =
    kControllerDisclosureWidth + kLifecycleControlGap + kControllerNameWidth +
    kLifecycleControlGap + kControllerDeviceWidth + kLifecycleControlGap + kVariantFieldWidth;
inline constexpr float kActiveControllerHeaderWidth =
    std::max(kActiveHeaderLine1Width, kActiveHeaderLine2Width);
// The device label's own floor keeps line one from shrinking along with line
// two's now-shorter lifecycle block; this catches at compile time if line
// two ever grows back past what line one already reserves.
static_assert(kActiveHeaderLine2Width <= kActiveHeaderLine1Width,
             "the active row's line two must not grow past line one's own width");
// The sentence a row shows while it differs from its preset. Restore moves
// onto this same line, beside the words that describe it.
inline constexpr const char* kPresetNoticeText =
    "This row differs from its preset. Restore replaces its mappings and "
    "discards any edits.";
// Line three: the notice label, then Restore. Task 1.3's glyph-measurement
// check, not a static_assert, is what proves the sentence fits this box.
inline constexpr float kControllerNoticeWidth =
    kActiveHeaderLine1Width - kLifecycleControlGap - kLifecycleRestoreWidth;
// A row that differs from its preset grows a third header line.
inline constexpr float kControllerHeaderHeightWithNotice =
    3.0f * kControllerHeaderLineHeight;
// Line one: name, device, Released badge.
inline constexpr float kBlacklistedHeaderLine1Width =
    kControllerNameWidth + kLifecycleControlGap + kControllerDeviceWidth +
    kLifecycleControlGap + kBlacklistedBadgeWidth;
// Line two: the two stored-endpoint labels followed by the lifecycle controls.
inline constexpr float kBlacklistedHeaderLine2Width =
    kBlacklistedEndpointLabelWidth + kLifecycleControlGap + kBlacklistedEndpointLabelWidth +
    kLifecycleControlGap + kBlacklistedLifecycleWidth;
inline constexpr float kBlacklistedControllerHeaderWidth =
    std::max(kBlacklistedHeaderLine1Width, kBlacklistedHeaderLine2Width);
// The same device-label floor also holds line one wider than the blacklisted
// row's own (now shorter) line two.
static_assert(kBlacklistedHeaderLine2Width <= kBlacklistedHeaderLine1Width,
             "the blacklisted row's line two must not grow past line one's own width");
inline constexpr float kControllerHeaderMinWidth =
    std::max(kActiveControllerHeaderWidth, kBlacklistedControllerHeaderWidth);
inline constexpr float kSectionMaxHeight = 220.0f;
// Connect messages (openSysEx): one hex-text field per stored message, wide
// enough for a typical mode-switch SysEx string, plus its delete button.
inline constexpr float kConnectMessageFieldWidth = 320.0f;

inline int FieldEditorWidth(MidiMappingRowVM::Field field)
{
    using Field = MidiMappingRowVM::Field;
    switch (field)
    {
        case Field::MessageKind:
        case Field::AppAction:
        case Field::ShiftAction:
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
        case Field::BlockStartNote:
        case Field::BlockEndNote:
        case Field::Note:
            return 66;
        // "Start Gesture" (13 characters) is measured, at Froggers' narrowest
        // host and this page's default text size, wider than the other
        // block-field columns' shared 66px: real glyph measurement
        // (juce::GlyphArrangement against pagestyle::kDefaultTextStyle, the
        // same font a Label node renders at) gives 69.705px, so 70 is the
        // smallest whole-pixel width the header shows in full at.
        case Field::BlockStartGesture:
            return 70;
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

// A field-commit action's value packs its raw text last, after a fixed run
// of ':'-joined index/field tokens -- and that raw text can itself contain
// a ':' (a connect message's hex bytes, say), so it is rejoined from every
// remaining part rather than read as a single token like the ones before it.
inline std::string JoinRemainingTokens(const std::vector<std::string>& parts, std::size_t fromIx)
{
    std::string rawValue;
    for (std::size_t ix = fromIx; ix < parts.size(); ++ix)
    {
        if (ix > fromIx)
        {
            rawValue += ':';
        }
        rawValue += parts[ix];
    }
    return rawValue;
}

inline bool IsAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// A field-commit's raw text must be wholly a finite number -- not a prefix
// of one (std::stod would otherwise accept "3abc" as 3) and not NaN/infinity.
// An integer field's raw text must be wholly an integer literal (an optional
// '-' then decimal digits, nothing else) once any leading or trailing
// whitespace is trimmed: parsing it as a double first would let its rounding
// turn a fractional value like "15.9999999999999999" into a whole number the
// player never typed, and trimming only the leading side refused a value
// typed with trailing whitespace while accepting the same value with leading
// whitespace. Returns nullopt for anything else, which the mapping-row field
// commit refuses on. For an integer field, `outOfRange` (when given) is set
// when the text is a well-formed integer literal that is refused only for
// its magnitude -- too large for `long long` to hold, or larger in
// magnitude than 2^53, past which not every integer is exactly
// representable in a `double` -- so the caller can tell that case apart from
// text that is not an integer at all.
inline std::optional<double> ParseFiniteNumericToken(const std::string& rawValue, bool requireInteger,
                                                     bool* outOfRange = nullptr)
{
    if (outOfRange != nullptr)
    {
        *outOfRange = false;
    }
    if (requireInteger)
    {
        std::size_t begin = 0;
        std::size_t end = rawValue.size();
        while (begin < end && IsAsciiSpace(rawValue[begin]))
        {
            ++begin;
        }
        while (end > begin && IsAsciiSpace(rawValue[end - 1]))
        {
            --end;
        }
        if (begin == end)
        {
            return std::nullopt;
        }
        long long integerValue = 0;
        const char* first = rawValue.data() + begin;
        const char* last = rawValue.data() + end;
        const std::from_chars_result result = std::from_chars(first, last, integerValue);
        if (result.ptr != last)
        {
            return std::nullopt;
        }
        if (result.ec == std::errc::result_out_of_range)
        {
            if (outOfRange != nullptr)
            {
                *outOfRange = true;
            }
            return std::nullopt;
        }
        if (result.ec != std::errc())
        {
            return std::nullopt;
        }
        constexpr long long kMaxExactIntegerInDouble = 9007199254740992LL;  // 2^53
        if (integerValue > kMaxExactIntegerInDouble || integerValue < -kMaxExactIntegerInDouble)
        {
            if (outOfRange != nullptr)
            {
                *outOfRange = true;
            }
            return std::nullopt;
        }
        return static_cast<double>(integerValue);
    }
    try
    {
        std::size_t consumed = 0;
        const double numericValue = std::stod(rawValue, &consumed);
        if (consumed != rawValue.size() || !std::isfinite(numericValue))
        {
            return std::nullopt;
        }
        return numericValue;
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

// The row's device label: the descriptor's display name when the row's
// stored wizard id still resolves against the page's layouts (a device
// preset, active or blacklisted), else the MIDI input device the row is
// bound to -- the same identity StoredEndpointLabel already gives the
// blacklisted row's "MIDI in:" line, so an unresolved or Custom row still
// says which device it is instead of the internal profile kind.
inline std::string ControllerDeviceLabel(const MidiControllerRowVM& rowVm,
                                         const std::vector<ControllerWizardDescriptor>& layouts)
{
    if (rowVm.wizardId.has_value())
    {
        const ControllerWizardDescriptor* descriptor =
            FindControllerWizardDescriptor(layouts, *rowVm.wizardId);
        if (descriptor != nullptr)
        {
            return descriptor->displayName;
        }
    }
    return StoredEndpointLabel(rowVm.storedInput);
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
// wizard, opens a blank form (ConfigForm()), generates a profile from
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
    std::unique_ptr<ControllerConfigForm> form = wizard->ConfigForm();
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

// The Launchpad models the model selector offers, in this order. The
// option id is the index into this list, which is what HandleVariantSelect
// parses back -- one list, so the offered order and the parsed meaning cannot
// drift apart.
inline constexpr LaunchpadController kLaunchpadVariants[] = {
    LaunchpadController::LaunchpadX,
    LaunchpadController::LaunchpadProMk3,
    LaunchpadController::LaunchpadMiniMk3,
};

inline std::vector<ui::ControlOption> BuildLaunchpadVariantOptions(LaunchpadController selected,
                                                                   std::string& selectedOptionId)
{
    std::vector<ui::ControlOption> options;
    selectedOptionId = "0";
    for (std::size_t ix = 0; ix < std::size(kLaunchpadVariants); ++ix)
    {
        const std::string id = std::to_string(ix);
        if (kLaunchpadVariants[ix] == selected)
        {
            selectedOptionId = id;
        }
        options.push_back({id, LaunchpadControllerDisplayName(kLaunchpadVariants[ix])});
    }
    return options;
}

// The plain Custom entry's option id: a preset is only a starting point, so
// the add row offers exactly one unbound, no-mappings entry rather than one
// per MidiProfileKind. It always installs the Generic kind (kCustomKind
// below): encoders, system messages and analogs, the same three sections
// WrldBldr also supports -- but not grid mappings, Launchpad pad positions,
// the Launchpad model selector, or Twister side-button system rows, which
// the view model gates on the row's literal kind (WrldBldr/Launchpad/
// MfTwister), not on section support, so a Generic row cannot hold them. A
// device that needs one of those starts from its own descriptor, or the
// library one MakeControllerWizardRegistry appends for a kind the catalog's
// libraryDeviceKinds lists and its device defaults do not cover, and edits
// from there. Connect messages (below) are unconditional on every kind,
// Custom included.
inline constexpr const char* kCustomPresetOptionId = "custom";
inline constexpr const char* kCustomPresetLabel = "Custom";
inline constexpr MidiProfileKind kCustomKind = MidiProfileKind::Generic;

// The add row's Preset combo: every registry descriptor's display name
// (option id = descriptor id), then the one Custom entry.
inline std::vector<ui::ControlOption> BuildAddPresetOptions(
    const std::vector<ControllerWizardDescriptor>& layouts)
{
    std::vector<ui::ControlOption> options;
    options.reserve(layouts.size() + 1);
    for (const ControllerWizardDescriptor& descriptor : layouts)
    {
        options.push_back({descriptor.id, descriptor.displayName});
    }
    options.push_back({kCustomPresetOptionId, kCustomPresetLabel});
    return options;
}

// The add row's Preset combo defaults to its first option when no draft has
// been recorded yet. When a connected device is waiting to be set up, that
// default is its own first matching preset, so pressing Add without touching
// the combo installs the waiting device; otherwise it falls back to the
// registry's first descriptor, derived from BuildAddPresetOptions itself --
// not a second "first option" rule -- so the row's displayed default and the
// id HandleAddController installs from cannot drift apart.
inline std::string EffectiveAddPresetId(const std::vector<ControllerWizardDescriptor>& layouts,
                                        const std::string& draft,
                                        const WizardDiscovery& discovery)
{
    if (!draft.empty())
    {
        return draft;
    }
    if (!discovery.available.empty())
    {
        return discovery.available.front().wizardId;
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
    // (MfTwister/Launchpad/WRLD.Bldr -- MidiAppCatalog's own default
    // libraryDeviceKinds, with no device defaults to cover them) -- a host
    // with an app catalog fills this from
    // MakeControllerWizardRegistry(engine.MidiCatalog()), which appends a
    // library descriptor only for a kind that catalog's own
    // libraryDeviceKinds lists and its device defaults do not cover.
    std::vector<ControllerWizardDescriptor> layouts;
    // The app's gesture count. Unset means no cap (MidiConfigViewModel's own
    // default) -- a host with an app catalog fills this from
    // engine.Manager().GestureCount(), so a Gestures row or a Hold/Toggle
    // Gesture Select/Set Gesture Value argument can never name a gesture the
    // app does not have.
    std::optional<std::size_t> gestureCount;
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
        ConfigureViewModel(m_vm);
        m_dirty = true;
    }

    ui::NodeTree BuildTree() override
    {
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
               action.name == Actions::kVariantSelect ||
               action.name == Actions::kToggleSection ||
               action.name == Actions::kDeleteRow || action.name == Actions::kAddSingle ||
               action.name == Actions::kAddBlock || action.name == Actions::kEndpointSelect ||
               action.name == Actions::kMappingFieldCommit ||
               action.name == Actions::kAddController ||
               action.name == Actions::kControllerRenameDraft ||
               action.name == Actions::kControllerRename ||
               action.name == Actions::kControllerDelete ||
               action.name == Actions::kControllerRestore ||
               action.name == Actions::kConnectMessageCommit ||
               action.name == Actions::kConnectMessageDelete ||
               action.name == Actions::kConnectMessageAdd;
    }

private:
    // Every handler that accepts a view-model edit and then calls Commit()
    // needs the same refusal text for the one case the view model itself
    // cannot detect: a host that rejects an otherwise-valid instrument.
    static constexpr const char* kHostRejectedCommitStatus = "Refused: host rejected the instrument commit";
    // Shared by handlers that parse an action value into a controller
    // identity token and find it malformed; one of the three call sites
    // (SnapshotForLifecycleIdentity) also uses this when the host's
    // instrument-snapshot callback is unset.
    static constexpr const char* kInvalidControllerIdentityStatus = "Refused: invalid controller identity";
    // Set when the add-flow's candidate record is built and `AddController`
    // rejects it.
    static constexpr const char* kGeneratedRecordInvalidStatus =
        "Refused: generated controller record is invalid";
    // Set by the rename, delete, and restore actions' identity lookup by name
    // (SnapshotForLifecycleIdentity), which re-reads the committed instrument
    // and finds the row they started from no longer matches.
    static constexpr const char* kControllerRecordChangedStatus =
        "Refused: controller record changed; refresh and try again";
    // Set by `Commit` when the host accepts an edit but the runtime
    // configuration save that follows it fails; the edit stays committed.
    static constexpr const char* kRuntimeConfigSaveFailedStatus =
        "The controller was committed, but runtime configuration save failed";

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

    // The one place a Controllers-page edit is persisted: commits the
    // instrument, then saves the runtime configuration so the edit survives a
    // reload however the player leaves the page, then sets exactly one
    // status. A refused commit reports the shared refusal text; a commit that
    // lands but fails to save reports the save failure without losing the
    // edit; otherwise the caller's own success text is shown. The return
    // value means the instrument was committed, whether or not the save that
    // follows succeeds.
    bool Commit(MidiInstrumentConfig out, std::string successText)
    {
        if (!m_callbacks.commitInstrument ||
            !m_callbacks.commitInstrument(std::move(out)))
        {
            SetStatus(kHostRejectedCommitStatus);
            return false;
        }
        m_dirty = true;
        if (!m_callbacks.saveRuntimeConfiguration ||
            !m_callbacks.saveRuntimeConfiguration())
        {
            SetStatus(kRuntimeConfigSaveFailedStatus);
            return true;
        }
        SetStatus(std::move(successText));
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

        if (action.name == Actions::kVariantSelect)
        {
            HandleVariantSelect(action.value);
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

        if (action.name == Actions::kConnectMessageCommit)
        {
            HandleConnectMessageCommit(action.value);
            return;
        }

        if (action.name == Actions::kConnectMessageDelete)
        {
            HandleConnectMessageDelete(action.value);
            return;
        }

        if (action.name == Actions::kConnectMessageAdd)
        {
            HandleConnectMessageAdd(action.value);
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
                SetStatus(kInvalidControllerIdentityStatus);
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

        if (action.name == Actions::kControllerRestore)
        {
            HandleRestoreController(action.value);
            return;
        }
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

    // A row Add just installed opens: its disclosure and every section it
    // lists start expanded, through the same toggles a player's own click on
    // them uses, so those same clicks still collapse it. The row does not
    // exist in the view model until the committed instrument has been
    // rebuilt, so this rebuilds first and then finds it by name.
    void OpenAddedRow(const std::string& name)
    {
        if (!m_callbacks.instrumentSnapshot || !m_callbacks.connectionState)
        {
            return;
        }
        m_vm.Rebuild(m_callbacks.instrumentSnapshot(), m_callbacks.connectionState());
        const std::vector<MidiControllerRowVM>& rows = m_vm.Controllers();
        for (std::size_t ix = 0; ix < rows.size(); ++ix)
        {
            if (rows[ix].name != name)
            {
                continue;
            }
            if (!rows[ix].configExpanded)
            {
                m_vm.ToggleConfig(ix);
            }
            for (MidiConfigSection section : rows[ix].sections)
            {
                if (!m_vm.SectionExpanded(ix, section))
                {
                    m_vm.ToggleSection(ix, section);
                }
            }
            return;
        }
    }

    bool SnapshotForLifecycleIdentity(
        const std::optional<std::pair<std::size_t, std::string>>& identity,
        MidiInstrumentConfig& instrument)
    {
        if (!identity.has_value() || !m_callbacks.instrumentSnapshot)
        {
            SetStatus(kInvalidControllerIdentityStatus);
            return false;
        }
        instrument = m_callbacks.instrumentSnapshot();
        if (identity->first >= instrument.controllers.size() ||
            instrument.controllers[identity->first].name != identity->second)
        {
            SetStatus(kControllerRecordChangedStatus);
            return false;
        }
        return true;
    }

    // Configures `viewModel`'s app-supplied catalogs (message kinds, analog
    // actions, wizard layouts, gesture count) from m_callbacks, exactly as
    // the constructor configures m_vm. CommitLifecycleAction's throwaway
    // mutation view model needs the same configuration: without it,
    // Layouts() falls back to the library's own registry, which holds none
    // of an app's presets, so RestoreController can never resolve an app
    // preset's wizard id, and an unset gesture count would let a lifecycle
    // action re-commit a gesture reference the app's own cap refuses.
    void ConfigureViewModel(MidiConfigViewModel& viewModel) const
    {
        if (!m_callbacks.messageCatalog.empty())
        {
            viewModel.SetMessageCatalog(m_callbacks.messageCatalog);
        }
        if (!m_callbacks.analogActionCatalog.empty())
        {
            viewModel.SetAnalogActionCatalog(m_callbacks.analogActionCatalog);
        }
        if (!m_callbacks.layouts.empty())
        {
            viewModel.SetLayouts(m_callbacks.layouts);
        }
        if (m_callbacks.gestureCount.has_value())
        {
            viewModel.SetGestureCount(m_callbacks.gestureCount);
        }
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
        ConfigureViewModel(mutationViewModel);
        mutationViewModel.Rebuild(instrument, MidiConnectionState{});
        MidiInstrumentConfig out;
        std::string reason;
        if (!mutate(mutationViewModel, identity->first, out, &reason))
        {
            SetStatus("Refused: " + reason);
            return false;
        }
        if (!Commit(std::move(out), std::move(success)))
        {
            return false;
        }
        RefreshDiscoveryFromCallbacks();
        return true;
    }

    void HandleRenameController(const std::string& token)
    {
        const std::optional<std::pair<std::size_t, std::string>> identity =
            NodeIds::ControllerActionIdentityFromToken(token);
        if (!identity.has_value())
        {
            SetStatus(kInvalidControllerIdentityStatus);
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

    void HandleRestoreController(const std::string& token)
    {
        // No separate invalid-identity check here: CommitLifecycleAction
        // re-derives this same token's identity through
        // SnapshotForLifecycleIdentity, which sets the same
        // kInvalidControllerIdentityStatus on the same condition
        // (NodeIds::ControllerActionIdentityFromToken is a pure function of
        // `token`, so the two derivations agree). `identity` is still parsed
        // here for the success-only NoteControllerConfigReplaced() call
        // below, whose `identity->second` deref is safe because
        // CommitLifecycleAction can only return true once
        // SnapshotForLifecycleIdentity has confirmed this same token parses
        // to a valid identity.
        const std::optional<std::pair<std::size_t, std::string>> identity =
            NodeIds::ControllerActionIdentityFromToken(token);
        if (CommitLifecycleAction(
            token, [&](MidiConfigViewModel& viewModel, std::size_t controllerIx,
                       MidiInstrumentConfig& out, std::string* reason) {
                return viewModel.RestoreController(controllerIx, out, reason);
            },
            "Restored controller"))
        {
            // The restored config replaces the row's mappings out from under
            // any open section; without dropping the cached rows, the page
            // would keep showing the pre-Restore presentation and the next
            // encoder edit would flush it back over the restored mappings.
            m_vm.NoteControllerConfigReplaced(identity->second);
        }
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

    // "<controllerIx>:<optionId>": the row's own index, then the option id the
    // backend appends, which BuildLaunchpadVariantOptions writes as the index
    // into kLaunchpadVariants.
    void HandleVariantSelect(const std::string& value)
    {
        const auto parts = Split(value, ':');
        if (parts.size() != 2)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::size_t variantIx = ParseIndex(parts[1]);
        if (variantIx >= std::size(ControllersLayout::kLaunchpadVariants))
        {
            return;
        }
        const LaunchpadController model = ControllersLayout::kLaunchpadVariants[variantIx];

        MidiInstrumentConfig out;
        std::string reason;
        if (m_vm.SetLaunchpadModel(controllerIx, model, out, &reason))
        {
            Commit(std::move(out), std::string("Set ") + LaunchpadControllerDisplayName(model));
            return;
        }
        // A model the row is already on is not a refusal worth a status line:
        // the combo simply reports the selection it already showed.
        if (reason != "model is unchanged")
        {
            SetStatus("Refused: " + reason);
        }
        ++m_treeRevision;
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
                Commit(std::move(out), "Cleared device");
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
                Commit(std::move(out), "Selected " + device.name);
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

        const std::string rawValue = ControllersLayout::JoinRemainingTokens(parts, 4);
        const bool fieldIsInteger = FieldIsInteger(*field);
        bool valueOutOfRange = false;
        const std::optional<double> parsedValue =
            ControllersLayout::ParseFiniteNumericToken(rawValue, fieldIsInteger, &valueOutOfRange);
        if (!parsedValue.has_value())
        {
            if (fieldIsInteger && valueOutOfRange)
            {
                SetStatus("Refused: value is out of range");
            }
            else
            {
                SetStatus(fieldIsInteger ? "Refused: value must be an integer" : "Refused: value must be a finite number");
            }
            return;
        }
        const double numericValue = *parsedValue;
        MidiInstrumentConfig out;
        std::string reason;
        bool presentationChanged = false;
        if (m_vm.ApplyMappingEdit(controllerIx, *section, rowIx, *field, numericValue, out, &reason,
                                  &presentationChanged))
        {
            Commit(std::move(out), "OK");
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
            Commit(std::move(out), "Deleted");
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    // value: "<controllerIx>:<messageIx>", with the typed hex text appended
    // by the host after one more ':' (DispatchCurrentNodeActionWithAppendedValue).
    void HandleConnectMessageCommit(const std::string& value)
    {
        const auto parts = Split(value, ':');
        // An entirely empty hex field ("<controllerIx>:<messageIx>:") tokenises
        // to exactly 2 parts -- Split's std::getline loop drops the trailing
        // empty token -- so the arity floor is 2, not 3: that shape must reach
        // SetConnectMessage below and be refused there as an empty message,
        // rather than being silently dropped here as too few parts.
        if (parts.size() < 2)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::size_t messageIx = ParseIndex(parts[1]);
        const std::string hexText = ControllersLayout::JoinRemainingTokens(parts, 2);
        MidiInstrumentConfig out;
        std::string reason;
        if (m_vm.SetConnectMessage(controllerIx, messageIx, hexText, out, &reason))
        {
            Commit(std::move(out), "OK");
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    void HandleConnectMessageDelete(const std::string& value)
    {
        const auto parts = Split(value, ':');
        if (parts.size() != 2)
        {
            return;
        }
        const std::size_t controllerIx = ParseIndex(parts[0]);
        const std::size_t messageIx = ParseIndex(parts[1]);
        MidiInstrumentConfig out;
        std::string reason;
        if (m_vm.DeleteConnectMessage(controllerIx, messageIx, out, &reason))
        {
            Commit(std::move(out), "Deleted");
        }
        else
        {
            SetStatus("Refused: " + reason);
        }
    }

    void HandleConnectMessageAdd(const std::string& value)
    {
        const std::size_t controllerIx = ParseIndex(value);
        MidiInstrumentConfig out;
        std::string reason;
        if (m_vm.AddConnectMessage(controllerIx, out, &reason))
        {
            Commit(std::move(out), "Added connect message");
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
            Commit(std::move(out), asBlock ? "Added block" : "Added");
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
        const std::string presetId =
            ControllersLayout::EffectiveAddPresetId(layouts, m_addPresetId, m_discovery);

        if (presetId == ControllersLayout::kCustomPresetOptionId)
        {
            const std::string name =
                AvailableControllerName(instrument, ControllersLayout::kCustomPresetLabel);
            MidiInstrumentConfig out;
            std::string reason;
            if (m_vm.AddController(name, ControllersLayout::kCustomKind, out, &reason))
            {
                if (Commit(std::move(out), "Added " + name))
                {
                    OpenAddedRow(name);
                }
            }
            else
            {
                SetStatus("Refused: " + reason);
            }
            return;
        }

        const ControllerWizardDescriptor* descriptor = FindControllerWizardDescriptor(layouts, presetId);
        if (descriptor == nullptr)
        {
            SetStatus("Refused: unknown controller preset");
            return;
        }

        MidiControllerSlot slot;
        slot.name = AvailableControllerName(instrument, descriptor->displayName);

        // A candidate needs an unclaimed input AND output matching the CHOSEN
        // preset's own aliases -- not only the descriptor discovery happened to
        // assign the pair to, since discovery binds a pair to the first
        // matching descriptor and two presets (an APC40 mkII's Generic and
        // Ableton entries) can share the exact same aliases. With only one
        // side connected there is no candidate, and both ports are left unset
        // so their combos read "(none)".
        const WizardDiscovery addDiscovery = DiscoverControllerWizards(m_devices, instrument, layouts);
        for (const WizardCandidate& candidate : addDiscovery.available)
        {
            if (MatchesAnyAlias(candidate.input.name, descriptor->inputAliases) &&
                MatchesAnyAlias(candidate.output.name, descriptor->outputAliases))
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
            SetStatus(kGeneratedRecordInvalidStatus);
            return;
        }
        if (Commit(std::move(out), "Added " + slot.name))
        {
            OpenAddedRow(slot.name);
        }
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

        // Every registry preset whose aliases match a waiting pair's port
        // names, in registry order -- not only the one descriptor discovery
        // happened to assign the pair to, since two presets (an APC40 mkII's
        // Generic and Ableton entries) can share the exact same aliases.
        const auto matchingPresetNames = [&](const WizardCandidate& candidate) {
            std::vector<std::string> names;
            for (const ControllerWizardDescriptor& descriptor : vm.Layouts())
            {
                if (MatchesAnyAlias(candidate.input.name, descriptor.inputAliases) &&
                    MatchesAnyAlias(candidate.output.name, descriptor.outputAliases))
                {
                    names.push_back(descriptor.displayName);
                }
            }
            return names;
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
                                       "No connected controller is waiting to be set up",
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
                                       const std::vector<std::string> presetNames =
                                           matchingPresetNames(candidate);
                                       std::string joinedNames;
                                       for (std::size_t nameIx = 0; nameIx < presetNames.size();
                                            ++nameIx)
                                       {
                                           if (nameIx > 0)
                                           {
                                               joinedNames += ", ";
                                           }
                                           joinedNames += presetNames[nameIx];
                                       }
                                       // Status only: no button dispatches an action for a
                                       // waiting device, so one label carries its port names
                                       // and every preset that matches them.
                                       available.StatusText(
                                           NodeIds::AvailableRow(candidateIx),
                                           candidate.input.name + " / " + candidate.output.name +
                                               ": " + joinedNames,
                                           statusStyle(ControllersLayout::kStatusRowHeight));
                                   }
                               }
                               if (!discovery.unmatchedInputs.empty())
                               {
                                   available.StatusText(
                                       NodeIds::kAvailableUnmatchedInputs,
                                       diagnosticText("Other inputs: ", discovery.unmatchedInputs),
                                       statusStyle(ControllersLayout::kStatusRowHeight));
                               }
                               if (!discovery.unmatchedOutputs.empty())
                               {
                                   available.StatusText(
                                       NodeIds::kAvailableUnmatchedOutputs,
                                       diagnosticText("Other outputs: ", discovery.unmatchedOutputs),
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
            // Shared by every "pick one of a fixed catalog by index" combo
            // below (MessageKind/AppAction/EncoderMode/ShiftAction): same
            // node id, same commit action, same style -- only the offered
            // options and the currently-selected index differ per field, and
            // each caller computes those two through whichever accessor
            // fits its field (a dedicated index accessor, or RowFieldValue).
            // A negative selectedIndex (not found / row has no value yet)
            // selects "0".
            const auto emitIndexCombo = [&](std::vector<ui::ControlOption> options, int selectedIndex) {
                mappingRow.ComboBox(NodeIds::MappingField(controllerIx, section, mappingRowIx, field),
                                    std::move(options),
                                    selectedIndex >= 0 ? std::to_string(selectedIndex) : "0",
                                    ui::Action::WithValue(
                                        Actions::kMappingFieldCommit,
                                        std::to_string(controllerIx) + ":" +
                                            ControllersLayout::SectionToken(section) + ":" +
                                            std::to_string(mappingRowIx) + ":" +
                                            ControllersLayout::FieldToken(field)),
                                    fieldStyle);
            };
            if (field == MidiMappingRowVM::Field::MessageKind)
            {
                std::vector<ui::ControlOption> options;
                const auto& catalog = vm.MessageCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
                }
                int selectedIndex = vm.UISystemMessageIndex(controllerIx, section, mappingRowIx);
                if (selectedIndex < 0)
                {
                    const std::optional<std::string> storedKindName =
                        vm.UncatalogedSystemMessageKindName(controllerIx, section, mappingRowIx);
                    if (storedKindName.has_value())
                    {
                        selectedIndex = static_cast<int>(options.size());
                        options.push_back({std::to_string(selectedIndex), *storedKindName});
                    }
                }
                emitIndexCombo(std::move(options), selectedIndex);
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
                const int selected =
                    vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current)
                        ? static_cast<int>(current)
                        : -1;
                emitIndexCombo(std::move(options), selected);
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
                const int selected =
                    vm.RowFieldValue(controllerIx, section, mappingRowIx, field, current)
                        ? static_cast<int>(current)
                        : -1;
                emitIndexCombo(std::move(options), selected);
                return;
            }
            if (field == MidiMappingRowVM::Field::ShiftAction)
            {
                if (section == MidiConfigSection::Encoders)
                {
                    // An encoder turn's shifted job is its own fixed
                    // three-entry catalog (none, Scene Blend, BPM), not a
                    // system message target, so it neither reads
                    // ShiftCatalog() nor
                    // selects through ShiftChoiceIndex() -- see
                    // EncoderTurnShiftedJobIndex() and EncoderShiftedJobCatalog().
                    std::vector<ui::ControlOption> options;
                    const auto& shiftedJobCatalog = EncoderShiftedJobCatalog();
                    for (int ix = 0; ix < static_cast<int>(shiftedJobCatalog.size()); ++ix)
                    {
                        options.push_back({std::to_string(ix), shiftedJobCatalog[static_cast<std::size_t>(ix)]});
                    }
                    emitIndexCombo(std::move(options),
                                  vm.EncoderTurnShiftedJobIndex(controllerIx, section, mappingRowIx));
                    return;
                }
                std::vector<ui::ControlOption> options;
                const auto& catalog = vm.ShiftCatalog();
                for (int ix = 0; ix < static_cast<int>(catalog.size()); ++ix)
                {
                    options.push_back({std::to_string(ix), catalog[static_cast<std::size_t>(ix)].label});
                }
                emitIndexCombo(std::move(options), vm.ShiftChoiceIndex(controllerIx, section, mappingRowIx));
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
            const bool showsPresetNotice =
                rowVm.hasResolvedWizard && !rowVm.matchesWizardProfile;
            scroll.Column(
                NodeIds::ControllerRow(controllerIx),
                rowLayout(showsPresetNotice
                              ? ControllersLayout::kControllerHeaderHeightWithNotice
                              : ControllersLayout::kControllerHeaderHeight,
                          scrollWidth,
                          0.0f),
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
                                row.Label(NodeIds::ControllerDevice(controllerIx),
                                         ControllersLayout::ControllerDeviceLabel(rowVm, vm.Layouts()),
                                         labelStyle(ControllersLayout::kControllerDeviceWidth));
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
                                row.Button(
                                    NodeIds::ControllerDelete(controllerIx),
                                    "Delete",
                                    ui::Action::WithValue(
                                        Actions::kControllerDelete,
                                        NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                    button(ControllersLayout::kLifecycleDeleteWidth));
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
                            row.Label(NodeIds::ControllerDevice(controllerIx),
                                     ControllersLayout::ControllerDeviceLabel(rowVm, vm.Layouts()),
                                     labelStyle(ControllersLayout::kControllerDeviceWidth));
                            if (rowVm.kind == MidiProfileKind::Launchpad)
                            {
                                std::string selectedVariant;
                                ui::ControlStyle variantStyle =
                                    fieldControl(ControllersLayout::kVariantFieldWidth);
                                variantStyle.caption = "Model";
                                row.ComboBox(
                                    NodeIds::ControllerVariant(controllerIx),
                                    ControllersLayout::BuildLaunchpadVariantOptions(
                                        rowVm.launchpadModel, selectedVariant),
                                    selectedVariant,
                                    ui::Action::WithValue(Actions::kVariantSelect,
                                                          std::to_string(controllerIx)),
                                    variantStyle);
                            }
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
                        });

                    if (showsPresetNotice)
                    {
                        section.Row(
                            NodeIds::ControllerRow(controllerIx) + ".line3",
                            rowLayout(ControllersLayout::kControllerHeaderLineHeight,
                                     scrollWidth,
                                     ControllersLayout::kLifecycleControlGap),
                            [&](ui::Builder& row) {
                                row.Label(NodeIds::ControllerPresetNotice(controllerIx),
                                         ControllersLayout::kPresetNoticeText,
                                         labelStyle(ControllersLayout::kControllerNoticeWidth));
                                row.Button(
                                    NodeIds::ControllerRestore(controllerIx),
                                    "Restore",
                                    ui::Action::WithValue(
                                        Actions::kControllerRestore,
                                        NodeIds::ControllerActionToken(controllerIx, rowVm.name)),
                                    button(ControllersLayout::kLifecycleRestoreWidth));
                            });
                    }
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
                    // Connect messages (openSysEx): not a MidiConfigSection --
                    // always shown, on every kind, independent of the kind's
                    // section support -- so a hand-configured row can send a
                    // device's connect message the same as a preset row can.
                    scroll.Column(
                        NodeIds::ConnectMessages(controllerIx),
                        columnLayout(ui::Extent::Intrinsic(), ControllersLayout::kRowGap),
                        [&](ui::Builder& connect) {
                            connect.Label(NodeIds::ConnectMessagesHeading(controllerIx),
                                         "Connect messages",
                                         labelStyle(ControllersLayout::kControllerNameWidth));
                            const std::size_t messageCount = vm.ConnectMessageCount(controllerIx);
                            for (std::size_t messageIx = 0; messageIx < messageCount; ++messageIx)
                            {
                                connect.Row(
                                    NodeIds::ConnectMessageRow(controllerIx, messageIx),
                                    // Weight(1.0), not scrollWidth: this row is
                                    // nested inside the connect-messages Column,
                                    // which itself fills whatever cross space its
                                    // own parent gives it (columnLayout's default
                                    // Weight(1.0)), and that can resolve narrower
                                    // than scrollWidth's horizontal-scroll floor.
                                    // A fixed scrollWidth here overflows the
                                    // Column's own resolved bounds.
                                    layout(ui::Extent::Px(ControllersLayout::kMappingRowHeight),
                                          ui::Extent::Weight(1.0f),
                                          ControllersLayout::kEditorColumnGap),
                                    [&](ui::Builder& row) {
                                        ui::ControlStyle fieldStyle = fieldControl(
                                            ControllersLayout::kConnectMessageFieldWidth,
                                            ControllersLayout::kMappingRowHeight);
                                        fieldStyle.caption = "Message";
                                        row.TextField(
                                            NodeIds::ConnectMessageField(controllerIx, messageIx),
                                            "Message",
                                            vm.ConnectMessageHex(controllerIx, messageIx),
                                            ui::Action::WithValue(
                                                Actions::kConnectMessageCommit,
                                                std::to_string(controllerIx) + ":" +
                                                    std::to_string(messageIx)),
                                            fieldStyle);
                                        row.Button(
                                            NodeIds::ConnectMessageDelete(controllerIx, messageIx),
                                            "x",
                                            ui::Action::WithValue(
                                                Actions::kConnectMessageDelete,
                                                std::to_string(controllerIx) + ":" +
                                                    std::to_string(messageIx)),
                                            button(ControllersLayout::kDeleteButtonWidth,
                                                  ControllersLayout::kMappingRowHeight));
                                    });
                            }
                            connect.Row(
                                NodeIds::ConnectMessageAddRow(controllerIx),
                                // Same Weight(1.0) reasoning as ConnectMessageRow
                                // above: this row is nested inside the same
                                // Weight(1.0)-sized Column, so its own cross
                                // extent must match that, not scrollWidth's
                                // horizontal-scroll floor.
                                layout(ui::Extent::Px(ControllersLayout::kMappingRowHeight),
                                      ui::Extent::Weight(1.0f),
                                      ControllersLayout::kEditorColumnGap),
                                [&](ui::Builder& addRow) {
                                    addRow.Button(
                                        NodeIds::ConnectMessageAdd(controllerIx),
                                        "Add",
                                        ui::Action::WithValue(Actions::kConnectMessageAdd,
                                                              std::to_string(controllerIx)),
                                        button(ControllersLayout::kAddButtonWidth, 28.0f));
                                });
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
                                   ControllersLayout::EffectiveAddPresetId(vm.Layouts(), addPresetId, discovery);
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
