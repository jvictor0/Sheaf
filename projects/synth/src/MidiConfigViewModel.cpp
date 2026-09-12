#include "synth/MidiConfigViewModel.hpp"

#include "synth/ControllerWizard.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/MidiAppCatalog.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <tuple>

namespace synth {

namespace {

using Field = MidiMappingRowVM::Field;

std::optional<std::size_t> PrimaryMessageArg(const MessageIn& message) {
    switch (message.type) {
        case MessageIn::Type::ParamIncDec:
        case MessageIn::Type::ParamSetAbsolute:
        case MessageIn::Type::ParamPush:
            return message.position;
        case MessageIn::Type::ToggleGestureSelect:
        case MessageIn::Type::SetGestureSelect:
        case MessageIn::Type::SetGestureValue:
            return message.gestureIx;
        case MessageIn::Type::SelectParamBank:
            return message.bankIx;
        case MessageIn::Type::NextParamBank:
        case MessageIn::Type::PrevParamBank:
            return message.slotIx;
        case MessageIn::Type::SceneSelect:
            return message.sceneIx;
        case MessageIn::Type::ToggleReset:
        case MessageIn::Type::ToggleRandom:
        case MessageIn::Type::ToggleRandomMod:
        case MessageIn::Type::Start:
        case MessageIn::Type::Continue:
        case MessageIn::Type::Stop:
        case MessageIn::Type::Clock:
        case MessageIn::Type::SetSceneBlend:
        case MessageIn::Type::GridPress:
        case MessageIn::Type::GridRelease:
        case MessageIn::Type::GridPressureChange:
        case MessageIn::Type::SelectGrid:
        case MessageIn::Type::ParamSetAbsoluteOnBank:
        case MessageIn::Type::AppAction:
        case MessageIn::Type::HoldDrill:
            return std::nullopt;
    }
    return std::nullopt;
}

bool SetPrimaryMessageArg(MessageIn& message, std::size_t arg) {
    switch (message.type) {
        case MessageIn::Type::ParamIncDec:
        case MessageIn::Type::ParamSetAbsolute:
        case MessageIn::Type::ParamPush:
            message.position = arg;
            return true;
        case MessageIn::Type::ToggleGestureSelect:
        case MessageIn::Type::SetGestureSelect:
        case MessageIn::Type::SetGestureValue:
            message.gestureIx = arg;
            return true;
        case MessageIn::Type::SelectParamBank:
            message.bankIx = arg;
            return true;
        case MessageIn::Type::NextParamBank:
        case MessageIn::Type::PrevParamBank:
            message.slotIx = arg;
            return true;
        case MessageIn::Type::SceneSelect:
            message.sceneIx = arg;
            return true;
        case MessageIn::Type::ToggleReset:
        case MessageIn::Type::ToggleRandom:
        case MessageIn::Type::ToggleRandomMod:
        case MessageIn::Type::Start:
        case MessageIn::Type::Continue:
        case MessageIn::Type::Stop:
        case MessageIn::Type::Clock:
        case MessageIn::Type::SetSceneBlend:
        case MessageIn::Type::GridPress:
        case MessageIn::Type::GridRelease:
        case MessageIn::Type::GridPressureChange:
        case MessageIn::Type::SelectGrid:
        case MessageIn::Type::ParamSetAbsoluteOnBank:
        case MessageIn::Type::AppAction:
        case MessageIn::Type::HoldDrill:
            return false;
    }
    return false;
}

bool UISystemMessageHasArg(UISystemMessage message) {
    switch (message) {
        case UISystemMessage::ParamIncDec:
        case UISystemMessage::ParamSetAbsolute:
        case UISystemMessage::ParamPush:
        case UISystemMessage::ToggleGestureSelect:
        case UISystemMessage::HoldGestureSelect:
        case UISystemMessage::SelectParamBank:
        case UISystemMessage::NextParamBank:
        case UISystemMessage::PrevParamBank:
        case UISystemMessage::SetGestureValue:
        case UISystemMessage::SceneSelect:
            return true;
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
        case UISystemMessage::SetSceneBlend:
        case UISystemMessage::AppAction:
        case UISystemMessage::HoldDrill:
            return false;
    }
    return false;
}

std::size_t AssociationPrimaryArg(const MidiControllerSystemMessageAssociation& association) {
    if (const std::optional<std::size_t> arg = PrimaryMessageArg(association.press)) {
        return *arg;
    }
    if (association.release.has_value()) {
        if (const std::optional<std::size_t> arg = PrimaryMessageArg(*association.release)) {
            return *arg;
        }
    }
    return 0;
}

UISystemMessage UISystemMessageForAssociation(const MidiControllerSystemMessageAssociation& association) {
    const MessageIn& press = association.press;
    switch (press.type) {
        case MessageIn::Type::ParamIncDec:
            return UISystemMessage::ParamIncDec;
        case MessageIn::Type::ParamSetAbsolute:
            return UISystemMessage::ParamSetAbsolute;
        case MessageIn::Type::ParamPush:
            return UISystemMessage::ParamPush;
        case MessageIn::Type::ToggleReset:
            return press.hasBoolValue ? UISystemMessage::HoldReset : UISystemMessage::ToggleReset;
        case MessageIn::Type::ToggleRandom:
            return press.hasBoolValue ? UISystemMessage::HoldRandom : UISystemMessage::ToggleRandom;
        case MessageIn::Type::ToggleRandomMod:
            return press.hasBoolValue ? UISystemMessage::HoldRandomMod : UISystemMessage::ToggleRandomMod;
        case MessageIn::Type::ToggleGestureSelect:
            return UISystemMessage::ToggleGestureSelect;
        case MessageIn::Type::SetGestureSelect:
            return UISystemMessage::HoldGestureSelect;
        case MessageIn::Type::SelectParamBank:
            return UISystemMessage::SelectParamBank;
        case MessageIn::Type::NextParamBank:
            return UISystemMessage::NextParamBank;
        case MessageIn::Type::PrevParamBank:
            return UISystemMessage::PrevParamBank;
        case MessageIn::Type::Start:
            return UISystemMessage::Start;
        case MessageIn::Type::Continue:
            return UISystemMessage::Continue;
        case MessageIn::Type::Stop:
            return UISystemMessage::Stop;
        case MessageIn::Type::Clock:
            return UISystemMessage::Clock;
        case MessageIn::Type::SetGestureValue:
            return UISystemMessage::SetGestureValue;
        case MessageIn::Type::SceneSelect:
            return UISystemMessage::SceneSelect;
        case MessageIn::Type::SetSceneBlend:
            return UISystemMessage::SetSceneBlend;
        case MessageIn::Type::GridPress:
        case MessageIn::Type::GridRelease:
        case MessageIn::Type::GridPressureChange:
        case MessageIn::Type::SelectGrid:
        case MessageIn::Type::ParamSetAbsoluteOnBank:
            return UISystemMessage::Clock;
        case MessageIn::Type::AppAction:
            return UISystemMessage::AppAction;
        case MessageIn::Type::HoldDrill:
            return UISystemMessage::HoldDrill;
    }
    return UISystemMessage::Clock;
}

MessageIn PressForUISystemMessage(UISystemMessage message, const MidiControllerSystemMessageAssociation& previous) {
    const std::size_t arg = AssociationPrimaryArg(previous);
    switch (message) {
        case UISystemMessage::ParamIncDec:
            return MessageIn::ParamIncDec(0, previous.press.slotIx, arg, previous.press.delta);
        case UISystemMessage::ParamSetAbsolute:
            return MessageIn::ParamSetAbsolute(0, previous.press.slotIx, arg, previous.press.value);
        case UISystemMessage::ParamPush:
            return MessageIn::ParamPush(0, previous.press.slotIx, arg);
        case UISystemMessage::ToggleReset:
            return MessageIn::ToggleReset(0);
        case UISystemMessage::HoldReset:
            return MessageIn::SetReset(0, true);
        case UISystemMessage::ToggleRandom:
            return MessageIn::ToggleRandom(0);
        case UISystemMessage::HoldRandom:
            return MessageIn::SetRandom(0, true);
        case UISystemMessage::ToggleRandomMod:
            return MessageIn::ToggleRandomMod(0);
        case UISystemMessage::HoldRandomMod:
            return MessageIn::SetRandomMod(0, true);
        case UISystemMessage::ToggleGestureSelect:
            return MessageIn::ToggleGestureSelect(0, arg);
        case UISystemMessage::HoldGestureSelect:
            return MessageIn::SetGestureSelect(0, arg, true);
        case UISystemMessage::SelectParamBank:
            return MessageIn::SelectParamBank(0, previous.press.slotIx, arg);
        case UISystemMessage::NextParamBank:
            return MessageIn::NextParamBank(0, arg);
        case UISystemMessage::PrevParamBank:
            return MessageIn::PrevParamBank(0, arg);
        case UISystemMessage::Start:
            return MessageIn::Start(0);
        case UISystemMessage::Continue:
            return MessageIn::Continue(0);
        case UISystemMessage::Stop:
            return MessageIn::Stop(0);
        case UISystemMessage::Clock:
            return MessageIn::Clock(0);
        case UISystemMessage::SetGestureValue:
            return MessageIn::SetGestureValue(0, arg, previous.press.value);
        case UISystemMessage::SceneSelect:
            return MessageIn::SceneSelect(0, arg);
        case UISystemMessage::SetSceneBlend:
            return MessageIn::SetSceneBlend(0, previous.press.value);
        case UISystemMessage::AppAction:
            return MessageIn::AppAction(0, previous.press.appActionIx, 0.0f);
        case UISystemMessage::HoldDrill:
            return MessageIn::HoldDrill(0, true);
    }
    return MessageIn::Clock(0);
}

std::optional<MessageIn> ReleaseForUISystemMessage(UISystemMessage message, const MessageIn& press) {
    switch (message) {
        case UISystemMessage::HoldReset:
            return MessageIn::SetReset(0, false);
        case UISystemMessage::HoldRandom:
            return MessageIn::SetRandom(0, false);
        case UISystemMessage::HoldRandomMod:
            return MessageIn::SetRandomMod(0, false);
        case UISystemMessage::HoldGestureSelect:
            return MessageIn::SetGestureSelect(0, press.gestureIx, false);
        case UISystemMessage::ParamIncDec:
        case UISystemMessage::ParamSetAbsolute:
        case UISystemMessage::ParamPush:
        case UISystemMessage::ToggleReset:
        case UISystemMessage::ToggleRandom:
        case UISystemMessage::ToggleRandomMod:
        case UISystemMessage::ToggleGestureSelect:
        case UISystemMessage::SelectParamBank:
        case UISystemMessage::NextParamBank:
        case UISystemMessage::PrevParamBank:
        case UISystemMessage::Start:
        case UISystemMessage::Continue:
        case UISystemMessage::Stop:
        case UISystemMessage::Clock:
        case UISystemMessage::SetGestureValue:
        case UISystemMessage::SceneSelect:
        case UISystemMessage::SetSceneBlend:
        case UISystemMessage::AppAction:
            return std::nullopt;
        case UISystemMessage::HoldDrill:
            return MessageIn::HoldDrill(0, false);
    }
    return std::nullopt;
}

void ApplyUISystemMessage(MidiControllerSystemMessageAssociation& association, UISystemMessage message) {
    association.press = PressForUISystemMessage(message, association);
    association.release = ReleaseForUISystemMessage(message, association.press);
    association.feedback = association.press;
}

bool SetUISystemMessageArg(MidiControllerSystemMessageAssociation& association, std::size_t arg) {
    if (!UISystemMessageHasArg(UISystemMessageForAssociation(association))) {
        return false;
    }
    if (!SetPrimaryMessageArg(association.press, arg)) {
        return false;
    }
    if (association.release.has_value()) {
        SetPrimaryMessageArg(*association.release, arg);
    }
    SetPrimaryMessageArg(association.feedback, arg);
    return true;
}

}  // namespace

using Field = MidiMappingRowVM::Field;

bool FieldIsInteger(MidiMappingRowVM::Field field) {
    switch (field) {
        case Field::Channel:
        case Field::Cc:
        case Field::SlotIx:
        case Field::Position:
        case Field::GestureIx:
        case Field::LaunchpadX:
        case Field::LaunchpadY:
        case Field::WrldBldrX:
        case Field::WrldBldrY:
        case Field::SceneBlend:
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
        case Field::BlockRowMajor:
        case Field::BlockOutputFeedback:
        case Field::MessageArg:
        case Field::GridSlotIx:
        case Field::GridXMin:
        case Field::GridXMax:
        case Field::GridYMin:
        case Field::GridYMax:
            return true;
        case Field::TurnStep:
        case Field::EncoderMode:
        case Field::AddressType:
        case Field::MessageKind:
        case Field::BlockMessageType:
        case Field::AppAction:
            return false;
    }
    return false;
}

const std::vector<std::string>& EncoderModeCatalog() {
    // All EncoderMode choices, in declaration order (MidiController.hpp):
    // 0 = Signed7Bit, 1 = DirectionOnly, 2 = Absolute.
    static const std::vector<std::string> catalog = {"Signed 7-bit", "Direction only", "Absolute"};
    return catalog;
}

const std::vector<std::string>& ControlAddressTypeCatalog() {
    static const std::vector<std::string> catalog = {"CC", "Note"};
    return catalog;
}

const std::vector<std::string>& BlockableMessageCatalog() {
    // Indexed by BlockableMessage's declaration order (MidiConfigBlocks.hpp):
    // 0 = SceneSelect, 1 = BankSelect, 2 = GestureSelect.
    static const std::vector<std::string> catalog = {"Scene select", "Bank select", "Gesture select"};
    return catalog;
}

const char* FieldShortLabel(MidiMappingRowVM::Field field) {
    switch (field) {
        case Field::Channel:
            return "Ch";
        case Field::Cc:
            return "CC";
        case Field::SlotIx:
            return "Slot";
        case Field::Position:
            return "Pos";
        case Field::EncoderMode:
            return "Mode";
        case Field::TurnStep:
            return "Step";
        case Field::MessageKind:
            return "Message";
        case Field::AppAction:
            return "Target";
        case Field::MessageArg:
            return "Arg";
        case Field::LaunchpadX:
        case Field::WrldBldrX:
            return "X";
        case Field::LaunchpadY:
        case Field::WrldBldrY:
            return "Y";
        case Field::GestureIx:
            return "Gesture";
        case Field::SceneBlend:
            return "CC";
        case Field::Button:
            return "Btn";
        case Field::BlockStartCc:
            return "Start CC";
        case Field::BlockEndCc:
            return "End CC";
        case Field::BlockStartPos:
            return "Start Pos";
        case Field::BlockStartArg:
            return "Start Arg";
        case Field::BlockBankSlotIx:
            return "Bank Slot";
        case Field::BlockStartX:
            return "Start X";
        case Field::BlockStartY:
            return "Start Y";
        case Field::BlockEndX:
            return "End X";
        case Field::BlockEndY:
            return "End Y";
        case Field::BlockRowMajor:
            return "Row-major";
        case Field::BlockOutputFeedback:
            return "Feedback";
        case Field::BlockMessageType:
            return "Type";
        case Field::AddressType:
            return "Addr";
        case Field::GridSlotIx:
            return "Grid Slot";
        case Field::GridXMin:
            return "X Min";
        case Field::GridXMax:
            return "X Max";
        case Field::GridYMin:
            return "Y Min";
        case Field::GridYMax:
            return "Y Max";
    }
    return "";
}

const std::vector<UISystemMessageChoice>& UISystemMessageCatalog() {
    static const std::vector<UISystemMessageChoice> catalog = {
        UISystemMessageChoice{.label = "Param Inc/Dec", .message = UISystemMessage::ParamIncDec},
        UISystemMessageChoice{.label = "Param Set Absolute", .message = UISystemMessage::ParamSetAbsolute},
        UISystemMessageChoice{.label = "Param Push", .message = UISystemMessage::ParamPush},
        UISystemMessageChoice{.label = "Toggle Reset", .message = UISystemMessage::ToggleReset},
        UISystemMessageChoice{.label = "Hold Reset", .message = UISystemMessage::HoldReset},
        UISystemMessageChoice{.label = "Toggle Random", .message = UISystemMessage::ToggleRandom},
        UISystemMessageChoice{.label = "Hold Random", .message = UISystemMessage::HoldRandom},
        UISystemMessageChoice{.label = "Toggle Random Mod", .message = UISystemMessage::ToggleRandomMod},
        UISystemMessageChoice{.label = "Hold Random Mod", .message = UISystemMessage::HoldRandomMod},
        UISystemMessageChoice{.label = "Toggle Gesture Select", .message = UISystemMessage::ToggleGestureSelect},
        UISystemMessageChoice{.label = "Hold Gesture Select", .message = UISystemMessage::HoldGestureSelect},
        UISystemMessageChoice{.label = "Bank Select", .message = UISystemMessage::SelectParamBank},
        UISystemMessageChoice{.label = "Next Bank", .message = UISystemMessage::NextParamBank},
        UISystemMessageChoice{.label = "Previous Bank", .message = UISystemMessage::PrevParamBank},
        UISystemMessageChoice{.label = "Start", .message = UISystemMessage::Start},
        UISystemMessageChoice{.label = "Continue", .message = UISystemMessage::Continue},
        UISystemMessageChoice{.label = "Stop", .message = UISystemMessage::Stop},
        UISystemMessageChoice{.label = "Clock", .message = UISystemMessage::Clock},
        UISystemMessageChoice{.label = "Gesture Value", .message = UISystemMessage::SetGestureValue},
        UISystemMessageChoice{.label = "Scene Select", .message = UISystemMessage::SceneSelect},
        UISystemMessageChoice{.label = "Scene Blend", .message = UISystemMessage::SetSceneBlend},
    };
    return catalog;
}

const UISystemMessageChoice* FindUISystemMessageChoice(UISystemMessage message,
                                                        const std::vector<UISystemMessageChoice>& catalog,
                                                        std::string_view appAction,
                                                        std::string_view appActionValue) {
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const UISystemMessageChoice& choice) {
        if (choice.message != message) {
            return false;
        }
        if (message == UISystemMessage::AppAction) {
            return choice.appAction == appAction && choice.appActionValue == appActionValue;
        }
        return true;
    });
    return found == catalog.end() ? nullptr : &*found;
}

MidiControllerSystemMessageAssociation MakeUISystemMessageAssociation(
    const UISystemMessageChoice& choice, std::size_t argument) {
    MidiControllerSystemMessageAssociation association;
    association.press = MessageIn::Clock(0);
    association.feedback = association.press;
    if (choice.message == UISystemMessage::AppAction) {
        association.press = MessageIn::AppAction(0, choice.appActionIx, 0.0f);
        association.release = std::nullopt;
        association.feedback = association.press;
        association.appAction = choice.appAction;
        association.appActionValue = choice.appActionValue;
    } else {
        ApplyUISystemMessage(association, choice.message);
        SetUISystemMessageArg(association, argument);
    }
    return association;
}

std::vector<UISystemMessageChoice> MakeUISystemMessageChoices(const MidiAppCatalog& catalog) {
    if (catalog.libraryKinds.empty() && catalog.actions.empty()) {
        return UISystemMessageCatalog();
    }
    std::vector<UISystemMessageChoice> choices;
    choices.reserve(catalog.libraryKinds.size() + catalog.actions.size());
    for (const UISystemMessage kind : catalog.libraryKinds) {
        if (kind == UISystemMessage::HoldDrill) {
            choices.push_back(UISystemMessageChoice{.label = "Hold Drill", .message = UISystemMessage::HoldDrill});
            continue;
        }
        if (const UISystemMessageChoice* found = FindUISystemMessageChoice(kind)) {
            choices.push_back(*found);
        }
    }
    for (std::size_t ix = 0; ix < catalog.actions.size(); ++ix) {
        const MidiAppAction& action = catalog.actions[ix];
        choices.push_back(UISystemMessageChoice{.label = action.label,
                                                 .message = UISystemMessage::AppAction,
                                                 .appAction = action.action,
                                                 .appActionValue = action.value,
                                                 .appActionIx = ix});
    }
    return choices;
}

std::vector<UISystemMessageChoice> MakeAnalogAppActionChoices(const MidiAppCatalog& catalog) {
    std::vector<UISystemMessageChoice> choices;
    for (std::size_t ix = 0; ix < catalog.actions.size(); ++ix) {
        const MidiAppAction& action = catalog.actions[ix];
        if (!action.analogRange.has_value()) {
            continue;
        }
        choices.push_back(UISystemMessageChoice{.label = action.label,
                                                 .message = UISystemMessage::AppAction,
                                                 .appAction = action.action,
                                                 .appActionValue = action.value,
                                                 .appActionIx = ix});
    }
    return choices;
}

void MidiConfigViewModel::SetMessageCatalog(std::vector<UISystemMessageChoice> choices) {
    messageCatalog_ = std::move(choices);
}

void MidiConfigViewModel::SetAnalogActionCatalog(std::vector<UISystemMessageChoice> choices) {
    analogActionCatalog_ = std::move(choices);
}

void MidiConfigViewModel::SetLayouts(std::vector<ControllerWizardDescriptor> layouts) {
    layouts_ = std::move(layouts);
}

const std::vector<ControllerWizardDescriptor>& MidiConfigViewModel::Layouts() const {
    if (!layouts_.empty()) {
        return layouts_;
    }
    static const std::vector<ControllerWizardDescriptor> defaultLayouts =
        MakeControllerWizardRegistry(MidiAppCatalog{});
    return defaultLayouts;
}

namespace {

std::string DeviceLabel(const MidiEndpointRef& ref, MidiEndpointStatus status) {
    // Present device name when online; stored ref name (falling back to
    // identifier) + " (offline)" when configured-but-absent; "(none)" when
    // unconfigured. Online labels come from the stored ref's own `name`
    // field: MidiConnectionManager's reconcile pass keeps it fresh via
    // UpdateInputRef/UpdateOutputRef whenever a live device is matched (see
    // MidiReconcile.hpp), so `ref.name` already IS the present device name
    // once status is Online.
    if (status == MidiEndpointStatus::Unconfigured) {
        return "(none)";
    }
    std::string stored = !ref.name.empty() ? ref.name : ref.identifier;
    if (stored.empty()) {
        stored = "(unknown device)";
    }
    if (status == MidiEndpointStatus::Offline) {
        return stored + " (offline)";
    }
    return stored;
}


std::vector<MidiConfigSection> SectionsForKind(MidiProfileKind kind) {
    const MidiKindSupport support = KindSupport(kind);
    std::vector<MidiConfigSection> sections;
    if (support.encoders) {
        sections.push_back(MidiConfigSection::Encoders);
    }
    if (support.systemMessages) {
        sections.push_back(MidiConfigSection::SystemMessages);
    }
    if (support.analogs) {
        sections.push_back(MidiConfigSection::Analogs);
    }
    return sections;
}

// Whether `slot`'s config is still exactly what its stored wizard id would
// generate today: false with no stored wizard id, an id that resolves to no
// known descriptor, or a descriptor whose freshly generated profile
// serializes differently from the slot's current config. Regenerates the
// descriptor's profile into a scratch slot via the one shared install path
// (runtime_ui::ControllersLayout::InstallDescriptorProfile) and compares
// serialized JSON rather than adding struct equality, so this stays in sync
// with whatever ToJSON already treats as significant.
bool SlotMatchesWizardProfile(const MidiControllerSlot& slot,
                              const std::vector<ControllerWizardDescriptor>& layouts) {
    if (!slot.wizardId.has_value()) {
        return false;
    }
    const auto descriptorIt = std::find_if(layouts.begin(), layouts.end(),
                                           [&](const ControllerWizardDescriptor& descriptor) {
                                               return descriptor.id == *slot.wizardId;
                                           });
    if (descriptorIt == layouts.end()) {
        return false;
    }

    MidiControllerSlot generated = slot;
    if (!runtime_ui::ControllersLayout::InstallDescriptorProfile(layouts, *descriptorIt, generated, nullptr)) {
        return false;
    }

    JsonArena arena(256 * 1024);
    char* existingDump = ToJSON(arena, slot.config).Dumps(JSON_ENCODE_ANY);
    if (existingDump == nullptr) {
        return false;
    }
    const std::string existingJson(existingDump);
    std::free(existingDump);

    char* generatedDump = ToJSON(arena, generated.config).Dumps(JSON_ENCODE_ANY);
    if (generatedDump == nullptr) {
        return false;
    }
    const std::string generatedJson(generatedDump);
    std::free(generatedDump);

    return existingJson == generatedJson;
}

std::string DescribeMessage(const MessageIn& message) {
    std::ostringstream oss;
    switch (message.type) {
        case MessageIn::Type::ParamIncDec:
            oss << "param inc/dec slot " << message.slotIx << " pos " << message.position << " delta "
                << message.delta;
            break;
        case MessageIn::Type::ParamSetAbsolute:
            oss << "param set absolute slot " << message.slotIx << " pos " << message.position << " value "
                << message.value;
            break;
        case MessageIn::Type::ParamPush:
            oss << "param push slot " << message.slotIx << " pos " << message.position;
            break;
        case MessageIn::Type::ToggleReset:
            oss << (message.hasBoolValue ? (message.boolValue ? "reset on" : "reset off") : "toggle reset");
            break;
        case MessageIn::Type::ToggleRandom:
            oss << (message.hasBoolValue ? (message.boolValue ? "random on" : "random off") : "toggle random");
            break;
        case MessageIn::Type::ToggleRandomMod:
            oss << (message.hasBoolValue ? (message.boolValue ? "random-mod on" : "random-mod off")
                                         : "toggle random-mod");
            break;
        case MessageIn::Type::ToggleGestureSelect:
            oss << "toggle gesture " << message.gestureIx;
            break;
        case MessageIn::Type::SetGestureSelect:
            oss << "set gesture " << message.gestureIx << " " << (message.boolValue ? "on" : "off");
            break;
        case MessageIn::Type::SelectParamBank:
            oss << "select bank " << message.bankIx << " (slot " << message.slotIx << ")";
            break;
        case MessageIn::Type::NextParamBank:
            oss << "next bank (slot " << message.slotIx << ")";
            break;
        case MessageIn::Type::PrevParamBank:
            oss << "previous bank (slot " << message.slotIx << ")";
            break;
        case MessageIn::Type::Start:
            oss << "start";
            break;
        case MessageIn::Type::Continue:
            oss << "continue";
            break;
        case MessageIn::Type::Stop:
            oss << "stop";
            break;
        case MessageIn::Type::Clock:
            oss << "clock";
            break;
        case MessageIn::Type::SetGestureValue:
            oss << "set gesture " << message.gestureIx << " value " << message.value;
            break;
        case MessageIn::Type::SceneSelect:
            oss << "scene select " << message.sceneIx;
            break;
        case MessageIn::Type::SetSceneBlend:
            oss << "scene blend " << message.value;
            break;
        case MessageIn::Type::GridPress:
            oss << "grid press slot " << message.gridSlotIx << " (" << message.gridX << "," << message.gridY
                << ") velocity " << static_cast<int>(message.velocity);
            break;
        case MessageIn::Type::GridRelease:
            oss << "grid release slot " << message.gridSlotIx << " (" << message.gridX << "," << message.gridY
                << ")";
            break;
        case MessageIn::Type::GridPressureChange:
            oss << "grid pressure slot " << message.gridSlotIx << " (" << message.gridX << "," << message.gridY
                << ") pressure " << static_cast<int>(message.velocity);
            break;
        case MessageIn::Type::SelectGrid:
            oss << "select grid " << message.gridIx << " (slot " << message.gridSlotIx << ")";
            break;
        case MessageIn::Type::ParamSetAbsoluteOnBank:
            break;
        case MessageIn::Type::AppAction:
            oss << "app action " << message.appActionIx;
            break;
        case MessageIn::Type::HoldDrill:
            oss << (message.boolValue ? "hold drill on" : "hold drill off");
            break;
    }
    return oss.str();
}

std::string EncoderTurnLabel(const EncoderMidiMapping& mapping) {
    std::ostringstream oss;
    oss << "turn ch" << static_cast<int>(mapping.control.channel) << " cc" << static_cast<int>(mapping.control.cc)
        << " -> slot " << mapping.slotIx << " pos " << mapping.position;
    return oss.str();
}

std::string EncoderPushLabel(const EncoderMidiMapping& mapping) {
    std::ostringstream oss;
    oss << "push ch" << static_cast<int>(mapping.control.channel) << " cc" << static_cast<int>(mapping.control.cc)
        << " -> slot " << mapping.slotIx << " pos " << mapping.position;
    return oss.str();
}

std::string EncoderModeLabel(EncoderMode mode) {
    switch (mode) {
    case EncoderMode::Signed7Bit:
        return "encoder mode: signed 7-bit";
    case EncoderMode::DirectionOnly:
        return "encoder mode: direction only";
    case EncoderMode::Absolute:
        return "encoder mode: absolute";
    }
    return "encoder mode";
}

std::string TurnStepLabel(float step) {
    std::ostringstream oss;
    oss << "turn step (relative modes only): " << step;
    return oss.str();
}

std::string GestureLabel(const AnalogMidiMapping& mapping) {
    std::ostringstream oss;
    oss << "gesture ch" << static_cast<int>(mapping.control.channel) << " cc" << static_cast<int>(mapping.control.cc)
        << " -> gesture " << mapping.gestureIx;
    return oss.str();
}

std::string AppActionLabel(const AnalogAppActionMapping& mapping) {
    std::ostringstream oss;
    oss << "app action ch" << static_cast<int>(mapping.control.channel) << " cc"
        << static_cast<int>(mapping.control.cc) << " -> " << mapping.appAction;
    return oss.str();
}

std::string SceneBlendLabel(const std::optional<MidiControlAddress>& address) {
    // Issue #11: this row must read as clearly and distinctly "Scene blend"
    // -- not just another gesture -- since the renderer visually separates
    // it (RowGroup::AnalogSceneBlend, a divider + caption) from the
    // AnalogGesture rows above it.
    if (!address.has_value()) {
        return "Scene blend (unassigned)";
    }
    std::ostringstream oss;
    oss << "Scene blend  ch" << static_cast<int>(address->channel) << " cc" << static_cast<int>(address->cc);
    return oss.str();
}

std::string SystemMessageAddressLabel(const MidiControllerSystemMessageAssociation& association,
                                      MidiProfileKind kind) {
    std::ostringstream oss;
    if (kind == MidiProfileKind::Launchpad && association.launchpadPosition.has_value()) {
        oss << "pad (" << association.launchpadPosition->x << "," << association.launchpadPosition->y << ")";
    } else if (kind == MidiProfileKind::WrldBldr && association.wrldBldrPosition.has_value()) {
        // control->channel is authoritative for the channel (see the Channel /
        // X/Y edit cases in ApplyMappingEdit); fall back to the position's own
        // channel only when there is no control address.
        const int channel = association.control.has_value() ? static_cast<int>(association.control->channel)
                                                            : static_cast<int>(association.wrldBldrPosition->channel);
        oss << "pos ch" << channel << " (" << static_cast<int>(association.wrldBldrPosition->x) << ","
            << static_cast<int>(association.wrldBldrPosition->y) << ")";
    } else if (kind == MidiProfileKind::MfTwister && association.control.has_value()) {
        // Twister's sole address is the logical side button (cc -
        // 8); the fixed channel 3 is shown read-only alongside it.
        const int button = static_cast<int>(association.control->cc) - 8;
        oss << "ch" << static_cast<int>(association.control->channel) << " btn" << button;
    } else if (association.control.has_value()) {
        oss << "ch" << static_cast<int>(association.control->channel) << " cc"
            << static_cast<int>(association.control->cc);
    } else {
        oss << "(no address)";
    }
    return oss.str();
}

std::string SystemMessageLabel(const MidiControllerSystemMessageAssociation& association, MidiProfileKind kind) {
    std::ostringstream oss;
    oss << SystemMessageAddressLabel(association, kind) << " -> press: " << DescribeMessage(association.press);
    if (association.press.type == MessageIn::Type::AppAction) {
        oss << " " << association.appAction << " " << association.appActionValue;
    }
    if (association.release.has_value()) {
        oss << ", release: " << DescribeMessage(*association.release);
    }
    return oss.str();
}

// --- Open section presentation -----------------------------------------------

using PresentationRow = detail::PresentationRow;
using SectionPresentation = detail::SectionPresentation;
using EncoderModeRow = detail::EncoderModeRow;
using EncoderStepRow = detail::EncoderStepRow;
using AnalogSceneBlendRow = detail::AnalogSceneBlendRow;
using RowKind = MidiMappingRowVM::Kind;
using RowGroup = MidiMappingRowVM::RowGroup;

} // namespace

void MidiConfigViewModel::Rebuild(const MidiInstrumentConfig& instrument, const MidiConnectionState& connection) {
    instrument_ = instrument;
    connection_ = connection;

    controllers_.clear();
    controllers_.reserve(instrument_.controllers.size());

    for (std::size_t ix = 0; ix < instrument_.controllers.size(); ++ix) {
        const MidiControllerSlot& slot = instrument_.controllers[ix];
        MidiEndpointConnection inputConnection;
        MidiEndpointConnection outputConnection;
        if (ix < connection_.controllers.size()) {
            inputConnection = connection_.controllers[ix].input;
            outputConnection = connection_.controllers[ix].output;
        }

        MidiControllerRowVM row;
        row.name = slot.name;
        row.kind = slot.kind;
        row.disposition = slot.disposition;
        row.hasResolvedWizard = slot.wizardId.has_value() &&
            std::any_of(Layouts().begin(), Layouts().end(),
                        [&](const ControllerWizardDescriptor& descriptor) {
                            return descriptor.id == *slot.wizardId;
                        });
        row.matchesWizardProfile = SlotMatchesWizardProfile(slot, Layouts());
        row.wizardId = slot.wizardId;
        row.hasCompleteEndpointPair = slot.input.IsConfigured() && slot.output.IsConfigured();
        row.inputStatus = inputConnection.status;
        row.outputStatus = outputConnection.status;
        row.inputDeviceLabel = DeviceLabel(slot.input, inputConnection.status);
        row.outputDeviceLabel = DeviceLabel(slot.output, outputConnection.status);
        row.storedInput = slot.input;
        row.storedOutput = slot.output;
        row.sections = SectionsForKind(slot.kind);

        // Ensure expand state exists for this controller (first appearance
        // starts fully collapsed); already-known names keep whatever the UI
        // last set, surviving this Rebuild().
        ExpandState& state = StateFor(slot.name);
        for (MidiConfigSection section : row.sections) {
            state.sections.try_emplace(section, false);
        }
        row.configExpanded = state.configExpanded;

        controllers_.push_back(std::move(row));
    }

    // Erase expand-state entries for controller names no longer
    // present, for the same reason presentation entries are erased below --
    // a same-name readd should start "fully collapsed" like any other
    // first-ever appearance (this class's own doc comment above, and
    // Rebuild()'s "first appearance starts fully collapsed" comment just
    // above), not silently inherit whatever expand/section state the OLD,
    // now-gone controller of that name was left in. Does NOT affect
    // ToggleStateKeyedByNameSurvivesReordering (which keeps the same set of
    // names present, just reordered -- no name here is ever missing from
    // `instrument_.controllers`, so ExpandStateNamesStillPresent below never
    // erases anything that test relies on).
    std::vector<std::string> expandStateNamesToErase;
    for (const auto& [name, state] : expandState_) {
        (void)state;
        if (instrument_.FindController(name) == nullptr) {
            expandStateNamesToErase.push_back(name);
        }
    }
    for (const std::string& name : expandStateNamesToErase) {
        expandState_.erase(name);
    }

    // Existing presentation entries are kept verbatim while their controller
    // still exists. Open rows are the UI-level representation; Rebuild() must
    // not sort, regroup, drop, or append rows from persisted truth.
    //
    // Entries for controllers no longer present in this
    // instrument are ERASED (not just cleared to empty rows) -- a stale
    // empty-but-present map entry would make a LATER PresentationFor() call
    // for a same-named controller that reappears (remove, then re-add with
    // the same name -- e.g. undo/redo, or a deliberate re-add in one
    // session) find that stale entry via presentations_.find() and treat it
    // as "already built" (a presentation with zero rows), rather than
    // lazily building a fresh reconstruction from the reappeared
    // controller's actual config. Erasing lets PresentationFor's
    // find()-miss -> BuildFreshPresentation() path run again for that name,
    // so re-expanding presents a fresh minimal reconstruction (a same-name
    // readd is, presentation-wise, exactly like a fresh expand).
    // Collects orphaned keys first and erases in a second pass, since
    // erasing a std::map entry while range-for is iterating that SAME
    // element is undefined behavior; this stays correct without relying on
    // erase-during-iteration guarantees.
    std::vector<PresentationKey> orphanedKeys;
    for (const auto& [presentationKey, presentation] : presentations_) {
        (void)presentation;
        const auto& [name, section] = presentationKey;
        (void)section;
        const MidiControllerSlot* slot = instrument_.FindController(name);
        if (slot == nullptr) {
            orphanedKeys.push_back(presentationKey);
        }
    }
    for (const PresentationKey& key : orphanedKeys) {
        presentations_.erase(key);
    }
}

MidiConfigViewModel::ExpandState& MidiConfigViewModel::StateFor(const std::string& name) {
    return expandState_[name];
}

const MidiConfigViewModel::ExpandState* MidiConfigViewModel::StateForConst(const std::string& name) const {
    auto it = expandState_.find(name);
    return it != expandState_.end() ? &it->second : nullptr;
}

// expandState_ and presentations_ are two caches of one concept -- per-row UI
// state keyed by a controller name the user can change -- so a rename has to
// move both together or the row's cached state gets orphaned (and discarded)
// by Rebuild()'s name-based sweeps the next time it runs.
void MidiConfigViewModel::NoteControllerRenamed(const std::string& from, const std::string& to) {
    auto expandIt = expandState_.find(from);
    if (expandIt != expandState_.end()) {
        expandState_[to] = std::move(expandIt->second);
        expandState_.erase(expandIt);
    }

    // Collects the affected keys first and rewrites them in a second pass,
    // since erasing a std::map entry while range-for is iterating that SAME
    // element is undefined behavior.
    std::vector<PresentationKey> renamedKeys;
    for (const auto& [presentationKey, presentation] : presentations_) {
        (void)presentation;
        if (presentationKey.first == from) {
            renamedKeys.push_back(presentationKey);
        }
    }
    for (const PresentationKey& key : renamedKeys) {
        presentations_[PresentationKey{to, key.second}] = std::move(presentations_[key]);
        presentations_.erase(key);
    }
}

void MidiConfigViewModel::ToggleConfig(std::size_t controllerIx) {
    if (controllerIx >= controllers_.size()) {
        return;
    }
    ExpandState& state = StateFor(controllers_[controllerIx].name);
    state.configExpanded = !state.configExpanded;
    controllers_[controllerIx].configExpanded = state.configExpanded;
}

void MidiConfigViewModel::ToggleSection(std::size_t controllerIx, MidiConfigSection section) {
    if (controllerIx >= controllers_.size()) {
        return;
    }
    const std::string& name = controllers_[controllerIx].name;
    ExpandState& state = StateFor(name);
    bool& expanded = state.sections[section];
    expanded = !expanded;
    if (!expanded) {
        // Discarded on expanded->collapsed -- the next expand rebuilds
        // a fresh minimal reconstruction ("collapsing and
        // re-expanding present the fresh minimal reconstruction").
        DiscardPresentation(name, section);
    } else {
        // Opening a section starts the edit session immediately. Otherwise a
        // click on "+" before the renderer's first SectionRows() read would
        // leave no presentation to append into, and the next lazy read would
        // coalesce the just-added rows from scratch.
        (void)PresentationFor(controllerIx, section);
    }
}

bool MidiConfigViewModel::SectionExpanded(std::size_t controllerIx, MidiConfigSection section) const {
    if (controllerIx >= controllers_.size()) {
        return false;
    }
    const ExpandState* state = StateForConst(controllers_[controllerIx].name);
    if (state == nullptr) {
        return false;
    }
    auto it = state->sections.find(section);
    return it != state->sections.end() && it->second;
}

namespace {

// editableFields for an Individual SystemMessages row, per kind --
// factored out of the old SectionRows() SystemMessages case so
// BuildFreshPresentation/BuildSectionRows share the exact same table.
std::vector<Field> SystemRowEditableFields(MidiProfileKind kind,
                                           const MidiControllerSystemMessageAssociation& association) {
    std::vector<Field> fields;
    for (SystemAddressField addressField : SystemAddressSchema(kind)) {
        switch (addressField) {
            case SystemAddressField::AddressType:
                fields.push_back(Field::AddressType);
                break;
            case SystemAddressField::Channel:
                fields.push_back(Field::Channel);
                break;
            case SystemAddressField::WrldBldrX:
                fields.push_back(Field::WrldBldrX);
                break;
            case SystemAddressField::WrldBldrY:
                fields.push_back(Field::WrldBldrY);
                break;
            case SystemAddressField::LaunchpadX:
                fields.push_back(Field::LaunchpadX);
                break;
            case SystemAddressField::LaunchpadY:
                fields.push_back(Field::LaunchpadY);
                break;
            case SystemAddressField::Button:
                fields.push_back(Field::Button);
                break;
            case SystemAddressField::Cc:
                fields.push_back(Field::Cc);
                break;
        }
    }
    fields.push_back(Field::MessageKind);
    if (UISystemMessageHasArg(UISystemMessageForAssociation(association))) {
        fields.push_back(Field::MessageArg);
    }
    return fields;
}

// editableFields for a Block row, per its form.
// See MidiMappingRowVM::Field's Block* doc comment.
std::vector<Field> EncoderBlockEditableFields() {
    return {Field::Channel, Field::BlockStartCc, Field::BlockEndCc, Field::SlotIx, Field::BlockStartPos};
}
std::vector<Field> AnalogBlockEditableFields() {
    return {Field::Channel, Field::BlockStartCc, Field::BlockEndCc, Field::BlockStartArg};
}
std::vector<Field> SystemBlockEditableFields(const SystemBlock& block) {
    std::vector<Field> fields = {Field::BlockMessageType};
    const std::vector<SystemAddressField> addressSchema = SystemAddressSchema(block.kind);
    if (std::find(addressSchema.begin(), addressSchema.end(), SystemAddressField::AddressType) !=
        addressSchema.end()) {
        fields.push_back(Field::AddressType);
    }
    if (block.kind == MidiProfileKind::WrldBldr) {
        fields.push_back(Field::Channel);
    }
    if (block.kind == MidiProfileKind::WrldBldr || block.kind == MidiProfileKind::Launchpad) {
        fields.insert(fields.end(), {Field::BlockStartX, Field::BlockStartY, Field::BlockEndX, Field::BlockEndY,
                                     Field::BlockRowMajor});
    } else {
        fields.insert(fields.end(), {Field::Channel, Field::BlockStartCc, Field::BlockEndCc});
    }
    fields.push_back(Field::BlockStartArg);
    if (block.message == BlockableMessage::BankSelect) {
        fields.push_back(Field::BlockBankSlotIx);
    }
    fields.push_back(Field::BlockOutputFeedback);
    return fields;
}

std::vector<Field> GridButtonEditableFields(const GridButton& button) {
    std::vector<Field> fields;
    if (button.kind == MidiProfileKind::WrldBldr) {
        fields.push_back(Field::Channel);
    }
    fields.insert(fields.end(), {Field::GridSlotIx, Field::GridXMin, Field::GridYMin});
    return fields;
}

std::vector<Field> GridBlockEditableFields(const GridBlock& block) {
    std::vector<Field> fields;
    if (block.kind == MidiProfileKind::WrldBldr) {
        fields.push_back(Field::Channel);
    }
    fields.insert(fields.end(), {Field::GridSlotIx, Field::GridXMin, Field::GridXMax,
                                 Field::GridYMin, Field::GridYMax});
    return fields;
}

std::string EncoderBlockLabel(const EncoderBlock& block) {
    std::ostringstream oss;
    oss << (block.isPush ? "push block ch" : "turn block ch") << static_cast<int>(block.channel) << " cc"
        << static_cast<int>(block.startCc) << ".." << static_cast<int>(block.endCc) << " -> slot " << block.slotIx
        << " pos " << block.startPosition << "..";
    return oss.str();
}

std::string AnalogBlockLabel(const AnalogBlock& block) {
    std::ostringstream oss;
    oss << "gesture block ch" << static_cast<int>(block.channel) << " cc" << static_cast<int>(block.startCc) << ".."
        << static_cast<int>(block.endCc) << " -> gesture " << block.startGestureIx << "..";
    return oss.str();
}

const char* BlockableMessageName(BlockableMessage message) {
    switch (message) {
        case BlockableMessage::SceneSelect:
            return "scene select";
        case BlockableMessage::BankSelect:
            return "bank select";
        case BlockableMessage::GestureSelect:
            return "gesture select";
    }
    return "";
}

std::string SystemBlockLabel(const SystemBlock& block) {
    std::ostringstream oss;
    oss << BlockableMessageName(block.message) << " block ";
    if (block.kind == MidiProfileKind::WrldBldr || block.kind == MidiProfileKind::Launchpad) {
        oss << "(" << block.startX << "," << block.startY << ")..(" << block.endX << "," << block.endY << ")";
    } else {
        oss << "ch" << static_cast<int>(block.channel) << " cc" << static_cast<int>(block.startCc) << ".."
            << static_cast<int>(block.endCc);
    }
    oss << " -> arg " << block.startArg << "..";
    return oss.str();
}

std::string GridButtonLabel(const GridButton& button) {
    std::ostringstream oss;
    oss << "Grid Button slot " << button.gridSlotIx << " (" << button.x << "," << button.y << ")";
    return oss.str();
}

std::string GridBlockLabel(const GridBlock& block) {
    std::ostringstream oss;
    oss << "Grid Block slot " << block.gridSlotIx << " [" << block.startX << "," << block.endX
        << ") x [" << block.startY << "," << block.endY << ")";
    return oss.str();
}

}  // namespace

namespace {

// Builds a FRESH presentation for one section from scratch: the config-level
// rows (Encoders' EncoderMode/TurnStep, Analogs' SceneBlend) as individual
// ConfigLevel PresentationRows, and the blockable groups (encoder turns/
// pushes, analog gestures, system messages) reconstructed via
// MidiConfigBlocks.hpp's Reconstruct* (built at the collapsed ->
// expanded transition). This is also what a collapse+re-expand cycle
// produces (DiscardPresentation followed by the next PresentationFor()
// call), where re-expanding presents a fresh minimal reconstruction.
SectionPresentation BuildFreshPresentation(const MidiControllerProfileConfig& config, MidiProfileKind kind,
                                           MidiConfigSection section) {
    SectionPresentation presentation;

    switch (section) {
        case MidiConfigSection::Encoders: {
            MidiControllerProfileConfig scratch = config;
            NormalizeMidiProfileConfig(scratch, kind);
            if (!scratch.encoderInput.has_value()) {
                return presentation;
            }
            const auto& encoderInput = *scratch.encoderInput;
            for (bool isPush : {false, true}) {
                const std::vector<EncoderMidiMapping>& mappings = isPush ? encoderInput.pushes : encoderInput.turns;
                const RowGroup group = isPush ? RowGroup::EncoderPush : RowGroup::EncoderTurn;
                for (const ReconstructedEncoderRow& reconstructed : ReconstructEncoderBlocks(mappings, isPush)) {
                    PresentationRow row;
                    row.group = group;
                    if (reconstructed.isBlock) {
                        row.kind = RowKind::Block;
                        row.block = reconstructed.block;
                    } else {
                        row.kind = RowKind::Individual;
                        const EncoderMidiMapping& mapping = mappings[reconstructed.indices.front()];
                        row.data = mapping;
                    }
                    presentation.rows.push_back(std::move(row));
                }
            }
            {
                PresentationRow row;
                row.kind = RowKind::ConfigLevel;
                row.group = RowGroup::EncoderMode;
                row.data = EncoderModeRow{.mode = encoderInput.mode};
                presentation.rows.push_back(std::move(row));
            }
            {
                PresentationRow row;
                row.kind = RowKind::ConfigLevel;
                row.group = RowGroup::EncoderStep;
                row.data = EncoderStepRow{.turnStep = encoderInput.turnStep};
                presentation.rows.push_back(std::move(row));
            }
            break;
        }
        case MidiConfigSection::Analogs: {
            MidiControllerProfileConfig scratch = config;
            NormalizeMidiProfileConfig(scratch, kind);
            if (!scratch.analogInput.has_value()) {
                return presentation;
            }
            const auto& analogInput = *scratch.analogInput;
            for (const ReconstructedAnalogRow& reconstructed : ReconstructAnalogBlocks(analogInput.gestures)) {
                PresentationRow row;
                row.group = RowGroup::AnalogGesture;
                if (reconstructed.isBlock) {
                    row.kind = RowKind::Block;
                    row.block = reconstructed.block;
                } else {
                    row.kind = RowKind::Individual;
                    const AnalogMidiMapping& mapping = analogInput.gestures[reconstructed.indices.front()];
                    row.data = mapping;
                }
                presentation.rows.push_back(std::move(row));
            }
            for (const AnalogAppActionMapping& mapping : analogInput.appActions) {
                PresentationRow row;
                row.kind = RowKind::Individual;
                row.group = RowGroup::AnalogAppAction;
                row.data = mapping;
                presentation.rows.push_back(std::move(row));
            }
            {
                PresentationRow row;
                row.kind = RowKind::ConfigLevel;
                row.group = RowGroup::AnalogSceneBlend;
                row.data = AnalogSceneBlendRow{.sceneBlend = analogInput.sceneBlend};
                presentation.rows.push_back(std::move(row));
            }
            break;
        }
        case MidiConfigSection::SystemMessages: {
            const std::vector<PolyphonicPressureMapping> pressureMappings =
                config.pressureInput.has_value() ? config.pressureInput->mappings
                                                 : std::vector<PolyphonicPressureMapping>{};
            GridMappingReconstruction grids =
                ReconstructGridMappings(config.systemMessages, pressureMappings, kind);
            presentation.hiddenPressureMappings = std::move(grids.orphanPressureMappings);

            MidiControllerProfileConfig scratch;
            scratch.systemMessages = std::move(grids.remainingSystemMessages);
            NormalizeMidiProfileConfig(scratch, kind);
            const std::vector<MidiControllerSystemMessageAssociation>& sorted = scratch.systemMessages;
            for (const ReconstructedSystemRow& reconstructed : ReconstructSystemBlocks(sorted, kind)) {
                PresentationRow row;
                row.group = RowGroup::System;
                if (reconstructed.isBlock) {
                    row.kind = RowKind::Block;
                    row.block = reconstructed.block;
                } else {
                    row.kind = RowKind::Individual;
                    const std::size_t sortedIx = reconstructed.indices.front();
                    row.data = sorted[sortedIx];
                }
                presentation.rows.push_back(std::move(row));
            }
            for (const ReconstructedGridRow& reconstructed : grids.rows) {
                PresentationRow row;
                row.group = RowGroup::Grid;
                if (reconstructed.isBlock) {
                    row.kind = RowKind::Block;
                    row.block = reconstructed.block;
                } else {
                    row.kind = RowKind::Individual;
                    row.data = reconstructed.button;
                }
                presentation.rows.push_back(std::move(row));
            }
            break;
        }
    }
    return presentation;
}

}  // namespace

detail::SectionPresentation& MidiConfigViewModel::PresentationFor(std::size_t controllerIx,
                                                                   MidiConfigSection section) const {
    const std::string& name = instrument_.controllers[controllerIx].name;
    const PresentationKey key{name, section};
    auto it = presentations_.find(key);
    if (it != presentations_.end()) {
        return it->second;
    }
    const MidiControllerSlot& slot = instrument_.controllers[controllerIx];
    auto [inserted, ok] = presentations_.emplace(key, BuildFreshPresentation(slot.config, slot.kind, section));
    (void)ok;
    return inserted->second;
}

void MidiConfigViewModel::DiscardPresentation(const std::string& name, MidiConfigSection section) {
    presentations_.erase(PresentationKey{name, section});
}

namespace {

// Where a new row of `group` should land: immediately after the last
// EXISTING row of that group, if any; otherwise immediately before the
// first row of the nearest LATER group in RowGroup's declaration order
// (EncoderTurn < EncoderPush < EncoderMode < EncoderStep < AnalogGesture <
// AnalogAppAction < AnalogSceneBlend < System, which is exactly section
// display order); if
// neither exists (no rows of this group AND no later-group rows either),
// the very end of the presentation. Shared by AddSingle/AddBlock
// ("+"/"+B" append presentation rows at the end of their group) so both
// paths agree on where "end of group" means -- a
// group with zero existing rows still has a well-defined "end" (immediately
// before mode/step/scene-blend), not the tail of the whole section.
std::size_t InsertionIndexForGroup(const SectionPresentation& presentation, RowGroup group) {
    std::size_t lastOfGroup = presentation.rows.size();
    std::size_t firstOfLaterGroup = presentation.rows.size();
    for (std::size_t ix = 0; ix < presentation.rows.size(); ++ix) {
        if (presentation.rows[ix].group == group) {
            lastOfGroup = ix + 1;
        } else if (presentation.rows[ix].group > group && firstOfLaterGroup == presentation.rows.size()) {
            firstOfLaterGroup = ix;
        }
    }
    if (lastOfGroup != presentation.rows.size()) {
        return lastOfGroup;
    }
    return firstOfLaterGroup;
}

// Appends a new Block row at the end of its group. The row itself is the
// open-session representation; persisted truth is rewritten from this row,
// and Rebuild() does not discover or reshape it from config storage.
template <typename BlockT>
void AppendBlockPresentationRow(SectionPresentation& presentation, RowGroup group, const BlockT& block) {
    const std::size_t insertAt = InsertionIndexForGroup(presentation, group);
    PresentationRow row;
    row.kind = RowKind::Block;
    row.group = group;
    row.block = block;
    presentation.rows.insert(presentation.rows.begin() + static_cast<std::ptrdiff_t>(insertAt), std::move(row));
}

}  // namespace

std::vector<MidiMappingRowVM> MidiConfigViewModel::BuildSectionRows(std::size_t controllerIx,
                                                                     MidiConfigSection section) const {
    const MidiControllerSlot& slot = instrument_.controllers[controllerIx];
    const SectionPresentation& presentation = PresentationFor(controllerIx, section);

    std::vector<MidiMappingRowVM> rows;
    rows.reserve(presentation.rows.size());
    for (const PresentationRow& presentationRow : presentation.rows) {
        MidiMappingRowVM row;
        row.kind = presentationRow.kind;
        row.group = presentationRow.group;
        row.deletable = presentationRow.kind != RowKind::ConfigLevel;

        if (presentationRow.kind == RowKind::ConfigLevel) {
            if (presentationRow.group == RowGroup::EncoderMode) {
                const auto* data = std::get_if<EncoderModeRow>(&presentationRow.data);
                row.editableFields = {Field::EncoderMode};
                row.label = EncoderModeLabel(data != nullptr ? data->mode : EncoderMode::Signed7Bit);
            } else if (presentationRow.group == RowGroup::EncoderStep) {
                const auto* data = std::get_if<EncoderStepRow>(&presentationRow.data);
                row.editableFields = {Field::TurnStep};
                row.label = TurnStepLabel(data != nullptr ? data->turnStep : 1.0f);
            } else if (presentationRow.group == RowGroup::AnalogSceneBlend) {
                const auto* data = std::get_if<AnalogSceneBlendRow>(&presentationRow.data);
                row.editableFields = {Field::SceneBlend};
                row.label = SceneBlendLabel(data != nullptr ? data->sceneBlend : std::optional<MidiControlAddress>{});
            }
        } else if (presentationRow.kind == RowKind::Block) {
            if (const auto* encoderBlock = std::get_if<EncoderBlock>(&presentationRow.block)) {
                row.editableFields = EncoderBlockEditableFields();
                if (encoderBlock->isPush) {
                    row.editableFields.insert(row.editableFields.begin(), Field::AddressType);
                }
                row.label = EncoderBlockLabel(*encoderBlock);
            } else if (const auto* analogBlock = std::get_if<AnalogBlock>(&presentationRow.block)) {
                row.editableFields = AnalogBlockEditableFields();
                row.label = AnalogBlockLabel(*analogBlock);
            } else if (const auto* systemBlock = std::get_if<SystemBlock>(&presentationRow.block)) {
                row.editableFields = SystemBlockEditableFields(*systemBlock);
                row.label = SystemBlockLabel(*systemBlock);
            } else if (const auto* gridBlock = std::get_if<GridBlock>(&presentationRow.block)) {
                row.editableFields = GridBlockEditableFields(*gridBlock);
                row.label = GridBlockLabel(*gridBlock);
            }
        } else {
            if (const auto* mapping = std::get_if<EncoderMidiMapping>(&presentationRow.data)) {
                row.editableFields = {Field::Channel, Field::Cc, Field::SlotIx, Field::Position};
                if (presentationRow.group == RowGroup::EncoderPush) {
                    row.editableFields.insert(row.editableFields.begin(), Field::AddressType);
                }
                row.label = presentationRow.group == RowGroup::EncoderPush ? EncoderPushLabel(*mapping)
                                                                            : EncoderTurnLabel(*mapping);
            } else if (const auto* mapping = std::get_if<AnalogMidiMapping>(&presentationRow.data)) {
                row.editableFields = {Field::Channel, Field::Cc, Field::GestureIx};
                row.label = GestureLabel(*mapping);
            } else if (const auto* mapping = std::get_if<AnalogAppActionMapping>(&presentationRow.data)) {
                row.editableFields = {Field::Channel, Field::Cc, Field::AppAction};
                row.label = AppActionLabel(*mapping);
            } else if (const auto* association = std::get_if<MidiControllerSystemMessageAssociation>(&presentationRow.data)) {
                row.editableFields = SystemRowEditableFields(slot.kind, *association);
                row.label = SystemMessageLabel(*association, slot.kind);
            } else if (const auto* gridButton = std::get_if<GridButton>(&presentationRow.data)) {
                row.editableFields = GridButtonEditableFields(*gridButton);
                row.label = GridButtonLabel(*gridButton);
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<MidiMappingRowVM> MidiConfigViewModel::SectionRows(std::size_t controllerIx,
                                                                MidiConfigSection section) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return {};
    }
    return BuildSectionRows(controllerIx, section);
}

namespace {

// Reads `field`'s current value off a block struct (shared by
// RowFieldValue's Block case and, indirectly, the label builders above via
// their own direct field access). Returns false for a field not applicable
// to this block's variant/form (caller has already gated against
// editableFields, so this is a belt-and-suspenders internal consistency
// check, not a user-facing refusal path).
bool BlockFieldValue(const std::variant<std::monostate, EncoderBlock, AnalogBlock, SystemBlock, GridBlock>& block,
                    Field field,
                    double& out) {
    if (const auto* encoderBlock = std::get_if<EncoderBlock>(&block)) {
        switch (field) {
            case Field::AddressType:
                out = static_cast<double>(encoderBlock->controlType);
                return true;
            case Field::Channel:
                out = static_cast<double>(encoderBlock->channel);
                return true;
            case Field::BlockStartCc:
                out = static_cast<double>(encoderBlock->startCc);
                return true;
            case Field::BlockEndCc:
                out = static_cast<double>(encoderBlock->endCc);
                return true;
            case Field::SlotIx:
                out = static_cast<double>(encoderBlock->slotIx);
                return true;
            case Field::BlockStartPos:
                out = static_cast<double>(encoderBlock->startPosition);
                return true;
            default:
                return false;
        }
    }
    if (const auto* analogBlock = std::get_if<AnalogBlock>(&block)) {
        switch (field) {
            case Field::Channel:
                out = static_cast<double>(analogBlock->channel);
                return true;
            case Field::BlockStartCc:
                out = static_cast<double>(analogBlock->startCc);
                return true;
            case Field::BlockEndCc:
                out = static_cast<double>(analogBlock->endCc);
                return true;
            case Field::BlockStartArg:
                out = static_cast<double>(analogBlock->startGestureIx);
                return true;
            default:
                return false;
        }
    }
    if (const auto* systemBlock = std::get_if<SystemBlock>(&block)) {
        switch (field) {
            case Field::AddressType:
                out = static_cast<double>(systemBlock->controlType);
                return true;
            case Field::BlockMessageType:
                out = static_cast<double>(systemBlock->message);
                return true;
            case Field::Channel:
                out = static_cast<double>(systemBlock->channel);
                return true;
            case Field::BlockStartCc:
                out = static_cast<double>(systemBlock->startCc);
                return true;
            case Field::BlockEndCc:
                out = static_cast<double>(systemBlock->endCc);
                return true;
            case Field::BlockStartX:
                out = static_cast<double>(systemBlock->startX);
                return true;
            case Field::BlockStartY:
                out = static_cast<double>(systemBlock->startY);
                return true;
            case Field::BlockEndX:
                out = static_cast<double>(systemBlock->endX);
                return true;
            case Field::BlockEndY:
                out = static_cast<double>(systemBlock->endY);
                return true;
            case Field::BlockStartArg:
                out = static_cast<double>(systemBlock->startArg);
                return true;
            case Field::BlockBankSlotIx:
                out = static_cast<double>(systemBlock->bankSlotIx);
                return true;
            case Field::BlockRowMajor:
                out = systemBlock->rowMajor ? 1.0 : 0.0;
                return true;
            case Field::BlockOutputFeedback:
                out = systemBlock->outputFeedback ? 1.0 : 0.0;
                return true;
            default:
                return false;
        }
    }
    if (const auto* gridBlock = std::get_if<GridBlock>(&block)) {
        switch (field) {
            case Field::Channel:
                out = static_cast<double>(gridBlock->channel);
                return true;
            case Field::GridSlotIx:
                out = static_cast<double>(gridBlock->gridSlotIx);
                return true;
            case Field::GridXMin:
                out = static_cast<double>(gridBlock->startX);
                return true;
            case Field::GridXMax:
                out = static_cast<double>(gridBlock->endX);
                return true;
            case Field::GridYMin:
                out = static_cast<double>(gridBlock->startY);
                return true;
            case Field::GridYMax:
                out = static_cast<double>(gridBlock->endY);
                return true;
            default:
                return false;
        }
    }
    return false;
}

}  // namespace

bool MidiConfigViewModel::RowFieldValue(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx,
                                        MidiMappingRowVM::Field field, double& out) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return false;
    }
    // Same gate ApplyMappingEdit applies before touching anything: refuse a
    // field this row doesn't advertise. SectionRows() is the single source
    // of truth for row shape/editable fields, reused here so the two can
    // never drift.
    const std::vector<MidiMappingRowVM> rows = SectionRows(controllerIx, section);
    if (rowIx >= rows.size()) {
        return false;
    }
    const std::vector<MidiMappingRowVM::Field>& editable = rows[rowIx].editableFields;
    if (std::find(editable.begin(), editable.end(), field) == editable.end()) {
        return false;
    }
    if (field == Field::MessageKind || field == Field::BlockMessageType) {
        return false;
    }

    const SectionPresentation& presentation = PresentationFor(controllerIx, section);
    const PresentationRow& presentationRow = presentation.rows[rowIx];

    if (presentationRow.kind == RowKind::Block) {
        return BlockFieldValue(presentationRow.block, field, out);
    }
    if (presentationRow.kind == RowKind::ConfigLevel) {
        if (presentationRow.group == RowGroup::EncoderMode && field == Field::EncoderMode) {
            const auto* data = std::get_if<EncoderModeRow>(&presentationRow.data);
            if (data == nullptr) {
                return false;
            }
            const auto index = static_cast<std::size_t>(data->mode);
            if (index >= EncoderModeCatalog().size()) {
                return false;
            }
            out = static_cast<double>(index);
            return true;
        }
        if (presentationRow.group == RowGroup::EncoderStep && field == Field::TurnStep) {
            const auto* data = std::get_if<EncoderStepRow>(&presentationRow.data);
            if (data == nullptr) {
                return false;
            }
            out = static_cast<double>(data->turnStep);
            return true;
        }
        if (presentationRow.group == RowGroup::AnalogSceneBlend && field == Field::SceneBlend) {
            // An unassigned sceneBlend (a common,
            // valid state -- SlotValidForKind places no requirement on it,
            // see SceneBlendLabelReadsClearlyWhenAssignedAndUnassigned)
            // used to make THIS return false even though the row always
            // advertises SceneBlend as editable and ApplyMappingEdit
            // genuinely accepts assigning it (defaulting the address via
            // value_or(MidiControlAddress{})). Returning false here made
            // ControllersPage.hpp's renderer skip building an editor
            // entirely for this row once it started checking
            // RowFieldValue's return value, which would
            // have made an unassigned scene blend permanently unassignable
            // from the UI -- a real regression, and not what "visible but not
            // silently corruptible" aims at.
            // 0.0 (cc 0) matches ApplyMappingEdit's own default-construction
            // value for the same unassigned case, so a freshly-seeded
            // editor and a freshly-committed edit agree on what "unassigned,
            // about to be assigned" starts from.
            const auto* data = std::get_if<AnalogSceneBlendRow>(&presentationRow.data);
            out = data != nullptr && data->sceneBlend.has_value() ? static_cast<double>(data->sceneBlend->cc) : 0.0;
            return true;
        }
        return false;
    }

    if (const auto* mapping = std::get_if<EncoderMidiMapping>(&presentationRow.data)) {
        switch (field) {
            case Field::AddressType:
                out = static_cast<double>(mapping->control.type);
                return true;
            case Field::Channel:
                out = static_cast<double>(mapping->control.channel);
                return true;
            case Field::Cc:
                out = static_cast<double>(mapping->control.cc);
                return true;
            case Field::SlotIx:
                out = static_cast<double>(mapping->slotIx);
                return true;
            case Field::Position:
                out = static_cast<double>(mapping->position);
                return true;
            default:
                return false;
        }
    }
    if (const auto* mapping = std::get_if<AnalogMidiMapping>(&presentationRow.data)) {
        switch (field) {
            case Field::Channel:
                out = static_cast<double>(mapping->control.channel);
                return true;
            case Field::Cc:
                out = static_cast<double>(mapping->control.cc);
                return true;
            case Field::GestureIx:
                out = static_cast<double>(mapping->gestureIx);
                return true;
            default:
                return false;
        }
    }
    if (const auto* mapping = std::get_if<AnalogAppActionMapping>(&presentationRow.data)) {
        switch (field) {
            case Field::Channel:
                out = static_cast<double>(mapping->control.channel);
                return true;
            case Field::Cc:
                out = static_cast<double>(mapping->control.cc);
                return true;
            case Field::AppAction: {
                const auto& catalog = AnalogActionCatalog();
                out = 0.0;
                for (std::size_t ix = 0; ix < catalog.size(); ++ix) {
                    if (catalog[ix].appAction == mapping->appAction &&
                        catalog[ix].appActionValue == mapping->appActionValue) {
                        out = static_cast<double>(ix);
                        break;
                    }
                }
                return true;
            }
            default:
                return false;
        }
    }
    if (const auto* association = std::get_if<MidiControllerSystemMessageAssociation>(&presentationRow.data)) {
        switch (field) {
            case Field::AddressType:
                if (!association->control.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->control->type);
                return true;
            case Field::Channel:
                if (!association->control.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->control->channel);
                return true;
            case Field::Cc:
                if (!association->control.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->control->cc);
                return true;
            case Field::LaunchpadX:
                if (!association->launchpadPosition.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->launchpadPosition->x);
                return true;
            case Field::LaunchpadY:
                if (!association->launchpadPosition.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->launchpadPosition->y);
                return true;
            case Field::WrldBldrX:
                if (!association->wrldBldrPosition.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->wrldBldrPosition->x);
                return true;
            case Field::WrldBldrY:
                if (!association->wrldBldrPosition.has_value()) {
                    return false;
                }
                out = static_cast<double>(association->wrldBldrPosition->y);
                return true;
            case Field::Button:
                if (!association->control.has_value() || association->control->cc < 8 ||
                    association->control->cc > 13) {
                    return false;
                }
                out = static_cast<double>(association->control->cc - 8);
                return true;
            case Field::MessageArg: {
                if (!UISystemMessageHasArg(UISystemMessageForAssociation(*association))) {
                    return false;
                }
                out = static_cast<double>(AssociationPrimaryArg(*association));
                return true;
            }
            default:
                return false;
        }
    }
    if (const auto* button = std::get_if<GridButton>(&presentationRow.data)) {
        switch (field) {
            case Field::Channel:
                out = static_cast<double>(button->channel);
                return true;
            case Field::GridSlotIx:
                out = static_cast<double>(button->gridSlotIx);
                return true;
            case Field::GridXMin:
                out = static_cast<double>(button->x);
                return true;
            case Field::GridYMin:
                out = static_cast<double>(button->y);
                return true;
            default:
                return false;
        }
    }
    return false;
}

namespace {

// True when `value` is representable as a non-negative integer -- the
// baseline domain check for SlotIx/Position/GestureIx/bank & scene indices
// and catalog indices ("integral (value == floor(value)),
// within the field's domain ... at minimum non-negative").
//
// Also bounds `value` from above so every caller's later
// `static_cast<std::size_t>(value)` is well-defined: without this, a value
// like 1e300 passes isfinite/>=0/==floor but casting it to std::size_t is
// undefined behavior (the double is far outside std::size_t's range). Two
// bounds apply: 2^53 (kMaxSafeInteger), the largest integer every double
// value up to it represents exactly (beyond it, doubles start skipping
// integers, so "value == floor(value)" no longer guarantees `value` names a
// specific integer); and std::numeric_limits<std::size_t>::max() converted
// to double, in case size_t is narrower than 53 bits (e.g. a 32-bit
// size_t). Domain-specific caps (e.g. Cc's 0-127, WrldBldrX/Y's 0-7) still
// apply on top of this via IsIntegerInRange/other callers.
bool IsNonNegativeInteger(double value) {
    constexpr double kMaxSafeInteger = 9007199254740992.0;  // 2^53
    const double maxSizeT = static_cast<double>(std::numeric_limits<std::size_t>::max());
    const double upperBound = std::min(kMaxSafeInteger, maxSizeT);
    return std::isfinite(value) && value >= 0.0 && value == std::floor(value) && value <= upperBound;
}

bool IsIntegerInRange(double value, double lo, double hi) {
    return std::isfinite(value) && value == std::floor(value) && value >= lo && value <= hi;
}

}  // namespace

int MidiConfigViewModel::UISystemMessageIndex(std::size_t controllerIx, MidiConfigSection section,
                                              std::size_t rowIx) const {
    if (section != MidiConfigSection::SystemMessages) {
        return -1;
    }
    if (controllerIx >= instrument_.controllers.size()) {
        return -1;
    }
    const SectionPresentation& presentation = PresentationFor(controllerIx, section);
    if (rowIx >= presentation.rows.size() || presentation.rows[rowIx].kind != RowKind::Individual) {
        return -1;
    }
    const auto* association =
        std::get_if<MidiControllerSystemMessageAssociation>(&presentation.rows[rowIx].data);
    if (association == nullptr) {
        return -1;
    }

    const UISystemMessage message = UISystemMessageForAssociation(*association);
    const auto& catalog = MessageCatalog();
    const UISystemMessageChoice* choice =
        FindUISystemMessageChoice(message, catalog, association->appAction, association->appActionValue);
    if (choice == nullptr) {
        return -1;
    }
    return static_cast<int>(choice - catalog.data());
}

int MidiConfigViewModel::BlockMessageTypeIndex(std::size_t controllerIx, MidiConfigSection section,
                                               std::size_t rowIx) const {
    if (section != MidiConfigSection::SystemMessages) {
        return -1;
    }
    if (controllerIx >= instrument_.controllers.size()) {
        return -1;
    }
    const SectionPresentation& presentation = PresentationFor(controllerIx, section);
    if (rowIx >= presentation.rows.size() || presentation.rows[rowIx].kind != RowKind::Block) {
        return -1;
    }
    const auto* systemBlock = std::get_if<SystemBlock>(&presentation.rows[rowIx].block);
    if (systemBlock == nullptr) {
        return -1;
    }
    return static_cast<int>(systemBlock->message);
}

namespace {

// Applies a system Block row's field edit to a scratch copy of `block`,
// validating domain per-field the same way the individual-row cases below
// do (Channel 0-15, coordinates integral, message-type/bool toggle indices
// in range). Does NOT re-validate the whole expansion (ApplyMappingEdit's
// Block case does that separately via ExpandSystemBlock, an
// all-or-nothing validation).
bool ApplyEncoderBlockField(EncoderBlock& block, Field field, double value, std::string& validationError) {
    switch (field) {
        case Field::AddressType:
            if (!IsIntegerInRange(value, 0.0, static_cast<double>(ControlAddressTypeCatalog().size() - 1))) {
                validationError = "address type index out of range";
                return false;
            }
            block.controlType = static_cast<MidiControlType>(static_cast<int>(value));
            return true;
        case Field::Channel:
            if (!IsIntegerInRange(value, 0.0, 15.0)) {
                validationError = "channel must be an integer 0-15";
                return false;
            }
            block.channel = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockStartCc:
            if (!IsIntegerInRange(value, 0.0, 127.0)) {
                validationError = "start cc must be an integer 0-127";
                return false;
            }
            block.startCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockEndCc:
            if (!IsIntegerInRange(value, 0.0, 128.0)) {
                validationError = "end cc must be an integer 0-128";
                return false;
            }
            block.endCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::SlotIx:
            if (!IsNonNegativeInteger(value)) {
                validationError = "slot index must be a non-negative integer";
                return false;
            }
            block.slotIx = static_cast<std::size_t>(value);
            return true;
        case Field::BlockStartPos:
            if (!IsNonNegativeInteger(value)) {
                validationError = "start position must be a non-negative integer";
                return false;
            }
            block.startPosition = static_cast<std::size_t>(value);
            return true;
        default:
            return false;
    }
}

bool ApplyAnalogBlockField(AnalogBlock& block, Field field, double value, std::string& validationError) {
    switch (field) {
        case Field::Channel:
            if (!IsIntegerInRange(value, 0.0, 15.0)) {
                validationError = "channel must be an integer 0-15";
                return false;
            }
            block.channel = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockStartCc:
            if (!IsIntegerInRange(value, 0.0, 127.0)) {
                validationError = "start cc must be an integer 0-127";
                return false;
            }
            block.startCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockEndCc:
            if (!IsIntegerInRange(value, 0.0, 128.0)) {
                validationError = "end cc must be an integer 0-128";
                return false;
            }
            block.endCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockStartArg:
            if (!IsNonNegativeInteger(value)) {
                validationError = "start gesture index must be a non-negative integer";
                return false;
            }
            block.startGestureIx = static_cast<std::size_t>(value);
            return true;
        default:
            return false;
    }
}

bool ApplySystemBlockField(SystemBlock& block, Field field, double value, std::string& validationError) {
    switch (field) {
        case Field::AddressType:
            if (!IsIntegerInRange(value, 0.0, static_cast<double>(ControlAddressTypeCatalog().size() - 1))) {
                validationError = "address type index out of range";
                return false;
            }
            block.controlType = static_cast<MidiControlType>(static_cast<int>(value));
            return true;
        case Field::BlockMessageType:
            if (!IsIntegerInRange(value, 0.0, static_cast<double>(BlockableMessageCatalog().size() - 1))) {
                validationError = "message type index out of range";
                return false;
            }
            block.message = static_cast<BlockableMessage>(static_cast<int>(value));
            return true;
        case Field::Channel:
            if (!IsIntegerInRange(value, 0.0, 15.0)) {
                validationError = "channel must be an integer 0-15";
                return false;
            }
            block.channel = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockStartCc:
            if (!IsIntegerInRange(value, 0.0, 127.0)) {
                validationError = "start cc must be an integer 0-127";
                return false;
            }
            block.startCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockEndCc:
            if (!IsIntegerInRange(value, 0.0, 128.0)) {
                validationError = "end cc must be an integer 0-128";
                return false;
            }
            block.endCc = static_cast<std::uint8_t>(value);
            return true;
        case Field::BlockStartX:
            if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                                  static_cast<double>(std::numeric_limits<int>::max()))) {
                validationError = "start x must be an integer";
                return false;
            }
            block.startX = static_cast<int>(value);
            return true;
        case Field::BlockStartY:
            if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                                  static_cast<double>(std::numeric_limits<int>::max()))) {
                validationError = "start y must be an integer";
                return false;
            }
            block.startY = static_cast<int>(value);
            return true;
        case Field::BlockEndX:
            if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                                  static_cast<double>(std::numeric_limits<int>::max()))) {
                validationError = "end x must be an integer";
                return false;
            }
            block.endX = static_cast<int>(value);
            return true;
        case Field::BlockEndY:
            if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                                  static_cast<double>(std::numeric_limits<int>::max()))) {
                validationError = "end y must be an integer";
                return false;
            }
            block.endY = static_cast<int>(value);
            return true;
        case Field::BlockStartArg:
            if (!IsNonNegativeInteger(value)) {
                validationError = "start argument must be a non-negative integer";
                return false;
            }
            block.startArg = static_cast<std::size_t>(value);
            return true;
        case Field::BlockBankSlotIx:
            if (!IsNonNegativeInteger(value)) {
                validationError = "bank slot index must be a non-negative integer";
                return false;
            }
            block.bankSlotIx = static_cast<std::size_t>(value);
            return true;
        case Field::BlockRowMajor:
            if (!IsIntegerInRange(value, 0.0, 1.0)) {
                validationError = "row-major must be 0 or 1";
                return false;
            }
            block.rowMajor = value != 0.0;
            return true;
        case Field::BlockOutputFeedback:
            if (!IsIntegerInRange(value, 0.0, 1.0)) {
                validationError = "output feedback must be 0 or 1";
                return false;
            }
            block.outputFeedback = value != 0.0;
            return true;
        default:
            return false;
    }
}

bool ApplyGridCoordinate(int& target, double value, const char* name, std::string& validationError) {
    if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                          static_cast<double>(std::numeric_limits<int>::max()))) {
        validationError = std::string(name) + " must be an integer";
        return false;
    }
    target = static_cast<int>(value);
    return true;
}

bool ApplyGridButtonField(GridButton& button, Field field, double value, std::string& validationError) {
    switch (field) {
        case Field::Channel:
            if (button.kind != MidiProfileKind::WrldBldr || !IsIntegerInRange(value, 0.0, 15.0)) {
                validationError = "channel must be an integer 0-15";
                return false;
            }
            button.channel = static_cast<std::uint8_t>(value);
            return true;
        case Field::GridSlotIx:
            if (!IsNonNegativeInteger(value)) {
                validationError = "grid slot index must be a non-negative integer";
                return false;
            }
            button.gridSlotIx = static_cast<std::size_t>(value);
            return true;
        case Field::GridXMin:
            return ApplyGridCoordinate(button.x, value, "x min", validationError);
        case Field::GridYMin:
            return ApplyGridCoordinate(button.y, value, "y min", validationError);
        default:
            return false;
    }
}

bool ApplyGridBlockField(GridBlock& block, Field field, double value, std::string& validationError) {
    switch (field) {
        case Field::Channel:
            if (block.kind != MidiProfileKind::WrldBldr || !IsIntegerInRange(value, 0.0, 15.0)) {
                validationError = "channel must be an integer 0-15";
                return false;
            }
            block.channel = static_cast<std::uint8_t>(value);
            return true;
        case Field::GridSlotIx:
            if (!IsNonNegativeInteger(value)) {
                validationError = "grid slot index must be a non-negative integer";
                return false;
            }
            block.gridSlotIx = static_cast<std::size_t>(value);
            return true;
        case Field::GridXMin:
            return ApplyGridCoordinate(block.startX, value, "x min", validationError);
        case Field::GridXMax:
            return ApplyGridCoordinate(block.endX, value, "x max", validationError);
        case Field::GridYMin:
            return ApplyGridCoordinate(block.startY, value, "y min", validationError);
        case Field::GridYMax:
            return ApplyGridCoordinate(block.endY, value, "y max", validationError);
        default:
            return false;
    }
}

// The section flush is the single commit path for every presentation edit:
// individual rows, block rows, adds, deletes, and Launchpad variant rewrites
// all serialize the open presentation into one candidate section and validate
// that candidate before returning `out`. Duplicate MIDI addresses are refused
// at this whole-section boundary, so a row edit that would collide with a
// sibling row fails the same way a block edit/add that would expand into a
// sibling address fails. This keeps persisted truth canonical without any
// special block-only duplicate path.
bool HasDuplicateEncoderAddress(const std::vector<EncoderMidiMapping>& mappings) {
    for (std::size_t ix = 0; ix < mappings.size(); ++ix) {
        for (std::size_t jx = ix + 1; jx < mappings.size(); ++jx) {
            if (mappings[ix].control == mappings[jx].control) {
                return true;
            }
        }
    }
    return false;
}

// Gestures and app actions share one address space per analog input (both
// key off (channel, cc) on the same physical control), so a duplicate check
// limited to one vector would miss a gesture/app-action collision -- checked
// pairwise within each vector, then across the two.
bool HasDuplicateAnalogAddress(const std::vector<AnalogMidiMapping>& gestures,
                               const std::vector<AnalogAppActionMapping>& appActions) {
    for (std::size_t ix = 0; ix < gestures.size(); ++ix) {
        for (std::size_t jx = ix + 1; jx < gestures.size(); ++jx) {
            if (gestures[ix].control == gestures[jx].control) {
                return true;
            }
        }
    }
    for (std::size_t ix = 0; ix < appActions.size(); ++ix) {
        for (std::size_t jx = ix + 1; jx < appActions.size(); ++jx) {
            if (appActions[ix].control == appActions[jx].control) {
                return true;
            }
        }
    }
    for (const AnalogMidiMapping& gesture : gestures) {
        for (const AnalogAppActionMapping& appAction : appActions) {
            if (gesture.control == appAction.control) {
                return true;
            }
        }
    }
    return false;
}

// One association's address tuple per the kind's schema -- the same
// fields SystemAddressSchema(kind) names, flattened to a comparable form.
// Twister's address is its `control` (channel 3, cc 8-13) same as generic;
// distinguishing twister isn't needed here since MfTwister never blocks
// and AddSingle is the only twister caller, which already refuses via
// NextFreeTwisterButton exhaustion before reaching this check.
bool SameSystemAddress(const MidiControllerSystemMessageAssociation& a,
                       const MidiControllerSystemMessageAssociation& b, MidiProfileKind kind) {
    if (kind == MidiProfileKind::Launchpad) {
        return a.launchpadPosition.has_value() && b.launchpadPosition.has_value() &&
              *a.launchpadPosition == *b.launchpadPosition;
    }
    if (kind == MidiProfileKind::WrldBldr) {
        return a.control.has_value() && b.control.has_value() && *a.control == *b.control;
    }
    // MfTwister and Generic: (channel, cc) via `control`.
    return a.control.has_value() && b.control.has_value() && *a.control == *b.control;
}

bool HasDuplicateSystemAddress(const std::vector<MidiControllerSystemMessageAssociation>& associations,
                               MidiProfileKind kind) {
    for (std::size_t ix = 0; ix < associations.size(); ++ix) {
        for (std::size_t jx = ix + 1; jx < associations.size(); ++jx) {
            if (SameSystemAddress(associations[ix], associations[jx], kind)) {
                return true;
            }
        }
    }
    return false;
}

bool FlushSectionPresentationToSlot(const SectionPresentation& presentation, MidiControllerSlot& slot,
                                    MidiConfigSection section, std::string* reason) {
    switch (section) {
        case MidiConfigSection::Encoders: {
            EncoderMidiInConfig next = slot.config.encoderInput.value_or(EncoderMidiInConfig{});
            next.turns.clear();
            next.pushes.clear();
            for (const PresentationRow& row : presentation.rows) {
                if (row.kind == RowKind::ConfigLevel) {
                    if (const auto* mode = std::get_if<EncoderModeRow>(&row.data)) {
                        next.mode = mode->mode;
                    } else if (const auto* step = std::get_if<EncoderStepRow>(&row.data)) {
                        next.turnStep = step->turnStep;
                    }
                } else if (row.kind == RowKind::Individual) {
                    const auto* mapping = std::get_if<EncoderMidiMapping>(&row.data);
                    if (mapping == nullptr) {
                        if (reason != nullptr) {
                            *reason = "encoder row has no mapping data";
                        }
                        return false;
                    }
                    if (row.group == RowGroup::EncoderPush) {
                        next.pushes.push_back(*mapping);
                    } else {
                        next.turns.push_back(*mapping);
                    }
                } else if (const auto* block = std::get_if<EncoderBlock>(&row.block)) {
                    std::vector<EncoderMidiMapping> expansion;
                    if (!ExpandEncoderBlock(*block, expansion, reason)) {
                        return false;
                    }
                    std::vector<EncoderMidiMapping>& target = block->isPush ? next.pushes : next.turns;
                    target.insert(target.end(), expansion.begin(), expansion.end());
                } else {
                    if (reason != nullptr) {
                        *reason = "encoder block row has no block data";
                    }
                    return false;
                }
            }
            if (HasDuplicateEncoderAddress(next.turns) || HasDuplicateEncoderAddress(next.pushes)) {
                if (reason != nullptr) {
                    *reason = "section would create a duplicate (channel, cc) address";
                }
                return false;
            }
            slot.config.encoderInput = std::move(next);
            break;
        }
        case MidiConfigSection::Analogs: {
            AnalogMidiInConfig next = slot.config.analogInput.value_or(AnalogMidiInConfig{});
            next.gestures.clear();
            next.sceneBlend = std::nullopt;
            next.appActions.clear();
            for (const PresentationRow& row : presentation.rows) {
                if (row.kind == RowKind::ConfigLevel) {
                    if (const auto* sceneBlend = std::get_if<AnalogSceneBlendRow>(&row.data)) {
                        next.sceneBlend = sceneBlend->sceneBlend;
                    }
                } else if (row.kind == RowKind::Individual) {
                    if (row.group == RowGroup::AnalogAppAction) {
                        const auto* appAction = std::get_if<AnalogAppActionMapping>(&row.data);
                        if (appAction == nullptr) {
                            if (reason != nullptr) {
                                *reason = "analog app-action row has no mapping data";
                            }
                            return false;
                        }
                        next.appActions.push_back(*appAction);
                        continue;
                    }
                    const auto* mapping = std::get_if<AnalogMidiMapping>(&row.data);
                    if (mapping == nullptr) {
                        if (reason != nullptr) {
                            *reason = "analog row has no mapping data";
                        }
                        return false;
                    }
                    next.gestures.push_back(*mapping);
                } else if (const auto* block = std::get_if<AnalogBlock>(&row.block)) {
                    std::vector<AnalogMidiMapping> expansion;
                    if (!ExpandAnalogBlock(*block, expansion, reason)) {
                        return false;
                    }
                    next.gestures.insert(next.gestures.end(), expansion.begin(), expansion.end());
                } else {
                    if (reason != nullptr) {
                        *reason = "analog block row has no block data";
                    }
                    return false;
                }
            }
            if (HasDuplicateAnalogAddress(next.gestures, next.appActions)) {
                if (reason != nullptr) {
                    *reason = "section would create a duplicate (channel, cc) address";
                }
                return false;
            }
            slot.config.analogInput = std::move(next);
            break;
        }
        case MidiConfigSection::SystemMessages: {
            std::vector<MidiControllerSystemMessageAssociation> next;
            GridMappingExpansion gridExpansion;
            for (const PresentationRow& row : presentation.rows) {
                if (row.group == RowGroup::Grid) {
                    if (row.kind == RowKind::Individual) {
                        const auto* button = std::get_if<GridButton>(&row.data);
                        if (button == nullptr || !ExpandGridButton(*button, gridExpansion, reason)) {
                            if (button == nullptr && reason != nullptr) {
                                *reason = "grid button row has no button data";
                            }
                            return false;
                        }
                    } else if (row.kind == RowKind::Block) {
                        const auto* block = std::get_if<GridBlock>(&row.block);
                        if (block == nullptr || !ExpandGridBlock(*block, gridExpansion, reason)) {
                            if (block == nullptr && reason != nullptr) {
                                *reason = "grid block row has no block data";
                            }
                            return false;
                        }
                    } else {
                        if (reason != nullptr) {
                            *reason = "grid rows cannot be config-level rows";
                        }
                        return false;
                    }
                } else if (row.kind == RowKind::Individual) {
                    const auto* association = std::get_if<MidiControllerSystemMessageAssociation>(&row.data);
                    if (association == nullptr) {
                        if (reason != nullptr) {
                            *reason = "system row has no association data";
                        }
                        return false;
                    }
                    next.push_back(*association);
                } else if (const auto* block = std::get_if<SystemBlock>(&row.block)) {
                    std::vector<MidiControllerSystemMessageAssociation> expansion;
                    if (!ExpandSystemBlock(*block, expansion, reason)) {
                        return false;
                    }
                    next.insert(next.end(), expansion.begin(), expansion.end());
                } else {
                    if (reason != nullptr) {
                        *reason = "system block row has no block data";
                    }
                    return false;
                }
            }
            next.insert(next.end(), gridExpansion.systemMessages.begin(), gridExpansion.systemMessages.end());
            if (HasDuplicateSystemAddress(next, slot.kind)) {
                if (reason != nullptr) {
                    *reason = "section would create a duplicate address";
                }
                return false;
            }
            slot.config.systemMessages = std::move(next);

            PolyphonicPressureMidiInConfig pressure;
            pressure.mappings = std::move(gridExpansion.pressureMappings);
            pressure.mappings.insert(pressure.mappings.end(), presentation.hiddenPressureMappings.begin(),
                                     presentation.hiddenPressureMappings.end());
            if (slot.config.pressureInput.has_value() || !pressure.mappings.empty()) {
                slot.config.pressureInput = std::move(pressure);
            } else {
                slot.config.pressureInput.reset();
            }
            break;
        }
    }
    NormalizeMidiProfileConfig(slot.config, slot.kind);
    return SlotValidForKind(slot, reason);
}

}  // namespace

bool MidiConfigViewModel::ApplyMappingEdit(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx,
                                           MidiMappingRowVM::Field field, double value, MidiInstrumentConfig& out,
                                           std::string* reason, bool* presentationChanged) const {
    if (presentationChanged != nullptr) {
        *presentationChanged = false;
    }
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller index out of range";
        }
        return false;
    }

    const std::vector<MidiMappingRowVM> rows = SectionRows(controllerIx, section);
    if (rowIx >= rows.size()) {
        if (reason != nullptr) {
            *reason = "row index out of range";
        }
        return false;
    }
    const std::vector<MidiMappingRowVM::Field>& editable = rows[rowIx].editableFields;
    if (std::find(editable.begin(), editable.end(), field) == editable.end()) {
        if (reason != nullptr) {
            *reason = "field not editable for this row";
        }
        return false;
    }

    SectionPresentation& presentation = PresentationFor(controllerIx, section);
    PresentationRow& presentationRow = presentation.rows[rowIx];

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot& slot = scratch.controllers[controllerIx];

    bool fieldValid = false;
    // Set alongside fieldValid=false when the field IS editable on this row
    // but `value` fails domain validation, so the caller gets a precise
    // reason instead of the generic "field is not editable" one.
    std::string validationError;

    if (presentationRow.kind == RowKind::Block) {
        if (auto* encoderBlock = std::get_if<EncoderBlock>(&presentationRow.block)) {
            fieldValid = ApplyEncoderBlockField(*encoderBlock, field, value, validationError);
        } else if (auto* analogBlock = std::get_if<AnalogBlock>(&presentationRow.block)) {
            fieldValid = ApplyAnalogBlockField(*analogBlock, field, value, validationError);
        } else if (auto* systemBlock = std::get_if<SystemBlock>(&presentationRow.block)) {
            fieldValid = ApplySystemBlockField(*systemBlock, field, value, validationError);
        } else if (auto* gridBlock = std::get_if<GridBlock>(&presentationRow.block)) {
            fieldValid = ApplyGridBlockField(*gridBlock, field, value, validationError);
        }
    } else if (presentationRow.kind == RowKind::ConfigLevel) {
        if (presentationRow.group == RowGroup::EncoderMode && field == Field::EncoderMode) {
            // Index-based: `value` selects EncoderModeCatalog() by position
            // in EncoderMode's declaration order, not a raw unvalidated enum
            // value, so a JUCE combo box can drive this directly off its
            // selected index.
            const std::vector<std::string>& catalog = EncoderModeCatalog();
            if (!IsIntegerInRange(value, 0.0, static_cast<double>(catalog.size() - 1))) {
                validationError = "encoder mode index out of range";
            } else {
                const auto index = static_cast<std::size_t>(value);
                presentationRow.data = EncoderModeRow{.mode = static_cast<EncoderMode>(index)};
                fieldValid = true;
            }
        } else if (presentationRow.group == RowGroup::EncoderStep && field == Field::TurnStep) {
            if (!std::isfinite(value) || value <= 0.0 || value > double(std::numeric_limits<float>::max())) {
                validationError = "turn step out of range";
            } else {
                presentationRow.data = EncoderStepRow{.turnStep = static_cast<float>(value)};
                fieldValid = true;
            }
        } else if (presentationRow.group == RowGroup::AnalogSceneBlend && field == Field::SceneBlend) {
            if (!IsIntegerInRange(value, 0.0, 127.0)) {
                validationError = "cc must be an integer 0-127";
            } else {
                const auto* existing = std::get_if<AnalogSceneBlendRow>(&presentationRow.data);
                MidiControlAddress address =
                    existing != nullptr ? existing->sceneBlend.value_or(MidiControlAddress{}) : MidiControlAddress{};
                address.cc = static_cast<std::uint8_t>(value);
                presentationRow.data = AnalogSceneBlendRow{.sceneBlend = address};
                fieldValid = true;
            }
        }
    } else {
        if (auto* mapping = std::get_if<EncoderMidiMapping>(&presentationRow.data)) {
            switch (field) {
                case Field::AddressType:
                    if (!IsIntegerInRange(value, 0.0,
                                          static_cast<double>(ControlAddressTypeCatalog().size() - 1))) {
                        validationError = "address type index out of range";
                        break;
                    }
                    mapping->control.type = static_cast<MidiControlType>(static_cast<int>(value));
                    fieldValid = true;
                    break;
                case Field::Channel:
                    if (!IsIntegerInRange(value, 0.0, 15.0)) {
                        validationError = "channel must be an integer 0-15";
                        break;
                    }
                    mapping->control.channel = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::Cc:
                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
                        validationError = "cc must be an integer 0-127";
                        break;
                    }
                    mapping->control.cc = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::SlotIx:
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "slot index must be a non-negative integer";
                        break;
                    }
                    mapping->slotIx = static_cast<std::size_t>(value);
                    fieldValid = true;
                    break;
                case Field::Position:
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "position must be a non-negative integer";
                        break;
                    }
                    mapping->position = static_cast<std::size_t>(value);
                    fieldValid = true;
                    break;
                default:
                    break;
            }
        } else if (auto* mapping = std::get_if<AnalogMidiMapping>(&presentationRow.data)) {
            switch (field) {
                case Field::Channel:
                    if (!IsIntegerInRange(value, 0.0, 15.0)) {
                        validationError = "channel must be an integer 0-15";
                        break;
                    }
                    mapping->control.channel = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::Cc:
                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
                        validationError = "cc must be an integer 0-127";
                        break;
                    }
                    mapping->control.cc = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::GestureIx:
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "gesture index must be a non-negative integer";
                        break;
                    }
                    mapping->gestureIx = static_cast<std::size_t>(value);
                    fieldValid = true;
                    break;
                default:
                    break;
            }
        } else if (auto* mapping = std::get_if<AnalogAppActionMapping>(&presentationRow.data)) {
            switch (field) {
                case Field::Channel:
                    if (!IsIntegerInRange(value, 0.0, 15.0)) {
                        validationError = "channel must be an integer 0-15";
                        break;
                    }
                    mapping->control.channel = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::Cc:
                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
                        validationError = "cc must be an integer 0-127";
                        break;
                    }
                    mapping->control.cc = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::AppAction: {
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "app action must be a non-negative integer catalog index";
                        break;
                    }
                    const auto& catalog = AnalogActionCatalog();
                    const auto choiceIx = static_cast<std::size_t>(value);
                    if (choiceIx >= catalog.size()) {
                        validationError = "app action index out of range";
                        break;
                    }
                    const UISystemMessageChoice& choice = catalog[choiceIx];
                    mapping->appAction = choice.appAction;
                    mapping->appActionValue = choice.appActionValue;
                    mapping->appActionIx = choice.appActionIx;
                    fieldValid = true;
                    break;
                }
                default:
                    break;
            }
        } else if (auto* association = std::get_if<MidiControllerSystemMessageAssociation>(&presentationRow.data)) {
            switch (field) {
                case Field::AddressType:
                    if (!association->control.has_value()) {
                        break;
                    }
                    if (!IsIntegerInRange(value, 0.0,
                                          static_cast<double>(ControlAddressTypeCatalog().size() - 1))) {
                        validationError = "address type index out of range";
                        break;
                    }
                    association->control->type = static_cast<MidiControlType>(static_cast<int>(value));
                    fieldValid = true;
                    break;
                case Field::Channel:
                    if (!association->control.has_value()) {
                        break;
                    }
                    if (!IsIntegerInRange(value, 0.0, 15.0)) {
                        validationError = "channel must be an integer 0-15";
                        break;
                    }
                    association->control->channel = static_cast<std::uint8_t>(value);
                    if (association->wrldBldrPosition.has_value()) {
                        association->wrldBldrPosition->channel = association->control->channel;
                    }
                    fieldValid = true;
                    break;
                case Field::Cc:
                    if (!association->control.has_value()) {
                        break;
                    }
                    if (!IsIntegerInRange(value, 0.0, 127.0)) {
                        validationError = "cc must be an integer 0-127";
                        break;
                    }
                    association->control->cc = static_cast<std::uint8_t>(value);
                    fieldValid = true;
                    break;
                case Field::LaunchpadX:
                case Field::LaunchpadY: {
                    if (!association->launchpadPosition.has_value()) {
                        break;
                    }
                    if (!IsIntegerInRange(value, static_cast<double>(std::numeric_limits<int>::min()),
                                          static_cast<double>(std::numeric_limits<int>::max()))) {
                        validationError = "launchpad coordinate must be an integer";
                        break;
                    }
                    LaunchpadGridPosition candidate = *association->launchpadPosition;
                    const int coordinate = static_cast<int>(value);
                    if (field == Field::LaunchpadX) {
                        candidate.x = coordinate;
                    } else {
                        candidate.y = coordinate;
                    }
                    if (!LaunchpadShapeSupports(candidate.controller, candidate.x, candidate.y)) {
                        validationError = "launchpad coordinate is outside this controller's grid";
                        break;
                    }
                    association->launchpadPosition = candidate;
                    fieldValid = true;
                    break;
                }
                case Field::WrldBldrX:
                case Field::WrldBldrY: {
                    if (!association->wrldBldrPosition.has_value()) {
                        break;
                    }
                    if (!IsIntegerInRange(value, 0.0, 7.0)) {
                        validationError = "WRLD.Bldr coordinate must be an integer 0-7";
                        break;
                    }
                    WrldBldrSystemPosition candidate = *association->wrldBldrPosition;
                    const std::uint8_t coordinate = static_cast<std::uint8_t>(value);
                    if (field == Field::WrldBldrX) {
                        candidate.x = coordinate;
                    } else {
                        candidate.y = coordinate;
                    }
                    const std::uint8_t channel =
                        association->control.has_value() ? association->control->channel : candidate.channel;
                    candidate.channel = channel;
                    association->wrldBldrPosition = candidate;
                    association->control =
                        MidiControlAddress{.channel = channel, .cc = WrldBldrPositionToCC(candidate.x, candidate.y)};
                    fieldValid = true;
                    break;
                }
                case Field::Button: {
                    if (!IsIntegerInRange(value, 0.0, 5.0)) {
                        validationError = "side button must be an integer 0-5";
                        break;
                    }
                    const std::uint8_t channel = association->control.has_value() ? association->control->channel
                                                                                 : static_cast<std::uint8_t>(3);
                    association->control = MidiControlAddress{.channel = channel,
                                                              .cc = static_cast<std::uint8_t>(8 + static_cast<int>(value))};
                    fieldValid = true;
                    break;
                }
                case Field::MessageKind: {
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "message kind must be a non-negative integer catalog index";
                        break;
                    }
                    const auto& catalog = MessageCatalog();
                    const auto choiceIx = static_cast<std::size_t>(value);
                    if (choiceIx >= catalog.size()) {
                        validationError = "message kind index out of range";
                        break;
                    }
                    const UISystemMessageChoice& choice = catalog[choiceIx];
                    ApplyUISystemMessage(*association, choice.message);
                    if (choice.message == UISystemMessage::AppAction) {
                        association->press.appActionIx = choice.appActionIx;
                        association->feedback = association->press;
                        association->appAction = choice.appAction;
                        association->appActionValue = choice.appActionValue;
                    } else {
                        association->appAction.clear();
                        association->appActionValue.clear();
                    }
                    fieldValid = true;
                    break;
                }
                case Field::MessageArg: {
                    if (!IsNonNegativeInteger(value)) {
                        validationError = "message argument must be a non-negative integer";
                        break;
                    }
                    const auto arg = static_cast<std::size_t>(value);
                    if (!SetUISystemMessageArg(*association, arg)) {
                        validationError = "message has no integer argument";
                        break;
                    }
                    fieldValid = true;
                    break;
                }
                default:
                    break;
            }
        } else if (auto* button = std::get_if<GridButton>(&presentationRow.data)) {
            fieldValid = ApplyGridButtonField(*button, field, value, validationError);
        }
    }

    if (!fieldValid) {
        if (reason != nullptr) {
            *reason = !validationError.empty() ? validationError : "field is not editable on this row";
        }
        return false;
    }

    if (presentationChanged != nullptr) {
        *presentationChanged = true;
    }

    if (!FlushSectionPresentationToSlot(presentation, slot, section, reason)) {
        return false;
    }

    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::AddController(std::string name, MidiProfileKind kind, MidiInstrumentConfig& out,
                                        std::string* reason) const {
    if (instrument_.FindController(name) != nullptr) {
        if (reason != nullptr) {
            *reason = "a controller with this name already exists";
        }
        return false;
    }

    MidiControllerSlot slot;
    slot.name = name;
    slot.kind = kind;
    switch (kind) {
        case MidiProfileKind::WrldBldr:
            slot.config = WrldBldrDefaultProfileConfig();
            break;
        case MidiProfileKind::MfTwister:
            slot.config = MfTwisterDefaultProfileConfig();
            break;
        case MidiProfileKind::Launchpad:
            slot.config = LaunchpadDefaultProfileConfig();
            break;
        case MidiProfileKind::Generic:
            slot.config = MidiControllerProfileConfig{};
            break;
    }
    // Every commit path normalizes, including a freshly-seeded default
    // profile (already canonical in practice for every factory today, but
    // this keeps the guarantee independent of that incidental fact).
    NormalizeMidiProfileConfig(slot.config, kind);

    MidiInstrumentConfig scratch = instrument_;
    if (!scratch.AddController(std::move(slot))) {
        if (reason != nullptr) {
            *reason = "controller could not be added (duplicate name or invalid slot)";
        }
        return false;
    }

    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::RenameController(std::size_t controllerIx, std::string name,
                                           MidiInstrumentConfig& out, std::string* reason) const {
    if (name.empty()) {
        if (reason != nullptr) {
            *reason = "name must not be empty";
        }
        return false;
    }
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller does not exist";
        }
        return false;
    }
    if (instrument_.controllers[controllerIx].name == name) {
        if (reason != nullptr) {
            *reason = "name is unchanged";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    if (!scratch.RenameController(controllerIx, std::move(name))) {
        if (reason != nullptr) {
            *reason = "a controller with this name already exists";
        }
        return false;
    }
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::DeleteController(std::size_t controllerIx, MidiInstrumentConfig& out,
                                           std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller does not exist";
        }
        return false;
    }
    if (instrument_.controllers[controllerIx].disposition != MidiControllerDisposition::Active) {
        if (reason != nullptr) {
            *reason = "only active controllers can be deleted";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    scratch.RemoveController(controllerIx);
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::BlacklistController(std::size_t controllerIx, MidiInstrumentConfig& out,
                                              std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller does not exist";
        }
        return false;
    }
    const MidiControllerSlot& existing = instrument_.controllers[controllerIx];
    if (!existing.input.IsConfigured() || !existing.output.IsConfigured()) {
        if (reason != nullptr) {
            *reason = "releasing requires both input and output endpoint references";
        }
        return false;
    }
    const bool resolved = existing.wizardId.has_value() &&
        std::any_of(Layouts().begin(), Layouts().end(),
                    [&](const ControllerWizardDescriptor& descriptor) {
                        return descriptor.id == *existing.wizardId;
                    });
    if (existing.disposition != MidiControllerDisposition::Active || !resolved) {
        if (reason != nullptr) {
            *reason = "only registry-supported active controllers can be released";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot blacklisted = scratch.controllers[controllerIx];
    blacklisted.disposition = MidiControllerDisposition::Blacklisted;
    blacklisted.dormantConfig = std::move(blacklisted.config);
    blacklisted.config = {};
    if (!scratch.ReplaceController(controllerIx, std::move(blacklisted))) {
        if (reason != nullptr) {
            *reason = "controller could not be released";
        }
        return false;
    }
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::RemoveFromBlacklist(std::size_t controllerIx, MidiInstrumentConfig& out,
                                              std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller does not exist";
        }
        return false;
    }
    if (instrument_.controllers[controllerIx].disposition != MidiControllerDisposition::Blacklisted) {
        if (reason != nullptr) {
            *reason = "only released controllers can be reclaimed";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    scratch.RemoveController(controllerIx);
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::RestoreController(std::size_t controllerIx, MidiInstrumentConfig& out,
                                            std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller does not exist";
        }
        return false;
    }
    const MidiControllerSlot& existing = instrument_.controllers[controllerIx];
    const auto descriptorIt = existing.wizardId.has_value()
        ? std::find_if(Layouts().begin(), Layouts().end(),
                       [&](const ControllerWizardDescriptor& descriptor) {
                           return descriptor.id == *existing.wizardId;
                       })
        : Layouts().end();
    if (descriptorIt == Layouts().end()) {
        if (reason != nullptr) {
            *reason = "restoring requires a resolved preset";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot restored = scratch.controllers[controllerIx];
    if (!runtime_ui::ControllersLayout::InstallDescriptorProfile(Layouts(), *descriptorIt, restored, reason)) {
        return false;
    }
    if (!scratch.ReplaceController(controllerIx, std::move(restored))) {
        if (reason != nullptr) {
            *reason = "controller could not be restored";
        }
        return false;
    }
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::SetEndpointRef(std::size_t controllerIx, bool output, MidiEndpointRef ref,
                                         MidiInstrumentConfig& out) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return false;
    }
    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot& slot = scratch.controllers[controllerIx];
    if (output) {
        slot.output = std::move(ref);
    } else {
        slot.input = std::move(ref);
    }
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::CanDeleteRow(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return false;
    }
    const SectionPresentation& presentation = PresentationFor(controllerIx, section);
    return rowIx < presentation.rows.size() && presentation.rows[rowIx].kind != RowKind::ConfigLevel;
}

bool MidiConfigViewModel::DeleteRow(std::size_t controllerIx, MidiConfigSection section, std::size_t rowIx,
                                    MidiInstrumentConfig& out, std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller index out of range";
        }
        return false;
    }
    SectionPresentation& presentation = PresentationFor(controllerIx, section);
    if (rowIx >= presentation.rows.size()) {
        if (reason != nullptr) {
            *reason = "row index out of range";
        }
        return false;
    }
    const PresentationRow& presentationRow = presentation.rows[rowIx];
    if (presentationRow.kind == RowKind::ConfigLevel) {
        if (reason != nullptr) {
            *reason = "config-level rows cannot be deleted";
        }
        return false;
    }

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot& slot = scratch.controllers[controllerIx];
    const PresentationRow rollback = presentationRow;
    presentation.rows.erase(presentation.rows.begin() + static_cast<std::ptrdiff_t>(rowIx));
    if (!FlushSectionPresentationToSlot(presentation, slot, section, reason)) {
        presentation.rows.insert(presentation.rows.begin() + static_cast<std::ptrdiff_t>(rowIx), rollback);
        return false;
    }
    out = std::move(scratch);
    return true;
}

namespace {

// Lowest non-negative integer not present in `used`. "+" appends one
// config with next-free defaults.
std::size_t LowestFree(const std::vector<bool>& used) {
    for (std::size_t ix = 0; ix < used.size(); ++ix) {
        if (!used[ix]) {
            return ix;
        }
    }
    return used.size();
}

std::size_t NextFreeEncoderPosition(const std::vector<EncoderMidiMapping>& mappings) {
    std::vector<bool> used;
    for (const EncoderMidiMapping& mapping : mappings) {
        if (mapping.position >= used.size()) {
            used.resize(mapping.position + 1, false);
        }
        used[mapping.position] = true;
    }
    return LowestFree(used);
}

std::size_t NextFreeGestureIx(const std::vector<AnalogMidiMapping>& mappings) {
    std::vector<bool> used;
    for (const AnalogMidiMapping& mapping : mappings) {
        if (mapping.gestureIx >= used.size()) {
            used.resize(mapping.gestureIx + 1, false);
        }
        used[mapping.gestureIx] = true;
    }
    return LowestFree(used);
}

std::uint8_t NextFreeCc(const std::vector<EncoderMidiMapping>& mappings, std::uint8_t channel) {
    std::vector<bool> used(128, false);
    for (const EncoderMidiMapping& mapping : mappings) {
        if (mapping.control.channel == channel && mapping.control.cc < 128) {
            used[mapping.control.cc] = true;
        }
    }
    return static_cast<std::uint8_t>(std::min<std::size_t>(LowestFree(used), 127));
}

std::uint8_t NextFreeCc(const std::vector<AnalogMidiMapping>& mappings, std::uint8_t channel) {
    std::vector<bool> used(128, false);
    for (const AnalogMidiMapping& mapping : mappings) {
        if (mapping.control.channel == channel && mapping.control.cc < 128) {
            used[mapping.control.cc] = true;
        }
    }
    return static_cast<std::uint8_t>(std::min<std::size_t>(LowestFree(used), 127));
}

// Gestures and app actions share one address space (HasDuplicateAnalogAddress
// above), so a new app-action row's default cc must dodge both, not just its
// own kind's existing rows.
std::uint8_t NextFreeCc(const std::vector<AnalogMidiMapping>& gestures,
                        const std::vector<AnalogAppActionMapping>& appActions, std::uint8_t channel) {
    std::vector<bool> used(128, false);
    for (const AnalogMidiMapping& mapping : gestures) {
        if (mapping.control.channel == channel && mapping.control.cc < 128) {
            used[mapping.control.cc] = true;
        }
    }
    for (const AnalogAppActionMapping& mapping : appActions) {
        if (mapping.control.channel == channel && mapping.control.cc < 128) {
            used[mapping.control.cc] = true;
        }
    }
    return static_cast<std::uint8_t>(std::min<std::size_t>(LowestFree(used), 127));
}

// Lowest-unused sceneIx/bankIx(slot 0)/gestureIx among existing system
// messages of the given BlockableMessage type -- the "next free argument"
// half of a system row's default.
std::size_t NextFreeSystemArg(const std::vector<MidiControllerSystemMessageAssociation>& associations,
                              BlockableMessage message) {
    std::vector<bool> used;
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        std::optional<std::size_t> arg;
        switch (message) {
            case BlockableMessage::SceneSelect:
                if (association.press.type == MessageIn::Type::SceneSelect) {
                    arg = association.press.sceneIx;
                }
                break;
            case BlockableMessage::BankSelect:
                if (association.press.type == MessageIn::Type::SelectParamBank && association.press.slotIx == 0) {
                    arg = association.press.bankIx;
                }
                break;
            case BlockableMessage::GestureSelect:
                if (association.press.type == MessageIn::Type::SetGestureSelect && association.press.boolValue) {
                    arg = association.press.gestureIx;
                }
                break;
        }
        if (arg.has_value()) {
            if (*arg >= used.size()) {
                used.resize(*arg + 1, false);
            }
            used[*arg] = true;
        }
    }
    return LowestFree(used);
}

// Lowest-unused (channel, cc) pair for a generic/MfTwister system row,
// scanning cc within channel 0 first (MfTwister's caller further restricts
// this to the 0..5 button domain -- see AddSingle's SystemMessages case).
std::pair<std::uint8_t, std::uint8_t> NextFreeGenericAddress(
    const std::vector<MidiControllerSystemMessageAssociation>& associations) {
    std::vector<bool> used(128, false);
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        if (association.control.has_value() && association.control->channel == 0 && association.control->cc < 128) {
            used[association.control->cc] = true;
        }
    }
    return {0, static_cast<std::uint8_t>(std::min<std::size_t>(LowestFree(used), 127))};
}

// Lowest-unused MfTwister side button 0..5 (the only shape twister
// associations can occupy -- returns 6 (invalid, refused by the caller)
// if all 6 are taken.
std::size_t NextFreeTwisterButton(const std::vector<MidiControllerSystemMessageAssociation>& associations) {
    std::vector<bool> used(6, false);
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        if (association.control.has_value() && association.control->cc >= 8 && association.control->cc <= 13) {
            used[association.control->cc - 8] = true;
        }
    }
    return LowestFree(used);
}

// Lowest-unused (x,y) WrldBldr 0-7 grid cell, row-major (y then x).
std::optional<std::pair<std::uint8_t, std::uint8_t>> NextFreeWrldBldrPosition(
    const std::vector<MidiControllerSystemMessageAssociation>& associations) {
    bool used[8][8] = {};
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        if (association.wrldBldrPosition.has_value()) {
            const auto& pos = *association.wrldBldrPosition;
            if (pos.x < 8 && pos.y < 8) {
                used[pos.y][pos.x] = true;
            }
        }
    }
    for (std::uint8_t y = 0; y < 8; ++y) {
        for (std::uint8_t x = 0; x < 8; ++x) {
            if (!used[y][x]) {
                return std::make_pair(x, y);
            }
        }
    }
    return std::nullopt;
}

bool PressureAddressUsed(const std::optional<PolyphonicPressureMidiInConfig>& pressureInput,
                         MidiNoteAddress address) {
    if (!pressureInput.has_value()) {
        return false;
    }
    return std::any_of(pressureInput->mappings.begin(), pressureInput->mappings.end(),
                       [address](const PolyphonicPressureMapping& mapping) {
                           return mapping.address == address;
                       });
}

bool WrldBldrGridCellUsed(
    const std::vector<MidiControllerSystemMessageAssociation>& associations,
    const std::optional<PolyphonicPressureMidiInConfig>& pressureInput,
    std::uint8_t channel, int x, int y) {
    const bool systemUsed = std::any_of(
        associations.begin(), associations.end(), [x, y](const auto& association) {
            return association.wrldBldrPosition.has_value() &&
                   association.wrldBldrPosition->x == x && association.wrldBldrPosition->y == y;
        });
    return systemUsed || PressureAddressUsed(
                             pressureInput,
                             MidiNoteAddress{.channel = channel,
                                             .note = WrldBldrPositionToCC(
                                                 static_cast<std::uint8_t>(x),
                                                 static_cast<std::uint8_t>(y))});
}

std::optional<std::pair<int, int>> NextFreeWrldBldrGridPosition(
    const std::vector<MidiControllerSystemMessageAssociation>& associations,
    const std::optional<PolyphonicPressureMidiInConfig>& pressureInput,
    std::uint8_t channel) {
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            if (!WrldBldrGridCellUsed(associations, pressureInput, channel, x, y)) {
                return std::make_pair(x, y);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::pair<int, int>> NextFreeWrldBldrGridPair(
    const std::vector<MidiControllerSystemMessageAssociation>& associations,
    const std::optional<PolyphonicPressureMidiInConfig>& pressureInput,
    std::uint8_t channel) {
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 7; ++x) {
            if (!WrldBldrGridCellUsed(associations, pressureInput, channel, x, y) &&
                !WrldBldrGridCellUsed(associations, pressureInput, channel, x + 1, y)) {
                return std::make_pair(x, y);
            }
        }
    }
    return std::nullopt;
}

// The slot's current Launchpad variant, read from the first launchpad
// association's controller (default LaunchpadX when the slot has no
// launchpad associations yet). Used by AddSingle/AddBlock's launchpad
// branches below so a new row/block added to an already-retargeted (e.g.
// Pro MK3) slot is seeded with THAT variant rather than a hardcoded
// LaunchpadX.
LaunchpadController CurrentLaunchpadVariant(const std::vector<MidiControllerSystemMessageAssociation>& associations) {
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        if (association.launchpadPosition.has_value()) {
            return association.launchpadPosition->controller;
        }
    }
    return LaunchpadController::LaunchpadX;
}

// Lowest-unused Launchpad (x,y) within the given controller's shape,
// row-major scan of a generous bounding box (Launchpad coordinates can be
// -1..9 per LaunchpadShapeSupports).
std::optional<LaunchpadGridPosition> NextFreeLaunchpadPosition(
    const std::vector<MidiControllerSystemMessageAssociation>& associations, LaunchpadController controller) {
    std::vector<LaunchpadGridPosition> used;
    for (const MidiControllerSystemMessageAssociation& association : associations) {
        if (association.launchpadPosition.has_value()) {
            used.push_back(*association.launchpadPosition);
        }
    }
    for (int y = -1; y <= 9; ++y) {
        for (int x = -1; x <= 9; ++x) {
            if (!LaunchpadShapeSupports(controller, x, y)) {
                continue;
            }
            const LaunchpadGridPosition candidate{.controller = controller, .x = x, .y = y};
            if (std::find(used.begin(), used.end(), candidate) == used.end()) {
                return candidate;
            }
        }
    }
    return std::nullopt;
}


std::optional<LaunchpadGridPosition> NextFreeLaunchpadGridPair(
    const std::vector<MidiControllerSystemMessageAssociation>& associations,
    const std::optional<PolyphonicPressureMidiInConfig>& pressureInput,
    LaunchpadController controller, bool pair) {
    std::vector<LaunchpadGridPosition> used;
    for (const auto& association : associations) {
        if (association.launchpadPosition.has_value()) {
            used.push_back(*association.launchpadPosition);
        }
    }
    auto isUsed = [&used, &pressureInput, controller](int x, int y) {
        if (std::find(used.begin(), used.end(),
                      LaunchpadGridPosition{.controller = controller, .x = x, .y = y}) != used.end()) {
            return true;
        }
        const auto note = LaunchpadPositionToNote(controller, x, y);
        return note.has_value() && PressureAddressUsed(
                                       pressureInput,
                                       MidiNoteAddress{.channel = 0, .note = *note});
    };
    for (int y = -1; y <= 9; ++y) {
        for (int x = -1; x <= (pair ? 8 : 9); ++x) {
            const bool nextCellValid = !pair || LaunchpadShapeSupports(controller, x + 1, y);
            const bool nextCellFree = !pair || !isUsed(x + 1, y);
            if (LaunchpadShapeSupports(controller, x, y) && nextCellValid &&
                !isUsed(x, y) && nextCellFree) {
                return LaunchpadGridPosition{.controller = controller, .x = x, .y = y};
            }
        }
    }
    return std::nullopt;
}

}  // namespace

bool MidiConfigViewModel::AddSingle(std::size_t controllerIx, MidiConfigSection section,
                                    MidiMappingRowVM::RowGroup group, MidiInstrumentConfig& out,
                                    std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller index out of range";
        }
        return false;
    }
    if (!GroupSupportsAdd(controllerIx, section, group)) {
        if (reason != nullptr) {
            *reason = "this group does not support adding individual rows";
        }
        return false;
    }
    SectionPresentation& presentation = PresentationFor(controllerIx, section);
    MidiControllerSlot visibleSlot = instrument_.controllers[controllerIx];
    if (!FlushSectionPresentationToSlot(presentation, visibleSlot, section, reason)) {
        return false;
    }

    PresentationRow rowToAppend;
    if (section == MidiConfigSection::Encoders && (group == RowGroup::EncoderTurn || group == RowGroup::EncoderPush)) {
        if (!visibleSlot.config.encoderInput.has_value()) {
            visibleSlot.config.encoderInput = EncoderMidiInConfig{};
        }
        const bool isPush = group == RowGroup::EncoderPush;
        const std::vector<EncoderMidiMapping>& mappings =
            isPush ? visibleSlot.config.encoderInput->pushes : visibleSlot.config.encoderInput->turns;
        const std::uint8_t channel = mappings.empty() ? (isPush ? std::uint8_t{1} : std::uint8_t{0})
                                                       : mappings.front().control.channel;
        EncoderMidiMapping mapping;
        mapping.control.channel = channel;
        mapping.control.cc = NextFreeCc(mappings, channel);
        mapping.slotIx = mappings.empty() ? 0 : mappings.front().slotIx;
        mapping.position = NextFreeEncoderPosition(mappings);
        rowToAppend.kind = RowKind::Individual;
        rowToAppend.group = group;
        rowToAppend.data = mapping;
    } else if (section == MidiConfigSection::Analogs && group == RowGroup::AnalogGesture) {
        if (!visibleSlot.config.analogInput.has_value()) {
            visibleSlot.config.analogInput = AnalogMidiInConfig{};
        }
        const std::vector<AnalogMidiMapping>& mappings = visibleSlot.config.analogInput->gestures;
        const std::uint8_t channel = mappings.empty() ? std::uint8_t{0} : mappings.front().control.channel;
        AnalogMidiMapping mapping;
        mapping.control.channel = channel;
        mapping.control.cc = NextFreeCc(mappings, channel);
        mapping.gestureIx = NextFreeGestureIx(mappings);
        rowToAppend.kind = RowKind::Individual;
        rowToAppend.group = group;
        rowToAppend.data = mapping;
    } else if (section == MidiConfigSection::Analogs && group == RowGroup::AnalogAppAction) {
        if (AnalogActionCatalog().empty()) {
            if (reason != nullptr) {
                *reason = "no analog app actions in this app's catalog";
            }
            return false;
        }
        if (!visibleSlot.config.analogInput.has_value()) {
            visibleSlot.config.analogInput = AnalogMidiInConfig{};
        }
        const std::vector<AnalogMidiMapping>& gestures = visibleSlot.config.analogInput->gestures;
        const std::vector<AnalogAppActionMapping>& appActions = visibleSlot.config.analogInput->appActions;
        const std::uint8_t channel = !gestures.empty()      ? gestures.front().control.channel
                                     : !appActions.empty() ? appActions.front().control.channel
                                                            : std::uint8_t{0};
        const UISystemMessageChoice& choice = AnalogActionCatalog().front();
        AnalogAppActionMapping mapping;
        mapping.control.channel = channel;
        mapping.control.cc = NextFreeCc(gestures, appActions, channel);
        mapping.appAction = choice.appAction;
        mapping.appActionValue = choice.appActionValue;
        mapping.appActionIx = choice.appActionIx;
        rowToAppend.kind = RowKind::Individual;
        rowToAppend.group = group;
        rowToAppend.data = mapping;
    } else if (section == MidiConfigSection::SystemMessages && group == RowGroup::Grid) {
        GridButton button;
        button.kind = visibleSlot.kind;
        button.gridSlotIx = 0;
        button.outputFeedback = true;
        if (visibleSlot.kind == MidiProfileKind::WrldBldr) {
            button.channel = visibleSlot.config.systemMessages.empty()
                                 ? std::uint8_t{5}
                                 : (visibleSlot.config.systemMessages.front().control.has_value()
                                        ? visibleSlot.config.systemMessages.front().control->channel
                                        : std::uint8_t{5});
            const auto position = NextFreeWrldBldrGridPosition(
                visibleSlot.config.systemMessages, visibleSlot.config.pressureInput, button.channel);
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free WRLD.Bldr grid position for a new grid button";
                }
                return false;
            }
            button.x = position->first;
            button.y = position->second;
        } else if (visibleSlot.kind == MidiProfileKind::Launchpad) {
            const LaunchpadController controller = CurrentLaunchpadVariant(visibleSlot.config.systemMessages);
            const auto position = NextFreeLaunchpadGridPair(
                visibleSlot.config.systemMessages, visibleSlot.config.pressureInput, controller,
                /*pair=*/false);
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free launchpad grid position for a new grid button";
                }
                return false;
            }
            button.launchpadController = controller;
            button.x = position->x;
            button.y = position->y;
        } else {
            if (reason != nullptr) {
                *reason = "grid mappings require WRLD.Bldr or Launchpad";
            }
            return false;
        }
        GridMappingExpansion expansion;
        if (!ExpandGridButton(button, expansion, reason)) {
            return false;
        }
        rowToAppend.kind = RowKind::Individual;
        rowToAppend.group = group;
        rowToAppend.data = button;
    } else if (section == MidiConfigSection::SystemMessages && group == RowGroup::System) {
        const std::size_t sceneIx = NextFreeSystemArg(visibleSlot.config.systemMessages, BlockableMessage::SceneSelect);
        MidiControllerSystemMessageAssociation association;
        association.press = MessageIn::SceneSelect(0, sceneIx);
        association.feedback = association.press;
        association.outputFeedback = true;

        switch (visibleSlot.kind) {
            case MidiProfileKind::WrldBldr: {
                const auto position = NextFreeWrldBldrPosition(visibleSlot.config.systemMessages);
                if (!position.has_value()) {
                    if (reason != nullptr) {
                        *reason = "no free WRLD.Bldr grid position for a new system row";
                    }
                    return false;
                }
                const std::uint8_t channel = visibleSlot.config.systemMessages.empty()
                                                 ? std::uint8_t{5}
                                                 : (visibleSlot.config.systemMessages.front().control.has_value()
                                                       ? visibleSlot.config.systemMessages.front().control->channel
                                                       : std::uint8_t{5});
                association.wrldBldrPosition =
                    WrldBldrSystemPosition{.channel = channel, .x = position->first, .y = position->second};
                association.control = MidiControlAddress{.channel = channel,
                                                          .cc = WrldBldrPositionToCC(position->first, position->second)};
                break;
            }
            case MidiProfileKind::Launchpad: {
                const auto position = NextFreeLaunchpadPosition(
                    visibleSlot.config.systemMessages, CurrentLaunchpadVariant(visibleSlot.config.systemMessages));
                if (!position.has_value()) {
                    if (reason != nullptr) {
                        *reason = "no free launchpad grid position for a new system row";
                    }
                    return false;
                }
                association.launchpadPosition = *position;
                break;
            }
            case MidiProfileKind::MfTwister: {
                const std::size_t button = NextFreeTwisterButton(visibleSlot.config.systemMessages);
                if (button >= 6) {
                    if (reason != nullptr) {
                        *reason = "no free twister side button for a new system row";
                    }
                    return false;
                }
                association.control =
                    MidiControlAddress{.channel = 3, .cc = static_cast<std::uint8_t>(8 + button)};
                break;
            }
            case MidiProfileKind::Generic: {
                const auto [channel, cc] = NextFreeGenericAddress(visibleSlot.config.systemMessages);
                association.control = MidiControlAddress{.channel = channel, .cc = cc};
                break;
            }
        }
        rowToAppend.kind = RowKind::Individual;
        rowToAppend.group = group;
        rowToAppend.data = association;
    } else {
        if (reason != nullptr) {
            *reason = "this group does not support adding individual rows";
        }
        return false;
    }

    const SectionPresentation rollback = presentation;
    const std::size_t insertAt = InsertionIndexForGroup(presentation, group);
    presentation.rows.insert(presentation.rows.begin() + static_cast<std::ptrdiff_t>(insertAt),
                             std::move(rowToAppend));

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot& slot = scratch.controllers[controllerIx];
    if (!FlushSectionPresentationToSlot(presentation, slot, section, reason)) {
        presentation = rollback;
        return false;
    }
    out = std::move(scratch);
    return true;
}

bool MidiConfigViewModel::AddBlock(std::size_t controllerIx, MidiConfigSection section,
                                   MidiMappingRowVM::RowGroup group, MidiInstrumentConfig& out,
                                   std::string* reason) const {
    if (controllerIx >= instrument_.controllers.size()) {
        if (reason != nullptr) {
            *reason = "controller index out of range";
        }
        return false;
    }
    if (!GroupSupportsAdd(controllerIx, section, group)) {
        if (reason != nullptr) {
            *reason = "this group does not support adding a block";
        }
        return false;
    }
    if (section == MidiConfigSection::SystemMessages && group == RowGroup::System &&
        instrument_.controllers[controllerIx].kind == MidiProfileKind::MfTwister) {
        if (reason != nullptr) {
            *reason = "twister system messages never block";
        }
        return false;
    }
    SectionPresentation& presentation = PresentationFor(controllerIx, section);
    MidiControllerSlot visibleSlot = instrument_.controllers[controllerIx];
    if (!FlushSectionPresentationToSlot(presentation, visibleSlot, section, reason)) {
        return false;
    }

    // Default block width (a small default run -- see this
    // method's header doc comment); large enough to demonstrate a block (>=2)
    // without presuming a huge free range exists.
    constexpr std::size_t kDefaultBlockWidth = 2;

    PresentationRow rowToAppend;
    if (section == MidiConfigSection::Encoders && (group == RowGroup::EncoderTurn || group == RowGroup::EncoderPush)) {
        if (!visibleSlot.config.encoderInput.has_value()) {
            visibleSlot.config.encoderInput = EncoderMidiInConfig{};
        }
        const bool isPush = group == RowGroup::EncoderPush;
        const std::vector<EncoderMidiMapping>& mappings =
            isPush ? visibleSlot.config.encoderInput->pushes : visibleSlot.config.encoderInput->turns;
        const std::uint8_t channel = mappings.empty() ? (isPush ? std::uint8_t{1} : std::uint8_t{0})
                                                       : mappings.front().control.channel;
        EncoderBlock block;
        block.isPush = isPush;
        block.channel = channel;
        block.startCc = NextFreeCc(mappings, channel);
        block.endCc = static_cast<std::uint8_t>(std::min<std::size_t>(
            static_cast<std::size_t>(block.startCc) + kDefaultBlockWidth, 128));
        block.slotIx = mappings.empty() ? 0 : mappings.front().slotIx;
        block.startPosition = NextFreeEncoderPosition(mappings);
        std::vector<EncoderMidiMapping> expansion;
        if (!ExpandEncoderBlock(block, expansion, reason)) {
            return false;
        }
        rowToAppend.kind = RowKind::Block;
        rowToAppend.group = group;
        rowToAppend.block = block;
    } else if (section == MidiConfigSection::Analogs && group == RowGroup::AnalogGesture) {
        if (!visibleSlot.config.analogInput.has_value()) {
            visibleSlot.config.analogInput = AnalogMidiInConfig{};
        }
        const std::vector<AnalogMidiMapping>& mappings = visibleSlot.config.analogInput->gestures;
        const std::uint8_t channel = mappings.empty() ? std::uint8_t{0} : mappings.front().control.channel;
        AnalogBlock block;
        block.channel = channel;
        block.startCc = NextFreeCc(mappings, channel);
        block.endCc = static_cast<std::uint8_t>(std::min<std::size_t>(
            static_cast<std::size_t>(block.startCc) + kDefaultBlockWidth, 128));
        block.startGestureIx = NextFreeGestureIx(mappings);
        std::vector<AnalogMidiMapping> expansion;
        if (!ExpandAnalogBlock(block, expansion, reason)) {
            return false;
        }
        rowToAppend.kind = RowKind::Block;
        rowToAppend.group = group;
        rowToAppend.block = block;
    } else if (section == MidiConfigSection::SystemMessages && group == RowGroup::Grid) {
        GridBlock block;
        block.kind = visibleSlot.kind;
        block.gridSlotIx = 0;
        block.outputFeedback = true;
        if (visibleSlot.kind == MidiProfileKind::WrldBldr) {
            block.channel = visibleSlot.config.systemMessages.empty()
                                ? std::uint8_t{5}
                                : (visibleSlot.config.systemMessages.front().control.has_value()
                                       ? visibleSlot.config.systemMessages.front().control->channel
                                       : std::uint8_t{5});
            const auto position = NextFreeWrldBldrGridPair(
                visibleSlot.config.systemMessages, visibleSlot.config.pressureInput, block.channel);
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free WRLD.Bldr two-cell range for a new grid block";
                }
                return false;
            }
            block.startX = position->first;
            block.startY = position->second;
        } else if (visibleSlot.kind == MidiProfileKind::Launchpad) {
            const LaunchpadController controller = CurrentLaunchpadVariant(visibleSlot.config.systemMessages);
            const auto position = NextFreeLaunchpadGridPair(
                visibleSlot.config.systemMessages, visibleSlot.config.pressureInput, controller,
                /*pair=*/true);
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free launchpad two-cell range for a new grid block";
                }
                return false;
            }
            block.launchpadController = controller;
            block.startX = position->x;
            block.startY = position->y;
        } else {
            if (reason != nullptr) {
                *reason = "grid mappings require WRLD.Bldr or Launchpad";
            }
            return false;
        }
        block.endX = block.startX + 2;
        block.endY = block.startY + 1;
        GridMappingExpansion expansion;
        if (!ExpandGridBlock(block, expansion, reason)) {
            return false;
        }
        rowToAppend.kind = RowKind::Block;
        rowToAppend.group = group;
        rowToAppend.block = block;
    } else if (section == MidiConfigSection::SystemMessages && group == RowGroup::System) {
        SystemBlock block;
        block.kind = visibleSlot.kind;
        block.message = BlockableMessage::SceneSelect;
        block.startArg = NextFreeSystemArg(visibleSlot.config.systemMessages, BlockableMessage::SceneSelect);
        block.outputFeedback = true;
        block.rowMajor = true;

        if (visibleSlot.kind == MidiProfileKind::WrldBldr) {
            const auto position = NextFreeWrldBldrPosition(visibleSlot.config.systemMessages);
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free WRLD.Bldr grid position for a new block";
                }
                return false;
            }
            block.channel = 5;
            block.startX = position->first;
            block.startY = position->second;
            // Exclusive ends: endX = min(8, startX + width) (8 = one past
            // the WrldBldr grid's max index 7); a single-row default block
            // uses y direction d = +1, so endY = startY + 1.
            block.endX = std::min(8, position->first + static_cast<int>(kDefaultBlockWidth));
            block.endY = position->second + 1;
        } else if (visibleSlot.kind == MidiProfileKind::Launchpad) {
            const auto position = NextFreeLaunchpadPosition(
                visibleSlot.config.systemMessages, CurrentLaunchpadVariant(visibleSlot.config.systemMessages));
            if (!position.has_value()) {
                if (reason != nullptr) {
                    *reason = "no free launchpad grid position for a new block";
                }
                return false;
            }
            block.launchpadController = position->controller;
            block.startX = position->x;
            block.startY = position->y;
            // Exclusive ends, no clamp (launchpad edge positions are legit,
            // x can reach 9); single-row default block uses d = +1.
            block.endX = position->x + static_cast<int>(kDefaultBlockWidth);
            block.endY = position->y + 1;
        } else {
            const auto [channel, cc] = NextFreeGenericAddress(visibleSlot.config.systemMessages);
            block.channel = channel;
            block.startCc = cc;
            block.endCc = static_cast<std::uint8_t>(
                std::min<std::size_t>(static_cast<std::size_t>(cc) + kDefaultBlockWidth, 128));
        }

        std::vector<MidiControllerSystemMessageAssociation> expansion;
        if (!ExpandSystemBlock(block, expansion, reason)) {
            return false;
        }
        rowToAppend.kind = RowKind::Block;
        rowToAppend.group = group;
        rowToAppend.block = block;
    } else {
        if (reason != nullptr) {
            *reason = "this group does not support adding a block";
        }
        return false;
    }

    const SectionPresentation rollback = presentation;
    const std::size_t insertAt = InsertionIndexForGroup(presentation, group);
    presentation.rows.insert(presentation.rows.begin() + static_cast<std::ptrdiff_t>(insertAt),
                             std::move(rowToAppend));

    MidiInstrumentConfig scratch = instrument_;
    MidiControllerSlot& slot = scratch.controllers[controllerIx];
    if (!FlushSectionPresentationToSlot(presentation, slot, section, reason)) {
        presentation = rollback;
        return false;
    }
    out = std::move(scratch);
    return true;
}

// The renderer's "+" gating used to reimplement
// this dispatch itself (SectionBody::AddableGroup in ControllersPage.hpp);
// that page-local copy could silently drift from AddSingle's actual switch
// above. This is the single source of truth both now share -- kept
// literally adjacent to AddSingle/AddBlock so a future group added to one
// dispatch is impossible to add to the other without touching this
// function too.
bool MidiConfigViewModel::GroupSupportsAdd(std::size_t controllerIx, MidiConfigSection section,
                                           MidiMappingRowVM::RowGroup group) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return false;
    }
    using RowGroup = MidiMappingRowVM::RowGroup;
    switch (group) {
        case RowGroup::EncoderTurn:
        case RowGroup::EncoderPush:
            return section == MidiConfigSection::Encoders;
        case RowGroup::AnalogGesture:
            return section == MidiConfigSection::Analogs;
        case RowGroup::AnalogAppAction:
            // An empty catalog (no analog-range app actions) leaves this
            // group with no legal target, so no add affordance for it --
            // matches AddSingle's own refusal for the same case.
            return section == MidiConfigSection::Analogs && !AnalogActionCatalog().empty();
        case RowGroup::System:
            return section == MidiConfigSection::SystemMessages;
        case RowGroup::Grid:
            return section == MidiConfigSection::SystemMessages &&
                   (instrument_.controllers[controllerIx].kind == MidiProfileKind::WrldBldr ||
                    instrument_.controllers[controllerIx].kind == MidiProfileKind::Launchpad);
        case RowGroup::EncoderMode:
        case RowGroup::EncoderStep:
        case RowGroup::AnalogSceneBlend:
            return false;
    }
    return false;
}

// The renderer's "+B" gating used to reimplement
// this dispatch itself (SectionBody::GroupSupportsBlocks in
// ControllersPage.hpp, including the twister no-block special case); this
// is now the single source of truth, mirroring AddBlock's own dispatch
// above (including its MfTwister refusal) so the two can never
// drift apart.
bool MidiConfigViewModel::GroupSupportsBlocks(std::size_t controllerIx, MidiConfigSection section,
                                              MidiMappingRowVM::RowGroup group) const {
    if (!GroupSupportsAdd(controllerIx, section, group)) {
        return false;
    }
    using RowGroup = MidiMappingRowVM::RowGroup;
    if (group == RowGroup::AnalogAppAction) {
        // Individual only: a gesture row can span a block of
        // consecutive addresses, but an app-action row's target is a single
        // catalog choice with no "block of choices" concept to expand into.
        return false;
    }
    if (section == MidiConfigSection::SystemMessages && group == RowGroup::System) {
        return instrument_.controllers[controllerIx].kind != MidiProfileKind::MfTwister;
    }
    return true;
}

std::vector<MidiMappingRowVM::RowGroup> MidiConfigViewModel::AddableGroups(std::size_t controllerIx,
                                                                          MidiConfigSection section) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return {};
    }
    // RowGroup's declaration order IS the canonical order (matches
    // InsertionIndexForGroup/SectionRows' own section-display order, per
    // this method's header doc comment) -- walking the enum in that order
    // and filtering through GroupSupportsAdd (the single dispatch source of
    // truth AddSingle/AddBlock themselves share) is the
    // only way to build this list, so it can never drift from what
    // GroupSupportsAdd itself would say for any individual group.
    static constexpr MidiMappingRowVM::RowGroup kCanonicalOrder[] = {
        MidiMappingRowVM::RowGroup::EncoderTurn,      MidiMappingRowVM::RowGroup::EncoderPush,
        MidiMappingRowVM::RowGroup::EncoderMode,      MidiMappingRowVM::RowGroup::EncoderStep,
        MidiMappingRowVM::RowGroup::AnalogGesture,    MidiMappingRowVM::RowGroup::AnalogAppAction,
        MidiMappingRowVM::RowGroup::AnalogSceneBlend, MidiMappingRowVM::RowGroup::System,
        MidiMappingRowVM::RowGroup::Grid,
    };
    std::vector<MidiMappingRowVM::RowGroup> groups;
    for (const MidiMappingRowVM::RowGroup group : kCanonicalOrder) {
        if (GroupSupportsAdd(controllerIx, section, group)) {
            groups.push_back(group);
        }
    }
    return groups;
}

std::vector<MidiMappingRowVM::Field> MidiConfigViewModel::GroupColumnFields(std::size_t controllerIx,
                                                                            MidiConfigSection section,
                                                                            MidiMappingRowVM::RowGroup group) const {
    if (controllerIx >= instrument_.controllers.size()) {
        return {};
    }
    using RowGroup = MidiMappingRowVM::RowGroup;
    // Same per-group field tables BuildSectionRows()' Individual-row branch
    // uses (EncoderBlockEditableFields/AnalogBlockEditableFields are the
    // BLOCK forms -- deliberately not reused here, since this method answers
    // "what would a fresh INDIVIDUAL row show," per its header doc comment).
    if (section == MidiConfigSection::Encoders &&
        (group == RowGroup::EncoderTurn || group == RowGroup::EncoderPush)) {
        if (group == RowGroup::EncoderPush) {
            return {Field::AddressType, Field::Channel, Field::Cc, Field::SlotIx, Field::Position};
        }
        return {Field::Channel, Field::Cc, Field::SlotIx, Field::Position};
    }
    if (section == MidiConfigSection::Analogs && group == RowGroup::AnalogGesture) {
        return {Field::Channel, Field::Cc, Field::GestureIx};
    }
    if (section == MidiConfigSection::Analogs && group == RowGroup::AnalogAppAction) {
        return {Field::Channel, Field::Cc, Field::AppAction};
    }
    if (section == MidiConfigSection::SystemMessages && group == RowGroup::System) {
        MidiControllerSystemMessageAssociation association;
        association.press = MessageIn::SceneSelect(0, 0);
        association.feedback = association.press;
        return SystemRowEditableFields(instrument_.controllers[controllerIx].kind, association);
    }
    if (section == MidiConfigSection::SystemMessages && group == RowGroup::Grid) {
        GridButton button;
        button.kind = instrument_.controllers[controllerIx].kind;
        return GridButtonEditableFields(button);
    }
    return {};
}

} // namespace synth
