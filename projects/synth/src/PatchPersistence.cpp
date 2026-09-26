#include "synth/PatchPersistence.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace synth {

namespace {

constexpr std::string_view kPatchVersionSuffix = "-000.json";
constexpr std::size_t kRuntimeConfigInitialArenaCapacity = 256 * 1024;
constexpr std::size_t kRuntimeConfigMaxArenaCapacity = 8 * 1024 * 1024;

bool IsObject(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Object;
}

bool IsString(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::String;
}

bool IsInteger(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Integer;
}

bool IsBoolean(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Boolean;
}

bool ValidPatchRoot(JSON root) {
    if (!IsObject(root)) {
        return false;
    }
    const JSON schema = root.Get("schema");
    const JSON version = root.Get("schemaVersion");
    return IsString(schema) && std::string_view(schema.StringValue()) == "sheaf.synth.patch" &&
           IsInteger(version) && version.IntegerValue() == 1;
}

std::optional<int> RuntimeConfigVersion(JSON root) {
    if (!IsObject(root)) {
        return std::nullopt;
    }
    const JSON schema = root.Get("schema");
    const JSON version = root.Get("schemaVersion");
    if (!IsString(schema) || std::string_view(schema.StringValue()) != kRuntimeConfigSchema ||
        !IsInteger(version) ||
        (version.IntegerValue() != 1 && version.IntegerValue() != 2 && version.IntegerValue() != 3)) {
        return std::nullopt;
    }
    return static_cast<int>(version.IntegerValue());
}

std::string SanitizePatchName(std::string_view patchName) {
    std::string result;
    result.reserve(patchName.size());
    for (char ch : patchName) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '-' || ch == '_' || ch == '.' || ch == ' ') {
            result.push_back(ch);
        } else {
            result.push_back('_');
        }
    }
    const auto first = result.find_first_not_of(' ');
    const auto last = result.find_last_not_of(' ');
    if (first == std::string::npos) {
        return "Untitled";
    }
    return result.substr(first, last - first + 1);
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("could not open patch version for reading");
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return contents.str();
}

std::filesystem::path RuntimeConfigTempPath(const std::filesystem::path& configFile) {
    return std::filesystem::path(configFile.string() + ".tmp");
}

} // namespace

JSON ToJSON(JsonArena& arena, const AudioDeviceState& state) {
    JSON json = arena.Object();
    json.SetNew("outputDeviceName", arena.String(state.outputDeviceName.c_str()));
    json.SetNew("inputDeviceName", arena.String(state.inputDeviceName.c_str()));
    return json;
}

bool FromJSON(JSON json, AudioDeviceState& state) {
    if (!IsObject(json)) {
        return false;
    }
    AudioDeviceState parsed;
    const JSON output = json.Get("outputDeviceName");
    const JSON input = json.Get("inputDeviceName");
    if (!output.IsNull()) {
        if (!IsString(output)) {
            return false;
        }
        parsed.outputDeviceName = output.StringValue();
    }
    if (!input.IsNull()) {
        if (!IsString(input)) {
            return false;
        }
        parsed.inputDeviceName = input.StringValue();
    }
    state = std::move(parsed);
    return true;
}

JSON ToJSON(JsonArena& arena, const SyncConfig& config) {
    JSON json = arena.Object();
    json.SetNew("sendClock", arena.Boolean(config.sendClock));
    json.SetNew("receiveClock", arena.Boolean(config.receiveClock));
    json.SetNew("sendTransport", arena.Boolean(config.sendTransport));
    json.SetNew("receiveTransport", arena.Boolean(config.receiveTransport));
    json.SetNew("ppqn", arena.Integer(config.ppqn));
    return json;
}

bool FromJSON(JSON json, SyncConfig& config) {
    if (!IsObject(json) || json.Size() != 5) {
        return false;
    }
    const JSON sendClock = json.Get("sendClock");
    const JSON receiveClock = json.Get("receiveClock");
    const JSON sendTransport = json.Get("sendTransport");
    const JSON receiveTransport = json.Get("receiveTransport");
    const JSON ppqn = json.Get("ppqn");
    if (!IsBoolean(sendClock) || !IsBoolean(receiveClock) ||
        !IsBoolean(sendTransport) || !IsBoolean(receiveTransport) || !IsInteger(ppqn) ||
        ppqn.IntegerValue() < 1 || ppqn.IntegerValue() > 960) {
        return false;
    }
    SyncConfig parsed{
        .sendClock = sendClock.BooleanValue(),
        .receiveClock = receiveClock.BooleanValue(),
        .sendTransport = sendTransport.BooleanValue(),
        .receiveTransport = receiveTransport.BooleanValue(),
        .ppqn = static_cast<int>(ppqn.IntegerValue()),
    };
    config = parsed;
    return true;
}

JSON BuildRuntimeConfigJSON(JsonArena& arena,
                            const MidiInstrumentConfig& instrument,
                            const AudioDeviceState& audioDevice,
                            const SyncConfig& sync) {
    JSON root = arena.Object();
    root.SetNew("schema", arena.String(kRuntimeConfigSchema));
    root.SetNew("schemaVersion", arena.Integer(kRuntimeConfigSchemaVersion));
    root.SetNew("midiInstrument", ToJSON(arena, instrument));
    root.SetNew("audioDevice", ToJSON(arena, audioDevice));
    root.SetNew("sync", ToJSON(arena, sync));
    return root;
}

bool LoadRuntimeConfigJSON(JSON root,
                           MidiInstrumentConfig& instrument,
                           AudioDeviceState& audioDevice,
                           SyncConfig& sync) {
    const std::optional<int> version = RuntimeConfigVersion(root);
    if (!version.has_value()) {
        return false;
    }

    MidiInstrumentConfig parsedInstrument;
    if (!FromJSON(root.Get("midiInstrument"), parsedInstrument)) {
        return false;
    }

    AudioDeviceState parsedAudioDevice;
    if (!FromJSON(root.Get("audioDevice"), parsedAudioDevice)) {
        return false;
    }
    if (*version < 3) {
        // A device name persisted under the older schema is not evidence that the
        // operator chose it, so it is dropped here rather than restored and opened.
        parsedAudioDevice.inputDeviceName.clear();
    }

    SyncConfig parsedSync;
    if (*version >= 2 && !FromJSON(root.Get("sync"), parsedSync)) {
        return false;
    }

    instrument = std::move(parsedInstrument);
    audioDevice = std::move(parsedAudioDevice);
    sync = parsedSync;
    return true;
}

bool ValidateRuntimeConfigJSON(JSON root) {
    MidiInstrumentConfig instrument;
    AudioDeviceState audioDevice;
    SyncConfig sync;
    return LoadRuntimeConfigJSON(root, instrument, audioDevice, sync);
}

RuntimeConfigFileStatus LoadRuntimeConfigFile(const std::filesystem::path& configFile,
                                              MidiInstrumentConfig& instrument,
                                              AudioDeviceState& audioDevice,
                                              SyncConfig& sync) {
    std::error_code ec;
    if (!std::filesystem::exists(configFile, ec)) {
        return ec ? RuntimeConfigFileStatus::IOError : RuntimeConfigFileStatus::Missing;
    }
    if (!std::filesystem::is_regular_file(configFile, ec) || ec) {
        return RuntimeConfigFileStatus::IOError;
    }

    std::string text;
    try {
        text = ReadWholeFile(configFile);
    } catch (const std::exception&) {
        return RuntimeConfigFileStatus::IOError;
    }

    JsonArena arena(kRuntimeConfigInitialArenaCapacity);
    JSON root = arena.Loads(text.c_str());
    while (root.IsNull() && arena.Failed() && arena.Capacity() < kRuntimeConfigMaxArenaCapacity) {
        arena.GrowAndReset();
        root = arena.Loads(text.c_str());
    }
    if (root.IsNull() || arena.Failed()) {
        return RuntimeConfigFileStatus::Invalid;
    }

    return LoadRuntimeConfigJSON(root, instrument, audioDevice, sync)
        ? RuntimeConfigFileStatus::Ok
        : RuntimeConfigFileStatus::Invalid;
}

RuntimeConfigFileStatus SaveRuntimeConfigFile(const std::filesystem::path& configFile,
                                              const MidiInstrumentConfig& instrument,
                                              const AudioDeviceState& audioDevice,
                                              const SyncConfig& sync) {
    std::error_code ec;
    const std::filesystem::path parent = configFile.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return RuntimeConfigFileStatus::IOError;
        }
    }

    JsonArena arena(kRuntimeConfigInitialArenaCapacity);
    JSON root = BuildRuntimeConfigJSON(arena, instrument, audioDevice, sync);
    while ((root.IsNull() || arena.Failed()) && arena.Capacity() < kRuntimeConfigMaxArenaCapacity) {
        arena.GrowAndReset();
        root = BuildRuntimeConfigJSON(arena, instrument, audioDevice, sync);
    }
    if (root.IsNull() || arena.Failed()) {
        return RuntimeConfigFileStatus::Invalid;
    }

    char* dumped = root.Dumps(JSON_ENCODE_ANY);
    if (dumped == nullptr) {
        return RuntimeConfigFileStatus::Invalid;
    }
    const std::string jsonText(dumped);
    std::free(dumped);

    const std::filesystem::path tempFile = RuntimeConfigTempPath(configFile);
    std::filesystem::remove(tempFile, ec);

    std::ofstream out(tempFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::filesystem::remove(tempFile, ec);
        return RuntimeConfigFileStatus::IOError;
    }
    out << jsonText;
    if (!out) {
        out.close();
        std::filesystem::remove(tempFile, ec);
        return RuntimeConfigFileStatus::IOError;
    }
    out.close();
    if (!out) {
        std::filesystem::remove(tempFile, ec);
        return RuntimeConfigFileStatus::IOError;
    }

    std::filesystem::rename(tempFile, configFile, ec);
    if (ec) {
        std::filesystem::remove(tempFile, ec);
        return RuntimeConfigFileStatus::IOError;
    }
    return RuntimeConfigFileStatus::Ok;
}

const char* RuntimeConfigFileStatusName(RuntimeConfigFileStatus status) {
    switch (status) {
    case RuntimeConfigFileStatus::Ok:
        return "Ok";
    case RuntimeConfigFileStatus::Missing:
        return "Missing";
    case RuntimeConfigFileStatus::Invalid:
        return "Invalid";
    case RuntimeConfigFileStatus::IOError:
        return "IOError";
    }
    return "Unknown";
}

JSON BuildPatchJSON(JsonArena& arena, std::string_view patchName,
                    const ParameterManager& manager,
                    const MidiInstrumentConfig& instrument,
                    const AudioDeviceState& audioDevice) {
    (void)instrument;
    (void)audioDevice;
    JSON root = arena.Object();
    root.SetNew("schema", arena.String("sheaf.synth.patch"));
    root.SetNew("schemaVersion", arena.Integer(1));
    const std::string patchNameText(patchName);
    root.SetNew("patchName", arena.String(patchNameText.c_str()));
    root.SetNew("parameterValues", manager.ParameterValuesToJSON(arena));
    return root;
}

bool LoadPatchJSON(JSON root, ParameterManager& manager,
                   MidiInstrumentConfig& instrument,
                   AudioDeviceState* audioDevice) {
    (void)instrument;
    (void)audioDevice;
    if (!ValidPatchRoot(root) || !IsString(root.Get("patchName"))) {
        return false;
    }

    const JSON parameterValues = root.Get("parameterValues");
    if (!IsObject(parameterValues)) {
        return false;
    }

    if (!manager.LoadParameterValuesFromJSON(parameterValues)) {
        return false;
    }
    manager.CollectNeutralLocalParameters();
    return true;
}

bool ValidatePatchJSON(JSON root) {
    return ValidPatchRoot(root) && IsString(root.Get("patchName")) && IsObject(root.Get("parameterValues"));
}

std::string TimestampPatchFilename(std::chrono::system_clock::time_point now) {
    const std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream name;
    name << std::put_time(&tm, "%Y-%m-%dT%H-%M-%S") << kPatchVersionSuffix;
    return name.str();
}

std::filesystem::path PatchDirectory(const std::filesystem::path& patchesRoot, std::string_view patchName) {
    return patchesRoot / SanitizePatchName(patchName);
}

std::filesystem::path SavePatchVersion(const std::filesystem::path& patchesRoot, std::string_view patchName,
                                       const std::string& jsonText,
                                       std::chrono::system_clock::time_point now) {
    return SavePatchVersionInDirectory(PatchDirectory(patchesRoot, patchName), jsonText, now);
}

std::filesystem::path SavePatchVersionInDirectory(
    const std::filesystem::path& patchDir, const std::string& jsonText,
    std::chrono::system_clock::time_point now) {
    std::filesystem::create_directories(patchDir);

    const std::string base = TimestampPatchFilename(now);
    const std::string stem = base.substr(0, base.size() - kPatchVersionSuffix.size());
    std::filesystem::path versionFile = patchDir / base;
    for (int suffix = 1; std::filesystem::exists(versionFile); ++suffix) {
        std::ostringstream candidate;
        candidate << stem << '-' << std::setw(3) << std::setfill('0') << suffix << ".json";
        versionFile = patchDir / candidate.str();
    }

    const std::filesystem::path tempFile = versionFile.string() + ".tmp";
    std::filesystem::remove(tempFile);

    std::ofstream out(tempFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("could not open patch version for writing");
    }
    out << jsonText;
    if (!out) {
        std::filesystem::remove(tempFile);
        throw std::runtime_error("could not write patch version");
    }
    out.close();
    if (!out) {
        std::filesystem::remove(tempFile);
        throw std::runtime_error("could not close patch version");
    }
    std::filesystem::rename(tempFile, versionFile);
    return versionFile;
}

std::optional<std::filesystem::path> LatestPatchVersion(const std::filesystem::path& patchDir) {
    if (!std::filesystem::exists(patchDir) || !std::filesystem::is_directory(patchDir)) {
        return std::nullopt;
    }

    std::vector<std::filesystem::path> versions;
    for (const auto& entry : std::filesystem::directory_iterator(patchDir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            versions.push_back(entry.path());
        }
    }
    if (versions.empty()) {
        return std::nullopt;
    }

    std::sort(versions.begin(), versions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.filename().string() < rhs.filename().string();
    });
    return versions.back();
}

std::string LoadPatchVersionText(const std::filesystem::path& versionFile) {
    return ReadWholeFile(versionFile);
}

PatchMessageIn PatchMessageIn::LoadFromJSON(JsonDocument document) {
    PatchMessageIn message;
    message.type = Type::LoadFromJSON;
    message.document = std::move(document);
    return message;
}

PatchMessageIn PatchMessageIn::RevertAllToDefault() {
    PatchMessageIn message;
    message.type = Type::RevertAllToDefault;
    return message;
}

PatchMessageIn PatchMessageIn::SerializeToJSON(std::uint64_t requestId, std::string patchName) {
    PatchMessageIn message;
    message.type = Type::SerializeToJSON;
    message.requestId = requestId;
    message.patchName = std::move(patchName);
    return message;
}

MessageOut MessageOut::SerializedJSON(std::uint64_t requestId, JsonDocument document) {
    MessageOut message;
    message.type = Type::SerializedJSON;
    message.requestId = requestId;
    message.document = std::move(document);
    return message;
}

PatchMessageInBus::PatchMessageInBus(std::size_t capacity)
    : queue_(capacity == 0 ? 1 : capacity) {}

bool PatchMessageInBus::Push(const PatchMessageIn& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size >= queue_.size()) {
        return false;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    queue_[tail] = message;
    tail_.store((tail + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_add(1, std::memory_order_release);
    return true;
}

bool PatchMessageInBus::Pop(PatchMessageIn& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size == 0) {
        return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    message = std::move(queue_[head]);
    head_.store((head + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_sub(1, std::memory_order_release);
    return true;
}

MessageOutBus::MessageOutBus(std::size_t capacity)
    : queue_(capacity == 0 ? 1 : capacity) {}

bool MessageOutBus::Push(const MessageOut& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size >= queue_.size()) {
        return false;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    queue_[tail] = message;
    tail_.store((tail + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_add(1, std::memory_order_release);
    return true;
}

bool MessageOutBus::Pop(MessageOut& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size == 0) {
        return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    message = std::move(queue_[head]);
    head_.store((head + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_sub(1, std::memory_order_release);
    return true;
}

PatchApplyStatus ApplyPatchMessage(
    const PatchMessageIn& message, ParameterManager& manager,
    MidiInstrumentConfig& instrument, const MidiInstrumentConfig& defaultInstrument,
    AudioDeviceState& audioDevice, const AudioDeviceState& defaultAudioDevice,
    MessageOutBus& outputBus, PatchSerializationContext context) {
    (void)defaultInstrument;
    (void)defaultAudioDevice;
    switch (message.type) {
    case PatchMessageIn::Type::LoadFromJSON:
        if (message.document.root.IsNull() ||
            !LoadPatchJSON(message.document.root, manager, instrument, &audioDevice)) {
            return PatchApplyStatus::InvalidJSON;
        }
        return PatchApplyStatus::Applied;
    case PatchMessageIn::Type::RevertAllToDefault:
        manager.RevertAllToDefaults();
        return PatchApplyStatus::Reverted;
    case PatchMessageIn::Type::SerializeToJSON: {
        const std::string patchName = message.patchName.empty() ? std::string("Untitled") : message.patchName;
        if (context.arena != nullptr) {
            // Caller-owned arena: reuse it in place (audio-thread-safe pointer
            // rewind) and never grow or reallocate it here. Growth on
            // exhaustion is the caller's (message-thread) responsibility.
            //
            // Lifetime contract: Reset() below rewinds the arena and
            // invalidates any JSON previously built in it. The MessageOut we
            // push aliases context.arena non-owningly (see aliasedArena
            // below), so the caller MUST pop and fully consume (finish
            // reading/writing out) that document before calling
            // ApplyPatchMessage again with a context that reuses this same
            // arena — otherwise the next Reset() clobbers the still-queued
            // document's backing memory. The caller must also keep the arena
            // alive until the document is consumed. See the doc comment on
            // PatchSerializationContext::arena for the full contract;
            // PatchManager's single-pending-save gate provides this ordering
            // for all serialize requests that flow through it.
            context.arena->Reset();
            const JSON root = BuildPatchJSON(*context.arena, patchName, manager, instrument, audioDevice);
            if (root.IsNull() || context.arena->Failed()) {
                return PatchApplyStatus::ArenaExhausted;
            }
            // Alias a non-owning shared_ptr so JsonDocument's ownership model
            // stays uniform without freeing the caller's arena.
            std::shared_ptr<JsonArena> aliasedArena(std::shared_ptr<void>(), context.arena);
            if (!outputBus.Push(MessageOut::SerializedJSON(
                    message.requestId, JsonDocument{.arena = std::move(aliasedArena), .root = root}))) {
                return PatchApplyStatus::OutputQueueFull;
            }
            return PatchApplyStatus::Serialized;
        }
        const std::size_t maxArenaCapacity =
            std::max(context.initialArenaCapacity, context.maxArenaCapacity == 0 ? std::size_t{1} : context.maxArenaCapacity);
        auto arena = std::make_shared<JsonArena>(context.initialArenaCapacity);
        JSON root;
        for (;;) {
            root = BuildPatchJSON(*arena, patchName, manager, instrument, audioDevice);
            if (!root.IsNull() && !arena->Failed()) {
                break;
            }
            if (arena->Capacity() >= maxArenaCapacity) {
                return PatchApplyStatus::ArenaExhausted;
            }
            arena->GrowAndReset();
        }
        if (!outputBus.Push(MessageOut::SerializedJSON(message.requestId, JsonDocument{.arena = arena, .root = root}))) {
            return PatchApplyStatus::OutputQueueFull;
        }
        return PatchApplyStatus::Serialized;
    }
    }
    return PatchApplyStatus::InvalidJSON;
}

const char* PatchApplyStatusName(PatchApplyStatus status) {
    switch (status) {
    case PatchApplyStatus::Applied:
        return "Applied";
    case PatchApplyStatus::Reverted:
        return "Reverted";
    case PatchApplyStatus::Serialized:
        return "Serialized";
    case PatchApplyStatus::InvalidJSON:
        return "InvalidJSON";
    case PatchApplyStatus::OutputQueueFull:
        return "OutputQueueFull";
    case PatchApplyStatus::ArenaExhausted:
        return "ArenaExhausted";
    }
    return "Unknown";
}

const char* PatchMessageInTypeName(PatchMessageIn::Type type) {
    switch (type) {
    case PatchMessageIn::Type::LoadFromJSON:
        return "LoadFromJSON";
    case PatchMessageIn::Type::RevertAllToDefault:
        return "RevertAllToDefault";
    case PatchMessageIn::Type::SerializeToJSON:
        return "SerializeToJSON";
    }
    return "Unknown";
}

const char* PatchCommandStatusName(PatchCommandStatus status) {
    switch (status) {
    case PatchCommandStatus::Ok:
        return "Ok";
    case PatchCommandStatus::Pending:
        return "Pending";
    case PatchCommandStatus::NoCompletion:
        return "NoCompletion";
    case PatchCommandStatus::Written:
        return "Written";
    case PatchCommandStatus::NeedsSaveAsPath:
        return "NeedsSaveAsPath";
    case PatchCommandStatus::Busy:
        return "Busy";
    case PatchCommandStatus::AlreadyExists:
        return "AlreadyExists";
    case PatchCommandStatus::NotFound:
        return "NotFound";
    case PatchCommandStatus::InvalidPatch:
        return "InvalidPatch";
    case PatchCommandStatus::QueueFull:
        return "QueueFull";
    case PatchCommandStatus::IOError:
        return "IOError";
    }
    return "Unknown";
}

PatchManager::PatchManager(PatchMessageInBus* inputBus, MessageOutBus* outputBus,
                           std::size_t initialArenaCapacity)
    : inputBus_(inputBus),
      outputBus_(outputBus),
      initialArenaCapacity_(initialArenaCapacity == 0 ? 1 : initialArenaCapacity) {}

void PatchManager::SetBuses(PatchMessageInBus* inputBus, MessageOutBus* outputBus) {
    inputBus_ = inputBus;
    outputBus_ = outputBus;
}

PatchCommandResult PatchManager::NewPatch() {
    if (inputBus_ == nullptr || !inputBus_->Push(PatchMessageIn::RevertAllToDefault())) {
        return {.status = PatchCommandStatus::QueueFull};
    }
    currentPatchDirectory_.reset();
    pendingSave_.reset();
    return {.status = PatchCommandStatus::Ok};
}

PatchCommandResult PatchManager::SavePatch() {
    if (!currentPatchDirectory_.has_value()) {
        return {.status = PatchCommandStatus::NeedsSaveAsPath};
    }
    return DispatchSerialize(PendingSave::Kind::Save, *currentPatchDirectory_);
}

PatchCommandResult PatchManager::SavePatchAs(const std::filesystem::path& patchDir) {
    if (pendingSave_.has_value()) {
        return {.status = PatchCommandStatus::Busy, .requestId = pendingSave_->requestId, .path = pendingSave_->patchDir};
    }
    std::error_code ec;
    if (std::filesystem::exists(patchDir, ec) || ec) {
        return {.status = PatchCommandStatus::AlreadyExists, .path = patchDir};
    }
    return DispatchSerialize(PendingSave::Kind::SaveAs, patchDir);
}

PatchCommandResult PatchManager::SavePatchAsOverwrite(const std::filesystem::path& patchDir) {
    if (pendingSave_.has_value()) {
        return {.status = PatchCommandStatus::Busy, .requestId = pendingSave_->requestId, .path = pendingSave_->patchDir};
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(patchDir, ec) || ec) {
        return {.status = ec ? PatchCommandStatus::IOError : PatchCommandStatus::NotFound, .path = patchDir};
    }
    return DispatchSerialize(PendingSave::Kind::SaveAsOverwrite, patchDir);
}

PatchCommandResult PatchManager::LoadPatch(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return {.status = ec ? PatchCommandStatus::IOError : PatchCommandStatus::NotFound, .path = path};
    }
    if (std::filesystem::is_directory(path, ec) && !ec) {
        const auto latest = LatestPatchVersion(path);
        if (!latest.has_value()) {
            return {.status = PatchCommandStatus::NotFound, .path = path};
        }
        return LoadPatchVersion(*latest, path);
    }
    if (ec) {
        return {.status = PatchCommandStatus::IOError, .path = path};
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec) {
        return {.status = ec ? PatchCommandStatus::IOError : PatchCommandStatus::NotFound, .path = path};
    }
    return LoadPatchVersion(path, path.parent_path());
}

PatchCommandResult PatchManager::RevertPatch() {
    if (!currentPatchDirectory_.has_value()) {
        return NewPatch();
    }
    const auto latest = LatestPatchVersion(*currentPatchDirectory_);
    if (!latest.has_value()) {
        return {.status = PatchCommandStatus::NotFound, .path = *currentPatchDirectory_};
    }
    return LoadPatchVersion(*latest, *currentPatchDirectory_);
}

PatchCommandResult PatchManager::ProcessResponses(std::chrono::system_clock::time_point now) {
    if (outputBus_ == nullptr || !pendingSave_.has_value()) {
        return {.status = PatchCommandStatus::NoCompletion};
    }

    MessageOut message;
    while (outputBus_->Pop(message)) {
        if (message.type != MessageOut::Type::SerializedJSON || message.requestId != pendingSave_->requestId) {
            continue;
        }
        const PendingSave pending = *pendingSave_;
        pendingSave_.reset();
        if (message.document.root.IsNull()) {
            return {.status = PatchCommandStatus::InvalidPatch, .requestId = pending.requestId, .path = pending.patchDir};
        }

        if (pending.kind == PendingSave::Kind::SaveAs) {
            std::error_code ec;
            if (std::filesystem::exists(pending.patchDir, ec) || ec) {
                return {.status = PatchCommandStatus::AlreadyExists, .requestId = pending.requestId, .path = pending.patchDir};
            }
        }

        char* dumped = message.document.root.Dumps(JSON_ENCODE_ANY);
        if (dumped == nullptr) {
            return {.status = PatchCommandStatus::InvalidPatch, .requestId = pending.requestId, .path = pending.patchDir};
        }
        const std::string jsonText(dumped);
        std::free(dumped);

        try {
            const std::filesystem::path versionFile = SavePatchVersionInDirectory(pending.patchDir, jsonText, now);
            if (pending.kind == PendingSave::Kind::SaveAs ||
                pending.kind == PendingSave::Kind::SaveAsOverwrite) {
                currentPatchDirectory_ = pending.patchDir;
            }
            return {.status = PatchCommandStatus::Written, .requestId = pending.requestId, .path = versionFile};
        } catch (const std::exception&) {
            return {.status = PatchCommandStatus::IOError, .requestId = pending.requestId, .path = pending.patchDir};
        }
    }
    return {.status = PatchCommandStatus::NoCompletion};
}

PatchCommandResult PatchManager::DispatchSerialize(PendingSave::Kind kind, const std::filesystem::path& patchDir) {
    if (pendingSave_.has_value()) {
        return {.status = PatchCommandStatus::Busy, .requestId = pendingSave_->requestId, .path = pendingSave_->patchDir};
    }
    if (inputBus_ == nullptr || outputBus_ == nullptr) {
        return {.status = PatchCommandStatus::QueueFull, .path = patchDir};
    }
    const std::uint64_t requestId = nextRequestId_++;
    if (!inputBus_->Push(PatchMessageIn::SerializeToJSON(requestId, PatchNameForDirectory(patchDir)))) {
        return {.status = PatchCommandStatus::QueueFull, .requestId = requestId, .path = patchDir};
    }
    pendingSave_ = PendingSave{.kind = kind, .requestId = requestId, .patchDir = patchDir};
    return {.status = PatchCommandStatus::Pending, .requestId = requestId, .path = patchDir};
}

PatchCommandResult PatchManager::LoadPatchVersion(const std::filesystem::path& versionFile,
                                                  const std::filesystem::path& currentPatchDirectory) {
    if (inputBus_ == nullptr) {
        return {.status = PatchCommandStatus::QueueFull, .path = versionFile};
    }
    try {
        JsonDocument document = ParsePatchText(LoadPatchVersionText(versionFile));
        if (document.root.IsNull() || !ValidatePatchJSON(document.root)) {
            return {.status = PatchCommandStatus::InvalidPatch, .path = versionFile};
        }
        if (!inputBus_->Push(PatchMessageIn::LoadFromJSON(std::move(document)))) {
            return {.status = PatchCommandStatus::QueueFull, .path = versionFile};
        }
        currentPatchDirectory_ = currentPatchDirectory;
        pendingSave_.reset();
        return {.status = PatchCommandStatus::Ok, .path = versionFile};
    } catch (const std::exception&) {
        return {.status = PatchCommandStatus::IOError, .path = versionFile};
    }
}

JsonDocument PatchManager::ParsePatchText(const std::string& text) const {
    auto arena = std::make_shared<JsonArena>(initialArenaCapacity_);
    JSON root = arena->Loads(text.c_str());
    while (root.IsNull() && arena->Failed()) {
        arena->GrowAndReset();
        root = arena->Loads(text.c_str());
    }
    return {.arena = arena, .root = root};
}

std::string PatchManager::PatchNameForDirectory(const std::filesystem::path& patchDir) const {
    const std::string filename = patchDir.filename().string();
    return filename.empty() ? std::string("Untitled") : filename;
}

} // namespace synth
