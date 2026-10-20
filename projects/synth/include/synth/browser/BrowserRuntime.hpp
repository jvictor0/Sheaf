#pragma once

#include "synth/Engine.hpp"
#include "synth/RuntimeMainComponent.hpp"
#include "synth/browser/BrowserCommandBuffer.hpp"
#include "synth/browser/BrowserMidiBridge.hpp"
#include "synth/browser/BrowserPersistence.hpp"
#include "synth/browser/BrowserRuntimeMainServices.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/webaudio.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifndef __EMSCRIPTEN__
using EMSCRIPTEN_WEBAUDIO_T = int;
#endif

namespace synth_browser {

static constexpr std::size_t kMaxBrowserInputChannels = 32;
static constexpr std::size_t kMaxBrowserOutputChannels = 32;

// `BrowserAudioInputStatus` and its validity check live in BrowserAudioDevices.hpp
// beside the Audio page vocabulary that renders them; this header only publishes
// and forwards the codes.

struct BrowserAudioSampleFrameDescriptor {
    int numberOfChannels = 0;
    int samplesPerChannel = 0;
    float* data = nullptr;
};

enum class BrowserAudioInputConnectResult {
    Connected,  // the source is attached to the worklet input bus
    Deferred,   // there is no worklet node yet; attach it when one is created
    Failed
};

// The audio input claim the realtime callback and the Audio page both read.
//
// Three pieces of state, deliberately separate. `sourceHandle_` plus its count
// and status are the CLAIM: what the callback reads and the Audio page reports.
// `connectedHandle_` is the source the publication has CONFIRMED attached to the
// worklet input bus. `pendingHandle_` is a claimed source whose attachment was
// deferred because no worklet node existed yet -- claimed, but demonstrably not
// in the graph. Keeping the last two apart is what makes graph failures
// recoverable, because they call for opposite cleanup.
//
// Publication is ordered against the graph and is transactional. The count is
// stored BEFORE the source is connected, so the first callback after a
// successful connect already sees the real physical count instead of a stale
// zero; and it is cleared BEFORE the source is disconnected, so no callback can
// read a count for a source that is already gone. That ordering is only sound if
// a graph operation that fails takes the claim down with it.
//
// The failure stages are not the same failure:
//
//  - A failed DISCONNECT leaves the previous source attached. Forgetting it
//    would let the next publication connect a second source beside it and the
//    input bus would sum both, so `connectedHandle_` keeps that identity and no
//    new source is connected until the stuck one is verifiably gone.
//  - A failed CONNECT happens after the previous source is already detached, so
//    nothing is attached, `connectedHandle_` is 0, and the next publication has
//    nothing to clean up first.
//  - A failed DEFERRED attachment never reached the graph at all. Disconnecting
//    it would ask the host to detach something that was never attached, and a
//    lookup failure there would retain an identity that blocks every later
//    publication forever. So the intent is simply dropped with the claim.
//
// Clearing needs no rollback -- cleared is the safe direction -- but a failed
// disconnect is still reported and still remembered.
class BrowserAudioInputPublication {
public:
    std::uint32_t SourceHandle() const
    {
        return sourceHandle_.load(std::memory_order_acquire);
    }

    std::uint32_t PhysicalChannels() const
    {
        return physicalChannels_.load(std::memory_order_acquire);
    }

    std::uint32_t StatusCode() const
    {
        return statusCode_.load(std::memory_order_acquire);
    }

    // The source the publication has confirmed attached to the graph, which is
    // also the source a failed disconnect is still blocked on.
    std::uint32_t ConnectedSourceHandle() const
    {
        return connectedHandle_.load(std::memory_order_acquire);
    }

    // The claimed source still waiting for a worklet node to attach it to.
    std::uint32_t PendingSourceHandle() const
    {
        return pendingHandle_.load(std::memory_order_acquire);
    }

    // `disconnect(handle)` detaches the attached source and `connect(handle)`
    // attaches the new one, reporting whether it attached, could not attach yet,
    // or failed. Neither is called when the source being published is already
    // the attached or pending one: re-registering a live source must not tear
    // its connection down and build it again, and must not attach it twice.
    template <typename Disconnect, typename Connect>
    bool Publish(std::uint32_t sourceHandle,
                 std::uint32_t physicalChannels,
                 std::uint32_t statusCode,
                 Disconnect&& disconnect,
                 Connect&& connect)
    {
        const std::uint32_t attached = connectedHandle_.load(std::memory_order_acquire);
        const std::uint32_t pending = pendingHandle_.load(std::memory_order_acquire);
        sourceHandle_.store(sourceHandle, std::memory_order_release);
        statusCode_.store(statusCode, std::memory_order_release);
        physicalChannels_.store(physicalChannels, std::memory_order_release);
        if (attached == sourceHandle || (attached == 0 && pending == sourceHandle)) {
            return true;
        }
        if (attached != 0) {
            if (!disconnect(attached)) {
                // Still attached: refuse to add a second source to the bus.
                RollBack();
                return false;
            }
            connectedHandle_.store(0, std::memory_order_release);
        }
        // A superseded pending source was never in the graph, so it is dropped
        // rather than disconnected.
        pendingHandle_.store(0, std::memory_order_release);
        const BrowserAudioInputConnectResult result = connect(sourceHandle);
        if (result == BrowserAudioInputConnectResult::Failed) {
            // Nothing is attached, so nothing is blocked -- only the claim goes.
            RollBack();
            return false;
        }
        if (result == BrowserAudioInputConnectResult::Deferred) {
            pendingHandle_.store(sourceHandle, std::memory_order_release);
            return true;
        }
        connectedHandle_.store(sourceHandle, std::memory_order_release);
        return true;
    }

    // Attaches the source whose connection was deferred, once a worklet node
    // exists. Success promotes it from pending to connected. Failure drops the
    // claim and the intent outright -- the source never reached the graph, so
    // there is nothing to detach and nothing that may block a later source.
    template <typename Connect>
    bool ResolvePendingConnection(Connect&& connect)
    {
        const std::uint32_t pending = pendingHandle_.load(std::memory_order_acquire);
        if (pending == 0) {
            return true;
        }
        if (!connect(pending)) {
            pendingHandle_.store(0, std::memory_order_release);
            RollBack();
            return false;
        }
        pendingHandle_.store(0, std::memory_order_release);
        connectedHandle_.store(pending, std::memory_order_release);
        return true;
    }

    // `disconnect(handle)` removes the attached source from the graph and
    // returns false if it could not, in which case its identity is retained so a
    // later publication still cleans it up before connecting anything. A merely
    // pending source is dropped without a disconnect.
    template <typename Disconnect>
    bool Clear(std::uint32_t statusCode, Disconnect&& disconnect)
    {
        physicalChannels_.store(0, std::memory_order_release);
        statusCode_.store(statusCode, std::memory_order_release);
        sourceHandle_.store(0, std::memory_order_release);
        pendingHandle_.store(0, std::memory_order_release);
        const std::uint32_t attached = connectedHandle_.load(std::memory_order_acquire);
        if (attached == 0) {
            return true;
        }
        if (!disconnect(attached)) {
            return false;
        }
        connectedHandle_.store(0, std::memory_order_release);
        return true;
    }

private:
    void RollBack()
    {
        physicalChannels_.store(0, std::memory_order_release);
        sourceHandle_.store(0, std::memory_order_release);
        statusCode_.store(static_cast<std::uint32_t>(BrowserAudioInputStatus::ApiUnavailable),
                          std::memory_order_release);
    }

    std::atomic<std::uint32_t> sourceHandle_{0};
    std::atomic<std::uint32_t> physicalChannels_{0};
    std::atomic<std::uint32_t> statusCode_{
        static_cast<std::uint32_t>(BrowserAudioInputStatus::NotRequested)};
    std::atomic<std::uint32_t> connectedHandle_{0};
    std::atomic<std::uint32_t> pendingHandle_{0};
};

inline void SilenceBrowserAudioOutput(BrowserAudioSampleFrameDescriptor* outputs,
                                      int numOutputs) noexcept
{
    if (numOutputs <= 0 || outputs == nullptr) {
        return;
    }
    for (int outputIndex = 0; outputIndex < numOutputs; ++outputIndex) {
        BrowserAudioSampleFrameDescriptor& output = outputs[outputIndex];
        if (output.data == nullptr || output.numberOfChannels <= 0 ||
            output.samplesPerChannel <= 0) {
            continue;
        }
        std::fill_n(output.data,
                    static_cast<std::size_t>(output.numberOfChannels) *
                        static_cast<std::size_t>(output.samplesPerChannel),
                    0.0f);
    }
}

template <typename ProcessBlock>
bool AdaptBrowserAudioWorkletPlanarBlock(
    int numInputs,
    const BrowserAudioSampleFrameDescriptor* inputs,
    int numOutputs,
    BrowserAudioSampleFrameDescriptor* outputs,
    std::size_t requestedInputChannels,
    std::size_t publishedPhysicalInputChannels,
    std::size_t expectedOutputChannels,
    std::uint64_t timestampMicros,
    ProcessBlock&& processBlock)
{
    if (numOutputs <= 0 || outputs == nullptr || outputs[0].data == nullptr) {
        return false;
    }
    BrowserAudioSampleFrameDescriptor& output = outputs[0];
    if (output.numberOfChannels <= 0 || output.samplesPerChannel <= 0 ||
        expectedOutputChannels == 0 || expectedOutputChannels > kMaxBrowserOutputChannels ||
        static_cast<std::size_t>(output.numberOfChannels) != expectedOutputChannels) {
        SilenceBrowserAudioOutput(outputs, numOutputs);
        return false;
    }

    std::array<float*, kMaxBrowserOutputChannels> outputPointers{};
    for (int channel = 0; channel < output.numberOfChannels; ++channel) {
        outputPointers[static_cast<std::size_t>(channel)] =
            output.data + (channel * output.samplesPerChannel);
    }

    std::array<const float*, kMaxBrowserInputChannels> inputPointers{};
    const std::size_t requestedInputs =
        std::min(requestedInputChannels, kMaxBrowserInputChannels);
    const std::size_t physicalInputs =
        std::min(publishedPhysicalInputChannels, kMaxBrowserInputChannels);
    std::size_t inputBusChannels = 0;
    if (numInputs > 0 && inputs != nullptr && inputs[0].data != nullptr &&
        inputs[0].numberOfChannels > 0 &&
        inputs[0].samplesPerChannel >= output.samplesPerChannel) {
        inputBusChannels = std::min(static_cast<std::size_t>(inputs[0].numberOfChannels),
                                    kMaxBrowserInputChannels);
    }
    const std::size_t activeInputs =
        std::min(std::min(inputBusChannels, physicalInputs), requestedInputs);
    for (std::size_t channel = 0; channel < activeInputs; ++channel) {
        inputPointers[channel] = inputs[0].data +
                                 (channel * static_cast<std::size_t>(inputs[0].samplesPerChannel));
    }

    synth::AudioBlock block;
    block.inputs = activeInputs == 0 ? nullptr : inputPointers.data();
    block.outputs = outputPointers.data();
    block.numInputChannels = static_cast<int>(activeInputs);
    block.numOutputChannels = output.numberOfChannels;
    block.numFrames = static_cast<std::size_t>(output.samplesPerChannel);
    block.numRequestedInputChannels = static_cast<int>(requestedInputs);
    std::forward<ProcessBlock>(processBlock)(block, timestampMicros);
    return true;
}

class AudioWorkletDeadlineMeter final {
public:
    void RecordCallbackMicros(std::uint64_t elapsedMicros, std::uint64_t blockMicros)
    {
        if (blockMicros == 0) {
            return;
        }
        pendingElapsedMicros_ = SaturatingAdd(pendingElapsedMicros_, elapsedMicros);
        pendingBlockMicros_ = SaturatingAdd(pendingBlockMicros_, blockMicros);
        if (pendingBlockMicros_ < kPublishWindowMicros) {
            return;
        }
        publishedMicrounits_.store(
            DeadlineMicrounits(pendingElapsedMicros_, pendingBlockMicros_),
            std::memory_order_release);
        pendingElapsedMicros_ = 0;
        pendingBlockMicros_ = 0;
    }

    std::uint32_t SampleMicrounits() const
    {
        return publishedMicrounits_.load(std::memory_order_acquire);
    }

    float SamplePercent() const
    {
        return static_cast<float>(SampleMicrounits()) / 1'000'000.0f;
    }

private:
    static constexpr std::uint64_t kPublishWindowMicros = 100'000;

    static std::uint64_t SaturatingAdd(std::uint64_t lhs, std::uint64_t rhs)
    {
        return lhs > std::numeric_limits<std::uint64_t>::max() - rhs
                   ? std::numeric_limits<std::uint64_t>::max()
                   : lhs + rhs;
    }

    static std::uint32_t DeadlineMicrounits(std::uint64_t elapsedMicros,
                                            std::uint64_t blockMicros)
    {
        if (blockMicros == 0) {
            return 0;
        }
        const double percent = static_cast<double>(elapsedMicros) * 100.0 /
                               static_cast<double>(blockMicros);
        return static_cast<std::uint32_t>(
            std::min(static_cast<double>(std::numeric_limits<std::uint32_t>::max()),
                     std::max(0.0, percent * 1'000'000.0)));
    }

    std::uint64_t pendingElapsedMicros_ = 0;
    std::uint64_t pendingBlockMicros_ = 0;
    std::atomic<std::uint32_t> publishedMicrounits_{0};
};

template <synth::SynthApplication App>
class Runtime {
public:
    Runtime()
        : engine_([this] { return timestampMicros_.load(std::memory_order_relaxed); })
        , midiBridge_(engine_)
        , services_(engine_,
                    midiBridge_,
                    submittedAudioDevices_,
                    [this] { return AudioWorkletDeadlineSamplePercent(); },
                    [this] { return AudioInputStateSnapshot(); })
        , mainComponent_(engine_.Application(), services_)
    {
        engine_.Clock().SetOutputSchedulingHorizonMicros(
            BrowserMidiBridge<synth::Engine<App>>::kSchedulingLeadMicros);
        // sar-33: wires the external-input-routed signal's storage into the
        // AppContext apps see, before engine_.Initialize() can ever run
        // App::Init(). inputRoutingSignal_ is a member of this Runtime
        // (constructed above, in the member-init list, before this
        // constructor body runs), so its address is already stable here.
        // Defaults to not-routed, which is already correct for a fresh
        // Runtime -- no capture has been granted yet, so no explicit
        // RefreshInputRoutedState() call is needed until SetAudioInputSource/
        // ClearAudioInputSource run.
        engine_.Context().inputRoutingSignal = &inputRoutingSignal_;
    }

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    void SetRuntimeDataPaths(synth::RuntimeDataPaths paths)
    {
        RequireNotStarted();
        engine_.SetRuntimeDataPaths(std::move(paths));
    }

    void Start()
    {
        RequireNotStarted();
        engine_.Initialize();
        midiBridge_.Start();
        started_.store(true, std::memory_order_release);
    }

    void Stop()
    {
        started_.store(false, std::memory_order_release);
#ifdef __EMSCRIPTEN__
        if (audioNode_ != 0) {
            emscripten_destroy_web_audio_node(audioNode_);
            audioNode_ = 0;
        }
        if (audioContext_ != 0) {
            emscripten_destroy_audio_context(audioContext_);
            audioContext_ = 0;
        }
#endif
        midiBridge_.Stop();
        stopped_.store(true, std::memory_order_release);
    }

    bool IsRunning() const { return started_.load(std::memory_order_acquire); }
    std::size_t AudioOutputChannels() const { return engine_.Config().numAudioOutputs; }
    std::size_t AudioInputChannels() const { return requestedAudioInputChannels_; }
    bool AudioWorkletConfigurationSupported() const
    {
        const std::size_t outputs = AudioOutputChannels();
        return outputs > 0 && outputs <= kMaxBrowserOutputChannels &&
               AudioInputChannels() <= kMaxBrowserInputChannels;
    }
    bool RetainAfterStopForAudioWorklet() const
    {
#ifdef __EMSCRIPTEN__
        return audioWorkletStarted_.load(std::memory_order_acquire);
#else
        return false;
#endif
    }

    void SetTimestampEpochOffsetMicros(std::int64_t offsetMicros)
    {
        RequireNotStarted();
        timestampEpochOffsetMicros_.store(offsetMicros, std::memory_order_release);
    }

    std::int64_t TimestampEpochOffsetMicros() const
    {
        return timestampEpochOffsetMicros_.load(std::memory_order_acquire);
    }

    void Prepare(double sampleRate, std::size_t blockSize)
    {
        RequireStarted();
        if (blockSize > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::out_of_range("browser block size exceeds engine range");
        }
        engine_.Prepare(sampleRate, static_cast<int>(blockSize));
        services_.RecordAudioNegotiation(sampleRate, blockSize);
    }

    bool StartAudioWorklet(EMSCRIPTEN_WEBAUDIO_T suppliedContext = 0)
    {
        RequireStarted();
        if (!AudioWorkletConfigurationSupported()) {
            return false;
        }
#ifdef __EMSCRIPTEN__
        if (audioContext_ != 0) {
            return true;
        }
        audioContext_ = suppliedContext;
        if (audioContext_ == 0) {
            EmscriptenWebAudioCreateAttributes attributes{
                .latencyHint = "interactive",
                .sampleRate = 0,
                .renderSizeHint = AUDIO_CONTEXT_RENDER_SIZE_DEFAULT,
            };
            audioContext_ = emscripten_create_audio_context(&attributes);
        }
        if (audioContext_ == 0) {
            return false;
        }
        const int sampleRate = emscripten_audio_context_sample_rate(audioContext_);
        const int quantumSize = emscripten_audio_context_quantum_size(audioContext_);
        if (sampleRate <= 0 || quantumSize <= 0) {
            return false;
        }
        Prepare(static_cast<double>(sampleRate), static_cast<std::size_t>(quantumSize));
        const auto localNowMicros = static_cast<std::uint64_t>(
            std::llround(std::max(0.0, emscripten_get_now() * 1000.0)));
        const auto nowMicros = ApplyTimestampEpochOffset(
            localNowMicros,
            timestampEpochOffsetMicros_.load(std::memory_order_acquire));
        timestampMicros_.store(nowMicros, std::memory_order_release);
        audioCallbackTimestampMicros_.store(nowMicros, std::memory_order_release);
        audioCallbackBlockMicros_.store(
            static_cast<std::uint64_t>(
                std::llround(static_cast<double>(quantumSize) * 1'000'000.0 /
                             static_cast<double>(sampleRate))),
            std::memory_order_release);
        emscripten_resume_audio_context_sync(audioContext_);
        audioWorkletStarted_.store(true, std::memory_order_release);
        emscripten_start_wasm_audio_worklet_thread_async(audioContext_,
                                                         audioWorkletStack_.data(),
                                                         static_cast<std::uint32_t>(audioWorkletStack_.size()),
                                                         &Runtime::AudioWorkletThreadInitialized,
                                                         this);
        return true;
#else
        (void)suppliedContext;
        return false;
#endif
    }

    bool SetAudioInputSource(std::uint32_t sourceHandle,
                             std::uint32_t physicalChannels,
                             std::uint32_t statusCode)
    {
        if (!BrowserAudioInputStatusCodeValid(statusCode)) {
            return false;
        }
        if (sourceHandle == 0 || physicalChannels == 0 ||
            physicalChannels > kMaxBrowserInputChannels) {
            return false;
        }
        const bool published = audioInput_.Publish(
            sourceHandle,
            physicalChannels,
            statusCode,
            [this](std::uint32_t attached) { return DisconnectAudioInputSource(attached); },
            [this](std::uint32_t next) { return ConnectAudioInputSource(next); });
        RefreshInputRoutedState();
        return published;
    }

    bool ClearAudioInputSource(std::uint32_t statusCode)
    {
        if (!BrowserAudioInputStatusCodeValid(statusCode)) {
            return false;
        }
        const bool cleared = audioInput_.Clear(statusCode, [this](std::uint32_t previous) {
            return DisconnectAudioInputSource(previous);
        });
        RefreshInputRoutedState();
        return cleared;
    }

    // Substitutes the two Web Audio graph operations the input publication
    // drives. Production never calls this: unset means the real Emscripten
    // calls, so the shipped behaviour is exactly what it would be without this
    // seam. It exists because a native build has no Web Audio graph at all, so
    // an attachment there can only ever defer -- the recovery half of the
    // deferred-attachment sequence (a replacement that actually attaches) would
    // otherwise be unreachable from the production `Runtime`.
    void SetAudioInputGraphForTesting(
        std::function<BrowserAudioInputConnectResult(std::uint32_t)> connect,
        std::function<bool(std::uint32_t)> disconnect)
    {
        audioInputConnectOverride_ = std::move(connect);
        audioInputDisconnectOverride_ = std::move(disconnect);
    }

    // Attaches a source whose connection was deferred because no worklet node
    // existed when it was published. The Emscripten build calls this from
    // `AudioWorkletProcessorCreated`; it is public so the native contract test
    // can drive the same sequence.
    bool ResolveDeferredAudioInputConnection()
    {
        return audioInput_.ResolvePendingConnection([this](std::uint32_t sourceHandle) {
            return ConnectAudioInputSource(sourceHandle) ==
                   BrowserAudioInputConnectResult::Connected;
        });
    }

    // The source confirmed attached to the worklet input bus, and the claimed
    // source still waiting for a node to attach it to. Exactly one of them is
    // nonzero at a time, and both are zero once capture is offline.
    std::uint32_t ConnectedAudioInputSourceHandle() const
    {
        return audioInput_.ConnectedSourceHandle();
    }

    std::uint32_t PendingAudioInputSourceHandle() const
    {
        return audioInput_.PendingSourceHandle();
    }

    // What the Audio page currently knows about capture. The physical count is
    // already clamped to the application request, so a device that supplies more
    // channels than the application addresses never inflates the reported active
    // count.
    BrowserAudioInputState AudioInputStateSnapshot() const
    {
        BrowserAudioInputState state;
        state.requestedChannels = AudioInputChannels();
        state.activeChannels =
            std::min<std::size_t>(audioInput_.PhysicalChannels(), state.requestedChannels);
        state.status = static_cast<BrowserAudioInputStatus>(audioInput_.StatusCode());
        return state;
    }

    // sar-33: one derived flag over the existing capture-grant/-revoke
    // lifecycle above. Called from SetAudioInputSource (the grant point:
    // BrowserRuntimeAbi.cpp's synth_browser_set_audio_input_source calls
    // through to this method) and ClearAudioInputSource (the revoke point:
    // synth_browser_clear_audio_input_source), both invoked only from the JS
    // bridge (browser/src/audio.ts's AudioBridge, after
    // navigator.mediaDevices.getUserMedia settles from the user-activation-
    // bound request) -- never from the audio worklet render thread (see
    // Process()/ProcessAudioWorkletPlanarBlock, which only ever READ
    // audioInput_'s published state, never call these two setters). Routed
    // mirrors BrowserAudioInputCaptureLive (BrowserAudioDevices.hpp): only
    // Online/ChannelCountUnreported count as a live, user-gesture-granted
    // capture; every other status (including a zero-input application's
    // permanent NotRequested) is not-routed, identically to the JUCE side's
    // "not the platform default" rule.
    void RefreshInputRoutedState()
    {
        const BrowserAudioInputState state = AudioInputStateSnapshot();
        const bool routed = state.requestedChannels > 0 && BrowserAudioInputCaptureLive(state.status);
        inputRoutingSignal_.Publish(routed);
    }

    // The only sources of a pending request are the user pressing `Retry
    // Input` and selecting an input or output device on the Audio page
    // (sbw-4): capture loss alone never arms one, so a lost stream cannot
    // re-prompt off the back of an unrelated UI action. See
    // BrowserAudioDevices.hpp for the sentinel values this returns; outControl
    // reports which control (input or output) the returned index applies to.
    int ConsumePendingAudioRequest(BrowserAudioDeviceKind& outControl)
    {
        return services_.ConsumePendingAudioRequest(outControl);
    }

    std::uint32_t AudioWorkletBlockCount() const
    {
        return audioWorkletBlockCount_.load(std::memory_order_acquire);
    }

    std::uint32_t AudioWorkletPeakMicrounits() const
    {
        return audioWorkletPeakMicrounits_.load(std::memory_order_acquire);
    }

    std::uint32_t AudioWorkletDeadlineMicrounits() const
    {
        return audioWorkletDeadlineMeter_.SampleMicrounits();
    }

    float AudioWorkletDeadlineSamplePercent() const
    {
        return static_cast<float>(AudioWorkletDeadlineMicrounits()) / 1'000'000.0f;
    }

    void Process(float** outputs, std::size_t outputChannels, std::size_t frames, std::uint64_t timestampMicros)
    {
        RequireStarted();
        if (outputs != nullptr &&
            outputChannels != static_cast<std::size_t>(engine_.Config().numAudioOutputs)) {
            throw std::invalid_argument("browser audio channel count does not match app config");
        }
        this->timestampMicros_.store(timestampMicros, std::memory_order_relaxed);
        synth::AudioBlock block;
        block.outputs = outputs;
        block.numOutputChannels = outputs == nullptr ? 0 : outputChannels;
        block.numFrames = frames;
        block.numRequestedInputChannels = static_cast<int>(AudioInputChannels());
        engine_.ProcessBlock(block, timestampMicros);
    }

    bool ProcessAudioWorkletPlanarBlock(
        int numInputs,
        const BrowserAudioSampleFrameDescriptor* inputs,
        int numOutputs,
        BrowserAudioSampleFrameDescriptor* outputs,
        std::uint64_t timestampMicros) noexcept
    {
        if (!started_.load(std::memory_order_acquire)) {
            SilenceBrowserAudioOutput(outputs, numOutputs);
            return false;
        }
        try {
            timestampMicros_.store(timestampMicros, std::memory_order_relaxed);
            const bool processed = AdaptBrowserAudioWorkletPlanarBlock(
                numInputs,
                inputs,
                numOutputs,
                outputs,
                AudioInputChannels(),
                audioInput_.PhysicalChannels(),
                AudioOutputChannels(),
                timestampMicros,
                [this](synth::AudioBlock& block, std::uint64_t timestamp) {
                    engine_.ProcessBlock(block, timestamp);
                });
            if (processed) {
                audioWorkletBlockCount_.fetch_add(1, std::memory_order_acq_rel);
                PublishAudioWorkletPeak(outputs, numOutputs);
            }
        } catch (const std::exception&) {
            SilenceBrowserAudioOutput(outputs, numOutputs);
        }
        return true;
    }

    void MessageTick(std::uint64_t timestampMicros)
    {
        RequireStarted();
        this->timestampMicros_.store(timestampMicros, std::memory_order_relaxed);
        engine_.MessageThreadTick();
        if (const auto patchResult = engine_.ConsumeLastTickPatchResult();
            patchResult.has_value() && patchResult->status == synth::PatchCommandStatus::Written) {
            persistenceDirty_ = true;
        }
        mainComponent_.Refresh();
    }

    CommandBuffer BuildUiFrame()
    {
        RequireStarted();
        return SerializeNodeTree(mainComponent_.BuildTree());
    }

    void DispatchAction(std::string name, std::string value)
    {
        RequireStarted();
        mainComponent_.DispatchAction(
            synth::ui::Action::WithValue(std::move(name), std::move(value)));
        mainComponent_.Refresh();
    }

    void SubmitMidiEndpoints(const std::vector<typename BrowserMidiBridge<synth::Engine<App>>::Endpoint>& endpoints)
    {
        RequireStarted();
        midiBridge_.SubmitEndpoints(endpoints);
        services_.NoteMidiDeviceListChanged();
    }

    void SubmitAudioDevices(std::vector<BrowserAudioDevice> devices)
    {
        RequireStarted();
        submittedAudioDevices_ = std::move(devices);
    }

    std::optional<typename BrowserMidiBridge<synth::Engine<App>>::Action> DequeueMidiAction()
    {
        RequireStarted();
        return midiBridge_.DequeueAction();
    }

    bool DeliverMidi(std::size_t controllerIx, const std::vector<std::uint8_t>& bytes, std::uint64_t timestampMicros)
    {
        RequireStarted();
        return midiBridge_.DeliverIncoming(controllerIx, bytes, timestampMicros);
    }

    std::optional<typename BrowserMidiBridge<synth::Engine<App>>::OutboundMessage> DequeueMidiOutput()
    {
        RequireStarted();
        return midiBridge_.DequeueOutput();
    }

    typename BrowserMidiBridge<synth::Engine<App>>::Diagnostics MidiDiagnosticsSnapshot()
    {
        RequireStarted();
        return midiBridge_.DiagnosticsSnapshot();
    }

    synth::Engine<App>& Engine() { return engine_; }

    bool ConsumePersistenceDirty()
    {
        const bool servicesDirty = services_.ConsumePersistenceDirty();
        const bool dirty = persistenceDirty_ || servicesDirty;
        persistenceDirty_ = false;
        return dirty;
    }

private:
    static std::size_t StaticAudioInputChannels()
    {
        const synth::RuntimeConfig config = App::Config();
        // Pre-creation browser ABI calls must be infallible. Negative app
        // constants are reported as zero here; `Initialize()` runs the shared
        // RuntimeConfig validator and rejects them before startup.
        return config.numAudioInputs > 0 ? static_cast<std::size_t>(config.numAudioInputs)
                                         : std::size_t{0};
    }

    static std::uint64_t ApplyTimestampEpochOffset(
        std::uint64_t timestampMicros,
        std::int64_t offsetMicros) noexcept
    {
        if (offsetMicros >= 0) {
            const auto positiveOffset = static_cast<std::uint64_t>(offsetMicros);
            return timestampMicros > std::numeric_limits<std::uint64_t>::max() - positiveOffset
                ? std::numeric_limits<std::uint64_t>::max()
                : timestampMicros + positiveOffset;
        }
        const auto magnitude = static_cast<std::uint64_t>(-(offsetMicros + 1)) + 1;
        return timestampMicros < magnitude ? 0 : timestampMicros - magnitude;
    }

    void RequireStarted() const
    {
        if (!started_.load(std::memory_order_acquire)) {
            throw std::logic_error(stopped_.load(std::memory_order_acquire) ? "browser runtime is stopped" : "browser runtime is not started");
        }
    }

    void PublishAudioWorkletPeak(const BrowserAudioSampleFrameDescriptor* outputs,
                                 int numOutputs) noexcept
    {
        if (numOutputs <= 0 || outputs == nullptr) {
            return;
        }
        float peak = 0.0f;
        for (int outputIndex = 0; outputIndex < numOutputs; ++outputIndex) {
            const BrowserAudioSampleFrameDescriptor& output = outputs[outputIndex];
            if (output.data == nullptr || output.numberOfChannels <= 0 ||
                output.samplesPerChannel <= 0) {
                continue;
            }
            for (int channel = 0; channel < output.numberOfChannels; ++channel) {
                const float* samples = output.data + (channel * output.samplesPerChannel);
                for (int frame = 0; frame < output.samplesPerChannel; ++frame) {
                    peak = std::max(peak, std::abs(samples[frame]));
                }
            }
        }
        const auto peakMicrounits = static_cast<std::uint32_t>(
            std::min(1'000'000.0f, peak * 1'000'000.0f));
        std::uint32_t current = audioWorkletPeakMicrounits_.load(std::memory_order_acquire);
        while (peakMicrounits > current &&
               !audioWorkletPeakMicrounits_.compare_exchange_weak(
                   current, peakMicrounits, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
    }

    void RequireNotStarted() const
    {
        if (started_.load(std::memory_order_acquire) || stopped_.load(std::memory_order_acquire)) {
            throw std::logic_error("browser runtime cannot be started again");
        }
    }

#ifdef __EMSCRIPTEN__
    static void AudioWorkletThreadInitialized(EMSCRIPTEN_WEBAUDIO_T audioContext, bool success, void* userData)
    {
        auto* runtime = static_cast<Runtime*>(userData);
        if (runtime == nullptr || !success) {
            return;
        }
        WebAudioWorkletProcessorCreateOptions options{
            .name = "sheaf-synth-audio",
            .numAudioParams = 0,
            .audioParamDescriptors = nullptr,
        };
        emscripten_create_wasm_audio_worklet_processor_async(audioContext,
                                                             &options,
                                                             &Runtime::AudioWorkletProcessorCreated,
                                                             userData);
    }

    static void AudioWorkletProcessorCreated(EMSCRIPTEN_WEBAUDIO_T audioContext, bool success, void* userData)
    {
        auto* runtime = static_cast<Runtime*>(userData);
        if (runtime == nullptr || !success) {
            return;
        }
        const auto channels = static_cast<int>(runtime->AudioOutputChannels());
        const auto inputChannels = runtime->AudioInputChannels();
        if (channels <= 0 || static_cast<std::size_t>(channels) > kMaxBrowserOutputChannels ||
            inputChannels > kMaxBrowserInputChannels) {
            return;
        }
        runtime->audioOutputChannelCounts_[0] = channels;
        EmscriptenAudioWorkletNodeCreateOptions nodeOptions{
            .numberOfInputs = inputChannels == 0 ? 0 : 1,
            .numberOfOutputs = 1,
            .outputChannelCounts = runtime->audioOutputChannelCounts_.data(),
            .channelCount = static_cast<unsigned long>(inputChannels == 0
                                                           ? static_cast<std::size_t>(channels)
                                                           : inputChannels),
            .channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_EXPLICIT,
            .channelInterpretation = inputChannels == 0
                                         ? WEBAUDIO_CHANNEL_INTERPRETATION_SPEAKERS
                                         : WEBAUDIO_CHANNEL_INTERPRETATION_DISCRETE,
        };
        runtime->audioNode_ = emscripten_create_wasm_audio_worklet_node(audioContext,
                                                                        "sheaf-synth-audio",
                                                                        &nodeOptions,
                                                                        &Runtime::ProcessAudioWorklet,
                                                                        userData);
        if (runtime->audioNode_ != 0) {
            // A source claimed before this node existed is attached now. A
            // failure here publishes an offline input claim, but output startup
            // and the worklet thread stay live. Destroy() uses
            // audioWorkletStarted_ to retain this Runtime and its inline worklet
            // stack after thread creation succeeds.
            (void)runtime->ResolveDeferredAudioInputConnection();
            emscripten_audio_node_connect(runtime->audioNode_, audioContext, 0, 0);
            emscripten_resume_audio_context_sync(audioContext);
        }
    }

    static bool ProcessAudioWorklet(int numInputs,
                                    const AudioSampleFrame* inputs,
                                    int numOutputs,
                                    AudioSampleFrame* outputs,
                                    int,
                                    const AudioParamFrame*,
                                    void* userData)
    {
        auto* runtime = static_cast<Runtime*>(userData);
        if (runtime == nullptr || !runtime->started_.load(std::memory_order_acquire)) {
            return false;
        }
        BrowserAudioSampleFrameDescriptor inputDescriptors[1]{};
        const BrowserAudioSampleFrameDescriptor* inputDescriptorPointer = nullptr;
        int adaptedInputs = 0;
        if (numInputs > 0 && inputs != nullptr) {
            inputDescriptors[0] = {
                .numberOfChannels = inputs[0].numberOfChannels,
                .samplesPerChannel = inputs[0].samplesPerChannel,
                .data = inputs[0].data,
            };
            inputDescriptorPointer = inputDescriptors;
            adaptedInputs = 1;
        }
        BrowserAudioSampleFrameDescriptor outputDescriptors[1]{};
        BrowserAudioSampleFrameDescriptor* outputDescriptorPointer = nullptr;
        int adaptedOutputs = 0;
        if (numOutputs > 0 && outputs != nullptr) {
            outputDescriptors[0] = {
                .numberOfChannels = outputs[0].numberOfChannels,
                .samplesPerChannel = outputs[0].samplesPerChannel,
                .data = outputs[0].data,
            };
            outputDescriptorPointer = outputDescriptors;
            adaptedOutputs = 1;
        }
        const std::uint64_t timestamp = runtime->audioCallbackTimestampMicros_.fetch_add(
            runtime->audioCallbackBlockMicros_.load(std::memory_order_acquire),
            std::memory_order_acq_rel);
        const double callbackStartMs = emscripten_get_now();
        const bool keepAlive = runtime->ProcessAudioWorkletPlanarBlock(
            adaptedInputs,
            inputDescriptorPointer,
            adaptedOutputs,
            outputDescriptorPointer,
            timestamp);
        const double elapsedMicros = std::max(0.0, (emscripten_get_now() - callbackStartMs) * 1000.0);
        const auto blockMicros = runtime->audioCallbackBlockMicros_.load(std::memory_order_acquire);
        runtime->audioWorkletDeadlineMeter_.RecordCallbackMicros(
            static_cast<std::uint64_t>(std::llround(elapsedMicros)),
            blockMicros);
        return keepAlive;
    }

#endif

    // The Web Audio graph work behind the transactional publication. Both report
    // failure instead of throwing: a JavaScript exception raised inside EM_ASM
    // cannot be caught here, and the publication needs a verdict to decide
    // between rolling back and staying blocked. On a native build there is no
    // graph, so both succeed trivially and the failure stages are covered
    // directly through BrowserAudioInputPublication.
    //
    // Before the worklet node exists there is nothing to attach to, so connect
    // defers and the publication's `pendingHandle_` records intent;
    // AudioWorkletProcessorCreated resolves whatever is pending by then.
    bool DisconnectAudioInputSource(std::uint32_t sourceHandle)
    {
        if (audioInputDisconnectOverride_) {
            return audioInputDisconnectOverride_(sourceHandle);
        }
#ifdef __EMSCRIPTEN__
        if (audioNode_ == 0) {
            return true;
        }
        return EM_ASM_INT({
            if (typeof emscriptenGetAudioObject !== "function") return 1;
            const source = emscriptenGetAudioObject($0);
            const destination = emscriptenGetAudioObject($1);
            if (!source || !destination) return 2;
            try {
                source.disconnect(destination);
            } catch (error) {
                // Already disconnected is the state this asked for.
                if (error && error.name === "InvalidAccessError") return 0;
                return 3;
            }
            return 0;
        }, sourceHandle, audioNode_) == 0;
#else
        (void)sourceHandle;
        return true;
#endif
    }

    // Deferred rather than connected while there is no worklet node: the source
    // is claimed but demonstrably not in the graph, and saying otherwise is what
    // would later ask the host to detach something it never attached.
    BrowserAudioInputConnectResult ConnectAudioInputSource(std::uint32_t sourceHandle)
    {
        if (audioInputConnectOverride_) {
            return audioInputConnectOverride_(sourceHandle);
        }
#ifdef __EMSCRIPTEN__
        if (audioNode_ == 0) {
            return BrowserAudioInputConnectResult::Deferred;
        }
        return EM_ASM_INT({
            if (typeof emscriptenGetAudioObject !== "function") return 1;
            const source = emscriptenGetAudioObject($0);
            const destination = emscriptenGetAudioObject($1);
            if (!source || !destination) return 2;
            try {
                source.connect(destination, 0, 0);
            } catch (error) {
                return 3;
            }
            return 0;
        }, sourceHandle, audioNode_) == 0
            ? BrowserAudioInputConnectResult::Connected
            : BrowserAudioInputConnectResult::Failed;
#else
        // A native build has no Web Audio graph at all, so a source can only
        // ever be claimed, never attached.
        (void)sourceHandle;
        return BrowserAudioInputConnectResult::Deferred;
#endif
    }

    std::atomic<std::uint64_t> timestampMicros_{0};
    std::atomic<std::int64_t> timestampEpochOffsetMicros_{0};
    std::atomic<std::uint64_t> audioCallbackTimestampMicros_{0};
    std::atomic<std::uint64_t> audioCallbackBlockMicros_{0};
    std::atomic<std::uint32_t> audioWorkletBlockCount_{0};
    std::atomic<std::uint32_t> audioWorkletPeakMicrounits_{0};
    BrowserAudioInputPublication audioInput_;
    // sar-33: this Runtime's storage for AppContext::inputRoutingSignal
    // (wired in the constructor, above). See RefreshInputRoutedState for the
    // browser derivation and its two call sites.
    synth::InputRoutingSignal inputRoutingSignal_;
    // Empty in production; see SetAudioInputGraphForTesting.
    std::function<BrowserAudioInputConnectResult(std::uint32_t)> audioInputConnectOverride_;
    std::function<bool(std::uint32_t)> audioInputDisconnectOverride_;
    AudioWorkletDeadlineMeter audioWorkletDeadlineMeter_;
    const std::size_t requestedAudioInputChannels_ = StaticAudioInputChannels();
    synth::Engine<App> engine_;
    BrowserMidiBridge<synth::Engine<App>> midiBridge_;
    // The devices JS most recently submitted through SubmitAudioDevices,
    // stored beside midiBridge_ the same way its own submitted list lives on
    // this Runtime; services_ below holds this by reference, so it must be
    // declared (and therefore constructed) before services_ is.
    std::vector<BrowserAudioDevice> submittedAudioDevices_;
    BrowserRuntimeMainServices<App> services_;
    synth::runtime_ui::RuntimeMainComponent<App, BrowserRuntimeMainServices<App>> mainComponent_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
    bool persistenceDirty_ = false;
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBAUDIO_T audioContext_ = 0;
    EMSCRIPTEN_WEBAUDIO_T audioNode_ = 0;
    std::atomic<bool> audioWorkletStarted_{false};
    alignas(16) std::array<std::uint8_t, 16 * 1024> audioWorkletStack_{};
    std::array<int, 1> audioOutputChannelCounts_{};
#endif
};

}  // namespace synth_browser

namespace synth_browser {

struct MidiEndpointDescriptor {
    const char* identifier = nullptr;
    std::uint32_t identifierSize = 0;
    const char* name = nullptr;
    std::uint32_t nameSize = 0;
    std::uint32_t kind = 0;
};

// Same shape as MidiEndpointDescriptor -- two UTF-8 (pointer, size) fields
// plus a 0/1 kind discriminator -- carrying a browser audio device instead of
// a MIDI endpoint. See DecodeBrowserDescriptorArray below for the packing
// discipline both descriptor arrays share on receipt.
struct AudioDeviceDescriptor {
    const char* deviceId = nullptr;
    std::uint32_t deviceIdSize = 0;
    const char* label = nullptr;
    std::uint32_t labelSize = 0;
    std::uint32_t kind = 0;
};

struct MidiActionDescriptor {
    std::uint32_t type = 0;
    std::uint32_t controllerIx = 0;
    const char* identifier = nullptr;
    std::uint32_t identifierSize = 0;
    const char* name = nullptr;
    std::uint32_t nameSize = 0;
};

// Stable Wasm32 ABI record for one bounded browser-output dequeue. Scheduled
// deadlines use the browser engine's performance.timeOrigin-relative
// microsecond epoch. Immediate feedback uses delivery=0 and dueTimeMicros=0.
struct MidiOutputDescriptor {
    std::uint32_t controllerIx = 0;
    std::uint32_t size = 0;
    std::uint32_t delivery = 0;
    std::uint32_t reserved = 0;
    std::uint64_t dueTimeMicros = 0;
};

static_assert(sizeof(MidiOutputDescriptor) == 24);
static_assert(offsetof(MidiOutputDescriptor, dueTimeMicros) == 16);

struct MidiDiagnosticsDescriptor {
    std::uint64_t droppedImmediateOutputCount = 0;
    std::uint64_t droppedScheduledOutputCount = 0;
    std::uint64_t lateScheduledOutputCount = 0;
};

static_assert(sizeof(MidiDiagnosticsDescriptor) == 24);
static_assert(offsetof(MidiDiagnosticsDescriptor, droppedScheduledOutputCount) == 8);
static_assert(offsetof(MidiDiagnosticsDescriptor, lateScheduledOutputCount) == 16);

// Decodes one (pointer, size) UTF-8 field shared by a descriptor entry,
// copying it into a std::string during this call -- the same
// copy-during-the-call contract that makes the JS side's synchronous free of
// its packed buffer safe. A null pointer with a nonzero size is malformed and
// reported as such; a null pointer with a zero size is an empty field.
inline std::optional<std::string> DecodeBrowserUtf8Field(const char* pointer, std::uint32_t size)
{
    if (pointer == nullptr && size != 0) {
        return std::nullopt;
    }
    return pointer == nullptr ? std::string{} : std::string(pointer, size);
}

// Shared by SubmitMidiEndpoints and SubmitAudioDevices below: both receive a
// descriptor array from JS and must decode it into a vector of Elements
// before returning, since the descriptors only point at buffers JS is free to
// release once this call returns. `decodeOne` converts a single descriptor,
// returning std::nullopt for a malformed entry; a single malformed entry
// fails the whole array, matching each function's prior all-or-nothing
// behaviour.
template <typename Descriptor, typename Element, typename DecodeOne>
std::optional<std::vector<Element>> DecodeBrowserDescriptorArray(
    const Descriptor* items, std::uint32_t count, DecodeOne&& decodeOne)
{
    if (count > 0 && items == nullptr) {
        return std::nullopt;
    }
    std::vector<Element> converted;
    converted.reserve(count);
    for (std::uint32_t ix = 0; ix < count; ++ix) {
        std::optional<Element> element = decodeOne(items[ix]);
        if (!element.has_value()) {
            return std::nullopt;
        }
        converted.push_back(std::move(*element));
    }
    return converted;
}

// The ABI erases an application-specific Runtime<App>. BrowserAppEntry is the
// sole binding point that instantiates this adapter for a concrete app.
class RuntimeAbi {
public:
    virtual ~RuntimeAbi() = default;
    virtual std::size_t AudioOutputChannels() const = 0;
    virtual std::size_t AudioInputChannels() const = 0;
    virtual int Initialize(const char* publisherId, const char* appId,
                           std::uint32_t runtimeConfigVersion) = 0;
    virtual int Prepare(double sampleRate, std::size_t blockSize) = 0;
    virtual int Process(float** outputs, std::size_t outputChannels, std::size_t frames,
                        std::uint64_t timestampMicros) = 0;
    virtual int StartAudioWorklet(std::uint32_t audioContextHandle) = 0;
    virtual std::uint32_t AudioWorkletBlockCount() const = 0;
    virtual std::uint32_t AudioWorkletPeakMicrounits() const = 0;
    virtual std::uint32_t AudioWorkletDeadlineMicrounits() const = 0;
    virtual int SetAudioInputSource(std::uint32_t sourceHandle,
                                    std::uint32_t physicalChannels,
                                    std::uint32_t statusCode) = 0;
    virtual int ClearAudioInputSource(std::uint32_t statusCode) = 0;
    virtual int ConsumePendingAudioRequest(std::uint32_t* outControl) = 0;
    virtual int SetTimestampEpochOffsetMicros(std::int64_t offsetMicros) = 0;
    virtual int MessageTick(std::uint64_t timestampMicros) = 0;
    virtual const std::uint8_t* BuildUiFrame(std::size_t* size) = 0;
    virtual int DispatchAction(const char* name, const char* value) = 0;
    virtual bool ConsumePersistenceDirty() = 0;
    virtual int SubmitMidiEndpoints(const MidiEndpointDescriptor* endpoints, std::uint32_t count) = 0;
    virtual int SubmitAudioDevices(const AudioDeviceDescriptor* devices, std::uint32_t count) = 0;
    virtual int DequeueMidiAction(MidiActionDescriptor* action) = 0;
    virtual int DeliverMidi(std::uint32_t controllerIx, const std::uint8_t* bytes, std::uint32_t size,
                            std::uint64_t timestampMicros) = 0;
    virtual const std::uint8_t* DequeueMidiOutput(MidiOutputDescriptor* descriptor) = 0;
    virtual int MidiDiagnostics(MidiDiagnosticsDescriptor* descriptor) = 0;
    virtual void Destroy() = 0;
};

template <synth::SynthApplication App>
class RuntimeAbiAdapter final : public RuntimeAbi {
public:
    std::size_t AudioOutputChannels() const override { return runtime_.AudioOutputChannels(); }
    std::size_t AudioInputChannels() const override { return runtime_.AudioInputChannels(); }

    int Initialize(const char* publisherId, const char* appId,
                   std::uint32_t runtimeConfigVersion) override
    {
        if (publisherId == nullptr || appId == nullptr) {
            return -1;
        }
        try {
            runtime_.SetRuntimeDataPaths(
                BrowserPersistentDataPaths(publisherId, appId, runtimeConfigVersion));
            runtime_.Start();
            return 0;
        } catch (const std::exception&) {
            return -1;
        }
    }

    int Prepare(double sampleRate, std::size_t blockSize) override
    {
        return Invoke([this, sampleRate, blockSize] { runtime_.Prepare(sampleRate, blockSize); });
    }

    int Process(float** outputs, std::size_t outputChannels, std::size_t frames,
                std::uint64_t timestampMicros) override
    {
        return Invoke([this, outputs, outputChannels, frames, timestampMicros] {
            runtime_.Process(outputs, outputChannels, frames, timestampMicros);
        });
    }

    int StartAudioWorklet(std::uint32_t audioContextHandle) override
    {
        return Invoke([this, audioContextHandle] {
            if (!runtime_.StartAudioWorklet(
                    static_cast<EMSCRIPTEN_WEBAUDIO_T>(audioContextHandle))) {
                throw std::runtime_error("browser runtime failed to start AudioWorklet");
            }
        });
    }

    std::uint32_t AudioWorkletBlockCount() const override
    {
        return runtime_.AudioWorkletBlockCount();
    }

    std::uint32_t AudioWorkletPeakMicrounits() const override
    {
        return runtime_.AudioWorkletPeakMicrounits();
    }

    std::uint32_t AudioWorkletDeadlineMicrounits() const override
    {
        return runtime_.AudioWorkletDeadlineMicrounits();
    }

    int SetAudioInputSource(std::uint32_t sourceHandle,
                            std::uint32_t physicalChannels,
                            std::uint32_t statusCode) override
    {
        return runtime_.SetAudioInputSource(sourceHandle, physicalChannels, statusCode) ? 0 : -1;
    }

    int ClearAudioInputSource(std::uint32_t statusCode) override
    {
        return runtime_.ClearAudioInputSource(statusCode) ? 0 : -1;
    }

    int ConsumePendingAudioRequest(std::uint32_t* outControl) override
    {
        // A null out-param cannot report which control the index applies to,
        // so the request is left armed rather than consumed on a guess.
        if (outControl == nullptr) {
            return kNoPendingAudioRequest;
        }
        BrowserAudioDeviceKind control = BrowserAudioDeviceKind::Input;
        const int pending = runtime_.ConsumePendingAudioRequest(control);
        *outControl = static_cast<std::uint32_t>(control);
        return pending;
    }

    int SetTimestampEpochOffsetMicros(std::int64_t offsetMicros) override
    {
        return Invoke([this, offsetMicros] {
            runtime_.SetTimestampEpochOffsetMicros(offsetMicros);
        });
    }

    int MessageTick(std::uint64_t timestampMicros) override
    {
        return Invoke([this, timestampMicros] { runtime_.MessageTick(timestampMicros); });
    }

    const std::uint8_t* BuildUiFrame(std::size_t* size) override
    {
        if (size == nullptr) {
            return nullptr;
        }
        try {
            frame_ = runtime_.BuildUiFrame();
            *size = frame_.bytes.size();
            return reinterpret_cast<const std::uint8_t*>(frame_.bytes.data());
        } catch (const std::exception&) {
            *size = 0;
            return nullptr;
        }
    }

    int DispatchAction(const char* name, const char* value) override
    {
        if (name == nullptr || value == nullptr) {
            return -1;
        }
        return Invoke([this, name, value] { runtime_.DispatchAction(name, value); });
    }

    bool ConsumePersistenceDirty() override
    {
        return runtime_.ConsumePersistenceDirty();
    }

    int SubmitMidiEndpoints(const MidiEndpointDescriptor* endpoints, std::uint32_t count) override
    {
        using Endpoint = typename BrowserMidiBridge<synth::Engine<App>>::Endpoint;
        using EndpointKind = typename BrowserMidiBridge<synth::Engine<App>>::EndpointKind;
        std::optional<std::vector<Endpoint>> converted =
            DecodeBrowserDescriptorArray<MidiEndpointDescriptor, Endpoint>(
                endpoints, count, [](const MidiEndpointDescriptor& endpoint) -> std::optional<Endpoint> {
                    if (endpoint.kind > 1) {
                        return std::nullopt;
                    }
                    std::optional<std::string> identifier =
                        DecodeBrowserUtf8Field(endpoint.identifier, endpoint.identifierSize);
                    std::optional<std::string> name =
                        DecodeBrowserUtf8Field(endpoint.name, endpoint.nameSize);
                    if (!identifier.has_value() || !name.has_value()) {
                        return std::nullopt;
                    }
                    return Endpoint{
                        .identifier = std::move(*identifier),
                        .name = std::move(*name),
                        .kind = endpoint.kind == 0 ? EndpointKind::Input : EndpointKind::Output,
                    };
                });
        if (!converted.has_value()) {
            return -1;
        }
        return Invoke([this, &converted] { runtime_.SubmitMidiEndpoints(*converted); });
    }

    int SubmitAudioDevices(const AudioDeviceDescriptor* devices, std::uint32_t count) override
    {
        std::optional<std::vector<BrowserAudioDevice>> converted =
            DecodeBrowserDescriptorArray<AudioDeviceDescriptor, BrowserAudioDevice>(
                devices, count, [](const AudioDeviceDescriptor& device) -> std::optional<BrowserAudioDevice> {
                    if (device.kind > 1) {
                        return std::nullopt;
                    }
                    std::optional<std::string> deviceId =
                        DecodeBrowserUtf8Field(device.deviceId, device.deviceIdSize);
                    std::optional<std::string> label =
                        DecodeBrowserUtf8Field(device.label, device.labelSize);
                    if (!deviceId.has_value() || !label.has_value()) {
                        return std::nullopt;
                    }
                    return BrowserAudioDevice{
                        .deviceId = std::move(*deviceId),
                        .label = std::move(*label),
                        .kind = device.kind == 0 ? BrowserAudioDeviceKind::Input
                                                  : BrowserAudioDeviceKind::Output,
                    };
                });
        if (!converted.has_value()) {
            return -1;
        }
        return Invoke([this, &converted] { runtime_.SubmitAudioDevices(std::move(*converted)); });
    }

    int DequeueMidiAction(MidiActionDescriptor* action) override
    {
        if (action == nullptr) {
            return -1;
        }
        try {
            action_.reset();
            action_ = runtime_.DequeueMidiAction();
            if (!action_.has_value()) {
                return 0;
            }
            action->type = static_cast<std::uint32_t>(action_->type);
            action->controllerIx = static_cast<std::uint32_t>(action_->controllerIx);
            action->identifier = action_->identifier.data();
            action->identifierSize = static_cast<std::uint32_t>(action_->identifier.size());
            action->name = action_->name.data();
            action->nameSize = static_cast<std::uint32_t>(action_->name.size());
            return 1;
        } catch (const std::exception&) {
            return -1;
        }
    }

    int DeliverMidi(std::uint32_t controllerIx, const std::uint8_t* bytes, std::uint32_t size,
                    std::uint64_t timestampMicros) override
    {
        if (size == 0 || bytes == nullptr) {
            return -1;
        }
        return Invoke([this, controllerIx, bytes, size, timestampMicros] {
            if (!runtime_.DeliverMidi(controllerIx, std::vector<std::uint8_t>(bytes, bytes + size), timestampMicros)) {
                throw std::runtime_error("browser MIDI input has no selected controller");
            }
        });
    }

    const std::uint8_t* DequeueMidiOutput(MidiOutputDescriptor* descriptor) override
    {
        if (descriptor == nullptr) {
            return nullptr;
        }
        try {
            output_.reset();
            output_ = runtime_.DequeueMidiOutput();
            if (!output_.has_value()) {
                *descriptor = {};
                return nullptr;
            }
            *descriptor = MidiOutputDescriptor{
                .controllerIx = static_cast<std::uint32_t>(output_->controllerIx),
                .size = static_cast<std::uint32_t>(output_->bytes.size()),
                .delivery = static_cast<std::uint32_t>(output_->delivery),
                .dueTimeMicros = output_->dueTimeMicros,
            };
            return output_->bytes.data();
        } catch (const std::exception&) {
            *descriptor = {};
            return nullptr;
        }
    }

    int MidiDiagnostics(MidiDiagnosticsDescriptor* descriptor) override
    {
        if (descriptor == nullptr) {
            return -1;
        }
        try {
            const auto diagnostics = runtime_.MidiDiagnosticsSnapshot();
            *descriptor = MidiDiagnosticsDescriptor{
                .droppedImmediateOutputCount = diagnostics.droppedImmediateOutputCount,
                .droppedScheduledOutputCount = diagnostics.droppedScheduledOutputCount,
                .lateScheduledOutputCount = diagnostics.lateScheduledOutputCount,
            };
            return 0;
        } catch (const std::exception&) {
            *descriptor = {};
            return -1;
        }
    }

    void Destroy() override
    {
        const bool retainForAudioWorklet = runtime_.RetainAfterStopForAudioWorklet();
        runtime_.Stop();
        if (retainForAudioWorklet) {
            // Emscripten's WebAudio destroy path suspends the context but does
            // not expose a synchronous join for the Wasm AudioWorklet thread.
            // Keep the erased runtime and its worklet stack alive for the
            // browser page lifetime so late process callbacks cannot touch
            // freed userdata.
            return;
        }
        delete this;
    }

private:
    template <typename Operation>
    static int Invoke(Operation&& operation)
    {
        try {
            std::forward<Operation>(operation)();
            return 0;
        } catch (const std::exception&) {
            return -1;
        }
    }

    Runtime<App> runtime_;
    CommandBuffer frame_;
    std::optional<typename BrowserMidiBridge<synth::Engine<App>>::Action> action_;
    std::optional<typename BrowserMidiBridge<synth::Engine<App>>::OutboundMessage> output_;
};

}  // namespace synth_browser

extern "C" {

struct synth_browser_runtime;

std::uint32_t synth_browser_abi_version();
std::uint32_t synth_browser_ui_protocol_version();
std::uint32_t synth_browser_runtime_config_version();
synth_browser_runtime* synth_browser_create();
int synth_browser_initialize(synth_browser_runtime* runtime, const char* publisherId,
                             const char* appId, std::uint32_t runtimeConfigVersion);
std::size_t synth_browser_audio_output_channels(synth_browser_runtime* runtime);
std::size_t synth_browser_audio_input_channels(synth_browser_runtime* runtime);
int synth_browser_prepare(synth_browser_runtime* runtime, double sampleRate, std::size_t blockSize);
int synth_browser_process(synth_browser_runtime* runtime, float** outputs, std::size_t outputChannels,
                          std::size_t frames, std::uint64_t timestampMicros);
int synth_browser_start_audio_worklet(synth_browser_runtime* runtime,
                                      std::uint32_t audioContextHandle);
int synth_browser_set_audio_input_source(synth_browser_runtime* runtime,
                                         std::uint32_t sourceHandle,
                                         std::uint32_t physicalChannels,
                                         std::uint32_t statusCode);
int synth_browser_clear_audio_input_source(synth_browser_runtime* runtime,
                                           std::uint32_t statusCode);
int synth_browser_consume_pending_audio_request(synth_browser_runtime* runtime, std::uint32_t* outControl);
int synth_browser_set_timestamp_epoch_offset(
    synth_browser_runtime* runtime, std::int64_t offsetMicros);
std::uint32_t synth_browser_audio_worklet_block_count(synth_browser_runtime* runtime);
std::uint32_t synth_browser_audio_worklet_peak_microunits(synth_browser_runtime* runtime);
std::uint32_t synth_browser_audio_worklet_deadline_microunits(synth_browser_runtime* runtime);
int synth_browser_message_tick(synth_browser_runtime* runtime, std::uint64_t timestampMicros);
const std::uint8_t* synth_browser_build_ui_frame(synth_browser_runtime* runtime, std::size_t* size);
int synth_browser_dispatch_action(synth_browser_runtime* runtime, const char* name, const char* value);
int synth_browser_consume_persistence_dirty(synth_browser_runtime* runtime);
int synth_browser_submit_midi_endpoints(synth_browser_runtime* runtime,
                                        const synth_browser::MidiEndpointDescriptor* endpoints, std::uint32_t count);
int synth_browser_submit_audio_devices(synth_browser_runtime* runtime,
                                       const synth_browser::AudioDeviceDescriptor* devices, std::uint32_t count);
int synth_browser_dequeue_midi_action(synth_browser_runtime* runtime, synth_browser::MidiActionDescriptor* action);
int synth_browser_deliver_midi(synth_browser_runtime* runtime, std::uint32_t controllerIx, const std::uint8_t* bytes,
                               std::uint32_t size, std::uint64_t timestampMicros);
const std::uint8_t* synth_browser_dequeue_midi_output(
    synth_browser_runtime* runtime, synth_browser::MidiOutputDescriptor* descriptor);
int synth_browser_midi_diagnostics(
    synth_browser_runtime* runtime, synth_browser::MidiDiagnosticsDescriptor* descriptor);
void synth_browser_destroy(synth_browser_runtime* runtime);

}
