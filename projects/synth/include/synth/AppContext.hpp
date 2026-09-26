#pragma once

// Application/runtime contract types for the synth application runtime
// (sar-1, sar-2, sar-3). JUCE-free: consumed by applications, the engine,
// the JUCE runtime shell, and the headless test rig.

#include "synth/MidiController.hpp"
#include "synth/MasterClock.hpp"
#include "synth/ParameterModulation.hpp"
#include "synth/PatchPersistence.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace synth {

class GridManager;

// Static configuration supplied by the application (sar-2). Audio fields are
// a request: the host negotiates actual values with the device and reports
// them through the application's prepare hook.
struct RuntimeConfig {
    std::string appName;
    int numAudioInputs = 0;
    int numAudioOutputs = 2;
    double preferredSampleRate = 48000.0;
    int preferredBlockSize = 256;
    int uiWidth = 900;
    int uiHeight = 560;
    int uiFrameHz = 30;
    // Renames the runtime's own Audio page in the sidebar. It exists because
    // an application's vocabulary can already use a runtime page's name for
    // something else -- a synth whose first parameter bank is called Audio
    // renders two "Audio" buttons that mean different things -- and nothing
    // else here lets the application say so. Unset means the runtime's own
    // name, so an application that never sets it renders the sidebar exactly
    // as before. Only the page's BUTTON is renamed; its contents are the
    // runtime's.
    std::optional<std::string> audioPageTitle;
};

// Shared JUCE-free validation for RuntimeConfig requests. Throws
// std::invalid_argument for a negative input count before engine or device
// startup. Hosts translate that failure into an explicit startup diagnostic.
//
inline void ValidateRuntimeConfig(const RuntimeConfig& config) {
    if (config.numAudioInputs < 0) {
        throw std::invalid_argument("RuntimeConfig::numAudioInputs must be nonnegative");
    }
}

// Runtime-owned persistent data paths (sar-17). Applications do not choose
// production persistence roots; hosts resolve and inject these paths.
struct RuntimeDataPaths {
    std::filesystem::path dataRoot;
    std::filesystem::path patchesRoot;
    std::filesystem::path logsRoot;
    std::filesystem::path configFile;

    static RuntimeDataPaths FromDataRoot(std::filesystem::path root) {
        RuntimeDataPaths paths;
        paths.dataRoot = std::move(root);
        paths.patchesRoot = paths.dataRoot / "patches";
        paths.logsRoot = paths.dataRoot / "logs";
        paths.configFile = paths.dataRoot / "config.json";
        return paths;
    }

    static RuntimeDataPaths FromRoots(std::filesystem::path dataRoot,
                                      std::filesystem::path patchesRoot,
                                      std::filesystem::path logsRoot,
                                      std::filesystem::path configFile) {
        RuntimeDataPaths paths;
        paths.dataRoot = std::move(dataRoot);
        paths.patchesRoot = std::move(patchesRoot);
        paths.logsRoot = std::move(logsRoot);
        paths.configFile = std::move(configFile);
        return paths;
    }
};

// Non-owning view of one audio device block (sar-6). Channel counts are the
// device's actual counts, which may differ from the RuntimeConfig request.
// AudioInputView / AudioInputFrameView are callback-lifetime-only: they are
// trivially copyable non-owning views and must not be retained after the
// ProcessBlock callback returns.
class AudioInputFrameView {
public:
    AudioInputFrameView() = default;

    AudioInputFrameView(const float* const* inputs,
                        std::size_t requestedChannelCount,
                        std::size_t activeChannelCount,
                        std::size_t frameIndex) noexcept
        : inputs_(inputs)
        , requestedChannelCount_(requestedChannelCount)
        , activeChannelCount_(activeChannelCount)
        , frameIndex_(frameIndex) {}

    std::size_t RequestedChannelCount() const noexcept { return requestedChannelCount_; }
    std::size_t ActiveChannelCount() const noexcept { return activeChannelCount_; }
    std::size_t FrameIndex() const noexcept { return frameIndex_; }

    bool HasActiveChannel(std::size_t channel) const noexcept {
        return channel < activeChannelCount_ && inputs_ != nullptr && inputs_[channel] != nullptr;
    }

    float Sample(std::size_t channel) const noexcept {
        assert(HasActiveChannel(channel));
        return inputs_[channel][frameIndex_];
    }

    float SampleOrSilence(std::size_t channel) const noexcept {
        if (!HasActiveChannel(channel)) {
            return 0.0f;
        }
        return inputs_[channel][frameIndex_];
    }

private:
    const float* const* inputs_ = nullptr;
    std::size_t requestedChannelCount_ = 0;
    std::size_t activeChannelCount_ = 0;
    std::size_t frameIndex_ = 0;
};

class AudioInputView {
public:
    AudioInputView() = default;

    AudioInputView(const float* const* inputs,
                   std::size_t requestedChannelCount,
                   std::size_t activeChannelCount,
                   std::size_t frameCount) noexcept
        : inputs_(inputs)
        , requestedChannelCount_(requestedChannelCount)
        , activeChannelCount_(activeChannelCount)
        , frameCount_(frameCount) {}

    std::size_t RequestedChannelCount() const noexcept { return requestedChannelCount_; }
    std::size_t ActiveChannelCount() const noexcept { return activeChannelCount_; }
    std::size_t FrameCount() const noexcept { return frameCount_; }
    bool Empty() const noexcept { return activeChannelCount_ == 0; }

    bool HasActiveChannel(std::size_t channel) const noexcept {
        return channel < activeChannelCount_ && inputs_ != nullptr && inputs_[channel] != nullptr;
    }

    std::span<const float> Channel(std::size_t channel) const noexcept {
        assert(HasActiveChannel(channel));
        return std::span<const float>(inputs_[channel], frameCount_);
    }

    AudioInputFrameView Frame(std::size_t frame) const noexcept {
        assert(frame < frameCount_);
        return AudioInputFrameView(inputs_, requestedChannelCount_, activeChannelCount_, frame);
    }

    float SampleOrSilence(std::size_t channel, std::size_t frame) const noexcept {
        if (frame >= frameCount_ || !HasActiveChannel(channel)) {
            return 0.0f;
        }
        return inputs_[channel][frame];
    }

private:
    const float* const* inputs_ = nullptr;
    std::size_t requestedChannelCount_ = 0;
    std::size_t activeChannelCount_ = 0;
    std::size_t frameCount_ = 0;
};

struct AudioBlock {
    const float* const* inputs = nullptr;
    float* const* outputs = nullptr;
    int numInputChannels = 0;
    int numOutputChannels = 0;
    std::size_t numFrames = 0;
    std::uint64_t startSample = 0;
    // Non-owning view of the exact MasterClock::CurrentPlan() committed for
    // this callback. Non-null only when the MasterClock was successfully
    // prepared and this nonzero, contiguous block commit succeeded. It is
    // null in default-constructed views, before successful clock preparation,
    // for zero-frame callbacks, or when MasterClock rejects the commit. Apps
    // must null-check and must not retain it as an immutable snapshot beyond
    // this callback: the next successful commit replaces the pointed-to plan.
    const ClockBlockPlan* clockPlan = nullptr;
    // Application-requested logical input count for this block. Hosts set this
    // explicitly from immutable RuntimeConfig; InputView() clamps actual
    // numInputChannels defensively into [0, requested].
    int numRequestedInputChannels = 0;

    AudioInputView InputView() const noexcept {
        const int requested = std::max(0, numRequestedInputChannels);
        const int clampedActual = std::clamp(numInputChannels, 0, requested);
        return AudioInputView(inputs,
                              static_cast<std::size_t>(requested),
                              static_cast<std::size_t>(clampedActual),
                              numFrames);
    }
};

// External-input-routed signal (sar-33): true only while a user-chosen
// external input source is open and delivering -- an identical semantic on
// both backends. JUCE: the user-selected input device (never the
// platform-default device auto-opened at startup before any selection) is
// non-empty and IS the input device currently open (see
// runtime/Runtime.hpp's RefreshInputRoutedState, next to
// ApplyAudioDeviceInputSelection/OnEngineAudioDeviceChanged). Browser:
// user-gesture-granted input capture is currently live (see
// synth/browser/BrowserRuntime.hpp's RefreshInputRoutedState, hung off
// SetAudioInputSource/ClearAudioInputSource).
//
// One instance is owned by the host for the app's whole lifetime and
// referenced through AppContext::inputRoutingSignal below -- the same
// non-owning-pointer-to-host-owned-object convention every other AppContext
// member uses. Unlike those, this state cannot live directly on AppContext
// itself: AppContext is copied by value at several call sites (e.g.
// engine_tests.cpp / miniapp_system_tests.cpp's
// `AppContext context = rig.Engine().Context();`), and neither a
// std::atomic (non-copyable) nor a callback registration that must reach one
// canonical listener would survive being duplicated that way. Routing every
// copy through one shared pointee keeps both properties.
class InputRoutingSignal {
public:
    // Wait-free: a plain atomic load, no locks, no allocation -- safe to call
    // from any thread, including the audio thread.
    bool Routed() const noexcept { return routed_.load(std::memory_order_relaxed); }

    // Message-thread only: registers (or, with an empty std::function,
    // clears) the callback invoked on the message thread whenever Routed()
    // actually changes value. Mirrors Engine::SetAudioDeviceChangedCallback /
    // SetMidiProcessorsRebuiltCallback's Set<X>Callback(std::function<...>)
    // idiom (Engine.hpp) for host -> app change notification.
    void SetChangedCallback(std::function<void(bool)> callback) {
        changedCallback_ = std::move(callback);
    }

    // Host-only (the JUCE and browser derivations above): publishes a freshly
    // derived value. Message-thread only -- the atomic store by itself would
    // be audio-thread-safe, but this method also synchronously invokes the
    // registered callback when the value actually changed, and that callback
    // must run on the message thread (see SetChangedCallback), so Publish
    // itself must never be called from the audio thread.
    void Publish(bool routed) {
        const bool previous = routed_.exchange(routed, std::memory_order_relaxed);
        if (previous != routed && changedCallback_) {
            changedCallback_(routed);
        }
    }

private:
    std::atomic<bool> routed_{false};
    std::function<void(bool)> changedCallback_;
};

// Non-owning pointers to every framework object an application may touch
// (sar-3). The host owns all pointees; addresses are stable for the
// application's lifetime. Thread roles below are binding (sar-7); a member
// may only be used from its named thread.
struct AppContext {
    ParameterManager* parameterManager = nullptr;   // audio thread once running; message thread before start
    PatchManager* patchManager = nullptr;           // message thread only (commands + responses)
    MessageInBus* uiBus = nullptr;                  // producer: message thread; consumer: audio thread
    MessageInBus* midiBus = nullptr;                // producer: MIDI callback thread; consumer: audio thread
    ParameterMessageOutBus* parameterMessageOutBus = nullptr;  // producer: audio; consumer: message thread
    PatchMessageInBus* patchInputBus = nullptr;     // producer: message thread; consumer: audio thread
    MessageOutBus* patchOutputBus = nullptr;        // producer: audio; consumer: message thread
    MidiSender* midiSender = nullptr;               // enqueue from message thread; owned worker drains
    MasterClock* masterClock = nullptr;             // audio thread; stable for application lifetime
    MidiInstrumentConfig* instrument = nullptr;              // message thread only
    const MidiInstrumentConfig* defaultInstrument = nullptr; // immutable after init
    const RuntimeConfig* config = nullptr;          // immutable after construction
    ParameterManager::UIState* uiState = nullptr;   // null during Init; set before MIDI/audio/UI start
    // Init-only topology declaration; Engine owns this manager. Do not use it
    // for application runtime mutation after Engine finalizes grid topology.
    // Message thread during Init only.
    GridManager* gridManager = nullptr;

    // Shared monotonic timestamp source, the same one passed to the owning
    // synth::Engine<App>'s constructor (Runtime.hpp's NowMicros() under the
    // JUCE shell, SynthRig's NextTimestamp() under the headless test rig).
    // Callable from any thread; exists so a UI wrapper can stamp MessageIn
    // values it pushes onto uiBus (encoder drags, button presses) without
    // inventing a second, divergent clock. Null only in contexts that never
    // construct a real Engine (there are none today); UI code should treat a
    // null now as "unavailable" rather than crash.
    std::function<std::uint64_t()> now;

    // sar-33 external-input-routed signal; see InputRoutingSignal's class doc
    // comment above for the exact per-backend derivation. Host-owned,
    // non-owning, stable for the application's lifetime; null only in
    // contexts that never construct a real Engine (there are none today).
    // Apps should use InputRouted()/SetInputRoutedChangedCallback() below
    // rather than this pointer directly.
    InputRoutingSignal* inputRoutingSignal = nullptr;

    // The getter (sar-33): wait-free, safe from any thread including the
    // audio thread. Reads false when inputRoutingSignal is unset.
    bool InputRouted() const noexcept {
        return inputRoutingSignal != nullptr && inputRoutingSignal->Routed();
    }

    // Message-thread only: registers (or, with an empty std::function,
    // clears) the callback the host invokes on the message thread whenever
    // InputRouted() actually changes value. See InputRoutingSignal::
    // SetChangedCallback for the idiom this matches. A no-op when
    // inputRoutingSignal is unset.
    void SetInputRoutedChangedCallback(std::function<void(bool)> callback) {
        if (inputRoutingSignal != nullptr) {
            inputRoutingSignal->SetChangedCallback(std::move(callback));
        }
    }
};

}  // namespace synth
