#include "synth/MidiController.hpp"
#include "synth/ThreadId.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <string_view>
#include <utility>

namespace synth {

namespace {

std::uint8_t Clamp7Bit(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 127));
}

std::uint8_t FloatTo7Bit(float value) {
    return Clamp7Bit(static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 127.0f)));
}

constexpr double kAbsoluteEncoderCurveExponent = 1.011444814893185;
constexpr double kAbsoluteEncoderInverseCurveExponent = 1.0 / kAbsoluteEncoderCurveExponent;

float AbsoluteEncoderByteToNormalized(std::uint8_t value) {
    return static_cast<float>(std::pow(static_cast<double>(value) / 127.0, kAbsoluteEncoderCurveExponent));
}

std::uint8_t NormalizedToAbsoluteEncoderByte(float value) {
    return FloatTo7Bit(static_cast<float>(std::pow(std::clamp(value, 0.0f, 1.0f),
                                                    kAbsoluteEncoderInverseCurveExponent)));
}

std::uint8_t TwisterRgbBrightnessValue(float brightness) {
    return Clamp7Bit(static_cast<int>(std::lround(17.0f + std::clamp(brightness, 0.0f, 1.0f) * 30.0f)));
}

std::uint8_t TwisterRingBrightnessValue(float brightness) {
    return Clamp7Bit(static_cast<int>(std::lround(65.0f + std::clamp(brightness, 0.0f, 1.0f) * 30.0f)));
}

constexpr std::uint8_t kPrimaryPositionChannel = 0;
constexpr std::uint8_t kTwisterRgbColorChannel = 1;
constexpr std::uint8_t kTwisterRgbBrightnessChannel = 2;
constexpr std::uint8_t kTwisterRingBrightnessChannel = 5;
constexpr int kMidiInstrumentPreviousSchemaVersion = 1;

EncoderMidiInConfig RowMajorInputDefault(std::size_t slotIx) {
    EncoderMidiInConfig config;
    config.mode = EncoderMode::Signed7Bit;
    for (std::size_t position = 0; position < 16; ++position) {
        const std::uint8_t cc = EncoderPositionToCC(position);
        config.turns.push_back({.control = {.channel = 0, .cc = cc}, .slotIx = slotIx, .position = position});
        config.pushes.push_back({.control = {.channel = 1, .cc = cc}, .slotIx = slotIx, .position = position});
    }
    return config;
}

EncoderMidiOutConfig RowMajorOutputDefault(std::size_t slotIx) {
    EncoderMidiOutConfig config;
    for (std::size_t position = 0; position < 16; ++position) {
        config.mappings.push_back({.slotIx = slotIx, .position = position, .cc = EncoderPositionToCC(position)});
    }
    return config;
}

bool MappingIsFirstPosition(const EncoderMidiMapping& mapping, std::size_t count) {
    return mapping.position < count;
}

bool MappingIsFirstPosition(const EncoderMidiOutMapping& mapping, std::size_t count) {
    return mapping.position < count;
}

bool CacheNeedsResize(std::size_t size, std::size_t targetSize) {
    return size != targetSize;
}

std::optional<MidiControlAddress> MidiAddress(const BasicMidi& midi) {
    if (midi.IsCC()) {
        return MidiControlAddress{
            .channel = midi.Channel(),
            .cc = midi.GetCC(),
            .type = MidiControlType::Cc,
        };
    }
    if (midi.Size() >= 3 &&
        (midi.Status() == BasicMidi::kStatusNote || midi.Status() == BasicMidi::kStatusNoteOff)) {
        return MidiControlAddress{
            .channel = midi.Channel(),
            .cc = midi.GetNote(),
            .type = MidiControlType::Note,
        };
    }
    return std::nullopt;
}

bool IsObject(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Object;
}

bool ObjectHasKey(JSON json, const char* key) {
    if (!IsObject(json)) {
        return false;
    }
    const JsonContainer& container = json.m_node->m_container;
    const auto* members = static_cast<const JsonMember*>(container.m_entries);
    for (std::uint32_t ix = 0; ix < container.m_size; ++ix) {
        if (members[ix].m_key != nullptr && std::strcmp(members[ix].m_key, key) == 0) {
            return true;
        }
    }
    return false;
}

bool IsArray(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Array;
}

bool IsString(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::String;
}

bool IsInteger(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Integer;
}

bool IsNumber(JSON json) {
    return json.m_node != nullptr &&
           (json.m_node->m_type == JsonType::Integer || json.m_node->m_type == JsonType::Real);
}

bool IsBoolean(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Boolean;
}

bool ReadSize(JSON json, std::size_t& value) {
    if (!IsInteger(json) || json.IntegerValue() < 0) {
        return false;
    }
    value = static_cast<std::size_t>(json.IntegerValue());
    return true;
}

bool ReadU8(JSON json, std::uint8_t& value, std::uint8_t max = 0x7F) {
    if (!IsInteger(json) || json.IntegerValue() < 0 || json.IntegerValue() > max) {
        return false;
    }
    value = static_cast<std::uint8_t>(json.IntegerValue());
    return true;
}

bool ReadInt(JSON json, int& value) {
    if (!IsInteger(json)) {
        return false;
    }
    const std::int64_t integer = json.IntegerValue();
    if (integer < static_cast<std::int64_t>(std::numeric_limits<int>::min()) ||
        integer > static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    value = static_cast<int>(integer);
    return true;
}

bool ReadFloat(JSON json, float& value) {
    if (!IsNumber(json)) {
        return false;
    }
    value = static_cast<float>(json.NumberValue());
    return true;
}

bool ReadBool(JSON json, bool& value) {
    if (!IsBoolean(json)) {
        return false;
    }
    value = json.BooleanValue();
    return true;
}

const char* MessageTypeName(MessageIn::Type type) {
    switch (type) {
    case MessageIn::Type::ParamIncDec:
        return "paramIncDec";
    case MessageIn::Type::ParamSetAbsolute:
        return "paramSetAbsolute";
    case MessageIn::Type::ParamSetAbsoluteOnBank:
        return "paramSetAbsoluteOnBank";
    case MessageIn::Type::ParamPush:
        return "paramPush";
    case MessageIn::Type::ToggleReset:
        return "toggleReset";
    case MessageIn::Type::ToggleRandom:
        return "toggleRandom";
    case MessageIn::Type::ToggleRandomMod:
        return "toggleRandomMod";
    case MessageIn::Type::ToggleGestureSelect:
        return "toggleGestureSelect";
    case MessageIn::Type::SetGestureSelect:
        return "setGestureSelect";
    case MessageIn::Type::SelectParamBank:
        return "selectParamBank";
    case MessageIn::Type::NextParamBank:
        return "nextParamBank";
    case MessageIn::Type::PrevParamBank:
        return "prevParamBank";
    case MessageIn::Type::Start:
        return "start";
    case MessageIn::Type::Continue:
        return "continue";
    case MessageIn::Type::Stop:
        return "stop";
    case MessageIn::Type::Clock:
        return "clock";
    case MessageIn::Type::SetGestureValue:
        return "setGestureValue";
    case MessageIn::Type::SceneSelect:
        return "sceneSelect";
    case MessageIn::Type::SetSceneBlend:
        return "setSceneBlend";
    case MessageIn::Type::GridPress:
        return "gridPress";
    case MessageIn::Type::GridRelease:
        return "gridRelease";
    case MessageIn::Type::GridPressureChange:
        return "gridPressureChange";
    case MessageIn::Type::SelectGrid:
        return "selectGrid";
    case MessageIn::Type::AppAction:
        return "appAction";
    case MessageIn::Type::HoldDrill:
        return "holdDrill";
    }
    return "clock";
}

bool ParseMessageType(std::string_view value, MessageIn::Type& type) {
    if (value == "paramIncDec") {
        type = MessageIn::Type::ParamIncDec;
    } else if (value == "paramSetAbsolute") {
        type = MessageIn::Type::ParamSetAbsolute;
    } else if (value == "paramSetAbsoluteOnBank") {
        type = MessageIn::Type::ParamSetAbsoluteOnBank;
    } else if (value == "paramPush") {
        type = MessageIn::Type::ParamPush;
    } else if (value == "toggleReset" || value == "toggleShift" || value == "setReset" || value == "setShift") {
        type = MessageIn::Type::ToggleReset;
    } else if (value == "toggleRandom") {
        type = MessageIn::Type::ToggleRandom;
    } else if (value == "toggleRandomMod") {
        type = MessageIn::Type::ToggleRandomMod;
    } else if (value == "toggleGestureSelect") {
        type = MessageIn::Type::ToggleGestureSelect;
    } else if (value == "setGestureSelect") {
        type = MessageIn::Type::SetGestureSelect;
    } else if (value == "selectParamBank") {
        type = MessageIn::Type::SelectParamBank;
    } else if (value == "nextParamBank") {
        type = MessageIn::Type::NextParamBank;
    } else if (value == "prevParamBank") {
        type = MessageIn::Type::PrevParamBank;
    } else if (value == "start") {
        type = MessageIn::Type::Start;
    } else if (value == "continue") {
        type = MessageIn::Type::Continue;
    } else if (value == "stop") {
        type = MessageIn::Type::Stop;
    } else if (value == "clock") {
        type = MessageIn::Type::Clock;
    } else if (value == "setGestureValue") {
        type = MessageIn::Type::SetGestureValue;
    } else if (value == "sceneSelect") {
        type = MessageIn::Type::SceneSelect;
    } else if (value == "setSceneBlend") {
        type = MessageIn::Type::SetSceneBlend;
    } else if (value == "gridPress") {
        type = MessageIn::Type::GridPress;
    } else if (value == "gridRelease") {
        type = MessageIn::Type::GridRelease;
    } else if (value == "gridPressureChange") {
        type = MessageIn::Type::GridPressureChange;
    } else if (value == "selectGrid") {
        type = MessageIn::Type::SelectGrid;
    } else if (value == "appAction") {
        type = MessageIn::Type::AppAction;
    } else if (value == "holdDrill") {
        type = MessageIn::Type::HoldDrill;
    } else {
        return false;
    }
    return true;
}

void WriteMessageOrigin(JsonArena& arena, JSON json, const MessageIn& value) {
    if (value.origin == MessageIn::Origin::ExternalMidi) {
        json.SetNew("origin", arena.String("externalMidi"));
        json.SetNew("externalControllerSlot",
                    arena.Integer(static_cast<std::int64_t>(value.externalControllerSlot)));
    }
}

bool ReadMessageOrigin(JSON json, MessageIn& value) {
    const JSON origin = json.Get("origin");
    const bool hasExternalSlot = ObjectHasKey(json, "externalControllerSlot");
    if (origin.IsNull()) {
        return !hasExternalSlot;
    }
    if (!IsString(origin)) {
        return false;
    }
    const std::string_view name(origin.StringValue());
    if (name == "internal") {
        return !hasExternalSlot;
    }
    if (name != "externalMidi" || !hasExternalSlot ||
        !ReadSize(json.Get("externalControllerSlot"), value.externalControllerSlot)) {
        return false;
    }
    value.origin = MessageIn::Origin::ExternalMidi;
    return true;
}

const char* EncoderMidiOutProtocolName(EncoderMidiOutProtocol protocol) {
    switch (protocol) {
    case EncoderMidiOutProtocol::WrldBldr:
        return "wrldBldr";
    case EncoderMidiOutProtocol::Twister:
        return "twister";
    }
    return "wrldBldr";
}

bool ParseEncoderMidiOutProtocol(std::string_view value, EncoderMidiOutProtocol& protocol) {
    if (value == "wrldBldr") {
        protocol = EncoderMidiOutProtocol::WrldBldr;
        return true;
    }
    if (value == "twister") {
        protocol = EncoderMidiOutProtocol::Twister;
        return true;
    }
    return false;
}

template <typename T>
JSON VectorToJSON(JsonArena& arena, const std::vector<T>& values) {
    JSON array = arena.Array();
    for (const T& value : values) {
        array.AppendNew(ToJSON(arena, value));
    }
    return array;
}

template <typename T>
bool VectorFromJSON(JSON json, std::vector<T>& values) {
    if (!IsArray(json)) {
        return false;
    }
    std::vector<T> parsed;
    parsed.reserve(json.Size());
    for (std::size_t ix = 0; ix < json.Size(); ++ix) {
        T value;
        if (!FromJSON(json.GetAt(ix), value)) {
            return false;
        }
        parsed.push_back(std::move(value));
    }
    values = std::move(parsed);
    return true;
}

} // namespace

BasicMidi::BasicMidi(std::uint64_t newTimestamp, std::uint8_t status, std::uint8_t data1, std::uint8_t data2)
    : timestamp(newTimestamp),
      raw{status, data1, data2} {}

BasicMidi::BasicMidi(std::uint64_t newTimestamp, std::vector<std::uint8_t> bytes)
    : timestamp(newTimestamp),
      raw(std::move(bytes)) {}

BasicMidi BasicMidi::CC(std::uint64_t timestamp, std::uint8_t channel, std::uint8_t cc, std::uint8_t value) {
    return BasicMidi(timestamp, static_cast<std::uint8_t>(kStatusCC | (channel & 0x0F)), cc & 0x7F, value & 0x7F);
}

BasicMidi BasicMidi::Note(std::uint64_t timestamp, std::uint8_t channel, std::uint8_t note, std::uint8_t velocity) {
    const std::uint8_t status = velocity == 0 ? kStatusNoteOff : kStatusNote;
    return BasicMidi(timestamp, static_cast<std::uint8_t>(status | (channel & 0x0F)), note & 0x7F, velocity & 0x7F);
}

BasicMidi BasicMidi::NoteOff(std::uint64_t timestamp, std::uint8_t channel, std::uint8_t note) {
    return BasicMidi(timestamp, static_cast<std::uint8_t>(kStatusNoteOff | (channel & 0x0F)), note & 0x7F, 0);
}

BasicMidi BasicMidi::PolyPressure(std::uint64_t timestamp, std::uint8_t channel,
                                  std::uint8_t note, std::uint8_t pressure) {
    return BasicMidi(timestamp, static_cast<std::uint8_t>(kStatusPolyPressure | (channel & 0x0F)),
                     note & 0x7F, pressure & 0x7F);
}

BasicMidi BasicMidi::PitchBend(std::uint64_t timestamp, std::uint8_t channel, std::uint16_t value) {
    const std::uint16_t clamped = std::min<std::uint16_t>(value, 0x3FFF);
    return BasicMidi(timestamp, static_cast<std::uint8_t>(kStatusPitchBend | (channel & 0x0F)),
                     static_cast<std::uint8_t>(clamped & 0x7F),
                     static_cast<std::uint8_t>((clamped >> 7) & 0x7F));
}

BasicMidi BasicMidi::Realtime(std::uint64_t timestamp, std::uint8_t status) {
    if (!IsSupportedRealtimeStatus(status)) {
        return {};
    }
    return BasicMidi(timestamp, std::vector<std::uint8_t>{status});
}

BasicMidi BasicMidi::Clock(std::uint64_t timestamp) {
    return Realtime(timestamp, kStatusClock);
}

BasicMidi BasicMidi::TransportStart(std::uint64_t timestamp) {
    return Realtime(timestamp, kStatusTransportStart);
}

BasicMidi BasicMidi::TransportContinue(std::uint64_t timestamp) {
    return Realtime(timestamp, kStatusTransportContinue);
}

BasicMidi BasicMidi::TransportStop(std::uint64_t timestamp) {
    return Realtime(timestamp, kStatusTransportStop);
}

BasicMidi BasicMidi::SysEx(std::uint64_t timestamp, std::vector<std::uint8_t> bytes) {
    if (bytes.empty() || bytes.front() != 0xF0) {
        bytes.insert(bytes.begin(), 0xF0);
    }
    if (bytes.back() != 0xF7) {
        bytes.push_back(0xF7);
    }
    return BasicMidi(timestamp, std::move(bytes));
}

bool BasicMidi::IsSupportedRealtimeStatus(std::uint8_t status) {
    return status == kStatusClock || status == kStatusTransportStart || status == kStatusTransportContinue ||
           status == kStatusTransportStop;
}

std::uint8_t BasicMidi::Status() const {
    if (raw.empty()) {
        return 0;
    }
    if (raw[0] >= 0xF0) {
        return raw[0];
    }
    return raw[0] & 0xF0;
}

std::uint8_t BasicMidi::Channel() const {
    return raw.empty() || raw[0] >= 0xF0 ? 0 : static_cast<std::uint8_t>(raw[0] & 0x0F);
}

std::uint8_t BasicMidi::GetCC() const {
    return raw.size() > 1 ? raw[1] : 0;
}

std::uint8_t BasicMidi::GetNote() const {
    return raw.size() > 1 ? raw[1] : 0;
}

std::uint8_t BasicMidi::GetPressure() const {
    return raw.size() > 2 ? raw[2] : 0;
}

std::uint8_t BasicMidi::GetValue() const {
    return raw.size() > 2 ? raw[2] : 0;
}

std::uint16_t BasicMidi::GetPitchBend() const {
    if (raw.size() < 3) {
        return 0;
    }
    return static_cast<std::uint16_t>((raw[1] & 0x7F) | ((raw[2] & 0x7F) << 7));
}

MidiInProcessor::MidiInProcessor(MessageInBus* bus)
    : bus_(bus) {}

std::uint64_t MidiInProcessor::NextTimestamp() const {
    return timestampProvider_ == nullptr ? 0 : timestampProvider_();
}

bool MidiInProcessor::Push(const MessageIn& message) {
    return bus_ != nullptr && bus_->Push(message);
}

void MidiInProcessor::PassToThru(const BasicMidi& midi) {
    if (thru_ != nullptr) {
        thru_->Process(midi);
    }
}

void RealtimeMidiInProcessor::Process(const BasicMidi& midi) {
    if (midi.raw.size() != 1 || !BasicMidi::IsSupportedRealtimeStatus(midi.raw.front())) {
        PassToThru(midi);
        return;
    }

    MessageIn message;
    switch (midi.raw.front()) {
    case BasicMidi::kStatusClock:
        message = MessageIn::Clock(midi.timestamp, MessageIn::Origin::ExternalMidi, controllerSlot_);
        break;
    case BasicMidi::kStatusTransportStart:
        message = MessageIn::Start(midi.timestamp, MessageIn::Origin::ExternalMidi, controllerSlot_);
        break;
    case BasicMidi::kStatusTransportContinue:
        message = MessageIn::Continue(midi.timestamp, MessageIn::Origin::ExternalMidi, controllerSlot_);
        break;
    case BasicMidi::kStatusTransportStop:
        message = MessageIn::Stop(midi.timestamp, MessageIn::Origin::ExternalMidi, controllerSlot_);
        break;
    default:
        PassToThru(midi);
        return;
    }
    (void)Push(message);
}

EncoderMidiInConfig EncoderMidiInConfig::TwisterDefault(std::size_t slotIx) {
    return RowMajorInputDefault(slotIx);
}

EncoderMidiInConfig EncoderMidiInConfig::WrldBldrDefault(std::size_t slotIx) {
    return RowMajorInputDefault(slotIx);
}

void EncoderMidiInConfig::KeepFirstPositions(std::size_t count) {
    std::erase_if(turns, [count](const EncoderMidiMapping& mapping) { return !MappingIsFirstPosition(mapping, count); });
    std::erase_if(pushes, [count](const EncoderMidiMapping& mapping) { return !MappingIsFirstPosition(mapping, count); });
}

std::size_t AbsoluteFeedbackCoordinator::RouteHash(RouteKey key) {
    std::size_t hash = key.controllerSlot;
    hash ^= key.parameterSlot + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    hash ^= key.position + 0x9e3779b9U + (hash << 6U) + (hash >> 2U);
    return hash;
}

AbsoluteFeedbackCoordinator::RouteReservation AbsoluteFeedbackCoordinator::ReserveRoute(RouteKey key) {
    while (reservationGuard_.test_and_set(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    const std::size_t start = RouteHash(key) % kMaxRoutes;
    for (std::size_t probe = 0; probe < kMaxRoutes; ++probe) {
        const std::size_t index = (start + probe) % kMaxRoutes;
        RouteRecord& record = routes_[index];
        if (record.occupied && record.key == key) {
            reservationGuard_.clear(std::memory_order_release);
            return RouteReservation(index);
        }
        if (!record.occupied) {
            record.key = key;
            record.occupied = true;
            reservationGuard_.clear(std::memory_order_release);
            return RouteReservation(index);
        }
    }

    reservationGuard_.clear(std::memory_order_release);
    return RouteReservation(kMaxRoutes);
}

std::uint64_t AbsoluteFeedbackCoordinator::AllocateEpoch() {
    const std::uint64_t epoch = nextEpoch_.fetch_add(1, std::memory_order_relaxed);
    assert(epoch != 0 && epoch != std::numeric_limits<std::uint64_t>::max());
    return epoch;
}

AbsoluteFeedbackCoordinator::RouteGuard::RouteGuard(RouteRecord* record)
    : record_(record) {
    if (record_ != nullptr) {
        while (record_->guard.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
}

AbsoluteFeedbackCoordinator::RouteGuard::~RouteGuard() {
    if (record_ != nullptr) {
        record_->guard.clear(std::memory_order_release);
    }
}

AbsoluteFeedbackCoordinator::Expectation AbsoluteFeedbackCoordinator::RouteGuard::Snapshot() const {
    return record_ == nullptr ? Expectation{} : record_->expectation;
}

void AbsoluteFeedbackCoordinator::RouteGuard::Publish(std::uint64_t epoch, std::uint8_t receivedValue) {
    assert(record_ != nullptr && epoch != 0);
    record_->expectation = {.epoch = epoch, .receivedValue = receivedValue, .pending = true};
}

void AbsoluteFeedbackCoordinator::RouteGuard::Restore(const Expectation& expectation) {
    assert(record_ != nullptr);
    record_->expectation = expectation;
}

bool AbsoluteFeedbackCoordinator::RouteGuard::Resolve(std::uint64_t expectedEpoch) {
    if (record_ == nullptr || !record_->expectation.pending || record_->expectation.epoch != expectedEpoch) {
        return false;
    }
    record_->expectation.pending = false;
    return true;
}

AbsoluteFeedbackCoordinator::RouteGuard
AbsoluteFeedbackCoordinator::GuardRoute(RouteReservation route) {
    if (!route.IsTracked() || !routes_[route.index_].occupied) {
        return RouteGuard(nullptr);
    }
    return RouteGuard(&routes_[route.index_]);
}

bool AbsoluteFeedbackCoordinator::RouteGuardHeldForTests(RouteReservation route) const {
    return route.IsTracked() && routes_[route.index_].occupied &&
           routes_[route.index_].guard.test(std::memory_order_acquire);
}

AbsoluteFeedbackCoordinator::InputAlert
AbsoluteFeedbackCoordinator::BeginInput(RouteReservation route, std::uint8_t receivedValue) {
    InputAlert alert;
    if (!route.IsTracked()) {
        return alert;
    }

    auto guard = GuardRoute(route);
    if (!guard.IsTracked()) {
        return alert;
    }
    // Epoch allocation is part of the per-route linearization point. This
    // prevents two same-route callbacks from publishing epochs out of order.
    const std::uint64_t epoch = AllocateEpoch();
    alert.route_ = route;
    alert.previous_ = guard.Snapshot();
    alert.epoch_ = epoch;
    alert.published_ = true;
    guard.Publish(epoch, receivedValue);
    return alert;
}

bool AbsoluteFeedbackCoordinator::Rollback(const InputAlert& alert) {
    if (!alert.IsPublished()) {
        return false;
    }
    auto guard = GuardRoute(alert.route_);
    if (!guard.IsTracked() || guard.Snapshot().epoch != alert.epoch_) {
        return false;
    }
    guard.Restore(alert.previous_);
    return true;
}

std::optional<AbsoluteFeedbackCoordinator::Expectation>
AbsoluteFeedbackCoordinator::Snapshot(RouteReservation route) {
    auto guard = GuardRoute(route);
    if (!guard.IsTracked()) {
        return std::nullopt;
    }
    return guard.Snapshot();
}

EncoderMidiInProcessor::EncoderMidiInProcessor(EncoderMidiInConfig config, MessageInBus* bus,
                                               AbsoluteFeedbackCoordinator* absoluteFeedback,
                                               std::size_t controllerSlot,
                                               HoldDrillState* holdDrill)
    : MidiInProcessor(bus),
      config_(std::move(config)),
      absoluteFeedback_(absoluteFeedback),
      controllerSlot_(controllerSlot),
      holdDrill_(holdDrill) {
    ReserveAbsoluteRoutes();
}

void EncoderMidiInProcessor::SetConfig(EncoderMidiInConfig config) {
    config_ = std::move(config);
    ReserveAbsoluteRoutes();
}

void EncoderMidiInProcessor::ReserveAbsoluteRoutes() {
    absoluteTurnRoutes_.clear();
    if (config_.mode != EncoderMode::Absolute || absoluteFeedback_ == nullptr) {
        return;
    }
    absoluteTurnRoutes_.reserve(config_.turns.size());
    for (const EncoderMidiMapping& mapping : config_.turns) {
        absoluteTurnRoutes_.push_back(absoluteFeedback_->ReserveRoute({
            .controllerSlot = controllerSlot_,
            .parameterSlot = mapping.slotIx,
            .position = mapping.position,
        }));
    }
}

void EncoderMidiInProcessor::Process(const BasicMidi& midi) {
    if (midi.IsCC()) {
        if (const EncoderMidiMapping* mapping = FindTurn(midi)) {
            if (holdDrill_ != nullptr && holdDrill_->held) {
                const std::size_t mappingIx = static_cast<std::size_t>(mapping - config_.turns.data());
                if (holdDrill_->drilled.size() != config_.turns.size()) {
                    holdDrill_->drilled.assign(config_.turns.size(), false);
                }
                if (!holdDrill_->drilled[mappingIx]) {
                    holdDrill_->drilled[mappingIx] = true;
                    Push(MessageIn::ParamPush(NextTimestamp(), mapping->slotIx, mapping->position));
                }
                return;
            }
            if (config_.mode == EncoderMode::Absolute) {
                if (absoluteFeedback_ == nullptr) {
                    Push(MessageIn::ParamSetAbsolute(NextTimestamp(), mapping->slotIx, mapping->position,
                                                     AbsoluteEncoderByteToNormalized(midi.GetValue())));
                    return;
                }
                const std::size_t mappingIx = static_cast<std::size_t>(mapping - config_.turns.data());
                const auto route = absoluteTurnRoutes_[mappingIx];
                if (!route.IsTracked()) {
                    return;
                }
                const auto alert = absoluteFeedback_->BeginInput(route, midi.GetValue());
                if (!alert.IsPublished()) {
                    // A tracked absolute turn must never become visible to DSP
                    // without its matching output expectation.
                    return;
                }
                const bool pushed = Push(MessageIn::ParamSetAbsolute(
                    NextTimestamp(), mapping->slotIx, mapping->position,
                    AbsoluteEncoderByteToNormalized(midi.GetValue()), alert.Epoch()));
                if (!pushed) {
                    absoluteFeedback_->Rollback(alert);
                }
                return;
            }
            if (const std::optional<float> delta = DecodeDelta(midi.GetValue())) {
                Push(MessageIn::ParamIncDec(NextTimestamp(), mapping->slotIx, mapping->position, *delta));
            }
            return;
        }
    }

    if (const EncoderMidiMapping* mapping = FindPush(midi)) {
        const bool isPress = midi.IsCC() ? midi.GetValue() != 0
                                        : midi.Status() == BasicMidi::kStatusNote && midi.GetValue() > 0;
        if (isPress) {
            Push(MessageIn::ParamPush(NextTimestamp(), mapping->slotIx, mapping->position));
        }
        return;
    }

    PassToThru(midi);
}

const EncoderMidiMapping* EncoderMidiInProcessor::FindTurn(const BasicMidi& midi) const {
    const std::optional<MidiControlAddress> address = MidiAddress(midi);
    if (!address.has_value()) {
        return nullptr;
    }
    const auto itr = std::find_if(config_.turns.begin(), config_.turns.end(),
                                  [address](const EncoderMidiMapping& mapping) { return mapping.control == *address; });
    return itr == config_.turns.end() ? nullptr : &*itr;
}

const EncoderMidiMapping* EncoderMidiInProcessor::FindPush(const BasicMidi& midi) const {
    const std::optional<MidiControlAddress> address = MidiAddress(midi);
    if (!address.has_value()) {
        return nullptr;
    }
    const auto itr = std::find_if(config_.pushes.begin(), config_.pushes.end(),
                                  [address](const EncoderMidiMapping& mapping) { return mapping.control == *address; });
    return itr == config_.pushes.end() ? nullptr : &*itr;
}

std::optional<float> EncoderMidiInProcessor::DecodeDelta(std::uint8_t value) const {
    int ticks = 0;
    switch (config_.mode) {
    case EncoderMode::Signed7Bit:
        ticks = static_cast<int>(value) - 64;
        break;
    case EncoderMode::DirectionOnly:
        if (value > 64) {
            ticks = 1;
        } else if (value < 64) {
            ticks = -1;
        }
        break;
    case EncoderMode::Absolute:
        return std::nullopt;
    }
    if (ticks == 0) {
        return std::nullopt;
    }
    return static_cast<float>(ticks) * config_.turnStep;
}

namespace {

template <typename Mapping>
const Mapping* FindByControl(const std::vector<Mapping>& mappings, const BasicMidi& midi) {
    const MidiControlAddress address{.channel = midi.Channel(), .cc = midi.GetCC()};
    const auto itr = std::find_if(mappings.begin(), mappings.end(),
                                  [address](const Mapping& mapping) { return mapping.control == address; });
    return itr == mappings.end() ? nullptr : &*itr;
}

}  // namespace

AnalogMidiInProcessor::AnalogMidiInProcessor(AnalogMidiInConfig config, MessageInBus* bus)
    : MidiInProcessor(bus),
      config_(std::move(config)) {}

void AnalogMidiInProcessor::SetConfig(AnalogMidiInConfig config) {
    config_ = std::move(config);
}

void AnalogMidiInProcessor::Process(const BasicMidi& midi) {
    if (!midi.IsCC()) {
        PassToThru(midi);
        return;
    }

    const float normalized = static_cast<float>(midi.GetValue()) / 127.0f;
    if (const AnalogMidiMapping* mapping = FindGesture(midi)) {
        Push(MessageIn::SetGestureValue(NextTimestamp(), mapping->gestureIx, normalized));
        return;
    }

    if (const AnalogAppActionMapping* mapping = FindAppAction(midi)) {
        Push(MessageIn::AppAction(NextTimestamp(), mapping->appActionIx, normalized));
        return;
    }

    const MidiControlAddress address{.channel = midi.Channel(), .cc = midi.GetCC()};
    if (config_.sceneBlend.has_value() && *config_.sceneBlend == address) {
        Push(MessageIn::SetSceneBlend(NextTimestamp(), normalized));
        return;
    }

    PassToThru(midi);
}

const AnalogMidiMapping* AnalogMidiInProcessor::FindGesture(const BasicMidi& midi) const {
    return FindByControl(config_.gestures, midi);
}

const AnalogAppActionMapping* AnalogMidiInProcessor::FindAppAction(const BasicMidi& midi) const {
    return FindByControl(config_.appActions, midi);
}

namespace {

const char* PolyphonicPressureConfigError(const PolyphonicPressureMidiInConfig& config) {
    for (std::size_t ix = 0; ix < config.mappings.size(); ++ix) {
        const PolyphonicPressureMapping& mapping = config.mappings[ix];
        if (mapping.address.channel > 0x0F || mapping.address.note > 0x7F) {
            return "polyphonic-pressure addresses must use channel 0-15 and note 0-127";
        }
        if (mapping.pressure.type != MessageIn::Type::GridPressureChange) {
            return "polyphonic-pressure targets must be grid pressure-change messages";
        }
        for (std::size_t prior = 0; prior < ix; ++prior) {
            if (config.mappings[prior].address == mapping.address) {
                return "polyphonic-pressure addresses must be unique";
            }
        }
    }
    return nullptr;
}

} // namespace

PolyphonicPressureMidiInProcessor::PolyphonicPressureMidiInProcessor(
    PolyphonicPressureMidiInConfig config, MessageInBus* bus)
    : MidiInProcessor(bus) {
    SetConfig(std::move(config));
}

bool PolyphonicPressureMidiInProcessor::SetConfig(PolyphonicPressureMidiInConfig config) {
    if (PolyphonicPressureConfigError(config) != nullptr) {
        return false;
    }
    config_ = std::move(config);
    return true;
}

void PolyphonicPressureMidiInProcessor::Process(const BasicMidi& midi) {
    if (!midi.IsPolyPressure()) {
        PassToThru(midi);
        return;
    }

    const MidiNoteAddress address{.channel = midi.Channel(), .note = midi.GetNote()};
    const auto itr = std::find_if(config_.mappings.begin(), config_.mappings.end(),
                                  [address](const PolyphonicPressureMapping& mapping) {
                                      return mapping.address == address;
                                  });
    if (itr == config_.mappings.end()) {
        PassToThru(midi);
        return;
    }

    MessageIn pressure = itr->pressure;
    pressure.timestamp = NextTimestamp();
    pressure.velocity = midi.GetPressure();
    Push(pressure);
}

SystemButtonMidiInProcessor::SystemButtonMidiInProcessor(SystemButtonMidiInConfig config, MessageInBus* bus,
                                                          HoldDrillState* holdDrill)
    : MidiInProcessor(bus),
      config_(std::move(config)),
      holdDrill_(holdDrill) {}

void SystemButtonMidiInProcessor::SetConfig(SystemButtonMidiInConfig config) {
    config_ = std::move(config);
}

void SystemButtonMidiInProcessor::Process(const BasicMidi& midi) {
    const std::optional<MidiControlAddress> address = MidiAddress(midi);
    if (!address.has_value()) {
        PassToThru(midi);
        return;
    }

    const SystemButtonMidiAssociation* association = FindAssociation(midi);
    if (association == nullptr) {
        PassToThru(midi);
        return;
    }

    const bool isPress = midi.IsCC() ? midi.GetValue() > 0
                                    : midi.Status() == BasicMidi::kStatusNote && midi.GetValue() > 0;

    if (association->press.type == MessageIn::Type::HoldDrill) {
        // Hold Drill state lives in the profile's input chain, never the bus:
        // a knob turned while held is drilled instead of moved (see
        // EncoderMidiInProcessor::Process), so neither edge is pushed here.
        if (holdDrill_ == nullptr) {
            return;
        }
        if (isPress) {
            holdDrill_->held = true;
            holdDrill_->drilled.clear();
        } else {
            holdDrill_->held = false;
        }
        return;
    }

    if (isPress) {
        PushStamped(association->press);
        return;
    }

    if (association->release.has_value()) {
        PushStamped(*association->release);
    }
}

const SystemButtonMidiAssociation* SystemButtonMidiInProcessor::FindAssociation(const BasicMidi& midi) const {
    const std::optional<MidiControlAddress> address = MidiAddress(midi);

    for (const SystemButtonMidiAssociation& association : config_.associations) {
        if (address.has_value() && association.control.has_value() && *association.control == *address) {
            return &association;
        }
        if (address.has_value() && association.launchpadPosition.has_value()) {
            const LaunchpadGridPosition& position = *association.launchpadPosition;
            const std::optional<std::uint8_t> mapped =
                LaunchpadPositionToNote(position.controller, position.x, position.y);
            if (mapped.has_value() && *mapped == address->cc) {
                return &association;
            }
        }
    }
    return nullptr;
}

void SystemButtonMidiInProcessor::PushStamped(MessageIn message) {
    message.timestamp = NextTimestamp();
    Push(message);
}

MidiSender::MidiSender(std::size_t capacity, TimestampProvider timestampProvider)
    : queue_(capacity == 0 ? 1 : capacity),
      timestampProvider_(std::move(timestampProvider)) {
    if (!timestampProvider_) {
        timestampProvider_ = [] {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
}

MidiSender::~MidiSender() {
    Stop();
}

void MidiSender::SetSink(std::size_t sinkIx, IMidiOutputSink* sink) {
    if (sinkIx >= kMaxSinks) {
        return;
    }
    const MidiSchedulingCapability capability =
        sink == nullptr ? MidiSchedulingCapability::ImmediateOnly : sink->SchedulingCapability();
    const std::uint64_t schedulingLeadMicros =
        sink != nullptr && capability == MidiSchedulingCapability::HostTimestamped
            ? sink->SchedulingLeadMicros()
            : 0;
    {
        std::lock_guard lock(mutex_);
        sinks_[sinkIx] = sink;
        sinkCapabilities_[sinkIx] = capability;
        sinkScheduleLeadMicros_[sinkIx] = schedulingLeadMicros;
        ++sinkRegistrationGenerations_[sinkIx];
        ++sinkWakeGeneration_;
    }
    cv_.notify_all();
}

void MidiSender::ClearSinkSync(std::size_t sinkIx) {
    if (sinkIx >= kMaxSinks) {
        return;
    }
    std::unique_lock lock(mutex_);
    sinks_[sinkIx] = nullptr;
    sinkCapabilities_[sinkIx] = MidiSchedulingCapability::ImmediateOnly;
    sinkScheduleLeadMicros_[sinkIx] = 0;
    ++sinkRegistrationGenerations_[sinkIx];
    ++sinkWakeGeneration_;
    sendingCv_.wait(lock, [this, sinkIx] { return inFlightBySink_[sinkIx] == 0; });
    lock.unlock();
    cv_.notify_all();
}

void MidiSender::Start() {
    std::lock_guard lock(mutex_);
    if (running_) {
        return;
    }
    stopRequested_ = false;
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void MidiSender::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !thread_.joinable()) {
            realtimeRead_.store(realtimeWrite_.load(std::memory_order_acquire),
                                std::memory_order_release);
            return;
        }
        stopRequested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    running_ = false;
    stopRequested_ = false;
    head_ = 0;
    size_ = 0;
    pendingScheduledSize_ = 0;
    pendingScheduledCount_.store(0, std::memory_order_release);
    realtimeRead_.store(realtimeWrite_.load(std::memory_order_acquire),
                        std::memory_order_release);
    drainedCv_.notify_all();
}

bool MidiSender::Enqueue(std::size_t sinkIx, const BasicMidi& midi) {
    if (sinkIx >= kMaxSinks) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (size_ >= queue_.size()) {
        return false;
    }
    const std::size_t tail = (head_ + size_) % queue_.size();
    queue_[tail] = QueueEntry{
        .sinkIx = sinkIx,
        .midi = midi,
    };
    ++size_;
    cv_.notify_one();
    return true;
}

bool MidiSender::TryEnqueue(const ScheduledMidiEvent& event) noexcept {
    const std::uint64_t write = realtimeWrite_.load(std::memory_order_relaxed);
    const std::uint64_t read = realtimeRead_.load(std::memory_order_acquire);
    if (write - read >= kScheduledRealtimeCapacity) {
        producerOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    realtimeLane_[write % kScheduledRealtimeCapacity] = event;
    realtimeWrite_.store(write + 1, std::memory_order_release);
    cv_.notify_one();
    return true;
}

MidiSenderDiagnostics MidiSender::DiagnosticsSnapshot() const noexcept {
    return {
        .producerOverflowCount = producerOverflowCount_.load(std::memory_order_relaxed),
        .workerOverflowCount = workerOverflowCount_.load(std::memory_order_relaxed),
        .staleGenerationDropCount = staleGenerationDropCount_.load(std::memory_order_relaxed),
        .lateEventCount = lateEventCount_.load(std::memory_order_relaxed),
        .fallbackSendCount = fallbackSendCount_.load(std::memory_order_relaxed),
    };
}

bool MidiSender::IsRunning() const {
    std::lock_guard lock(mutex_);
    return running_;
}

bool MidiSender::FlushForTests(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return drainedCv_.wait_for(lock, timeout, [this] {
        return size_ == 0 && inFlight_ == 0 && RealtimeLaneEmpty() &&
               pendingScheduledCount_.load(std::memory_order_acquire) == 0;
    });
}

std::uint64_t MidiSender::NowMicros() const noexcept {
    // The constructor installs a steady_clock provider when none is injected,
    // so scheduled operation never silently falls into a non-advancing epoch.
    return timestampProvider_();
}

bool MidiSender::RealtimeLaneEmpty() const noexcept {
    return realtimeRead_.load(std::memory_order_acquire) ==
           realtimeWrite_.load(std::memory_order_acquire);
}

void MidiSender::ApplyGenerationCutoff(const ScheduledMidiEvent& cutoff) {
    std::size_t writeIx = 0;
    for (std::size_t readIx = 0; readIx < pendingScheduledSize_; ++readIx) {
        const ScheduledMidiEvent& pending = pendingScheduled_[readIx].event;
        const bool stale = pending.phaseGeneration == cutoff.invalidatedPhaseGeneration &&
                           pending.dueTimeMicros >= cutoff.phaseCutoffDueTimeMicros;
        if (stale) {
            staleGenerationDropCount_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (writeIx != readIx) {
            pendingScheduled_[writeIx] = pendingScheduled_[readIx];
        }
        ++writeIx;
    }
    pendingScheduledSize_ = writeIx;
    pendingScheduledCount_.store(writeIx, std::memory_order_release);
}

void MidiSender::InsertPending(const ScheduledMidiEvent& event) {
    if (pendingScheduledSize_ >= kScheduledRealtimeCapacity) {
        workerOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto before = [](const ScheduledMidiEvent& lhs, const ScheduledMidiEvent& rhs) {
        if (lhs.dueTimeMicros != rhs.dueTimeMicros) {
            return lhs.dueTimeMicros < rhs.dueTimeMicros;
        }
        if (lhs.orderingIntent != rhs.orderingIntent) {
            return lhs.orderingIntent < rhs.orderingIntent;
        }
        return lhs.sequence < rhs.sequence;
    };
    std::size_t insertIx = pendingScheduledSize_;
    while (insertIx > 0 && before(event, pendingScheduled_[insertIx - 1].event)) {
        pendingScheduled_[insertIx] = pendingScheduled_[insertIx - 1];
        --insertIx;
    }
    pendingScheduled_[insertIx] = PendingScheduledEntry{.event = event};
    ++pendingScheduledSize_;
    pendingScheduledCount_.store(pendingScheduledSize_, std::memory_order_release);
}

void MidiSender::DrainRealtimeLane() {
    std::uint64_t read = realtimeRead_.load(std::memory_order_relaxed);
    const std::uint64_t write = realtimeWrite_.load(std::memory_order_acquire);
    while (read != write) {
        const ScheduledMidiEvent event = realtimeLane_[read % kScheduledRealtimeCapacity];
        ++read;
        realtimeRead_.store(read, std::memory_order_release);
        if (event.kind == ScheduledMidiEventKind::PhaseGenerationCutoff) {
            ApplyGenerationCutoff(event);
        } else {
            InsertPending(event);
        }
    }
}

void MidiSender::CaptureScheduledSinks(PendingScheduledEntry& entry, std::uint64_t nowMicros) {
    std::lock_guard lock(mutex_);
    const std::size_t sinkCount = entry.event.broadcast ? kMaxSinks : 1;
    for (std::size_t sinkIx = 0; sinkIx < sinkCount; ++sinkIx) {
        if (sinks_[sinkIx] == nullptr) {
            continue;
        }
        const std::uint8_t bit = static_cast<std::uint8_t>(1u << sinkIx);
        entry.sinkRegistrationGenerations[sinkIx] = sinkRegistrationGenerations_[sinkIx];
        if (sinkCapabilities_[sinkIx] == MidiSchedulingCapability::HostTimestamped) {
            entry.hostScheduledMask = static_cast<std::uint8_t>(entry.hostScheduledMask | bit);
        } else {
            entry.immediateFallbackMask = static_cast<std::uint8_t>(entry.immediateFallbackMask | bit);
        }
    }
    entry.sinksCaptured = true;
    if ((entry.hostScheduledMask != 0 || entry.immediateFallbackMask != 0) &&
        nowMicros > entry.event.dueTimeMicros) {
        entry.lateCounted = true;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool MidiSender::BeginSinkCall(std::size_t sinkIx, std::uint64_t registrationGeneration,
                               IMidiOutputSink*& sink) {
    std::lock_guard lock(mutex_);
    if (sinkIx >= kMaxSinks || sinks_[sinkIx] == nullptr ||
        sinkRegistrationGenerations_[sinkIx] != registrationGeneration) {
        sink = nullptr;
        return false;
    }
    sink = sinks_[sinkIx];
    ++inFlight_;
    ++inFlightBySink_[sinkIx];
    return true;
}

void MidiSender::EndSinkCall(std::size_t sinkIx) {
    {
        std::lock_guard lock(mutex_);
        --inFlight_;
        --inFlightBySink_[sinkIx];
    }
    sendingCv_.notify_all();
    NotifyIfDrained();
}

void MidiSender::RemovePendingFront() {
    for (std::size_t ix = 1; ix < pendingScheduledSize_; ++ix) {
        pendingScheduled_[ix - 1] = pendingScheduled_[ix];
    }
    if (pendingScheduledSize_ > 0) {
        --pendingScheduledSize_;
    }
    pendingScheduledCount_.store(pendingScheduledSize_, std::memory_order_release);
}

std::uint64_t MidiSender::HostScheduleLeadMicros() const noexcept {
    std::lock_guard lock(mutex_);
    std::uint64_t leadMicros = 0;
    for (std::size_t sinkIx = 0; sinkIx < kMaxSinks; ++sinkIx) {
        if (sinks_[sinkIx] != nullptr &&
            sinkCapabilities_[sinkIx] == MidiSchedulingCapability::HostTimestamped) {
            leadMicros = std::max(leadMicros, sinkScheduleLeadMicros_[sinkIx]);
        }
    }
    return leadMicros;
}

bool MidiSender::ProcessScheduledFront(
    std::uint64_t nowMicros,
    std::uint64_t hostScheduleLeadMicros) {
    if (pendingScheduledSize_ == 0) {
        return false;
    }
    PendingScheduledEntry& entry = pendingScheduled_[0];
    if (!entry.sinksCaptured) {
        const std::uint64_t leadReadyAt = entry.event.dueTimeMicros > hostScheduleLeadMicros
            ? entry.event.dueTimeMicros - hostScheduleLeadMicros
            : 0;
        if (nowMicros < leadReadyAt) {
            return false;
        }
        CaptureScheduledSinks(entry, nowMicros);
        if (entry.hostScheduledMask == 0 && entry.immediateFallbackMask == 0) {
            RemovePendingFront();
            NotifyIfDrained();
            return true;
        }
    }

    if (!entry.lateCounted && nowMicros > entry.event.dueTimeMicros) {
        entry.lateCounted = true;
        lateEventCount_.fetch_add(1, std::memory_order_relaxed);
    }

    const BasicMidi midi = BasicMidi::Realtime(entry.event.dueTimeMicros,
                                                entry.event.MidiStatusByte());
    if (entry.hostScheduledMask != 0) {
        for (std::size_t sinkIx = 0; sinkIx < kMaxSinks; ++sinkIx) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << sinkIx);
            if ((entry.hostScheduledMask & bit) == 0) {
                continue;
            }
            entry.hostScheduledMask = static_cast<std::uint8_t>(entry.hostScheduledMask & ~bit);
            IMidiOutputSink* sink = nullptr;
            if (BeginSinkCall(sinkIx, entry.sinkRegistrationGenerations[sinkIx], sink)) {
                sink->SendScheduled(midi, entry.event.dueTimeMicros);
                EndSinkCall(sinkIx);
            }
        }
    }

    if (entry.immediateFallbackMask != 0 && nowMicros >= entry.event.dueTimeMicros) {
        for (std::size_t sinkIx = 0; sinkIx < kMaxSinks; ++sinkIx) {
            const std::uint8_t bit = static_cast<std::uint8_t>(1u << sinkIx);
            if ((entry.immediateFallbackMask & bit) == 0) {
                continue;
            }
            entry.immediateFallbackMask = static_cast<std::uint8_t>(entry.immediateFallbackMask & ~bit);
            IMidiOutputSink* sink = nullptr;
            if (BeginSinkCall(sinkIx, entry.sinkRegistrationGenerations[sinkIx], sink)) {
                sink->Send(midi);
                fallbackSendCount_.fetch_add(1, std::memory_order_relaxed);
                EndSinkCall(sinkIx);
            }
        }
    }

    if (entry.hostScheduledMask == 0 && entry.immediateFallbackMask == 0) {
        RemovePendingFront();
        NotifyIfDrained();
    }
    return true;
}

bool MidiSender::ProcessFeedbackFront() {
    BasicMidi midi;
    IMidiOutputSink* sink = nullptr;
    std::size_t sinkIx = kMaxSinks;
    {
        std::lock_guard lock(mutex_);
        if (size_ == 0) {
            return false;
        }
        const QueueEntry entry = queue_[head_];
        head_ = (head_ + 1) % queue_.size();
        --size_;
        sinkIx = entry.sinkIx;
        midi = entry.midi;
        if (sinks_[sinkIx] != nullptr) {
            sink = sinks_[sinkIx];
            ++inFlight_;
            ++inFlightBySink_[sinkIx];
        }
    }
    if (sink != nullptr) {
        sink->Send(midi);
        EndSinkCall(sinkIx);
    } else {
        NotifyIfDrained();
    }
    return true;
}

void MidiSender::NotifyIfDrained() {
    std::lock_guard lock(mutex_);
    if (size_ == 0 && inFlight_ == 0 && RealtimeLaneEmpty() &&
        pendingScheduledCount_.load(std::memory_order_acquire) == 0) {
        drainedCv_.notify_all();
    }
}

void MidiSender::Run() {
    ScopedThreadId scopedThreadId(ThreadId::MidiSender);
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (stopRequested_) {
                break;
            }
        }

        DrainRealtimeLane();

        {
            std::lock_guard lock(mutex_);
            if (stopRequested_) {
                break;
            }
        }

        const std::uint64_t nowMicros = NowMicros();
        const std::uint64_t hostScheduleLeadMicros = HostScheduleLeadMicros();
        bool feedbackReady = false;
        {
            std::lock_guard lock(mutex_);
            feedbackReady = size_ > 0;
        }

        bool realtimeDue = false;
        bool realtimeHostReady = false;
        if (pendingScheduledSize_ > 0) {
            PendingScheduledEntry& entry = pendingScheduled_[0];
            const std::uint64_t leadReadyAt = entry.event.dueTimeMicros > hostScheduleLeadMicros
                ? entry.event.dueTimeMicros - hostScheduleLeadMicros
                : 0;
            if (!entry.sinksCaptured && nowMicros >= leadReadyAt) {
                CaptureScheduledSinks(entry, nowMicros);
                if (entry.hostScheduledMask == 0 && entry.immediateFallbackMask == 0) {
                    RemovePendingFront();
                    NotifyIfDrained();
                    continue;
                }
            }
            if (entry.sinksCaptured) {
                realtimeDue = nowMicros >= entry.event.dueTimeMicros &&
                              (entry.hostScheduledMask != 0 || entry.immediateFallbackMask != 0);
                realtimeHostReady = entry.hostScheduledMask != 0;
            }
        }

        if (realtimeDue) {
            ProcessScheduledFront(nowMicros, hostScheduleLeadMicros);
            continue;
        }
        if (realtimeHostReady && feedbackReady) {
            if (preferRealtimeWhenBothReady_) {
                preferRealtimeWhenBothReady_ = false;
                ProcessScheduledFront(nowMicros, hostScheduleLeadMicros);
            } else {
                preferRealtimeWhenBothReady_ = true;
                ProcessFeedbackFront();
            }
            continue;
        }
        if (realtimeHostReady) {
            ProcessScheduledFront(nowMicros, hostScheduleLeadMicros);
            continue;
        }
        if (feedbackReady) {
            ProcessFeedbackFront();
            continue;
        }

        std::unique_lock lock(mutex_);
        const std::uint64_t sinkWakeGeneration = sinkWakeGeneration_;
        const auto ready = [this, sinkWakeGeneration] {
            return stopRequested_ || size_ > 0 || !RealtimeLaneEmpty() ||
                   sinkWakeGeneration_ != sinkWakeGeneration;
        };
        if (pendingScheduledSize_ == 0) {
            // TryEnqueue cannot take mutex_ without violating the audio-side
            // producer contract. Its notify may therefore land between this
            // predicate and the condition-variable wait. A short bounded wait
            // makes that race self-heal without adding any producer blocking.
            cv_.wait_for(lock, std::chrono::microseconds(kIdleSelfHealMicros), ready);
        } else {
            const PendingScheduledEntry& entry = pendingScheduled_[0];
            std::uint64_t wakeAt = entry.event.dueTimeMicros;
            if (!entry.sinksCaptured && wakeAt > hostScheduleLeadMicros) {
                wakeAt -= hostScheduleLeadMicros;
            }
            const std::uint64_t waitMicros = wakeAt > nowMicros ? wakeAt - nowMicros : 1;
            cv_.wait_for(lock, std::chrono::microseconds(waitMicros), ready);
        }
    }
    {
        std::lock_guard lock(mutex_);
        running_ = false;
    }
    drainedCv_.notify_all();
    sendingCv_.notify_all();
}

EncoderMidiOutConfig EncoderMidiOutConfig::TwisterDefault(std::size_t slotIx) {
    EncoderMidiOutConfig config = RowMajorOutputDefault(slotIx);
    config.protocol = EncoderMidiOutProtocol::Twister;
    return config;
}

EncoderMidiOutConfig EncoderMidiOutConfig::WrldBldrDefault(std::size_t slotIx) {
    EncoderMidiOutConfig config = RowMajorOutputDefault(slotIx);
    config.protocol = EncoderMidiOutProtocol::WrldBldr;
    return config;
}

void EncoderMidiOutConfig::KeepFirstPositions(std::size_t count) {
    std::erase_if(mappings, [count](const EncoderMidiOutMapping& mapping) { return !MappingIsFirstPosition(mapping, count); });
}

MidiOutProcessor::MidiOutProcessor(EncoderMidiOutConfig config, MidiSender* sender, ParameterManager::UIState* uiState,
                                   std::size_t sinkIx, EncoderMode feedbackMode,
                                   AbsoluteFeedbackCoordinator* absoluteFeedback,
                                   std::size_t controllerSlot)
    : config_(std::move(config)),
      sender_(sender),
      uiState_(uiState),
      sinkIx_(sinkIx),
      feedbackMode_(feedbackMode),
      absoluteFeedback_(absoluteFeedback),
      controllerSlot_(controllerSlot) {
    ReserveAbsoluteRoutes();
}

void MidiOutProcessor::SetConfig(EncoderMidiOutConfig config) {
    config_ = std::move(config);
    ReserveAbsoluteRoutes();
    Reset();
}

void MidiOutProcessor::Reset() {}

std::optional<MidiOutProcessor::CellSnapshot> MidiOutProcessor::LoadCellSnapshot(
    const EncoderMidiOutMapping& mapping) const {
    // Configured output mappings are app-independent. When a mapped slot or
    // position has no backing UI cell in the current view, treat that mapping
    // as stable blank feedback so hardware does not keep showing stale state.
    // `std::nullopt` is reserved for transient torn reads, which callers skip
    // and retry.
    if (uiState_ == nullptr || mapping.slotIx >= uiState_->slotCapacity) {
        return CellSnapshot{};
    }
    const BankSlot::UIState& slot = uiState_->slots[mapping.slotIx];
    if (mapping.position >= slot.cellCapacity) {
        return CellSnapshot{};
    }
    const Parameter::UIState& state = slot.cells[mapping.position];
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t startRevision = state.revision.load(std::memory_order_acquire);
        if ((startRevision & 1u) != 0) {
            continue;
        }
        CellSnapshot snapshot;
        snapshot.connected = state.connected.load(std::memory_order_relaxed);
        snapshot.bipolar = state.bipolar.load(std::memory_order_relaxed);
        snapshot.voiceCount = std::min(state.voiceCount.load(std::memory_order_relaxed), state.voiceCapacity);
        snapshot.baseColor = state.baseColor.Load(std::memory_order_relaxed);
        if (snapshot.voiceCount > 0) {
            snapshot.value = state.values[0].load(std::memory_order_relaxed);
            snapshot.indicatorColor = state.indicatorColors[0].Load(std::memory_order_relaxed);
        }
        if (feedbackMode_ == EncoderMode::Absolute) {
            snapshot.rawKnobValue = state.rawKnobValue.load(std::memory_order_relaxed);
            snapshot.processedAbsoluteEpoch =
                state.processedAbsoluteEpoch.load(std::memory_order_relaxed);
        }
        const std::uint32_t endRevision = state.revision.load(std::memory_order_acquire);
        if (startRevision == endRevision && (endRevision & 1u) == 0) {
            return snapshot;
        }
    }
    return std::nullopt;
}

void MidiOutProcessor::ReserveAbsoluteRoutes() {
    absoluteRoutes_.clear();
    if (feedbackMode_ != EncoderMode::Absolute || absoluteFeedback_ == nullptr) {
        return;
    }
    absoluteRoutes_.reserve(config_.mappings.size());
    for (const EncoderMidiOutMapping& mapping : config_.mappings) {
        absoluteRoutes_.push_back(absoluteFeedback_->ReserveRoute({
            .controllerSlot = controllerSlot_,
            .parameterSlot = mapping.slotIx,
            .position = mapping.position,
        }));
    }
}

void MidiOutProcessor::ProcessPosition(std::size_t mappingIx,
                                       MidiControlAddress outputAddress,
                                       const CellSnapshot& snapshot, bool blank,
                                       bool& cacheValid, std::uint8_t& cachedValue) {
    const std::uint8_t value = feedbackMode_ == EncoderMode::Absolute
                                   ? (blank ? 0 : NormalizedToAbsoluteEncoderByte(snapshot.rawKnobValue))
                                   : (blank ? 0 : FloatTo7Bit(NormalizeForDisplay(snapshot.value, snapshot.bipolar)));
    if (feedbackMode_ == EncoderMode::Absolute && absoluteFeedback_ != nullptr &&
        mappingIx < absoluteRoutes_.size() && absoluteRoutes_[mappingIx].IsTracked()) {
        auto guard = absoluteFeedback_->GuardRoute(absoluteRoutes_[mappingIx]);
        const AbsoluteFeedbackCoordinator::Expectation expectation = guard.Snapshot();
        if (expectation.pending) {
            // A mapped route outside the current visible-cell capacity has a
            // synthetic blank snapshot with epoch zero. If it somehow has a
            // tracked pending input, keep it conservatively gated: no real
            // cell can acknowledge that event, and blank is not an ack.
            if (snapshot.processedAbsoluteEpoch < expectation.epoch) {
                return;
            }
            if (value != expectation.receivedValue &&
                !Enqueue(BasicMidi::CC(0, outputAddress.channel, outputAddress.cc, value))) {
                return;
            }
            if (guard.Resolve(expectation.epoch)) {
                cachedValue = value;
                cacheValid = true;
            }
            return;
        }
        if (!cacheValid || cachedValue != value) {
            if (Enqueue(BasicMidi::CC(0, outputAddress.channel, outputAddress.cc, value))) {
                cachedValue = value;
                cacheValid = true;
            }
        }
        return;
    }

    if (!cacheValid || cachedValue != value) {
        const bool enqueued = Enqueue(BasicMidi::CC(0, outputAddress.channel, outputAddress.cc, value));
        if (feedbackMode_ != EncoderMode::Absolute || enqueued) {
            cachedValue = value;
            cacheValid = true;
        }
    }
}

bool MidiOutProcessor::Enqueue(const BasicMidi& midi) {
    return sender_ != nullptr && sender_->Enqueue(sinkIx_, midi);
}

float MidiOutProcessor::NormalizeForDisplay(float value, bool bipolar) {
    const float normalized = bipolar ? (value + 1.0f) * 0.5f : value;
    return std::clamp(normalized, 0.0f, 1.0f);
}

GenericMidiOutProcessor::Projection GenericMidiOutProcessor::ProjectInput(
    const EncoderMidiInConfig& input) {
    Projection projection;
    projection.mode = input.mode;
    projection.config.mappings.reserve(input.turns.size());
    projection.outputAddresses.reserve(input.turns.size());
    for (const EncoderMidiMapping& mapping : input.turns) {
        projection.config.mappings.push_back({
            .slotIx = mapping.slotIx,
            .position = mapping.position,
            .cc = mapping.control.cc,
        });
        projection.outputAddresses.push_back(mapping.control);
    }
    return projection;
}

GenericMidiOutProcessor::GenericMidiOutProcessor(
    const EncoderMidiInConfig& input, MidiSender* sender,
    ParameterManager::UIState* uiState, std::size_t sinkIx,
    AbsoluteFeedbackCoordinator* absoluteFeedback, std::size_t controllerSlot)
    : GenericMidiOutProcessor(ProjectInput(input), sender, uiState, sinkIx,
                              absoluteFeedback, controllerSlot) {}

GenericMidiOutProcessor::GenericMidiOutProcessor(
    Projection projection, MidiSender* sender, ParameterManager::UIState* uiState,
    std::size_t sinkIx, AbsoluteFeedbackCoordinator* absoluteFeedback,
    std::size_t controllerSlot)
    : MidiOutProcessor(std::move(projection.config), sender, uiState, sinkIx,
                       projection.mode, absoluteFeedback, controllerSlot),
      outputAddresses_(std::move(projection.outputAddresses)) {
    assert(config_.mappings.size() == outputAddresses_.size());
}

void GenericMidiOutProcessor::Reset() {
    cache_.clear();
}

void GenericMidiOutProcessor::Process() {
    assert(config_.mappings.size() == outputAddresses_.size());
    if (CacheNeedsResize(cache_.size(), config_.mappings.size())) {
        cache_.assign(config_.mappings.size(), {});
    }
    for (std::size_t ix = 0; ix < config_.mappings.size(); ++ix) {
        const EncoderMidiOutMapping& mapping = config_.mappings[ix];
        const std::optional<CellSnapshot> snapshot = LoadCellSnapshot(mapping);
        if (!snapshot.has_value()) {
            continue;
        }
        const bool blank = !snapshot->connected || snapshot->voiceCount == 0;
        CacheEntry& cache = cache_[ix];
        ProcessPosition(ix, outputAddresses_[ix], *snapshot, blank,
                        cache.valid, cache.value);
    }
}

void TwisterMidiOutProcessor::Reset() {
    cache_.clear();
}

void TwisterMidiOutProcessor::Process() {
    if (CacheNeedsResize(cache_.size(), config_.mappings.size())) {
        cache_.assign(config_.mappings.size(), {});
    }
    for (std::size_t ix = 0; ix < config_.mappings.size(); ++ix) {
        const EncoderMidiOutMapping& mapping = config_.mappings[ix];
        const std::optional<CellSnapshot> snapshot = LoadCellSnapshot(mapping);
        if (!snapshot.has_value()) {
            continue;
        }
        const bool blank = !snapshot->connected || snapshot->voiceCount == 0;
        const std::uint8_t rgbColor = blank ? 0 : ColorToTwister(snapshot->baseColor);
        const std::uint8_t rgbBrightness = TwisterRgbBrightnessValue(blank ? 0.0f : 1.0f);
        const std::uint8_t ringBrightness = TwisterRingBrightnessValue(blank ? 0.0f : 1.0f);
        CacheEntry& cache = cache_[ix];
        if (!cache.valid || cache.rgbColor != rgbColor) {
            Enqueue(BasicMidi::CC(0, kTwisterRgbColorChannel, mapping.cc, rgbColor));
        }
        if (!cache.valid || cache.rgbBrightness != rgbBrightness) {
            Enqueue(BasicMidi::CC(0, kTwisterRgbBrightnessChannel, mapping.cc, rgbBrightness));
        }
        if (!cache.valid || cache.ringBrightness != ringBrightness) {
            Enqueue(BasicMidi::CC(0, kTwisterRingBrightnessChannel, mapping.cc, ringBrightness));
        }
        cache.valid = true;
        cache.rgbColor = rgbColor;
        cache.rgbBrightness = rgbBrightness;
        cache.ringBrightness = ringBrightness;
        ProcessPosition(ix, {kPrimaryPositionChannel, mapping.cc}, *snapshot, blank,
                        cache.encoderRingValueValid, cache.encoderRingValue);
    }
}

void WrldBldrMidiOutProcessor::Reset() {
    cache_.clear();
}

void WrldBldrMidiOutProcessor::Process() {
    if (CacheNeedsResize(cache_.size(), config_.mappings.size())) {
        cache_.assign(config_.mappings.size(), {});
    }

    std::size_t colorBudget = config_.wrldBldrColorBudgetPerProcess == 0 ? config_.mappings.size()
                                                                          : config_.wrldBldrColorBudgetPerProcess;
    for (std::size_t ix = 0; ix < config_.mappings.size(); ++ix) {
        const EncoderMidiOutMapping& mapping = config_.mappings[ix];
        const std::optional<CellSnapshot> snapshot = LoadCellSnapshot(mapping);
        if (!snapshot.has_value()) {
            continue;
        }

        const bool blank = !snapshot->connected || snapshot->voiceCount == 0;
        const Color buttonColor = blank ? Color::Off : snapshot->baseColor;
        const Color indicatorColor = blank ? Color::Off : snapshot->indicatorColor;
        CacheEntry& cache = cache_[ix];
        ProcessPosition(ix, {kPrimaryPositionChannel, mapping.cc}, *snapshot, blank,
                        cache.valueValid, cache.value);
        if (!cache.valid || cache.buttonColor != buttonColor) {
            cache.buttonColor = buttonColor;
            cache.pendingButtonColor = true;
        }
        if (!cache.valid || cache.indicatorColor != indicatorColor) {
            cache.indicatorColor = indicatorColor;
            cache.pendingIndicatorColor = true;
        }
        cache.valid = true;

        if (colorBudget > 0 && cache.pendingButtonColor) {
            Enqueue(WrldBldrColorSysex(0, 1, mapping.cc, cache.buttonColor));
            cache.pendingButtonColor = false;
            --colorBudget;
        }
        if (colorBudget > 0 && cache.pendingIndicatorColor) {
            Enqueue(WrldBldrColorSysex(0, 0, mapping.cc, cache.indicatorColor));
            cache.pendingIndicatorColor = false;
            --colorBudget;
        }
    }
}

SystemMessageOutputInfo::SystemMessageOutputInfo(RuntimeUIState* uiState)
    : uiState_(uiState) {}

SystemMessageOutputInfo::SystemMessageOutputInfo(ParameterManager::UIState* uiState) {
    SetUIState(uiState);
}

SystemMessageOutputState SystemMessageOutputInfo::Evaluate(const MessageIn& message) const {
    ParameterManager::UIState* const parameters = UIState();

    switch (message.type) {
    case MessageIn::Type::SelectParamBank: {
        if (parameters == nullptr || message.bankIx >= parameters->bankCapacity) {
            return {};
        }
        const ParameterManager::BankUIState& bank = parameters->banks[message.bankIx];
        if (!bank.connected.load(std::memory_order_relaxed)) {
            return {};
        }
        const bool selected = bank.selected.load(std::memory_order_relaxed);
        const Color color = bank.bankColor.Load(std::memory_order_relaxed);
        return {.color = selected ? color : color.AdjustBrightness(0.35f), .isOn = selected};
    }
    case MessageIn::Type::ToggleReset: {
        if (parameters == nullptr) {
            return {};
        }
        const bool held = parameters->resetHeld.load(std::memory_order_relaxed);
        return {.color = held ? Color::White : Color::Grey, .isOn = held};
    }
    case MessageIn::Type::ToggleRandom: {
        if (parameters == nullptr) {
            return {};
        }
        const bool held = parameters->randomHeld.load(std::memory_order_relaxed);
        return {.color = held ? Color::White : Color::Grey, .isOn = held};
    }
    case MessageIn::Type::ToggleRandomMod: {
        if (parameters == nullptr) {
            return {};
        }
        const bool held = parameters->randomModHeld.load(std::memory_order_relaxed);
        return {.color = held ? Color::White : Color::Grey, .isOn = held};
    }
    case MessageIn::Type::SceneSelect: {
        if (parameters == nullptr || message.sceneIx >= parameters->sceneCapacity) {
            return {};
        }
        const std::size_t leftScene = parameters->leftScene.load(std::memory_order_relaxed);
        const std::size_t rightScene = parameters->rightScene.load(std::memory_order_relaxed);
        const float blend =
            std::clamp(parameters->sceneBlend.load(std::memory_order_relaxed), 0.0f, 1.0f);
        if (message.sceneIx == leftScene) {
            return {.color = Color::Orange.AdjustBrightness(0.5f + 0.5f * (1.0f - blend)), .isOn = true};
        }
        if (message.sceneIx == rightScene) {
            return {.color = Color::Green.AdjustBrightness(0.5f + 0.5f * blend), .isOn = true};
        }
        return {};
    }
    case MessageIn::Type::ToggleGestureSelect:
    case MessageIn::Type::SetGestureSelect: {
        if (parameters == nullptr || message.gestureIx >= parameters->gestures.gestureCapacity ||
            !parameters->gestures.connected[message.gestureIx].load(std::memory_order_relaxed)) {
            return {};
        }
        const bool selected =
            parameters->gestures.selected[message.gestureIx].load(std::memory_order_relaxed);
        if (selected) {
            return {.color = Color::White, .isOn = true};
        }
        return {.color = GestureColor(message.gestureIx), .isOn = false};
    }
    case MessageIn::Type::ParamIncDec:
    case MessageIn::Type::ParamSetAbsolute:
    case MessageIn::Type::ParamSetAbsoluteOnBank:
    case MessageIn::Type::ParamPush:
    case MessageIn::Type::NextParamBank:
    case MessageIn::Type::PrevParamBank:
    case MessageIn::Type::Start:
    case MessageIn::Type::Continue:
    case MessageIn::Type::Stop:
    case MessageIn::Type::Clock:
    case MessageIn::Type::SetGestureValue:
    case MessageIn::Type::SetSceneBlend:
    case MessageIn::Type::SelectGrid:
    case MessageIn::Type::AppAction:
    case MessageIn::Type::HoldDrill:
        return {};
    case MessageIn::Type::GridPress:
    case MessageIn::Type::GridRelease:
    case MessageIn::Type::GridPressureChange: {
        if (uiState_ == nullptr || uiState_->grids == nullptr ||
            message.gridSlotIx >= uiState_->grids->slots.size()) {
            return {};
        }
        const std::unique_ptr<Grid::UIState>& slot =
            uiState_->grids->slots[message.gridSlotIx];
        if (slot == nullptr) {
            return {};
        }
        const std::optional<std::size_t> cellIx = slot->range.IndexOf(message.gridX, message.gridY);
        if (!cellIx.has_value() || *cellIx >= slot->colors.size()) {
            return {};
        }
        const Color packed = slot->colors[*cellIx].Load(std::memory_order_relaxed);
        return {.color = Color::Rgb(packed.r, packed.g, packed.b), .isOn = packed.a != 0};
    }
    }
    return {};
}

Color SystemMessageOutputInfo::GestureColor(std::size_t gestureIx) const {
    ParameterManager::UIState* const parameters = UIState();
    const std::size_t count =
        parameters->gestures.bankAffectingCount[gestureIx].load(std::memory_order_relaxed);
    if (count == 0) {
        return Color::Grey.AdjustBrightness(0.5f);
    }
    if (count > 1) {
        return Color::White;
    }

    const std::uint32_t mask =
        parameters->gestures.bankAffectingMask[gestureIx].load(std::memory_order_relaxed);
    const std::size_t bankCount = std::min<std::size_t>(parameters->bankCapacity, 32);
    for (std::size_t bankIx = 0; bankIx < bankCount; ++bankIx) {
        if ((mask & (std::uint32_t{1} << bankIx)) == 0) {
            continue;
        }
        if (parameters->banks[bankIx].connected.load(std::memory_order_relaxed)) {
            return parameters->banks[bankIx].bankColor.Load(std::memory_order_relaxed);
        }
    }
    return Color::Grey.AdjustBrightness(0.5f);
}

SystemCcMidiOutProcessor::SystemCcMidiOutProcessor(SystemCcMidiOutConfig config, MidiSender* sender,
                                                   RuntimeUIState* uiState, std::size_t sinkIx)
    : config_(std::move(config)),
      sender_(sender),
      info_(uiState),
      sinkIx_(sinkIx) {}

SystemCcMidiOutProcessor::SystemCcMidiOutProcessor(SystemCcMidiOutConfig config, MidiSender* sender,
                                                   ParameterManager::UIState* uiState, std::size_t sinkIx)
    : SystemCcMidiOutProcessor(std::move(config), sender,
                               static_cast<RuntimeUIState*>(nullptr), sinkIx) {
    info_.SetUIState(uiState);
}

void SystemCcMidiOutProcessor::SetConfig(SystemCcMidiOutConfig config) {
    config_ = std::move(config);
    Reset();
}

void SystemCcMidiOutProcessor::Reset() {
    cache_.clear();
}

void SystemCcMidiOutProcessor::Process() {
    if (CacheNeedsResize(cache_.size(), config_.associations.size())) {
        cache_.assign(config_.associations.size(), {});
    }

    for (std::size_t ix = 0; ix < config_.associations.size(); ++ix) {
        const SystemCcMidiOutAssociation& association = config_.associations[ix];
        const SystemMessageOutputState state = info_.Evaluate(association.message);
        CacheEntry& cache = cache_[ix];
        if (!cache.valid || cache.isOn != state.isOn) {
            Enqueue(BasicMidi::CC(0, association.control.channel, association.control.cc, state.isOn ? 127 : 0));
        }
        cache = {.valid = true, .isOn = state.isOn};
    }
}

bool SystemCcMidiOutProcessor::Enqueue(const BasicMidi& midi) {
    return sender_ != nullptr && sender_->Enqueue(sinkIx_, midi);
}

WrldBldrSystemMidiOutProcessor::WrldBldrSystemMidiOutProcessor(WrldBldrSystemMidiOutConfig config,
                                                               MidiSender* sender,
                                                               RuntimeUIState* uiState,
                                                               std::size_t sinkIx)
    : config_(std::move(config)),
      sender_(sender),
      info_(uiState),
      sinkIx_(sinkIx) {}

WrldBldrSystemMidiOutProcessor::WrldBldrSystemMidiOutProcessor(WrldBldrSystemMidiOutConfig config,
                                                               MidiSender* sender,
                                                               ParameterManager::UIState* uiState,
                                                               std::size_t sinkIx)
    : WrldBldrSystemMidiOutProcessor(std::move(config), sender,
                                     static_cast<RuntimeUIState*>(nullptr), sinkIx) {
    info_.SetUIState(uiState);
}

void WrldBldrSystemMidiOutProcessor::SetConfig(WrldBldrSystemMidiOutConfig config) {
    config_ = std::move(config);
    Reset();
}

void WrldBldrSystemMidiOutProcessor::Reset() {
    cache_.clear();
}

void WrldBldrSystemMidiOutProcessor::Process() {
    if (CacheNeedsResize(cache_.size(), config_.associations.size())) {
        cache_.assign(config_.associations.size(), {});
    }

    for (std::size_t ix = 0; ix < config_.associations.size(); ++ix) {
        const WrldBldrSystemMidiOutAssociation& association = config_.associations[ix];
        const Color color = info_.Evaluate(association.message).color;
        CacheEntry& cache = cache_[ix];
        if (!cache.valid || cache.color != color) {
            Enqueue(WrldBldrColorSysex(0, association.position.channel,
                                       WrldBldrPositionToCC(association.position.x, association.position.y), color));
        }
        cache = {.valid = true, .color = color};
    }
}

bool WrldBldrSystemMidiOutProcessor::Enqueue(const BasicMidi& midi) {
    return sender_ != nullptr && sender_->Enqueue(sinkIx_, midi);
}

LaunchpadGridMidiOutProcessor::LaunchpadGridMidiOutProcessor(LaunchpadGridMidiOutConfig config,
                                                             MidiSender* sender,
                                                             RuntimeUIState* uiState,
                                                             std::size_t sinkIx)
    : config_(std::move(config)),
      sender_(sender),
      info_(uiState),
      sinkIx_(sinkIx) {}

LaunchpadGridMidiOutProcessor::LaunchpadGridMidiOutProcessor(LaunchpadGridMidiOutConfig config,
                                                             MidiSender* sender,
                                                             ParameterManager::UIState* uiState,
                                                             std::size_t sinkIx)
    : LaunchpadGridMidiOutProcessor(std::move(config), sender,
                                    static_cast<RuntimeUIState*>(nullptr), sinkIx) {
    info_.SetUIState(uiState);
}

void LaunchpadGridMidiOutProcessor::SetConfig(LaunchpadGridMidiOutConfig config) {
    config_ = std::move(config);
    Reset();
}

void LaunchpadGridMidiOutProcessor::Reset() {
    cache_.clear();
}

void LaunchpadGridMidiOutProcessor::Process() {
    if (CacheNeedsResize(cache_.size(), config_.associations.size())) {
        cache_.assign(config_.associations.size(), {});
    }

    for (std::size_t ix = 0; ix < config_.associations.size(); ++ix) {
        const LaunchpadGridMidiOutAssociation& association = config_.associations[ix];
        const Color color = info_.Evaluate(association.message).color;
        CacheEntry& cache = cache_[ix];
        if (!cache.valid || cache.color != color) {
            Enqueue(LaunchpadColorSysex(0, association.position.controller, association.position.x,
                                        association.position.y, color));
        }
        cache = {.valid = true, .color = color};
    }
}

bool LaunchpadGridMidiOutProcessor::Enqueue(const BasicMidi& midi) {
    return sender_ != nullptr && !midi.raw.empty() && sender_->Enqueue(sinkIx_, midi);
}

OpenSysExMidiOutProcessor::OpenSysExMidiOutProcessor(std::vector<std::vector<std::uint8_t>> messages,
                                                     MidiSender* sender, std::size_t sinkIx)
    : messages_(std::move(messages)), sender_(sender), sinkIx_(sinkIx) {}

void OpenSysExMidiOutProcessor::Reset() {
    pending_ = true;
}

void OpenSysExMidiOutProcessor::Process() {
    if (!pending_) {
        return;
    }
    pending_ = false;
    for (const std::vector<std::uint8_t>& message : messages_) {
        Enqueue(BasicMidi::SysEx(0, message));
    }
}

bool OpenSysExMidiOutProcessor::Enqueue(const BasicMidi& midi) {
    return sender_ != nullptr && !midi.raw.empty() && sender_->Enqueue(sinkIx_, midi);
}

JSON ToJSON(JsonArena& arena, EncoderMode value) {
    switch (value) {
    case EncoderMode::Signed7Bit:
        return arena.String("signed7Bit");
    case EncoderMode::DirectionOnly:
        return arena.String("directionOnly");
    case EncoderMode::Absolute:
        return arena.String("absolute");
    }
    return arena.String("signed7Bit");
}

bool FromJSON(JSON json, EncoderMode& value) {
    if (!IsString(json)) {
        return false;
    }
    const std::string_view mode(json.StringValue());
    if (mode == "signed7Bit") {
        value = EncoderMode::Signed7Bit;
        return true;
    }
    if (mode == "directionOnly") {
        value = EncoderMode::DirectionOnly;
        return true;
    }
    if (mode == "absolute") {
        value = EncoderMode::Absolute;
        return true;
    }
    return false;
}

JSON ToJSON(JsonArena& arena, const MidiControlAddress& value) {
    JSON json = arena.Object();
    json.SetNew("channel", arena.Integer(value.channel));
    json.SetNew("cc", arena.Integer(value.cc));
    json.SetNew("type", arena.String(value.type == MidiControlType::Note ? "note" : "cc"));
    return json;
}

bool FromJSON(JSON json, MidiControlAddress& value) {
    if (!IsObject(json)) {
        return false;
    }
    MidiControlAddress parsed;
    if (!ReadU8(json.Get("channel"), parsed.channel, 0x0F) || !ReadU8(json.Get("cc"), parsed.cc)) {
        return false;
    }
    const JSON type = json.Get("type");
    if (!type.IsNull()) {
        if (!IsString(type)) {
            return false;
        }
        const std::string_view name(type.StringValue());
        if (name == "cc") {
            parsed.type = MidiControlType::Cc;
        } else if (name == "note") {
            parsed.type = MidiControlType::Note;
        } else {
            return false;
        }
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const MidiNoteAddress& value) {
    JSON json = arena.Object();
    json.SetNew("channel", arena.Integer(value.channel));
    json.SetNew("note", arena.Integer(value.note));
    return json;
}

bool FromJSON(JSON json, MidiNoteAddress& value) {
    if (!IsObject(json)) {
        return false;
    }
    MidiNoteAddress parsed;
    if (!ReadU8(json.Get("channel"), parsed.channel, 0x0F) ||
        !ReadU8(json.Get("note"), parsed.note)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const PolyphonicPressureMapping& value) {
    JSON json = arena.Object();
    json.SetNew("address", ToJSON(arena, value.address));
    json.SetNew("pressure", ToJSON(arena, value.pressure));
    return json;
}

bool FromJSON(JSON json, PolyphonicPressureMapping& value) {
    if (!IsObject(json)) {
        return false;
    }
    PolyphonicPressureMapping parsed;
    if (!FromJSON(json.Get("address"), parsed.address) ||
        !FromJSON(json.Get("pressure"), parsed.pressure) ||
        parsed.pressure.type != MessageIn::Type::GridPressureChange) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const PolyphonicPressureMidiInConfig& value) {
    JSON json = arena.Object();
    json.SetNew("mappings", VectorToJSON(arena, value.mappings));
    return json;
}

bool FromJSON(JSON json, PolyphonicPressureMidiInConfig& value) {
    if (!IsObject(json)) {
        return false;
    }
    PolyphonicPressureMidiInConfig parsed;
    if (!VectorFromJSON(json.Get("mappings"), parsed.mappings) ||
        PolyphonicPressureConfigError(parsed) != nullptr) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const EncoderMidiMapping& value) {
    JSON json = arena.Object();
    json.SetNew("control", ToJSON(arena, value.control));
    json.SetNew("slotIx", arena.Integer(static_cast<int64_t>(value.slotIx)));
    json.SetNew("position", arena.Integer(static_cast<int64_t>(value.position)));
    return json;
}

bool FromJSON(JSON json, EncoderMidiMapping& value) {
    if (!IsObject(json)) {
        return false;
    }
    EncoderMidiMapping parsed;
    if (!FromJSON(json.Get("control"), parsed.control) || !ReadSize(json.Get("slotIx"), parsed.slotIx) ||
        !ReadSize(json.Get("position"), parsed.position)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const EncoderMidiInConfig& value) {
    JSON json = arena.Object();
    json.SetNew("mode", ToJSON(arena, value.mode));
    json.SetNew("turnStep", arena.Real(value.turnStep));
    json.SetNew("turns", VectorToJSON(arena, value.turns));
    json.SetNew("pushes", VectorToJSON(arena, value.pushes));
    return json;
}

bool FromJSON(JSON json, EncoderMidiInConfig& value) {
    if (!IsObject(json)) {
        return false;
    }
    EncoderMidiInConfig parsed;
    const JSON compatibleMode = ObjectHasKey(json, "mode") ? json.Get("mode") : json.Get("relativeMode");
    if (!FromJSON(compatibleMode, parsed.mode) || !ReadFloat(json.Get("turnStep"), parsed.turnStep) ||
        !std::isfinite(parsed.turnStep) || parsed.turnStep <= 0.0f ||
        !VectorFromJSON(json.Get("turns"), parsed.turns) || !VectorFromJSON(json.Get("pushes"), parsed.pushes)) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const AnalogMidiMapping& value) {
    JSON json = arena.Object();
    json.SetNew("control", ToJSON(arena, value.control));
    json.SetNew("gestureIx", arena.Integer(static_cast<int64_t>(value.gestureIx)));
    return json;
}

bool FromJSON(JSON json, AnalogMidiMapping& value) {
    if (!IsObject(json)) {
        return false;
    }
    AnalogMidiMapping parsed;
    if (!FromJSON(json.Get("control"), parsed.control) || !ReadSize(json.Get("gestureIx"), parsed.gestureIx)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const AnalogAppActionMapping& value) {
    JSON json = arena.Object();
    json.SetNew("control", ToJSON(arena, value.control));
    json.SetNew("appAction", arena.String(value.appAction.c_str()));
    json.SetNew("appActionValue", arena.String(value.appActionValue.c_str()));
    return json;
}

bool FromJSON(JSON json, AnalogAppActionMapping& value) {
    if (!IsObject(json)) {
        return false;
    }
    AnalogAppActionMapping parsed;
    if (!FromJSON(json.Get("control"), parsed.control)) {
        return false;
    }
    const JSON appAction = json.Get("appAction");
    const JSON appActionValue = json.Get("appActionValue");
    if (!IsString(appAction) || !IsString(appActionValue)) {
        return false;
    }
    parsed.appAction = appAction.StringValue();
    parsed.appActionValue = appActionValue.StringValue();
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const AnalogMidiInConfig& value) {
    JSON json = arena.Object();
    json.SetNew("gestures", VectorToJSON(arena, value.gestures));
    if (value.sceneBlend.has_value()) {
        json.SetNew("sceneBlend", ToJSON(arena, *value.sceneBlend));
    } else {
        json.SetNew("sceneBlend", arena.Null());
    }
    json.SetNew("appActions", VectorToJSON(arena, value.appActions));
    return json;
}

bool FromJSON(JSON json, AnalogMidiInConfig& value) {
    if (!IsObject(json)) {
        return false;
    }
    AnalogMidiInConfig parsed;
    if (!VectorFromJSON(json.Get("gestures"), parsed.gestures)) {
        return false;
    }
    const JSON sceneBlend = json.Get("sceneBlend");
    if (!sceneBlend.IsNull()) {
        MidiControlAddress address;
        if (!FromJSON(sceneBlend, address)) {
            return false;
        }
        parsed.sceneBlend = address;
    }
    if (ObjectHasKey(json, "appActions") && !VectorFromJSON(json.Get("appActions"), parsed.appActions)) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const EncoderMidiOutMapping& value) {
    JSON json = arena.Object();
    json.SetNew("slotIx", arena.Integer(static_cast<int64_t>(value.slotIx)));
    json.SetNew("position", arena.Integer(static_cast<int64_t>(value.position)));
    json.SetNew("cc", arena.Integer(value.cc));
    return json;
}

bool FromJSON(JSON json, EncoderMidiOutMapping& value) {
    if (!IsObject(json)) {
        return false;
    }
    EncoderMidiOutMapping parsed;
    if (!ReadSize(json.Get("slotIx"), parsed.slotIx) || !ReadSize(json.Get("position"), parsed.position) ||
        !ReadU8(json.Get("cc"), parsed.cc)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const EncoderMidiOutConfig& value) {
    JSON json = arena.Object();
    json.SetNew("protocol", arena.String(EncoderMidiOutProtocolName(value.protocol)));
    json.SetNew("mappings", VectorToJSON(arena, value.mappings));
    json.SetNew("wrldBldrColorBudgetPerProcess",
                arena.Integer(static_cast<int64_t>(value.wrldBldrColorBudgetPerProcess)));
    return json;
}

bool FromJSON(JSON json, EncoderMidiOutConfig& value) {
    if (!IsObject(json)) {
        return false;
    }
    EncoderMidiOutConfig parsed;
    if (!VectorFromJSON(json.Get("mappings"), parsed.mappings) ||
        !ReadSize(json.Get("wrldBldrColorBudgetPerProcess"), parsed.wrldBldrColorBudgetPerProcess)) {
        return false;
    }
    const JSON protocol = json.Get("protocol");
    if (!protocol.IsNull() &&
        (!IsString(protocol) || !ParseEncoderMidiOutProtocol(protocol.StringValue(), parsed.protocol))) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const MessageIn& value) {
    JSON json = arena.Object();
    json.SetNew("type", arena.String(MessageTypeName(value.type)));
    WriteMessageOrigin(arena, json, value);
    switch (value.type) {
    case MessageIn::Type::GridPress:
    case MessageIn::Type::GridPressureChange:
        json.SetNew("gridSlot", arena.Integer(static_cast<int64_t>(value.gridSlotIx)));
        json.SetNew("x", arena.Integer(value.gridX));
        json.SetNew("y", arena.Integer(value.gridY));
        json.SetNew("velocity", arena.Integer(value.velocity));
        return json;
    case MessageIn::Type::GridRelease:
        json.SetNew("gridSlot", arena.Integer(static_cast<int64_t>(value.gridSlotIx)));
        json.SetNew("x", arena.Integer(value.gridX));
        json.SetNew("y", arena.Integer(value.gridY));
        return json;
    case MessageIn::Type::SelectGrid:
        json.SetNew("gridSlot", arena.Integer(static_cast<int64_t>(value.gridSlotIx)));
        json.SetNew("grid", arena.Integer(static_cast<int64_t>(value.gridIx)));
        return json;
    case MessageIn::Type::AppAction:
        // appActionIx is not persisted -- an app's action list can reorder
        // between runs, so only the type name round-trips.
        return json;
    case MessageIn::Type::ParamIncDec:
    case MessageIn::Type::ParamSetAbsolute:
    case MessageIn::Type::ParamSetAbsoluteOnBank:
    case MessageIn::Type::ParamPush:
    case MessageIn::Type::ToggleReset:
    case MessageIn::Type::ToggleRandom:
    case MessageIn::Type::ToggleRandomMod:
    case MessageIn::Type::ToggleGestureSelect:
    case MessageIn::Type::SetGestureSelect:
    case MessageIn::Type::SelectParamBank:
    case MessageIn::Type::NextParamBank:
    case MessageIn::Type::PrevParamBank:
    case MessageIn::Type::Start:
    case MessageIn::Type::Continue:
    case MessageIn::Type::Stop:
    case MessageIn::Type::Clock:
    case MessageIn::Type::SetGestureValue:
    case MessageIn::Type::SceneSelect:
    case MessageIn::Type::SetSceneBlend:
    case MessageIn::Type::HoldDrill:
        break;
    }
    json.SetNew("slotIx", arena.Integer(static_cast<int64_t>(value.slotIx)));
    json.SetNew("position", arena.Integer(static_cast<int64_t>(value.position)));
    json.SetNew("gestureIx", arena.Integer(static_cast<int64_t>(value.gestureIx)));
    json.SetNew("bankIx", arena.Integer(static_cast<int64_t>(value.bankIx)));
    json.SetNew("sceneIx", arena.Integer(static_cast<int64_t>(value.sceneIx)));
    json.SetNew("value", arena.Real(value.value));
    json.SetNew("delta", arena.Real(value.delta));
    json.SetNew("boolValue", arena.Boolean(value.boolValue));
    json.SetNew("hasBoolValue", arena.Boolean(value.hasBoolValue));
    return json;
}

bool FromJSON(JSON json, MessageIn& value) {
    if (!IsObject(json) || !IsString(json.Get("type"))) {
        return false;
    }
    MessageIn parsed;
    if (!ParseMessageType(json.Get("type").StringValue(), parsed.type)) {
        return false;
    }
    if (!ReadMessageOrigin(json, parsed)) {
        return false;
    }
    switch (parsed.type) {
    case MessageIn::Type::GridPress:
    case MessageIn::Type::GridPressureChange:
        if (!ReadSize(json.Get("gridSlot"), parsed.gridSlotIx) || !ReadInt(json.Get("x"), parsed.gridX) ||
            !ReadInt(json.Get("y"), parsed.gridY) ||
            !ReadU8(json.Get("velocity"), parsed.velocity, std::numeric_limits<std::uint8_t>::max())) {
            return false;
        }
        value = parsed;
        return true;
    case MessageIn::Type::GridRelease:
        if (!ReadSize(json.Get("gridSlot"), parsed.gridSlotIx) || !ReadInt(json.Get("x"), parsed.gridX) ||
            !ReadInt(json.Get("y"), parsed.gridY)) {
            return false;
        }
        value = parsed;
        return true;
    case MessageIn::Type::SelectGrid:
        if (!ReadSize(json.Get("gridSlot"), parsed.gridSlotIx) || !ReadSize(json.Get("grid"), parsed.gridIx)) {
            return false;
        }
        value = parsed;
        return true;
    case MessageIn::Type::AppAction:
        // appActionIx round-trips as 0 -- ToJSON never wrote it.
        value = parsed;
        return true;
    case MessageIn::Type::ParamIncDec:
    case MessageIn::Type::ParamSetAbsolute:
    case MessageIn::Type::ParamSetAbsoluteOnBank:
    case MessageIn::Type::ParamPush:
    case MessageIn::Type::ToggleReset:
    case MessageIn::Type::ToggleRandom:
    case MessageIn::Type::ToggleRandomMod:
    case MessageIn::Type::ToggleGestureSelect:
    case MessageIn::Type::SetGestureSelect:
    case MessageIn::Type::SelectParamBank:
    case MessageIn::Type::NextParamBank:
    case MessageIn::Type::PrevParamBank:
    case MessageIn::Type::Start:
    case MessageIn::Type::Continue:
    case MessageIn::Type::Stop:
    case MessageIn::Type::Clock:
    case MessageIn::Type::SetGestureValue:
    case MessageIn::Type::SceneSelect:
    case MessageIn::Type::SetSceneBlend:
    case MessageIn::Type::HoldDrill:
        break;
    }
    if (!ReadSize(json.Get("slotIx"), parsed.slotIx) || !ReadSize(json.Get("position"), parsed.position) ||
        !ReadSize(json.Get("gestureIx"), parsed.gestureIx) ||
        !ReadSize(json.Get("bankIx"), parsed.bankIx) || !ReadSize(json.Get("sceneIx"), parsed.sceneIx) ||
        !ReadFloat(json.Get("value"), parsed.value) || !ReadFloat(json.Get("delta"), parsed.delta) ||
        !ReadBool(json.Get("boolValue"), parsed.boolValue) ||
        !ReadBool(json.Get("hasBoolValue"), parsed.hasBoolValue)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const WrldBldrSystemPosition& value) {
    JSON json = arena.Object();
    json.SetNew("channel", arena.Integer(value.channel));
    json.SetNew("x", arena.Integer(value.x));
    json.SetNew("y", arena.Integer(value.y));
    return json;
}

bool FromJSON(JSON json, WrldBldrSystemPosition& value) {
    if (!IsObject(json)) {
        return false;
    }
    WrldBldrSystemPosition parsed;
    if (!ReadU8(json.Get("channel"), parsed.channel, 0x0F) || !ReadU8(json.Get("x"), parsed.x, 7) ||
        !ReadU8(json.Get("y"), parsed.y, 15)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, LaunchpadController value) {
    switch (value) {
    case LaunchpadController::LaunchpadX:
        return arena.String("launchpadX");
    case LaunchpadController::LaunchpadProMk3:
        return arena.String("launchpadProMk3");
    case LaunchpadController::LaunchpadMiniMk3:
        return arena.String("launchpadMiniMk3");
    }
    return arena.String("launchpadX");
}

bool FromJSON(JSON json, LaunchpadController& value) {
    if (!IsString(json)) {
        return false;
    }
    const std::string_view name(json.StringValue());
    if (name == "launchpadX") {
        value = LaunchpadController::LaunchpadX;
        return true;
    }
    if (name == "launchpadProMk3") {
        value = LaunchpadController::LaunchpadProMk3;
        return true;
    }
    if (name == "launchpadMiniMk3") {
        value = LaunchpadController::LaunchpadMiniMk3;
        return true;
    }
    return false;
}

JSON ToJSON(JsonArena& arena, const LaunchpadGridPosition& value) {
    JSON json = arena.Object();
    json.SetNew("controller", ToJSON(arena, value.controller));
    json.SetNew("x", arena.Integer(value.x));
    json.SetNew("y", arena.Integer(value.y));
    return json;
}

bool FromJSON(JSON json, LaunchpadGridPosition& value) {
    if (!IsObject(json)) {
        return false;
    }
    LaunchpadGridPosition parsed;
    if (!FromJSON(json.Get("controller"), parsed.controller) || !ReadInt(json.Get("x"), parsed.x) ||
        !ReadInt(json.Get("y"), parsed.y) || !LaunchpadShapeSupports(parsed.controller, parsed.x, parsed.y)) {
        return false;
    }
    value = parsed;
    return true;
}

JSON ToJSON(JsonArena& arena, const MidiControllerSystemMessageAssociation& value) {
    JSON json = arena.Object();
    if (value.control.has_value()) {
        json.SetNew("control", ToJSON(arena, *value.control));
    } else {
        json.SetNew("control", arena.Null());
    }
    if (value.wrldBldrPosition.has_value()) {
        json.SetNew("wrldBldrPosition", ToJSON(arena, *value.wrldBldrPosition));
    } else {
        json.SetNew("wrldBldrPosition", arena.Null());
    }
    if (value.launchpadPosition.has_value()) {
        json.SetNew("launchpadPosition", ToJSON(arena, *value.launchpadPosition));
    } else {
        json.SetNew("launchpadPosition", arena.Null());
    }
    json.SetNew("press", ToJSON(arena, value.press));
    if (value.release.has_value()) {
        json.SetNew("release", ToJSON(arena, *value.release));
    } else {
        json.SetNew("release", arena.Null());
    }
    json.SetNew("feedback", ToJSON(arena, value.feedback));
    json.SetNew("outputFeedback", arena.Boolean(value.outputFeedback));
    if (value.press.type == MessageIn::Type::AppAction) {
        json.SetNew("appAction", arena.String(value.appAction.c_str()));
        json.SetNew("appActionValue", arena.String(value.appActionValue.c_str()));
    }
    return json;
}

bool FromJSON(JSON json, MidiControllerSystemMessageAssociation& value) {
    if (!IsObject(json)) {
        return false;
    }
    MidiControllerSystemMessageAssociation parsed;
    if (!FromJSON(json.Get("press"), parsed.press) || !FromJSON(json.Get("feedback"), parsed.feedback)) {
        return false;
    }
    const JSON control = json.Get("control");
    if (!control.IsNull()) {
        MidiControlAddress parsedControl;
        if (!FromJSON(control, parsedControl)) {
            return false;
        }
        parsed.control = parsedControl;
    }
    const JSON position = json.Get("wrldBldrPosition");
    if (!position.IsNull()) {
        WrldBldrSystemPosition parsedPosition;
        if (!FromJSON(position, parsedPosition)) {
            return false;
        }
        parsed.wrldBldrPosition = parsedPosition;
    }
    const JSON launchpadPosition = json.Get("launchpadPosition");
    if (!launchpadPosition.IsNull()) {
        LaunchpadGridPosition parsedPosition;
        if (!FromJSON(launchpadPosition, parsedPosition)) {
            return false;
        }
        parsed.launchpadPosition = parsedPosition;
    }
    const JSON release = json.Get("release");
    if (!release.IsNull()) {
        MessageIn parsedRelease;
        if (!FromJSON(release, parsedRelease)) {
            return false;
        }
        parsed.release = parsedRelease;
    }
    const JSON outputFeedback = json.Get("outputFeedback");
    if (!outputFeedback.IsNull() && !ReadBool(outputFeedback, parsed.outputFeedback)) {
        return false;
    }
    if (ObjectHasKey(json, "appAction")) {
        const JSON appAction = json.Get("appAction");
        if (!IsString(appAction)) {
            return false;
        }
        parsed.appAction = appAction.StringValue();
    }
    if (ObjectHasKey(json, "appActionValue")) {
        const JSON appActionValue = json.Get("appActionValue");
        if (!IsString(appActionValue)) {
            return false;
        }
        parsed.appActionValue = appActionValue.StringValue();
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const MidiControllerProfileConfig& value) {
    JSON json = arena.Object();
    json.SetNew("schema", arena.String(kMidiControllerProfileSchema));
    json.SetNew("schemaVersion", arena.Integer(kMidiControllerProfileSchemaVersion));
    if (value.encoderInput.has_value()) {
        json.SetNew("encoderInput", ToJSON(arena, *value.encoderInput));
    } else {
        json.SetNew("encoderInput", arena.Null());
    }
    if (value.encoderOutput.has_value()) {
        json.SetNew("encoderOutput", ToJSON(arena, *value.encoderOutput));
    } else {
        json.SetNew("encoderOutput", arena.Null());
    }
    if (value.analogInput.has_value()) {
        json.SetNew("analogInput", ToJSON(arena, *value.analogInput));
    } else {
        json.SetNew("analogInput", arena.Null());
    }
    if (value.pressureInput.has_value()) {
        json.SetNew("pressureInput", ToJSON(arena, *value.pressureInput));
    }
    json.SetNew("systemMessages", VectorToJSON(arena, value.systemMessages));
    JSON openSysEx = arena.Array();
    for (const std::vector<std::uint8_t>& message : value.openSysEx) {
        JSON messageJson = arena.Array();
        for (std::uint8_t byte : message) {
            messageJson.AppendNew(arena.Integer(byte));
        }
        openSysEx.AppendNew(messageJson);
    }
    json.SetNew("openSysEx", openSysEx);
    return json;
}

bool FromJSON(JSON json, MidiControllerProfileConfig& value) {
    if (!IsObject(json)) {
        return false;
    }
    const JSON schema = json.Get("schema");
    if (!IsString(schema) || std::string_view(schema.StringValue()) != kMidiControllerProfileSchema) {
        return false;
    }
    const JSON version = json.Get("schemaVersion");
    if (!IsInteger(version) || (version.IntegerValue() != 1 &&
                                version.IntegerValue() != kMidiControllerProfileSchemaVersion)) {
        return false;
    }

    MidiControllerProfileConfig parsed;
    const JSON encoderInput = json.Get("encoderInput");
    if (!encoderInput.IsNull()) {
        EncoderMidiInConfig config;
        if (!FromJSON(encoderInput, config)) {
            return false;
        }
        parsed.encoderInput = std::move(config);
    }
    const JSON encoderOutput = json.Get("encoderOutput");
    if (!encoderOutput.IsNull()) {
        EncoderMidiOutConfig config;
        if (!FromJSON(encoderOutput, config)) {
            return false;
        }
        parsed.encoderOutput = std::move(config);
    }
    const JSON analogInput = json.Get("analogInput");
    if (!analogInput.IsNull()) {
        AnalogMidiInConfig config;
        if (!FromJSON(analogInput, config)) {
            return false;
        }
        parsed.analogInput = std::move(config);
    }
    if (version.IntegerValue() == kMidiControllerProfileSchemaVersion) {
        const JSON pressureInput = json.Get("pressureInput");
        if (!pressureInput.IsNull()) {
            PolyphonicPressureMidiInConfig config;
            if (!FromJSON(pressureInput, config)) {
                return false;
            }
            parsed.pressureInput = std::move(config);
        }
    }
    const JSON systemMessages = json.Get("systemMessages");
    if (!systemMessages.IsNull() && !VectorFromJSON(systemMessages, parsed.systemMessages)) {
        return false;
    }
    const JSON openSysEx = json.Get("openSysEx");
    if (!openSysEx.IsNull()) {
        if (!IsArray(openSysEx)) {
            return false;
        }
        std::vector<std::vector<std::uint8_t>> parsedOpenSysEx;
        parsedOpenSysEx.reserve(openSysEx.Size());
        for (std::size_t ix = 0; ix < openSysEx.Size(); ++ix) {
            const JSON messageJson = openSysEx.GetAt(ix);
            if (!IsArray(messageJson)) {
                return false;
            }
            std::vector<std::uint8_t> message;
            message.reserve(messageJson.Size());
            for (std::size_t byteIx = 0; byteIx < messageJson.Size(); ++byteIx) {
                const JSON byteJson = messageJson.GetAt(byteIx);
                if (!IsInteger(byteJson) || byteJson.IntegerValue() < 0 || byteJson.IntegerValue() > 0xFF) {
                    return false;
                }
                message.push_back(static_cast<std::uint8_t>(byteJson.IntegerValue()));
            }
            parsedOpenSysEx.push_back(std::move(message));
        }
        parsed.openSysEx = std::move(parsedOpenSysEx);
    }
    value = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const MidiEndpointRef& value) {
    JSON json = arena.Object();
    json.SetNew("identifier", arena.String(value.identifier.c_str()));
    json.SetNew("name", arena.String(value.name.c_str()));
    return json;
}

bool FromJSON(JSON json, MidiEndpointRef& value) {
    if (!IsObject(json)) {
        return false;
    }
    MidiEndpointRef parsed;
    const JSON identifier = json.Get("identifier");
    if (!IsString(identifier)) {
        return false;
    }
    parsed.identifier = identifier.StringValue();
    const JSON name = json.Get("name");
    if (!IsString(name)) {
        return false;
    }
    parsed.name = name.StringValue();
    value = std::move(parsed);
    return true;
}

const char* MidiControllerDispositionName(MidiControllerDisposition value) {
    switch (value) {
    case MidiControllerDisposition::Active:
        return "active";
    case MidiControllerDisposition::Blacklisted:
        return "blacklisted";
    }
    return "active";
}

bool MidiControllerDispositionFromName(std::string_view value, MidiControllerDisposition& out) {
    if (value == "active") {
        out = MidiControllerDisposition::Active;
        return true;
    }
    if (value == "blacklisted") {
        out = MidiControllerDisposition::Blacklisted;
        return true;
    }
    return false;
}

JSON ToJSON(JsonArena& arena, const MidiControllerSlot& value) {
    JSON json = arena.Object();
    json.SetNew("name", arena.String(value.name.c_str()));
    json.SetNew("kind", arena.String(MidiProfileKindName(value.kind)));
    json.SetNew("disposition", arena.String(MidiControllerDispositionName(value.disposition)));
    if (value.wizardId.has_value()) {
        json.SetNew("wizardId", arena.String(value.wizardId->c_str()));
    }
    json.SetNew("input", ToJSON(arena, value.input));
    json.SetNew("output", ToJSON(arena, value.output));
    if (IsActive(value)) {
        json.SetNew("profile", ToJSON(arena, value.config));
    } else if (value.dormantConfig.has_value()) {
        json.SetNew("profile", ToJSON(arena, *value.dormantConfig));
    }
    return json;
}

bool MidiControllerSlotFromJSON(JSON json, MidiControllerSlot& value, int instrumentSchemaVersion) {
    if (!IsObject(json)) {
        return false;
    }
    const JSON name = json.Get("name");
    if (!IsString(name)) {
        return false;
    }
    const JSON kind = json.Get("kind");
    if (!IsString(kind)) {
        return false;
    }
    MidiControllerSlot parsed;
    parsed.name = name.StringValue();
    if (!MidiProfileKindFromName(kind.StringValue(), parsed.kind)) {
        return false;
    }
    if (instrumentSchemaVersion == kMidiInstrumentPreviousSchemaVersion) {
        parsed.disposition = MidiControllerDisposition::Active;
    } else if (instrumentSchemaVersion == kMidiInstrumentSchemaVersion) {
        const JSON disposition = json.Get("disposition");
        if (!IsString(disposition) ||
            !MidiControllerDispositionFromName(disposition.StringValue(), parsed.disposition)) {
            return false;
        }
    } else {
        return false;
    }
    if (instrumentSchemaVersion == kMidiInstrumentSchemaVersion && ObjectHasKey(json, "wizardId")) {
        const JSON wizardId = json.Get("wizardId");
        if (!IsString(wizardId) || std::string_view(wizardId.StringValue()).empty()) {
            return false;
        }
        parsed.wizardId = wizardId.StringValue();
    }
    if (!FromJSON(json.Get("input"), parsed.input) || !FromJSON(json.Get("output"), parsed.output)) {
        return false;
    }
    if (IsActive(parsed)) {
        if (!FromJSON(json.Get("profile"), parsed.config)) {
            return false;
        }
    } else if (ObjectHasKey(json, "profile")) {
        MidiControllerProfileConfig dormantConfig;
        if (!FromJSON(json.Get("profile"), dormantConfig)) {
            return false;
        }
        parsed.dormantConfig = std::move(dormantConfig);
    }
    if (!SlotValidForKind(parsed)) {
        return false;
    }
    value = std::move(parsed);
    return true;
}

bool FromJSON(JSON json, MidiControllerSlot& value) {
    return MidiControllerSlotFromJSON(json, value, kMidiInstrumentSchemaVersion);
}

JSON ToJSON(JsonArena& arena, const MidiInstrumentConfig& instrument) {
    JSON json = arena.Object();
    json.SetNew("schema", arena.String(kMidiInstrumentSchema));
    json.SetNew("schemaVersion", arena.Integer(kMidiInstrumentSchemaVersion));
    json.SetNew("controllers", VectorToJSON(arena, instrument.controllers));
    return json;
}

bool FromJSON(JSON json, MidiInstrumentConfig& out) {
    if (!IsObject(json)) {
        return false;
    }
    const JSON schema = json.Get("schema");
    if (!IsString(schema) || std::string_view(schema.StringValue()) != kMidiInstrumentSchema) {
        return false;
    }
    const JSON version = json.Get("schemaVersion");
    if (!IsInteger(version) ||
        (version.IntegerValue() != kMidiInstrumentPreviousSchemaVersion &&
         version.IntegerValue() != kMidiInstrumentSchemaVersion)) {
        return false;
    }
    const JSON controllers = json.Get("controllers");
    if (!IsArray(controllers)) {
        return false;
    }

    MidiInstrumentConfig scratch;
    for (std::size_t ix = 0; ix < controllers.Size(); ++ix) {
        MidiControllerSlot slot;
        if (!MidiControllerSlotFromJSON(controllers.GetAt(ix), slot, static_cast<int>(version.IntegerValue()))) {
            return false;
        }
        if (!scratch.AddController(std::move(slot))) {
            return false;
        }
    }

    out = std::move(scratch);
    return true;
}

namespace {

MidiControllerProfileResult CreateMidiControllerProfileImpl(
    const MidiControllerProfileConfig& config, MessageInBus* bus, MidiSender* sender,
    ParameterManager::UIState* parameterUIState, RuntimeUIState* runtimeUIState,
    MidiInProcessor::TimestampProvider timestampProvider, std::size_t sinkIx,
    AbsoluteFeedbackCoordinator* absoluteFeedback, std::size_t controllerSlot,
    std::optional<MidiProfileKind> profileKind) {
    MidiControllerProfileResult result;
    result.holdDrill = std::make_unique<HoldDrillState>();
    HoldDrillState* const holdDrill = result.holdDrill.get();
    const EncoderMode feedbackMode =
        config.encoderInput.has_value() ? config.encoderInput->mode : EncoderMode::Signed7Bit;
    AbsoluteFeedbackCoordinator* activeAbsoluteFeedback =
        feedbackMode == EncoderMode::Absolute ? absoluteFeedback : nullptr;
    MidiInProcessor* tail = nullptr;
    auto appendInput = [&](std::unique_ptr<MidiInProcessor> processor) {
        processor->SetMessageInBus(bus);
        processor->SetTimestampProvider(timestampProvider);
        if (result.input == nullptr) {
            result.input = std::move(processor);
            tail = result.input.get();
            return;
        }
        tail->SetThru(processor.get());
        tail = processor.get();
        result.inputThru.push_back(std::move(processor));
    };

    if (config.encoderInput.has_value()) {
        appendInput(std::make_unique<EncoderMidiInProcessor>(
            *config.encoderInput, bus, activeAbsoluteFeedback, controllerSlot, holdDrill));
    }
    if (config.analogInput.has_value()) {
        appendInput(std::make_unique<AnalogMidiInProcessor>(*config.analogInput, bus));
    }
    if (!config.systemMessages.empty()) {
        SystemButtonMidiInConfig systemInput;
        systemInput.associations.reserve(config.systemMessages.size());
        for (const MidiControllerSystemMessageAssociation& association : config.systemMessages) {
            systemInput.associations.push_back({
                .control = association.control,
                .launchpadPosition = association.launchpadPosition,
                .press = association.press,
                .release = association.release,
            });
        }
        appendInput(std::make_unique<SystemButtonMidiInProcessor>(std::move(systemInput), bus, holdDrill));
    }
    if (config.pressureInput.has_value()) {
        appendInput(std::make_unique<PolyphonicPressureMidiInProcessor>(*config.pressureInput, bus));
    }
    appendInput(std::make_unique<RealtimeMidiInProcessor>(controllerSlot, bus));

    if (config.encoderOutput.has_value()) {
        switch (config.encoderOutput->protocol) {
        case EncoderMidiOutProtocol::WrldBldr:
            result.outputs.push_back(std::make_unique<WrldBldrMidiOutProcessor>(
                *config.encoderOutput, sender, parameterUIState, sinkIx, feedbackMode,
                activeAbsoluteFeedback, controllerSlot));
            break;
        case EncoderMidiOutProtocol::Twister:
            result.outputs.push_back(std::make_unique<TwisterMidiOutProcessor>(
                *config.encoderOutput, sender, parameterUIState, sinkIx, feedbackMode,
                activeAbsoluteFeedback, controllerSlot));
            break;
        }
    } else if (profileKind == MidiProfileKind::Generic && config.encoderInput.has_value()) {
        result.outputs.push_back(std::make_unique<GenericMidiOutProcessor>(
            *config.encoderInput, sender, parameterUIState, sinkIx, activeAbsoluteFeedback,
            controllerSlot));
    }

    SystemCcMidiOutConfig ccOutput;
    WrldBldrSystemMidiOutConfig wrldOutput;
    LaunchpadGridMidiOutConfig launchpadXOutput;
    LaunchpadGridMidiOutConfig launchpadProOutput;
    LaunchpadGridMidiOutConfig launchpadMiniOutput;
    auto launchpadOutputFor = [&](LaunchpadController controller) -> LaunchpadGridMidiOutConfig& {
        switch (controller) {
        case LaunchpadController::LaunchpadX:
            return launchpadXOutput;
        case LaunchpadController::LaunchpadProMk3:
            return launchpadProOutput;
        case LaunchpadController::LaunchpadMiniMk3:
            return launchpadMiniOutput;
        }
        return launchpadXOutput;
    };
    for (const MidiControllerSystemMessageAssociation& association : config.systemMessages) {
        // MF Twister side buttons are CC input-only; position-based controllers still use their own output paths.
        if (association.control.has_value() && association.control->type == MidiControlType::Cc &&
            association.outputFeedback) {
            ccOutput.associations.push_back({
                .control = *association.control,
                .message = association.feedback,
            });
        }
        if (association.wrldBldrPosition.has_value()) {
            wrldOutput.associations.push_back({
                .position = *association.wrldBldrPosition,
                .message = association.feedback,
            });
        }
        if (association.launchpadPosition.has_value()) {
            launchpadOutputFor(association.launchpadPosition->controller).associations.push_back({
                .position = *association.launchpadPosition,
                .message = association.feedback,
            });
        }
    }
    if (!ccOutput.associations.empty()) {
        if (runtimeUIState != nullptr) {
            result.outputs.push_back(std::make_unique<SystemCcMidiOutProcessor>(
                std::move(ccOutput), sender, runtimeUIState, sinkIx));
        } else {
            result.outputs.push_back(std::make_unique<SystemCcMidiOutProcessor>(
                std::move(ccOutput), sender, parameterUIState, sinkIx));
        }
    }
    if (!wrldOutput.associations.empty()) {
        if (runtimeUIState != nullptr) {
            result.outputs.push_back(std::make_unique<WrldBldrSystemMidiOutProcessor>(
                std::move(wrldOutput), sender, runtimeUIState, sinkIx));
        } else {
            result.outputs.push_back(std::make_unique<WrldBldrSystemMidiOutProcessor>(
                std::move(wrldOutput), sender, parameterUIState, sinkIx));
        }
    }
    if (!launchpadXOutput.associations.empty()) {
        if (runtimeUIState != nullptr) {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadXOutput), sender, runtimeUIState, sinkIx));
        } else {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadXOutput), sender, parameterUIState, sinkIx));
        }
    }
    if (!launchpadProOutput.associations.empty()) {
        if (runtimeUIState != nullptr) {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadProOutput), sender, runtimeUIState, sinkIx));
        } else {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadProOutput), sender, parameterUIState, sinkIx));
        }
    }
    if (!launchpadMiniOutput.associations.empty()) {
        if (runtimeUIState != nullptr) {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadMiniOutput), sender, runtimeUIState, sinkIx));
        } else {
            result.outputs.push_back(std::make_unique<LaunchpadGridMidiOutProcessor>(
                std::move(launchpadMiniOutput), sender, parameterUIState, sinkIx));
        }
    }

    if (!config.openSysEx.empty()) {
        result.outputs.push_back(
            std::make_unique<OpenSysExMidiOutProcessor>(config.openSysEx, sender, sinkIx));
    }

    return result;
}

}  // namespace

MidiControllerProfileResult CreateBlacklistedMidiControllerProfile() {
    MidiControllerProfileResult result;
    result.input = std::make_unique<DropMidiInProcessor>();
    return result;
}

MidiControllerProfileResult CreateMidiControllerProfile(
    const MidiControllerProfileConfig& config, MessageInBus* bus, MidiSender* sender,
    RuntimeUIState* uiState, MidiInProcessor::TimestampProvider timestampProvider,
    std::size_t sinkIx, AbsoluteFeedbackCoordinator* absoluteFeedback,
    std::size_t controllerSlot, std::optional<MidiProfileKind> profileKind) {
    return CreateMidiControllerProfileImpl(config, bus, sender,
                                           uiState != nullptr ? uiState->parameters : nullptr,
                                           uiState, std::move(timestampProvider), sinkIx,
                                           absoluteFeedback, controllerSlot, profileKind);
}

MidiControllerProfileResult CreateMidiControllerProfile(
    const MidiControllerProfileConfig& config, MessageInBus* bus, MidiSender* sender,
    ParameterManager::UIState* uiState, MidiInProcessor::TimestampProvider timestampProvider,
    std::size_t sinkIx, AbsoluteFeedbackCoordinator* absoluteFeedback,
    std::size_t controllerSlot, std::optional<MidiProfileKind> profileKind) {
    return CreateMidiControllerProfileImpl(config, bus, sender, uiState, nullptr,
                                           std::move(timestampProvider), sinkIx,
                                           absoluteFeedback, controllerSlot, profileKind);
}

MidiControllerProfileResult CreateMidiControllerProfile(
    const MidiControllerProfileConfig& config, MessageInBus* bus, MidiSender* sender,
    std::nullptr_t, MidiInProcessor::TimestampProvider timestampProvider,
    std::size_t sinkIx, AbsoluteFeedbackCoordinator* absoluteFeedback,
    std::size_t controllerSlot, std::optional<MidiProfileKind> profileKind) {
    return CreateMidiControllerProfile(config, bus, sender,
                                       static_cast<RuntimeUIState*>(nullptr),
                                       std::move(timestampProvider), sinkIx,
                                       absoluteFeedback, controllerSlot, profileKind);
}

MidiControllerProfileConfig WrldBldrDefaultProfileConfig(WrldBldrDefaultProfileOptions options) {
    MidiControllerProfileConfig config;
    config.encoderInput = EncoderMidiInConfig::WrldBldrDefault(options.slotIx);
    config.encoderInput->KeepFirstPositions(options.visibleEncoderCount);
    config.encoderOutput = EncoderMidiOutConfig::WrldBldrDefault(options.slotIx);
    config.encoderOutput->KeepFirstPositions(options.visibleEncoderCount);

    auto addAnalogLogical = [&](MidiControlAddress control, std::size_t logicalIx) {
        if (logicalIx == 0) {
            config.analogInput->sceneBlend = control;
        } else if (logicalIx <= 16) {
            config.analogInput->gestures.push_back({.control = control, .gestureIx = logicalIx - 1});
        }
    };

    config.analogInput = AnalogMidiInConfig{};
    for (std::uint8_t cc = 0; cc <= 16; ++cc) {
        addAnalogLogical({.channel = 2, .cc = cc}, cc);
    }
    for (std::uint8_t cc = 0; cc <= 14; ++cc) {
        addAnalogLogical({.channel = 14, .cc = cc}, static_cast<std::size_t>(cc) + 2);
    }

    auto addSystemPosition = [&](std::uint8_t x, std::uint8_t y, MessageIn press,
                                 std::optional<MessageIn> release = std::nullopt) {
        const WrldBldrSystemPosition position{.channel = 5, .x = x, .y = y};
        config.systemMessages.push_back({
            .control = MidiControlAddress{.channel = 5, .cc = WrldBldrPositionToCC(x, y)},
            .wrldBldrPosition = position,
            .press = press,
            .release = release,
            .feedback = press,
        });
    };

    // Source-derived from TheNonagonSquiggleBoyWrldBldr.hpp AuxGrid:
    // channel 5 maps x = cc % 8, y = cc / 8; reset/random/random-mod begin
    // at row 4, scene selectors live on row 6, gesture selectors on rows 0/1,
    // and bank selectors occupy rows 3 (first eight) and 2 (second eight).
    addSystemPosition(0, 4, MessageIn::SetReset(0, true), MessageIn::SetReset(0, false));
    addSystemPosition(1, 4, MessageIn::SetRandom(0, true), MessageIn::SetRandom(0, false));
    addSystemPosition(2, 4, MessageIn::SetRandomMod(0, true), MessageIn::SetRandomMod(0, false));

    for (std::size_t sceneIx = 0; sceneIx < options.sceneCount; ++sceneIx) {
        addSystemPosition(static_cast<std::uint8_t>(sceneIx % 8), 6, MessageIn::SceneSelect(0, sceneIx));
    }

    for (std::size_t bankIx = 0; bankIx < options.bankButtonCount; ++bankIx) {
        const std::uint8_t x = static_cast<std::uint8_t>(bankIx % 8);
        const std::uint8_t y = static_cast<std::uint8_t>(bankIx < 8 ? 3 : 2);
        addSystemPosition(x, y, MessageIn::SelectParamBank(0, options.slotIx, bankIx));
    }

    for (std::size_t gestureIx = 0; gestureIx < options.gestureSelectorCount; ++gestureIx) {
        const std::uint8_t x = static_cast<std::uint8_t>(gestureIx % 8);
        const std::uint8_t y = static_cast<std::uint8_t>(gestureIx < 8 ? 0 : 1);
        addSystemPosition(x, y, MessageIn::SetGestureSelect(0, gestureIx, true),
                          MessageIn::SetGestureSelect(0, gestureIx, false));
    }

    return config;
}

MidiControllerSlot WrldBldrDefaultControllerSlot(std::string name, WrldBldrDefaultProfileOptions options) {
    MidiControllerSlot slot;
    slot.name = std::move(name);
    slot.kind = MidiProfileKind::WrldBldr;
    slot.disposition = MidiControllerDisposition::Active;
    slot.config = WrldBldrDefaultProfileConfig(options);
    return slot;
}

MidiInstrumentConfig DefaultMidiInstrumentConfig() {
    MidiInstrumentConfig instrument;
    instrument.AddController(WrldBldrDefaultControllerSlot());
    return instrument;
}

MidiControllerProfileResult CreateWrldBldrDefaultProfile(
    WrldBldrDefaultProfileOptions options, MessageInBus* bus, MidiSender* sender,
    ParameterManager::UIState* uiState, MidiInProcessor::TimestampProvider timestampProvider) {
    return CreateMidiControllerProfile(WrldBldrDefaultProfileConfig(options), bus, sender, uiState,
                                       std::move(timestampProvider));
}

MidiControllerProfileConfig MfTwisterDefaultProfileConfig(MfTwisterDefaultProfileOptions options) {
    MidiControllerProfileConfig config;
    config.encoderInput = EncoderMidiInConfig::TwisterDefault(options.slotIx);
    config.encoderInput->KeepFirstPositions(options.visibleEncoderCount);
    config.encoderOutput = EncoderMidiOutConfig::TwisterDefault(options.slotIx);
    config.encoderOutput->KeepFirstPositions(options.visibleEncoderCount);

    for (std::size_t ix = 0; ix < options.sideButtons.size(); ++ix) {
        if (!options.sideButtons[ix].has_value()) {
            continue;
        }
        MidiControllerSystemMessageAssociation association = *options.sideButtons[ix];
        association.control = MidiControlAddress{.channel = 3, .cc = static_cast<std::uint8_t>(8 + ix)};
        association.wrldBldrPosition = std::nullopt;
        association.launchpadPosition = std::nullopt;
        association.outputFeedback = false;
        config.systemMessages.push_back(std::move(association));
    }
    return config;
}

MidiControllerProfileResult CreateMfTwisterDefaultProfile(
    MfTwisterDefaultProfileOptions options, MessageInBus* bus, MidiSender* sender,
    ParameterManager::UIState* uiState, MidiInProcessor::TimestampProvider timestampProvider) {
    return CreateMidiControllerProfile(MfTwisterDefaultProfileConfig(options), bus, sender, uiState,
                                       std::move(timestampProvider));
}

MidiControllerProfileConfig LaunchpadDefaultProfileConfig(LaunchpadDefaultProfileOptions options) {
    MidiControllerProfileConfig config;

    auto addSystemPosition = [&](LaunchpadGridPosition position, MessageIn press,
                                 std::optional<MessageIn> release = std::nullopt,
                                 std::optional<MessageIn> feedback = std::nullopt) {
        if (!LaunchpadShapeSupports(position.controller, position.x, position.y)) {
            return;
        }
        config.systemMessages.push_back({
            .launchpadPosition = position,
            .press = press,
            .release = release,
            .feedback = feedback.value_or(press),
        });
    };

    auto position = [&](int x, int y) {
        return LaunchpadGridPosition{.controller = options.controller, .x = x, .y = y};
    };

    for (std::size_t sceneIx = 0; sceneIx < options.sceneCount; ++sceneIx) {
        addSystemPosition(position(static_cast<int>(sceneIx), -1), MessageIn::SceneSelect(0, sceneIx));
    }

    for (std::size_t bankIx = 0; bankIx < options.bankButtonCount; ++bankIx) {
        addSystemPosition(position(8, static_cast<int>(bankIx)),
                          MessageIn::SelectParamBank(0, options.slotIx, bankIx));
    }

    for (std::size_t gestureIx = 0; gestureIx < options.gestureSelectorCount; ++gestureIx) {
        addSystemPosition(position(static_cast<int>(gestureIx), 0),
                          MessageIn::SetGestureSelect(0, gestureIx, true),
                          MessageIn::SetGestureSelect(0, gestureIx, false));
    }

    const LaunchpadGridPosition defaultReset = position(8, -1);
    const LaunchpadGridPosition resetPosition = options.resetPosition.value_or(defaultReset);
    const bool useReset =
        options.resetPosition.has_value() ? resetPosition.controller == options.controller
                                          : LaunchpadShapeSupports(defaultReset.controller, defaultReset.x, defaultReset.y);
    if (useReset) {
        addSystemPosition(resetPosition, MessageIn::SetReset(0, true), MessageIn::SetReset(0, false),
                          MessageIn::ToggleReset(0));
    }

    return config;
}

MidiControllerProfileResult CreateLaunchpadDefaultProfile(
    LaunchpadDefaultProfileOptions options, MessageInBus* bus, MidiSender* sender,
    ParameterManager::UIState* uiState, MidiInProcessor::TimestampProvider timestampProvider) {
    return CreateMidiControllerProfile(LaunchpadDefaultProfileConfig(options), bus, sender, uiState,
                                       std::move(timestampProvider));
}

const char* MidiProfileKindName(MidiProfileKind kind) {
    switch (kind) {
        case MidiProfileKind::WrldBldr: return "wrldbldr";
        case MidiProfileKind::MfTwister: return "twister";
        case MidiProfileKind::Launchpad: return "launchpad";
        case MidiProfileKind::Generic: return "generic";
    }
    return "generic";
}

const char* MidiProfileKindDisplayName(MidiProfileKind kind) {
    switch (kind) {
        case MidiProfileKind::WrldBldr: return "WRLD.Bldr";
        case MidiProfileKind::MfTwister: return "MF Twister";
        case MidiProfileKind::Launchpad: return "Launchpad";
        case MidiProfileKind::Generic: return "Generic";
    }
    return "Generic";
}

bool MidiProfileKindFromName(std::string_view name, MidiProfileKind& out) {
    if (name == "wrldbldr") {
        out = MidiProfileKind::WrldBldr;
        return true;
    }
    if (name == "twister") {
        out = MidiProfileKind::MfTwister;
        return true;
    }
    if (name == "launchpad") {
        out = MidiProfileKind::Launchpad;
        return true;
    }
    if (name == "generic") {
        out = MidiProfileKind::Generic;
        return true;
    }
    return false;
}

MidiKindSupport KindSupport(MidiProfileKind kind) {
    switch (kind) {
        case MidiProfileKind::WrldBldr:
            return MidiKindSupport{.encoders = true, .systemMessages = true, .analogs = true};
        case MidiProfileKind::MfTwister:
            return MidiKindSupport{.encoders = true, .systemMessages = true, .analogs = false};
        case MidiProfileKind::Launchpad:
            return MidiKindSupport{.encoders = false, .systemMessages = true, .analogs = false};
        case MidiProfileKind::Generic:
            return MidiKindSupport{.encoders = true, .systemMessages = true, .analogs = true};
    }
    return MidiKindSupport{.encoders = false, .systemMessages = false, .analogs = false};
}

namespace {

bool Fail(std::string* reason, const char* message) {
    if (reason != nullptr) {
        *reason = message;
    }
    return false;
}

bool ProfileConfigValidForKind(MidiProfileKind kind, const MidiControllerProfileConfig& config,
                               std::string* reason) {
    const MidiKindSupport support = KindSupport(kind);

    if (!support.encoders && (config.encoderInput.has_value() || config.encoderOutput.has_value())) {
        return Fail(reason, "encoders not supported by this controller kind");
    }
    if (!support.analogs && config.analogInput.has_value()) {
        return Fail(reason, "analog input not supported by this controller kind");
    }
    if (!support.systemMessages && !config.systemMessages.empty()) {
        return Fail(reason, "system messages not supported by this controller kind");
    }
    if (config.pressureInput.has_value()) {
        if (const char* error = PolyphonicPressureConfigError(*config.pressureInput)) {
            return Fail(reason, error);
        }
    }

    if (config.encoderInput.has_value()) {
        for (const EncoderMidiMapping& mapping : config.encoderInput->turns) {
            if (mapping.control.type != MidiControlType::Cc) {
                return Fail(reason, "encoder turns must use CC control addresses");
            }
        }
    }
    if (config.analogInput.has_value()) {
        if (config.analogInput->sceneBlend.has_value() &&
            config.analogInput->sceneBlend->type != MidiControlType::Cc) {
            return Fail(reason, "scene blend must use a CC control address");
        }
        for (const AnalogMidiMapping& mapping : config.analogInput->gestures) {
            if (mapping.control.type != MidiControlType::Cc) {
                return Fail(reason, "analog gestures must use CC control addresses");
            }
        }
        for (const AnalogAppActionMapping& mapping : config.analogInput->appActions) {
            if (mapping.control.type != MidiControlType::Cc) {
                return Fail(reason, "analog app actions must use CC control addresses");
            }
        }
    }

    for (const MidiControllerSystemMessageAssociation& association : config.systemMessages) {
        if (kind != MidiProfileKind::Generic && association.control.has_value() &&
            association.control->type != MidiControlType::Cc) {
            return Fail(reason, "this controller kind requires CC system-message control addresses");
        }
        if (kind == MidiProfileKind::Launchpad) {
            if (!association.launchpadPosition.has_value()) {
                return Fail(reason, "launchpad system-message entries must carry a launchpad position");
            }
            if (association.control.has_value()) {
                return Fail(reason, "launchpad system-message entries must not carry a control address");
            }
            if (association.wrldBldrPosition.has_value()) {
                return Fail(reason, "launchpad system-message entries must not carry a WRLD.Bldr position");
            }
        } else if (kind == MidiProfileKind::WrldBldr) {
            if (association.launchpadPosition.has_value()) {
                return Fail(reason, "wrldbldr system-message entries must not carry a launchpad position");
            }
            if (!association.control.has_value()) {
                return Fail(reason, "wrldbldr system-message entries must carry a control address");
            }
        } else {
            // MfTwister and Generic: chan/CC addresses only, no positions of either kind.
            if (association.launchpadPosition.has_value()) {
                return Fail(reason, "this controller kind does not support launchpad positions");
            }
            if (association.wrldBldrPosition.has_value()) {
                return Fail(reason, "this controller kind does not support WRLD.Bldr positions");
            }
            if (!association.control.has_value()) {
                return Fail(reason, "this controller kind requires a control address for system-message entries");
            }
            if (kind == MidiProfileKind::MfTwister) {
                // Finding 5: the physical MF Twister side buttons are a
                // fixed hardware shape -- channel 3, cc 8..13 (6 logical
                // buttons, control->cc = 8 + button per D1) -- not an
                // arbitrary chan/cc pair. An association outside that shape
                // cannot come from real hardware and would render as a
                // bogus/unreadable button number (see RowFieldValue's
                // Field::Button case).
                if (association.control->channel != 3) {
                    return Fail(reason, "twister system-message entries must use the fixed hardware channel 3");
                }
                if (association.control->cc < 8 || association.control->cc > 13) {
                    return Fail(reason, "twister system-message entries must use cc 8-13 (side buttons 0-5)");
                }
            }
        }
    }

    return true;
}

} // namespace

bool IsActive(const MidiControllerSlot& slot) {
    return slot.disposition == MidiControllerDisposition::Active;
}

bool SlotValidForKind(const MidiControllerSlot& slot, std::string* reason) {
    if (slot.wizardId.has_value() && slot.wizardId->empty()) {
        return Fail(reason, "wizard id must be non-empty when present");
    }

    if (!IsActive(slot)) {
        if (!slot.wizardId.has_value()) {
            return Fail(reason, "blacklisted controller records require a wizard id");
        }
        if (!slot.input.IsConfigured() || !slot.output.IsConfigured()) {
            return Fail(reason, "blacklisted controller records require both endpoint references");
        }
        if (slot.dormantConfig.has_value()) {
            return ProfileConfigValidForKind(slot.kind, *slot.dormantConfig, reason);
        }
        return true;
    }

    if (slot.dormantConfig.has_value()) {
        return Fail(reason, "active controller records must not carry dormant profile data");
    }
    return ProfileConfigValidForKind(slot.kind, slot.config, reason);
}

bool MidiInstrumentConfig::AddController(MidiControllerSlot slot) {
    if (FindController(slot.name) != nullptr) {
        return false;
    }
    if (!SlotValidForKind(slot)) {
        return false;
    }
    controllers.push_back(std::move(slot));
    return true;
}

bool MidiInstrumentConfig::RenameController(std::size_t ix, std::string name) {
    if (ix >= controllers.size()) {
        return false;
    }
    if (controllers[ix].name != name && FindController(name) != nullptr) {
        return false;
    }
    controllers[ix].name = std::move(name);
    return true;
}

bool MidiInstrumentConfig::ReplaceController(std::size_t ix, MidiControllerSlot slot) {
    if (ix >= controllers.size()) {
        return false;
    }
    const MidiControllerSlot* existingWithName = FindController(slot.name);
    if (existingWithName != nullptr && existingWithName != &controllers[ix]) {
        return false;
    }
    if (!SlotValidForKind(slot)) {
        return false;
    }
    controllers[ix] = std::move(slot);
    return true;
}

void MidiInstrumentConfig::RemoveController(std::size_t ix) {
    if (ix >= controllers.size()) {
        return;
    }
    controllers.erase(controllers.begin() + static_cast<std::ptrdiff_t>(ix));
}

const MidiControllerSlot* MidiInstrumentConfig::FindController(std::string_view name) const {
    for (const MidiControllerSlot& slot : controllers) {
        if (slot.name == name) {
            return &slot;
        }
    }
    return nullptr;
}

std::uint8_t EncoderPositionToCC(std::size_t position) {
    return static_cast<std::uint8_t>(position % 16);
}

std::uint8_t WrldBldrPositionToCC(std::uint8_t x, std::uint8_t y) {
    return static_cast<std::uint8_t>((static_cast<unsigned>(y) * 8u + static_cast<unsigned>(x)) & 0x7F);
}

bool LaunchpadShapeSupports(LaunchpadController controller, int x, int y) {
    if (controller == LaunchpadController::LaunchpadX || controller == LaunchpadController::LaunchpadMiniMk3) {
        return x >= 0 && x < 9 && y >= -1 && y < 8;
    }
    if (controller == LaunchpadController::LaunchpadProMk3) {
        return x >= -1 && x < 9 && y >= -1 && y < 10;
    }
    return false;
}

std::optional<std::uint8_t> LaunchpadPositionToNote(LaunchpadController controller, int x, int y) {
    if (!LaunchpadShapeSupports(controller, x, y)) {
        return std::nullopt;
    }

    int physicalY = 8 - y - 1;
    if (physicalY == -1) {
        physicalY = 9;
    } else if (physicalY == -2) {
        physicalY = -1;
    }

    const int note = 11 + 10 * physicalY + x;
    if (note < 0 || note > 127) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(note);
}

std::optional<LaunchpadGridPosition> LaunchpadNoteToPosition(LaunchpadController controller, std::uint8_t note) {
    int x = 0;
    int y = 0;
    if (note < 10) {
        x = static_cast<int>(note) - 1;
        y = 9;
    } else {
        y = (static_cast<int>(note) - 11) / 10;
        x = (static_cast<int>(note) - 11) % 10;
        if (y == 9) {
            y = -1;
        }
        if (x == 9) {
            x = -1;
            y += 1;
        }
        y = 7 - y;
    }

    const auto matchesNote = [controller, note](int candidateX, int candidateY) {
        const std::optional<std::uint8_t> mapped = LaunchpadPositionToNote(controller, candidateX, candidateY);
        return mapped.has_value() && *mapped == note;
    };

    if (LaunchpadShapeSupports(controller, x, y) && matchesNote(x, y)) {
        return LaunchpadGridPosition{.controller = controller, .x = x, .y = y};
    }

    for (int candidateY = -1; candidateY < 10; ++candidateY) {
        for (int candidateX = -1; candidateX < 9; ++candidateX) {
            if (!LaunchpadShapeSupports(controller, candidateX, candidateY) ||
                !matchesNote(candidateX, candidateY)) {
                continue;
            }
            return LaunchpadGridPosition{.controller = controller, .x = candidateX, .y = candidateY};
        }
    }
    return std::nullopt;
}

std::optional<std::uint8_t> LaunchpadProductByte(LaunchpadController controller) {
    switch (controller) {
    case LaunchpadController::LaunchpadX:
        return 0x0C;
    case LaunchpadController::LaunchpadMiniMk3:
        return 0x0D;
    case LaunchpadController::LaunchpadProMk3:
        return 0x0E;
    }
    return std::nullopt;
}

std::uint8_t ColorToTwister(Color color) {
    if (color == Color::Off) {
        return 0;
    }
    auto codeFromHue = [](float hue) {
        // Smart Grid's MF Twister hue range is 1..126, anchored at 240-degree blue.
        constexpr float h0 = 240.0f / 360.0f;
        constexpr float step = 1.0f / 126.0f;
        const float t = std::fmod(h0 - hue + 1.0f, 1.0f);
        const int code = 1 + static_cast<int>(std::lround(t / step));
        return static_cast<std::uint8_t>(std::clamp(code, 1, 126));
    };

    const HsvColor hsv = ToHsv(color);
    if (hsv.saturation < 0.08f) {
        return codeFromHue(240.0f / 360.0f);
    }
    return codeFromHue(hsv.hueTurns);
}

std::uint8_t FullBrightnessAnimationValue() {
    return TwisterRgbBrightnessValue(1.0f);
}

BasicMidi WrldBldrColorSysex(std::uint64_t timestamp, std::uint8_t channel, std::uint8_t cc, Color color) {
    return BasicMidi::SysEx(timestamp, {
                                           0xF0,
                                           0x79,
                                           0x74,
                                           0x78,
                                           0x00,
                                           0x01,
                                           0x00,
                                           0x20,
                                           channel,
                                           cc,
                                           static_cast<std::uint8_t>(color.r / 2),
                                           static_cast<std::uint8_t>(color.g / 2),
                                           static_cast<std::uint8_t>(color.b / 2),
                                           0xF7,
                                       });
}

BasicMidi LaunchpadColorSysex(std::uint64_t timestamp, LaunchpadController controller, int x, int y, Color color) {
    const std::optional<std::uint8_t> product = LaunchpadProductByte(controller);
    const std::optional<std::uint8_t> note = LaunchpadPositionToNote(controller, x, y);
    if (!product.has_value() || !note.has_value()) {
        return {};
    }
    return BasicMidi::SysEx(timestamp, {
                                           0xF0,
                                           0x00,
                                           0x20,
                                           0x29,
                                           0x02,
                                           *product,
                                           0x03,
                                           0x03,
                                           *note,
                                           static_cast<std::uint8_t>(color.r / 2),
                                           static_cast<std::uint8_t>(color.g / 2),
                                           static_cast<std::uint8_t>(color.b / 2),
                                           0xF7,
                                       });
}

} // namespace synth
