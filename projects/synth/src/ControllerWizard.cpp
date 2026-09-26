#include "synth/ControllerWizard.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "synth/RuntimePageStyle.hpp"

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
constexpr std::string_view kMfTwisterFormRootId = "controller-wizard.twister";

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

std::string ButtonFieldId(std::size_t buttonIx, std::string_view field) {
    return std::string(kMfTwisterFormRootId) + ".button." + std::to_string(buttonIx) + "." +
           std::string(field);
}

// Twister form layout. scw-3 requires the six buttons to be presented as two
// columns of three in physical CC order, so the form owns this geometry rather
// than leaving it to each host's default flow. Every child's bounds are local
// to its parent, so hosts can nest the form anywhere.
namespace TwisterFormLayout {

inline constexpr float kMargin = 8.0f;
inline constexpr float kControlHeight = 28.0f;
inline constexpr float kErrorHeight = 20.0f;
inline constexpr float kErrorGap = 2.0f;
inline constexpr float kFieldGap = 8.0f;
inline constexpr float kRowGap = 6.0f;
inline constexpr float kSlotWidth = 160.0f;
inline constexpr float kSlotLabelWidth = 90.0f;
inline constexpr float kButtonLabelWidth = 70.0f;
inline constexpr float kFieldCaptionHeight = 14.0f;
inline constexpr float kFieldCaptionGap = 4.0f;
// DERIVED, and deliberately still here. See the note below the table.
inline constexpr float kFieldStackHeight = kFieldCaptionHeight + kFieldCaptionGap + kControlHeight;
// Wide enough for the longest message label the choice catalog offers. At 150
// the browser's `<select>` reported a 156px content width in a 150px box, so
// every one of the six message selectors rendered its longest options
// truncated -- found the first time the text-fit criterion was extended to
// ComboBox and TextField, which render their value text in the element the
// backend sized. 160 matches `kSlotWidth` rather than being a fresh magic
// number. It survives task 7.1 as a DECLARED extent, which sru-54's resolution
// explicitly does not ban -- what 7.1 removed is the arithmetic that derived
// container extents from constants like this one.
inline constexpr float kMessageWidth = 160.0f;
inline constexpr float kArgumentWidth = 80.0f;
inline constexpr float kColumnHeaderHeight = 22.0f;
inline constexpr float kColumnGap = 16.0f;
inline constexpr std::size_t kColumnCount = 2;
inline constexpr std::size_t kRowsPerColumn = 3;

// Each button row is: side-button name, message dropdown, argument field. No
// backend paints a container node's own label, so the row and column names are
// rendered nodes rather than Row/Section labels.
//
// The two DERIVED constants below, and why they survived task 7.1's cleanup.
//
// Every container extent this form used to compute -- the slot row's width, the
// button fields' width and height, a column's width and height, the form's own
// width and height, the top of the columns -- is gone, replaced by
// `Extent::Intrinsic()`. The resolver sums the same children and reaches the
// same numbers, so the form resolves byte-identically without the producer
// restating any of it.
//
// What could not follow is the inline argument error. It is an sru-44
// out-of-flow node, which is a sanctioned positioning mode, but its declared
// position is derived from its siblings' extents and its row reserves a fixed
// band for it. Making it an ordinary in-flow child removes both derivations at
// once -- and cannot be done with the library as it stands. An in-flow node's
// intrinsic cross extent propagates into every ancestor's, and the message
// "Argument must be a non-negative base-10 integer" reserves 424px against a
// 326px column, so an intrinsic column would widen by a third the moment a
// validation error appeared, shoving the second column sideways. Capping it
// with `Extent::Max` does not help: the clamp applies to the resolved extent as
// well as the intrinsic one, so the cap that keeps the column steady is the
// same cap that truncates the message. What is missing is a way for a producer
// to say "this text does not drive my container's intrinsic extent" -- an
// intrinsic-only cap, or text wrapping. That is a library facility, and adding
// one is outside this change; it is reported as a design gap instead.
inline constexpr float kColumnWidth =
    kButtonLabelWidth + kFieldGap + kMessageWidth + kFieldGap + kArgumentWidth;
inline constexpr float kButtonRowHeight = kFieldStackHeight + kErrorGap + kErrorHeight;

// A standalone preview surface for `BuildTree()`, which the `ui::Surface`
// interface requires and only the form's own tests use; hosts splice
// `BuildSubtree()` instead. It is a surface the form is rendered INTO, not a
// size the form derives: every extent inside is intrinsic, so the body keeps
// its own measurements whatever this is, and a form that outgrew it would fail
// sru-54's gate loudly rather than clip. The library has no "resolve to the
// root's intrinsic extent" entry point, which is why a number appears here at
// all.
inline constexpr float kPreviewSurfaceWidth = 1024.0f;
inline constexpr float kPreviewSurfaceHeight = 768.0f;

}  // namespace TwisterFormLayout

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

std::optional<UISystemMessage> MessageForOptionId(std::string_view id) {
    for (const TwisterMessageChoice& choice : kTwisterMessageChoices) {
        if (choice.id == id) {
            return choice.message;
        }
    }
    return std::nullopt;
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

bool FieldHasError(std::string_view text) {
    std::size_t ignored = 0;
    return !ParseSizeT(text, ignored);
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

bool MatchesAnyAlias(std::string_view name, const std::vector<std::string>& aliases) {
    return std::any_of(aliases.begin(), aliases.end(), [name](const std::string& alias) {
        return CaseInsensitiveEquals(name, alias);
    });
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

bool SameEncoderMapping(const EncoderMidiMapping& lhs, const EncoderMidiMapping& rhs) {
    return lhs.control == rhs.control && lhs.slotIx == rhs.slotIx && lhs.position == rhs.position;
}

bool SameEncoderInput(const EncoderMidiInConfig& lhs, const EncoderMidiInConfig& rhs) {
    if (lhs.mode != rhs.mode || lhs.turnStep != rhs.turnStep ||
        lhs.turns.size() != rhs.turns.size() || lhs.pushes.size() != rhs.pushes.size()) {
        return false;
    }
    for (std::size_t ix = 0; ix < lhs.turns.size(); ++ix) {
        if (!SameEncoderMapping(lhs.turns[ix], rhs.turns[ix])) {
            return false;
        }
    }
    for (std::size_t ix = 0; ix < lhs.pushes.size(); ++ix) {
        if (!SameEncoderMapping(lhs.pushes[ix], rhs.pushes[ix])) {
            return false;
        }
    }
    return true;
}

bool SameEncoderOutput(const EncoderMidiOutConfig& lhs, const EncoderMidiOutConfig& rhs) {
    if (lhs.protocol != rhs.protocol ||
        lhs.wrldBldrColorBudgetPerProcess != rhs.wrldBldrColorBudgetPerProcess ||
        lhs.mappings.size() != rhs.mappings.size()) {
        return false;
    }
    for (std::size_t ix = 0; ix < lhs.mappings.size(); ++ix) {
        if (lhs.mappings[ix].slotIx != rhs.mappings[ix].slotIx ||
            lhs.mappings[ix].position != rhs.mappings[ix].position ||
            lhs.mappings[ix].cc != rhs.mappings[ix].cc) {
            return false;
        }
    }
    return true;
}

std::size_t TwisterArgument(const MidiControllerSystemMessageAssociation& association,
                            UISystemMessage message) {
    switch (message) {
        case UISystemMessage::ToggleGestureSelect:
        case UISystemMessage::HoldGestureSelect:
            return association.press.gestureIx;
        case UISystemMessage::SelectParamBank:
            return association.press.bankIx;
        case UISystemMessage::SceneSelect:
            return association.press.sceneIx;
        default:
            return 0;
    }
}

bool SameAssociation(const MidiControllerSystemMessageAssociation& lhs,
                     const MidiControllerSystemMessageAssociation& rhs) {
    return lhs.control == rhs.control && lhs.press == rhs.press &&
           lhs.release == rhs.release && lhs.feedback == rhs.feedback &&
           lhs.outputFeedback == rhs.outputFeedback;
}

UISystemMessage TwisterMessageForAssociation(const MidiControllerSystemMessageAssociation& association) {
    const MessageIn& press = association.press;
    switch (press.type) {
        case MessageIn::Type::ToggleReset:
            return press.hasBoolValue ? UISystemMessage::HoldReset : UISystemMessage::ToggleReset;
        case MessageIn::Type::ToggleRandom:
            return press.hasBoolValue ? UISystemMessage::HoldRandom : UISystemMessage::ToggleRandom;
        case MessageIn::Type::ToggleRandomMod:
            return press.hasBoolValue ? UISystemMessage::HoldRandomMod : UISystemMessage::ToggleRandomMod;
        case MessageIn::Type::ToggleGestureSelect: return UISystemMessage::ToggleGestureSelect;
        case MessageIn::Type::SetGestureSelect: return UISystemMessage::HoldGestureSelect;
        case MessageIn::Type::SelectParamBank: return UISystemMessage::SelectParamBank;
        case MessageIn::Type::NextParamBank: return UISystemMessage::NextParamBank;
        case MessageIn::Type::PrevParamBank: return UISystemMessage::PrevParamBank;
        case MessageIn::Type::Start: return UISystemMessage::Start;
        case MessageIn::Type::Continue: return UISystemMessage::Continue;
        case MessageIn::Type::Stop: return UISystemMessage::Stop;
        case MessageIn::Type::Clock: return UISystemMessage::Clock;
        case MessageIn::Type::SceneSelect: return UISystemMessage::SceneSelect;
        case MessageIn::Type::ParamIncDec:
        case MessageIn::Type::ParamSetAbsolute:
        case MessageIn::Type::ParamPush:
        case MessageIn::Type::SetGestureValue:
        case MessageIn::Type::SetSceneBlend:
        case MessageIn::Type::GridPress:
        case MessageIn::Type::GridRelease:
        case MessageIn::Type::GridPressureChange:
        case MessageIn::Type::SelectGrid:
        case MessageIn::Type::ParamSetAbsoluteOnBank:
        case MessageIn::Type::AppAction:
        case MessageIn::Type::HoldDrill:
            return UISystemMessage::ParamIncDec;
    }
    return UISystemMessage::ParamIncDec;
}

}  // namespace

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

ui::Subtree MfTwisterConfigForm::BuildSubtree() {
    namespace Layout = TwisterFormLayout;

    const auto fixedRow = [](float height) {
        ui::LayoutOptions layout;
        layout.main = ui::Extent::Px(height);
        layout.padding = 0.0f;
        layout.gap = Layout::kFieldGap;
        return layout;
    };
    // The column is as wide as the button rows it stacks -- the resolver sums
    // them. It used to declare `kColumnWidth` and the constant had to be kept
    // in step with the fields by hand.
    const auto formColumn = [] {
        ui::LayoutOptions layout;
        layout.main = ui::Extent::Intrinsic();
        layout.cross = ui::Extent::Weight(1.0f);
        layout.padding = 0.0f;
        layout.gap = Layout::kRowGap;
        layout.formGrid = true;
        return layout;
    };
    const auto fixedControl = [](float width, float height, bool enabled = true) {
        ui::ControlStyle style;
        style.enabled = enabled;
        style.layout.main = ui::Extent::Px(width);
        style.layout.cross = ui::Extent::Px(height);
        return style;
    };
    const auto fixedField = [&](float width, float height, bool enabled = true) {
        ui::ControlStyle style = fixedControl(width, height, enabled);
        style.color = pagestyle::kDefaultPanel;
        return style;
    };
    const auto fixedVerticalField = [&](float width, float height, bool enabled = true) {
        ui::ControlStyle style;
        style.enabled = enabled;
        style.color = pagestyle::kDefaultPanel;
        style.layout.main = ui::Extent::Px(height);
        style.layout.cross = ui::Extent::Px(width);
        return style;
    };
    const auto fieldCell = [](float width) {
        ui::LayoutOptions layout;
        layout.main = ui::Extent::Px(width);
        layout.cross = ui::Extent::Weight(1.0f);
        layout.padding = 0.0f;
        layout.gap = Layout::kFieldCaptionGap;
        return layout;
    };
    const auto textRow = [](float height) {
        ui::ControlStyle style;
        style.layout.main = ui::Extent::Px(height);
        return style;
    };
    ui::LayoutOptions body;
    body.main = ui::Extent::Intrinsic();
    body.cross = ui::Extent::Intrinsic();
    body.padding = Layout::kMargin;
    body.gap = Layout::kRowGap;
    body.formGrid = true;

    ui::LayoutOptions slotRow = fixedRow(Layout::kControlHeight);
    slotRow.cross = ui::Extent::Intrinsic();

    ui::LayoutOptions columnsRow;
    columnsRow.main = ui::Extent::Intrinsic();
    columnsRow.cross = ui::Extent::Intrinsic();
    columnsRow.padding = 0.0f;
    columnsRow.gap = Layout::kColumnGap;

    ui::LayoutOptions columnsWrapper;
    columnsWrapper.main = ui::Extent::Intrinsic();
    columnsWrapper.cross = ui::Extent::Intrinsic();
    columnsWrapper.padding = 0.0f;
    columnsWrapper.gap = 0.0f;

    ui::LayoutOptions buttonFieldsRow;
    buttonFieldsRow.main = ui::Extent::Intrinsic();
    buttonFieldsRow.cross = ui::Extent::Intrinsic();
    buttonFieldsRow.padding = 0.0f;
    buttonFieldsRow.gap = Layout::kFieldGap;

    const std::string slotId = std::string(kMfTwisterFormRootId) + ".encoder-slot";
    ui::Builder builder;
    builder.Rootless();
    builder.Column(std::string(kMfTwisterFormRootId) + ".body", body, [&](ui::Builder& form) {
        form.Row(std::string(kMfTwisterFormRootId) + ".slot", slotRow, [&](ui::Builder& row) {
            row.Label(std::string(kMfTwisterFormRootId) + ".encoder-slot.caption",
                      "Encoder Slot",
                      fixedControl(Layout::kSlotLabelWidth, Layout::kControlHeight));
            row.TextField(slotId,
                          "",
                          encoderSlotText,
                          ui::Action::Named(slotId),
                          fixedField(Layout::kSlotWidth, Layout::kControlHeight));
        });
        if (FieldHasError(encoderSlotText)) {
            form.StatusText(slotId + ".error",
                            "Encoder Slot must be a non-negative base-10 integer",
                            textRow(Layout::kErrorHeight));
        }

        form.Section(std::string(kMfTwisterFormRootId) + ".columns-wrapper",
                     columnsWrapper,
                     [&](ui::Builder& wrapper) {
                         wrapper.Row(std::string(kMfTwisterFormRootId) + ".columns",
                                     columnsRow,
                                     [&](ui::Builder& columns) {
                                         for (std::size_t column = 0;
                                              column < Layout::kColumnCount;
                                              ++column) {
                                             columns.Section(
                                                 std::string(kMfTwisterFormRootId) + ".column." +
                                                     std::to_string(column),
                                                 formColumn(),
                                                 [&](ui::Builder& columnBuilder) {
                                                     columnBuilder.Label(
                                                         std::string(kMfTwisterFormRootId) +
                                                             ".column." + std::to_string(column) +
                                                             ".heading",
                                                         column == 0 ? "Left (CC 8-10)"
                                                                     : "Right (CC 11-13)",
                                                         textRow(Layout::kColumnHeaderHeight));
                                                     for (std::size_t row = 0;
                                                          row < Layout::kRowsPerColumn;
                                                          ++row) {
                                                         const std::size_t buttonIx =
                                                             column * Layout::kRowsPerColumn + row;
                                                         const MfTwisterButtonConfig& button =
                                                             buttons[buttonIx];
                                                         columnBuilder.Row(
                                                             std::string(kMfTwisterFormRootId) +
                                                                 ".button." +
                                                                 std::to_string(buttonIx),
                                                             fixedRow(Layout::kButtonRowHeight),
                                                             [&](ui::Builder& buttonRow) {
                                                                 buttonRow.Label(
                                                                     ButtonFieldId(buttonIx, "label"),
                                                                     "Button " +
                                                                         std::to_string(buttonIx + 1),
                                                                     fixedControl(
                                                                         Layout::kButtonLabelWidth,
                                                                         Layout::kControlHeight));

                                                                 buttonRow.Row(
                                                                     std::string(kMfTwisterFormRootId) +
                                                                         ".button." +
                                                                         std::to_string(buttonIx) +
                                                                         ".fields",
                                                                     buttonFieldsRow,
                                                                     [&](ui::Builder& fields) {
                                                                         std::vector<ui::ControlOption>
                                                                             options;
                                                                         for (const TwisterMessageChoice&
                                                                                  choice :
                                                                              kTwisterMessageChoices) {
                                                                             const UISystemMessageChoice*
                                                                                 catalogChoice =
                                                                                     FindUISystemMessageChoice(
                                                                                         choice.message);
                                                                             if (catalogChoice != nullptr) {
                                                                                 options.push_back(
                                                                                     {std::string(choice.id),
                                                                                      catalogChoice->label});
                                                                             }
                                                                         }
                                                                         const std::string messageId =
                                                                             ButtonFieldId(buttonIx,
                                                                                           "message");
                                                                         fields.Column(
                                                                             messageId + ".cell",
                                                                             fieldCell(Layout::kMessageWidth),
                                                                             [&](ui::Builder& fieldCell) {
                                                                                 fieldCell.Label(
                                                                                     messageId + ".caption",
                                                                                     "Message",
                                                                                     textRow(Layout::
                                                                                                 kFieldCaptionHeight));
                                                                                 fieldCell.ComboBox(
                                                                                     messageId,
                                                                                     std::move(options),
                                                                                     MessageOptionId(
                                                                                         button.message),
                                                                                     ui::Action::Named(messageId),
                                                                                     fixedVerticalField(
                                                                                         Layout::kMessageWidth,
                                                                                         Layout::kControlHeight));
                                                                             });

                                                                         const std::string argumentId =
                                                                             ButtonFieldId(buttonIx,
                                                                                           "argument");
                                                                         const bool argumentEnabled =
                                                                             TwisterArgumentEnabled(
                                                                                 button.message);
                                                                         fields.Column(
                                                                             argumentId + ".cell",
                                                                             fieldCell(Layout::kArgumentWidth),
                                                                             [&](ui::Builder& fieldCell) {
                                                                                 fieldCell.Label(
                                                                                     argumentId + ".caption",
                                                                                     "Argument",
                                                                                     textRow(Layout::
                                                                                                 kFieldCaptionHeight));
                                                                                 fieldCell.TextField(
                                                                                     argumentId,
                                                                                     "",
                                                                                     button.argumentText,
                                                                                     ui::Action::Named(argumentId),
                                                                                     fixedVerticalField(
                                                                                         Layout::kArgumentWidth,
                                                                                         Layout::kControlHeight,
                                                                                         argumentEnabled));
                                                                             });
                                                                     });
                                                                 const std::string argumentId =
                                                                     ButtonFieldId(buttonIx, "argument");
                                                                 const bool argumentEnabled =
                                                                     TwisterArgumentEnabled(button.message);
                                                                 if (argumentEnabled &&
                                                                     FieldHasError(
                                                                         button.argumentText)) {
                                                                     ui::ControlStyle errorStyle =
                                                                         textRow(Layout::kErrorHeight);
                                                                     errorStyle.layout.explicitBounds = {
                                                                         0.0f,
                                                                         Layout::kFieldStackHeight +
                                                                             Layout::kErrorGap,
                                                                         Layout::kColumnWidth,
                                                                         Layout::kErrorHeight};
                                                                     buttonRow.StatusText(
                                                                         argumentId + ".error",
                                                                         "Argument must be a non-negative base-10 integer",
                                                                         errorStyle);
                                                                 }
                                                             });
                                                     }
                                                 });
                                         }
                                     });
                     });
    });
    return builder.BuildSubtree();
}

ui::NodeTree MfTwisterConfigForm::BuildTree() {
    namespace Layout = TwisterFormLayout;
    const ui::Bounds preview{
        0.0f, 0.0f, Layout::kPreviewSurfaceWidth, Layout::kPreviewSurfaceHeight};
    ui::Builder builder;
    builder.Root(std::string(kMfTwisterFormRootId), preview);
    builder.Splice(BuildSubtree());
    return builder.Build(preview);
}

void MfTwisterConfigForm::SetActionHandler(ActionHandler handler) {
    actionHandler_ = std::move(handler);
}

void MfTwisterConfigForm::DispatchAction(const ui::Action& action) {
    if (action.name == std::string(kMfTwisterFormRootId) + ".encoder-slot") {
        encoderSlotText = action.value;
    } else {
        for (std::size_t buttonIx = 0; buttonIx < buttons.size(); ++buttonIx) {
            if (action.name == ButtonFieldId(buttonIx, "message")) {
                if (const auto message = MessageForOptionId(action.value)) {
                    buttons[buttonIx].message = *message;
                }
                break;
            }
            if (action.name == ButtonFieldId(buttonIx, "argument")) {
                buttons[buttonIx].argumentText = action.value;
                break;
            }
        }
    }
    if (actionHandler_) {
        actionHandler_(action);
    }
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

std::string_view MfTwisterConfigForm::ReconfigureWarning() const {
    return reconfigureWarning;
}

std::string_view MfTwisterControllerWizard::Id() const {
    return kMfTwisterWizardId;
}

std::unique_ptr<ControllerConfigForm>
MfTwisterControllerWizard::ConfigForm(const std::optional<MidiControllerSlot>& seed) const {
    if (!seed.has_value()) {
        return std::make_unique<MfTwisterConfigForm>();
    }

    const MidiControllerProfileConfig* profile =
        seed->disposition == MidiControllerDisposition::Active
            ? &seed->config
            : (seed->dormantConfig ? &*seed->dormantConfig : nullptr);
    if (profile != nullptr) {
        if (std::optional<MfTwisterConfigForm> extracted = ExtractMfTwisterWizardSeed(*profile)) {
            return std::make_unique<MfTwisterConfigForm>(std::move(*extracted));
        }
    }

    auto form = std::make_unique<MfTwisterConfigForm>();
    form->reconfigureWarning =
        "This stored profile cannot be represented by the wizard. Submit replaces the whole profile.";
    return form;
}

std::optional<MfTwisterConfigForm>
ExtractMfTwisterWizardSeed(const MidiControllerProfileConfig& profile) {
    if (profile.analogInput.has_value() || profile.pressureInput.has_value() ||
        !profile.encoderInput.has_value() || !profile.encoderOutput.has_value() ||
        profile.encoderInput->turns.empty()) {
        return std::nullopt;
    }

    const std::size_t slotIx = profile.encoderInput->turns.front().slotIx;
    const MidiControllerProfileConfig expected = MfTwisterDefaultProfileConfig(
        MfTwisterDefaultProfileOptions{.slotIx = slotIx});
    if (!SameEncoderInput(*profile.encoderInput, *expected.encoderInput) ||
        !SameEncoderOutput(*profile.encoderOutput, *expected.encoderOutput) ||
        profile.systemMessages.size() != MfTwisterConfigForm::kButtonCount) {
        return std::nullopt;
    }

    MfTwisterConfigForm form;
    form.encoderSlotText = std::to_string(slotIx);
    std::array<bool, MfTwisterConfigForm::kButtonCount> found{};
    for (const MidiControllerSystemMessageAssociation& association : profile.systemMessages) {
        if (!association.control.has_value() || association.control->type != MidiControlType::Cc ||
            association.control->channel != 3 || association.control->cc < 8 ||
            association.control->cc >= 8 + MfTwisterConfigForm::kButtonCount ||
            association.wrldBldrPosition.has_value() || association.launchpadPosition.has_value() ||
            association.outputFeedback) {
            return std::nullopt;
        }
        const std::size_t buttonIx = association.control->cc - 8;
        if (found[buttonIx]) {
            return std::nullopt;
        }

        const UISystemMessage message = TwisterMessageForAssociation(association);
        if (!TwisterMessageAllowed(message)) {
            return std::nullopt;
        }
        const std::size_t argument = TwisterArgument(association, message);
        MidiControllerSystemMessageAssociation expectedAssociation =
            MakeUISystemMessageAssociation(*FindUISystemMessageChoice(message), argument);
        if (message == UISystemMessage::SelectParamBank ||
            message == UISystemMessage::NextParamBank ||
            message == UISystemMessage::PrevParamBank) {
            expectedAssociation.press.slotIx = slotIx;
            expectedAssociation.feedback.slotIx = slotIx;
            if (expectedAssociation.release.has_value()) {
                expectedAssociation.release->slotIx = slotIx;
            }
        }
        expectedAssociation.control = MidiControlAddress{
            .channel = 3, .cc = static_cast<std::uint8_t>(8 + buttonIx)};
        expectedAssociation.outputFeedback = false;
        if (!SameAssociation(association, expectedAssociation)) {
            return std::nullopt;
        }
        form.buttons[buttonIx] = {.message = message, .argumentText = std::to_string(argument)};
        found[buttonIx] = true;
    }
    if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) {
        return std::nullopt;
    }
    return form;
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

// The form an app default's wizard presents: no fields, since the default's
// config is fixed and nothing on it is user-editable.
class AppDefaultConfigForm final : public ControllerConfigForm {
public:
    explicit AppDefaultConfigForm(std::string wizardId) : wizardId_(std::move(wizardId)) {}

    std::string_view WizardId() const override { return wizardId_; }

    ui::NodeTree BuildTree() override {
        namespace Layout = TwisterFormLayout;
        const ui::Bounds preview{0.0f, 0.0f, Layout::kPreviewSurfaceWidth, Layout::kPreviewSurfaceHeight};
        ui::Builder builder;
        builder.Root(wizardId_ + ".form", preview);
        return builder.Build(preview);
    }

    void SetActionHandler(ActionHandler handler) override { actionHandler_ = std::move(handler); }

    void DispatchAction(const ui::Action& action) override {
        if (actionHandler_) {
            actionHandler_(action);
        }
    }

    bool Validate(std::string& error) const override {
        error.clear();
        return true;
    }

private:
    std::string wizardId_;
    ActionHandler actionHandler_;
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

    std::unique_ptr<ControllerConfigForm>
    ConfigForm(const std::optional<MidiControllerSlot>&) const override {
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

std::vector<ControllerWizardDescriptor> MakeControllerWizardRegistry(const MidiAppCatalog& catalog) {
    if (catalog.deviceDefaults.empty()) {
        return {ControllerWizardDescriptor{
            .id = std::string(kMfTwisterWizardId),
            .displayName = std::string(kMfTwisterDisplayName),
            .kind = MidiProfileKind::MfTwister,
            .inputAliases = {std::string(kMfTwisterAlias)},
            .outputAliases = {std::string(kMfTwisterAlias)},
            .factory = [] { return std::make_unique<MfTwisterControllerWizard>(); }}};
    }

    std::vector<ControllerWizardDescriptor> registry;
    registry.reserve(catalog.deviceDefaults.size());
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

std::unique_ptr<ControllerWizard> MakeControllerWizard(
    const std::vector<ControllerWizardDescriptor>& registry, std::string_view id) {
    for (const ControllerWizardDescriptor& descriptor : registry) {
        if (descriptor.id == id) {
            if (!descriptor.factory) {
                return nullptr;
            }
            return descriptor.factory();
        }
    }
    return nullptr;
}

}  // namespace synth
