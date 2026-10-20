#pragma once

#include "synth/MidiController.hpp"
#include "synth/MasterClock.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synth {

struct AudioDeviceState {
    std::string outputDeviceName;  // empty = system default
    std::string inputDeviceName;   // empty = system default
};

// Value equality (field-wise). Used by runtime/config persistence code to
// compare host-visible audio device selections.
inline bool operator==(const AudioDeviceState& lhs, const AudioDeviceState& rhs) {
    return lhs.outputDeviceName == rhs.outputDeviceName && lhs.inputDeviceName == rhs.inputDeviceName;
}
inline bool operator!=(const AudioDeviceState& lhs, const AudioDeviceState& rhs) { return !(lhs == rhs); }

JSON ToJSON(JsonArena& arena, const AudioDeviceState& state);
bool FromJSON(JSON json, AudioDeviceState& state);

inline constexpr const char* kRuntimeConfigSchema = "sheaf.synth.runtime-config";
inline constexpr int kRuntimeConfigSchemaVersion = 3;

JSON ToJSON(JsonArena& arena, const SyncConfig& config);
bool FromJSON(JSON json, SyncConfig& config);

JSON BuildRuntimeConfigJSON(JsonArena& arena,
                            const MidiInstrumentConfig& instrument,
                            const AudioDeviceState& audioDevice,
                            const SyncConfig& sync);
bool LoadRuntimeConfigJSON(JSON root,
                           MidiInstrumentConfig& instrument,
                           AudioDeviceState& audioDevice,
                           SyncConfig& sync);
bool ValidateRuntimeConfigJSON(JSON root);

enum class RuntimeConfigFileStatus {
    Ok,
    Missing,
    Invalid,
    IOError,
};

RuntimeConfigFileStatus LoadRuntimeConfigFile(const std::filesystem::path& configFile,
                                              MidiInstrumentConfig& instrument,
                                              AudioDeviceState& audioDevice,
                                              SyncConfig& sync);
RuntimeConfigFileStatus SaveRuntimeConfigFile(const std::filesystem::path& configFile,
                                              const MidiInstrumentConfig& instrument,
                                              const AudioDeviceState& audioDevice,
                                              const SyncConfig& sync);
const char* RuntimeConfigFileStatusName(RuntimeConfigFileStatus status);

JSON BuildPatchJSON(JsonArena& arena, std::string_view patchName,
                    const ParameterManager& manager,
                    const MidiInstrumentConfig& instrument,
                    const AudioDeviceState& audioDevice = {});
// MIDI/audio arguments are retained for source compatibility with existing
// callers, but patch JSON is parameter-only. Runtime MIDI/audio configuration
// and sync policy are persisted separately through
// BuildRuntimeConfigJSON/LoadRuntimeConfigJSON.
// Legacy midiInstrument/audioDevice sections in patch JSON are tolerated and
// ignored. Patch load applies parameter values only and leaves runtime
// MIDI/audio/sync state untouched.
bool LoadPatchJSON(JSON root, ParameterManager& manager,
                   MidiInstrumentConfig& instrument,
                   AudioDeviceState* audioDevice = nullptr);
bool ValidatePatchJSON(JSON root);

std::string TimestampPatchFilename(std::chrono::system_clock::time_point now);
std::filesystem::path PatchDirectory(const std::filesystem::path& patchesRoot, std::string_view patchName);
std::filesystem::path SavePatchVersion(const std::filesystem::path& patchesRoot, std::string_view patchName,
                                       const std::string& jsonText,
                                       std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
std::filesystem::path SavePatchVersionInDirectory(
    const std::filesystem::path& patchDir, const std::string& jsonText,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
std::optional<std::filesystem::path> LatestPatchVersion(const std::filesystem::path& patchDir);
std::string LoadPatchVersionText(const std::filesystem::path& versionFile);

struct JsonDocument {
    std::shared_ptr<JsonArena> arena;
    JSON root;

    bool IsNull() const { return root.IsNull(); }
};

struct PatchMessageIn {
    enum class Type {
        LoadFromJSON,
        RevertAllToDefault,
        SerializeToJSON,
    };

    Type type = Type::RevertAllToDefault;
    std::uint64_t requestId = 0;
    std::string patchName;
    JsonDocument document;

    static PatchMessageIn LoadFromJSON(JsonDocument document);
    static PatchMessageIn RevertAllToDefault();
    static PatchMessageIn SerializeToJSON(std::uint64_t requestId, std::string patchName);
};

struct MessageOut {
    enum class Type {
        SerializedJSON,
    };

    Type type = Type::SerializedJSON;
    std::uint64_t requestId = 0;
    JsonDocument document;

    static MessageOut SerializedJSON(std::uint64_t requestId, JsonDocument document);
};

class PatchMessageInBus {
public:
    explicit PatchMessageInBus(std::size_t capacity = 64);

    bool Push(const PatchMessageIn& message);
    bool Pop(PatchMessageIn& message);
    std::size_t Size() const { return size_.load(std::memory_order_acquire); }
    std::size_t Capacity() const { return queue_.size(); }

private:
    std::vector<PatchMessageIn> queue_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::size_t> size_{0};
};

class MessageOutBus {
public:
    explicit MessageOutBus(std::size_t capacity = 64);

    bool Push(const MessageOut& message);
    bool Pop(MessageOut& message);
    std::size_t Size() const { return size_.load(std::memory_order_acquire); }
    std::size_t Capacity() const { return queue_.size(); }

private:
    std::vector<MessageOut> queue_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<std::size_t> size_{0};
};

struct PatchSerializationContext {
    std::size_t initialArenaCapacity = 256 * 1024;
    std::size_t maxArenaCapacity = 8 * 1024 * 1024;

    // Caller-owned serialization arena. When non-null, ApplyPatchMessage resets
    // and reuses this arena for SerializeToJSON instead of heap-allocating one
    // of its own. On exhaustion it reports PatchApplyStatus::ArenaExhausted
    // without growing the arena — growing a caller-owned arena is the caller's
    // (message-thread) responsibility.
    //
    // Lifetime contract: the MessageOut::document produced by a caller-arena
    // serialize aliases this arena non-owningly (see JsonDocument's aliased
    // shared_ptr in the .cpp). The caller MUST fully consume that document —
    // pop it off the output bus and finish reading/writing it out — before
    // making the next ApplyPatchMessage call that reuses this same arena,
    // since that call resets the arena and clobbers the previous document's
    // backing memory. The caller must also keep the arena alive until the
    // document has been consumed. PatchManager satisfies this automatically
    // via its single-pending-save gate (HasPendingSave()/pendingSave_), which
    // never issues a new serialize request while a prior one is outstanding —
    // as long as all serialize requests flow through PatchManager, this
    // ordering is guaranteed for you.
    JsonArena* arena = nullptr;
};

enum class PatchApplyStatus {
    Applied,
    Reverted,
    Serialized,
    InvalidJSON,
    OutputQueueFull,
    ArenaExhausted,
};

PatchApplyStatus ApplyPatchMessage(
    const PatchMessageIn& message, ParameterManager& manager,
    MidiInstrumentConfig& instrument, const MidiInstrumentConfig& defaultInstrument,
    AudioDeviceState& audioDevice, const AudioDeviceState& defaultAudioDevice,
    MessageOutBus& outputBus, PatchSerializationContext context = {});

// Printf-safe (%s) status-name helpers for slog-7 INFO logging (Engine.hpp's
// MessageThreadTick/ProcessBlock and the runtime shell's LogPatchCommand
// share these rather than each maintaining their own switch).
const char* PatchApplyStatusName(PatchApplyStatus status);
const char* PatchMessageInTypeName(PatchMessageIn::Type type);

enum class PatchCommandStatus {
    Ok,
    Pending,
    NoCompletion,
    Written,
    NeedsSaveAsPath,
    Busy,
    AlreadyExists,
    NotFound,
    InvalidPatch,
    QueueFull,
    IOError,
};

struct PatchCommandResult {
    PatchCommandStatus status = PatchCommandStatus::Ok;
    std::uint64_t requestId = 0;
    std::filesystem::path path;
};

// See the comment on PatchApplyStatusName above: shared by Engine.hpp's
// slog-7 logging and the runtime shell's LogPatchCommand.
const char* PatchCommandStatusName(PatchCommandStatus status);

class PatchManager {
public:
    explicit PatchManager(PatchMessageInBus* inputBus = nullptr, MessageOutBus* outputBus = nullptr,
                          std::size_t initialArenaCapacity = 256 * 1024);

    void SetBuses(PatchMessageInBus* inputBus, MessageOutBus* outputBus);
    const std::optional<std::filesystem::path>& CurrentPatchDirectory() const { return currentPatchDirectory_; }
    bool HasPendingSave() const { return pendingSave_.has_value(); }

    PatchCommandResult NewPatch();
    PatchCommandResult SavePatch();
    PatchCommandResult SavePatchAs(const std::filesystem::path& patchDir);
    PatchCommandResult SavePatchAsOverwrite(const std::filesystem::path& patchDir);
    PatchCommandResult LoadPatch(const std::filesystem::path& path);
    PatchCommandResult RevertPatch();
    PatchCommandResult ProcessResponses(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());

private:
    struct PendingSave {
        enum class Kind {
            Save,
            SaveAs,
            SaveAsOverwrite,
        };
        Kind kind = Kind::Save;
        std::uint64_t requestId = 0;
        std::filesystem::path patchDir;
    };

    PatchCommandResult DispatchSerialize(PendingSave::Kind kind, const std::filesystem::path& patchDir);
    PatchCommandResult LoadPatchVersion(const std::filesystem::path& versionFile,
                                        const std::filesystem::path& currentPatchDirectory);
    JsonDocument ParsePatchText(const std::string& text) const;
    std::string PatchNameForDirectory(const std::filesystem::path& patchDir) const;

    PatchMessageInBus* inputBus_ = nullptr;
    MessageOutBus* outputBus_ = nullptr;
    std::optional<std::filesystem::path> currentPatchDirectory_;
    std::optional<PendingSave> pendingSave_;
    std::uint64_t nextRequestId_ = 1;
    std::size_t initialArenaCapacity_ = 256 * 1024;
};

} // namespace synth
