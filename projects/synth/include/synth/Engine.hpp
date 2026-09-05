#pragma once

// synth::Engine — the JUCE-free engine core that owns every framework object
// an application touches (sar-3), wires AppContext, and drives the
// application through its pre-audio lifecycle (sar-5) and its audio-thread
// block pump (sar-6, Task 4). Task 5 (MessageThreadTick) fills in the
// message-thread pump: growing serializationArena_ when arenaGrowPending_ is set
// and completing patch/storage responses.
// Retrying pendingPatchMessage_ is NOT the tick's job — ProcessBlock alone
// retries the stash (first, before draining anything newer) once the tick
// has cleared arenaGrowPending_; see the tick contract note on
// MessageThreadTick.

#include "synth/AppConcepts.hpp"
#include "synth/AppContext.hpp"
#include "synth/AsyncLogger.hpp"
#include "synth/MidiController.hpp"
#include "synth/MasterClock.hpp"
#include "synth/ParameterModulation.hpp"
#include "synth/PatchPersistence.hpp"
#include "synth/RuntimeUIState.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace synth {

class ClockDiagnosticsPublication {
public:
    ClockDiagnosticsPublication() noexcept { Publish(ClockDiagnostics{}); }

    void Publish(const ClockDiagnostics& diagnostics) noexcept {
        const std::uint64_t prior = sequence_.fetch_add(1, std::memory_order_seq_cst);
        assert((prior & 1U) == 0U);
        const std::uint64_t metadata =
            static_cast<std::uint64_t>(diagnostics.acquisition) |
            (static_cast<std::uint64_t>(diagnostics.source) << 8U) |
            (static_cast<std::uint64_t>(diagnostics.hasActiveExternalSource) << 16U);
        metadata_.store(metadata, std::memory_order_seq_cst);
        activeExternalSourceSlot_.store(
            static_cast<std::uint64_t>(diagnostics.activeExternalSourceSlot),
            std::memory_order_seq_cst);
        currentBpmBits_.store(std::bit_cast<std::uint64_t>(diagnostics.currentBpm),
                              std::memory_order_seq_cst);
        outputLatencyMicros_.store(diagnostics.outputLatencyMicros, std::memory_order_seq_cst);
        ignoredInputCount_.store(diagnostics.ignoredInputCount, std::memory_order_seq_cst);
        lateEventCount_.store(diagnostics.lateEventCount, std::memory_order_seq_cst);
        droppedOutputCount_.store(diagnostics.droppedOutputCount, std::memory_order_seq_cst);
        sequence_.store(prior + 2U, std::memory_order_seq_cst);
    }

    ClockDiagnostics Snapshot() const noexcept {
        for (;;) {
            const std::uint64_t before = sequence_.load(std::memory_order_seq_cst);
            if ((before & 1U) != 0U) {
                continue;
            }
            const std::uint64_t metadata = metadata_.load(std::memory_order_seq_cst);
            ClockDiagnostics result{
                .acquisition = static_cast<ClockAcquisitionState>(metadata & 0xffU),
                .source = static_cast<ClockSource>((metadata >> 8U) & 0xffU),
                .hasActiveExternalSource = ((metadata >> 16U) & 1U) != 0U,
                .activeExternalSourceSlot = static_cast<std::size_t>(
                    activeExternalSourceSlot_.load(std::memory_order_seq_cst)),
                .currentBpm = std::bit_cast<double>(
                    currentBpmBits_.load(std::memory_order_seq_cst)),
                .outputLatencyMicros = outputLatencyMicros_.load(std::memory_order_seq_cst),
                .ignoredInputCount = ignoredInputCount_.load(std::memory_order_seq_cst),
                .lateEventCount = lateEventCount_.load(std::memory_order_seq_cst),
                .droppedOutputCount = droppedOutputCount_.load(std::memory_order_seq_cst),
            };
            const std::uint64_t after = sequence_.load(std::memory_order_seq_cst);
            if (before == after) {
                return result;
            }
        }
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> metadata_{0};
    std::atomic<std::uint64_t> activeExternalSourceSlot_{0};
    std::atomic<std::uint64_t> currentBpmBits_{0};
    std::atomic<std::uint64_t> outputLatencyMicros_{0};
    std::atomic<std::uint64_t> ignoredInputCount_{0};
    std::atomic<std::uint64_t> lateEventCount_{0};
    std::atomic<std::uint64_t> droppedOutputCount_{0};
};

inline constexpr std::uint64_t EncodeSyncConfiguration(const SyncConfig& config) noexcept {
    return static_cast<std::uint64_t>(config.sendClock) |
           (static_cast<std::uint64_t>(config.receiveClock) << 1U) |
           (static_cast<std::uint64_t>(config.sendTransport) << 2U) |
           (static_cast<std::uint64_t>(config.receiveTransport) << 3U) |
           (static_cast<std::uint64_t>(config.ppqn) << 4U);
}

inline constexpr SyncConfig DecodeSyncConfiguration(std::uint64_t word) noexcept {
    return SyncConfig{
        .sendClock = (word & 1U) != 0U,
        .receiveClock = ((word >> 1U) & 1U) != 0U,
        .sendTransport = ((word >> 2U) & 1U) != 0U,
        .receiveTransport = ((word >> 3U) & 1U) != 0U,
        .ppqn = static_cast<int>(word >> 4U),
    };
}

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

template <SynthApplicationCore App>
class Engine {
public:
    using TimestampProvider = std::function<std::uint64_t()>;

    explicit Engine(TimestampProvider timestampProvider, std::size_t initialArenaCapacity = 256 * 1024)
        : manager_()
        , gridManager_()
        , uiBus_(&manager_)
        , midiBus_(&manager_)
        , parameterMessageOutBus_()
        , patchInputBus_()
        , patchOutputBus_()
        , midiSender_(4096, timestampProvider)
        , absoluteFeedbackCoordinator_()
        , patchManager_(&patchInputBus_, &patchOutputBus_, initialArenaCapacity)
        , masterClock_()
        , instrumentConfig_()
        , defaultInstrumentConfig_()
        , audioDeviceState_()
        , defaultAudioDeviceState_()
        , lastNotifiedAudioDeviceState_()
        , serializationArena_(initialArenaCapacity)
        , serializationContext_()
        , config_()
        , context_()
        , app_()
        , uiState_()
        , gridUIState_()
        , runtimeUIState_()
        , midiProcessors_()
        , timestampProvider_(std::move(timestampProvider))
        , sampleCounter_(0)
        , midiProcessorsRebuiltCallback_()
        , midiProcessorsWillRebuildCallback_() {
        manager_.SetParameterMessageOutBus(&parameterMessageOutBus_);
        uiBus_.SetGridManager(&gridManager_);
        midiBus_.SetGridManager(&gridManager_);
        if constexpr (HasMidiCatalog<App>) {
            midiCatalog_ = app_.MidiCatalog();
            const bool forwardPress = !midiCatalog_.encoderPressAction.empty();
            uiBus_.SetAppActionOut(&parameterMessageOutBus_, forwardPress);
            midiBus_.SetAppActionOut(&parameterMessageOutBus_, forwardPress);
        }
        patchManager_.SetBuses(&patchInputBus_, &patchOutputBus_);
        serializationContext_.arena = &serializationArena_;
        serializationContext_.initialArenaCapacity = initialArenaCapacity;

        context_.parameterManager = &manager_;
        context_.patchManager = &patchManager_;
        context_.uiBus = &uiBus_;
        context_.midiBus = &midiBus_;
        context_.parameterMessageOutBus = &parameterMessageOutBus_;
        context_.patchInputBus = &patchInputBus_;
        context_.patchOutputBus = &patchOutputBus_;
        context_.midiSender = &midiSender_;
        context_.masterClock = &masterClock_;
        context_.instrument = &instrumentConfig_;
        context_.defaultInstrument = &defaultInstrumentConfig_;
        context_.config = &config_;
        context_.uiState = nullptr;
        context_.gridManager = &gridManager_;
        context_.now = timestampProvider_;
        masterClock_.SetScheduledMidiEventSink(&midiSender_);
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    // Full pre-audio lifecycle (sar-5, binding order):
    //   1. store config_ = App::Config() after ValidateRuntimeConfig rejects
    //      negative input counts before any other engine state mutation
    //   2. wire context (constructor already wired the stable pointers; config_
    //      is filled in here since it depends on the application)
    //   3. AsyncLogQueue::s_instance.SetSampleCounterSource(&sampleCounter_)
    //   4. app_.Init(&context_)                    -- context.uiState is null here
    //   4a. snapshot defaultInstrumentConfig_ = instrumentConfig_, and
    //       defaultAudioDeviceState_ = audioDeviceState_ (the app's
    //       Init-configured live instrument/audio device becomes the
    //       default that revert/new-patch restore to)
    //   4b. load runtime config from dataPaths_.configFile when present.
    //       Valid config replaces the live MIDI instrument/audio selection
    //       and installs sync policy before MIDI processors, clock prepare,
    //       or host startup reconciliation exist; missing/invalid config
    //       preserves the app defaults above.
    //   5. manager_.CaptureDefaultControlState()
    //   6. allocate parameter/grid UI state, publish initial grid state, bind
    //      the stable RuntimeUIState facade, and expose only its parameter
    //      member through context_.uiState
    //   7. RebuildMidiProcessors() with the stable facade (silent: this first,
    //      pre-startup-patch
    //      rebuild never invokes midiProcessorsRebuiltCallback_, since there
    //      is nothing new for a host to react to yet)
    //   8. startup patch: find LatestPatchDirectory(dataPaths_.patchesRoot); if
    //      found, patchManager_.LoadPatch(dir), then ApplyPendingPatchMessages()
    //      (drains patchInputBus_ synchronously). Patch files contain
    //      synthesizer parameter values only; MIDI/audio configuration is loaded
    //      separately from runtime config, so startup patch load does not rebuild
    //      MIDI processors or fire host reopen/audio callbacks. Finally
    //      patchManager_.ProcessResponses(). A missing/empty patchesRoot, or a
    //      startup patch that fails to apply, is skipped silently.
    void Initialize() {
        RuntimeConfig config = App::Config();
        ValidateRuntimeConfig(config);
        config_ = std::move(config);
        context_.config = &config_;

        AsyncLogQueue::s_instance.SetSampleCounterSource(&sampleCounter_);

        app_.Init(&context_);

        // Snapshot the app's Init-configured live instrument/audio device as
        // the default BEFORE any startup patch applies. Without this,
        // defaultInstrumentConfig_/defaultAudioDeviceState_ stay
        // default-constructed (empty), so a later RevertAllToDefault (via
        // NewPatch()/RevertPatch() with no saved patch) would reset MIDI
        // routing/audio device selection to empty instead of back to the
        // app's real default — mirroring the old miniapp's post-construction
        // `defaultMidiProfileConfig_ = midiProfileConfig_;` snapshot (the
        // pre-instrument-model predecessors of defaultInstrumentConfig_/
        // instrumentConfig_; see projects/synth/miniapp/Main.cpp history).
        defaultInstrumentConfig_ = instrumentConfig_;
        {
            // Pre-audio, single-threaded (no audio/message-thread
            // concurrency exists yet): the lock here is uncontended, but
            // held anyway for uniformity with every other touch point of
            // audioDeviceState_/lastNotifiedAudioDeviceState_ (see
            // audioDeviceStateMutex_'s doc comment).
            const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
            defaultAudioDeviceState_ = audioDeviceState_;
            // Seed the host-visible audio-device shadow from the same post-Init
            // snapshot. Runtime configuration loading may update this later;
            // patch files are parameter-only.
            lastNotifiedAudioDeviceState_ = audioDeviceState_;
        }

        LoadRuntimeConfiguration();

        manager_.CaptureDefaultControlState();
        uiState_ = manager_.CreateUIState();
        gridUIState_ = gridManager_.CreateUIState();
        gridManager_.PopulateUIState(*gridUIState_);
        runtimeUIState_.parameters = uiState_.get();
        runtimeUIState_.grids = gridUIState_.get();
        context_.uiState = runtimeUIState_.parameters;

        RebuildMidiProcessors();

        const std::optional<std::filesystem::path> patchDir = LatestPatchDirectory(dataPaths_.patchesRoot);
        if (patchDir.has_value()) {
            patchManager_.LoadPatch(*patchDir);
            ApplyPendingPatchMessages();
            patchManager_.ProcessResponses();
        }
    }

    // Stores negotiated output-audio values, prepares the MasterClock first,
    // computes the UI-state publish-throttle interval (in blocks), and then
    // forwards to the application's PrepareToPlay hook when present.
    // uiPublishInterval_ = max(1, round(sampleRate / (uiFrameHz * blockSize)));
    // before Prepare runs, the default of 1 (publish every block) applies.
    void Prepare(double sampleRate, int blockSize) {
        sampleRate_ = sampleRate;
        blockSize_ = blockSize;

        if (sampleRate > 0.0 && blockSize > 0) {
            (void)masterClock_.Prepare(sampleRate, static_cast<std::size_t>(blockSize));
        }
        PublishClockDiagnostics();

        const int uiFrameHz = config_.uiFrameHz > 0 ? config_.uiFrameHz : 30;
        if (sampleRate > 0.0 && blockSize > 0) {
            const long computed =
                std::lround(sampleRate / (static_cast<double>(uiFrameHz) * static_cast<double>(blockSize)));
            uiPublishInterval_ = static_cast<int>(std::max<long>(1, computed));
        } else {
            uiPublishInterval_ = 1;
        }
        blocksSinceUiPublish_ = 0;

        if constexpr (HasPrepareToPlay<App>) {
            app_.PrepareToPlay(sampleRate, blockSize);
        }
    }

    // Audio-thread block pump (sar-6, binding order):
    //   1. patch-drain phase (drain barrier): if a message is stashed in
    //      pendingPatchMessage_ AND arenaGrowPending_ is still set, the
    //      arena has not been grown yet — skip draining patchInputBus_
    //      entirely this block (never lose the stash, never reorder a
    //      newer message ahead of it). If a message is stashed but
    //      arenaGrowPending_ has been cleared (MessageThreadTick grew the
    //      arena), retry the stashed message FIRST: on success clear the
    //      stash and fall through to draining patchInputBus_ normally; on
    //      ArenaExhausted again, re-stash/re-set the flag and stop (skip
    //      draining new messages this block too). Otherwise (no stash),
    //      drain patchInputBus_ via ApplyPatchMessage using the engine
    //      serialization context; Applied/Reverted patch messages change
    //      synthesizer parameter values only. ArenaExhausted stashes the popped
    //      message in pendingPatchMessage_, sets arenaGrowPending_, and stops
    //      draining for this block (never grows the arena on the audio path).
    //   2. drain due UI messages, then due MIDI messages. Apply ordinary
    //      parameter/grid messages immediately; insert clock/transport into
    //      one fixed-capacity ordered batch. Its key is timestamp, Internal
    //      before ExternalMidi, external slot ascending, then stable drain
    //      order. Overflow retains the earliest messages and increments an
    //      observable newest-drop counter.
    //   3. route that ordered realtime batch into MasterClock. External
    //      receive gating/source ownership stays MasterClock policy; Internal
    //      transport bypasses external receive gates.
    //   4. sampleCounter_.fetch_add(block.numFrames, relaxed), store the
    //      returned pre-increment value as block.startSample, commit exactly
    //      one MasterClock plan, enqueue its analytical crossings through the
    //      injected sink, and publish CurrentPlan() as block.clockPlan. A
    //      rejected commit leaves block.clockPlan null under AudioBlock's
    //      documented contract; app delegation still occurs exactly once.
    //   5. if the app opts in via the HasProcessFrame concept, app_.ProcessFrame()
    //      exactly once: the optional once-per-block control-rate hook. Runs
    //      after message routing and clock commit (so it observes the exact
    //      current plan) and before app_.ProcessBlock.
    //   6. app_.ProcessBlock(block) exactly once
    //   7. throttled PopulateUIState every uiPublishInterval_ blocks
    void ProcessBlock(AudioBlock& block, std::uint64_t timestamp) {
        const std::uint64_t requestedSyncWord =
            requestedSyncWord_.load(std::memory_order_acquire);
        if (requestedSyncWord != appliedSyncWord_) {
            const SyncConfig requested = DecodeSyncConfiguration(requestedSyncWord);
            const bool applied = masterClock_.ApplySyncConfig(requested);
            assert(applied);
            if (applied) {
                appliedSyncWord_ = requestedSyncWord;
            }
        }

        if (pendingPatchMessage_.has_value()) {
            if (arenaGrowPending_.load(std::memory_order_acquire)) {
                // Barrier still up: MessageThreadTick has not grown the
                // arena yet. Skip the entire patch-drain phase this block so
                // no newer message can apply ahead of the stash and nothing
                // overwrites it.
            } else {
                // Barrier cleared: the tick grew the arena. Retry the
                // stashed message first, before draining anything new.
                PatchMessageIn stashed = std::move(*pendingPatchMessage_);
                pendingPatchMessage_.reset();
                PatchApplyStatus retryStatus;
                std::optional<MidiInstrumentConfig> loadedInstrument;
                {
                    // Patch-message application is a rare, user-initiated
                    // event within the sanctioned patch-boundary non-RT
                    // exception (ApplyPatchMessage may already allocate on
                    // this path -- see LogPatchApplyOutcome's doc comment);
                    // locking here does not touch the steady-state pump
                    // path, since this branch only runs while retrying a
                    // stashed message. See audioDeviceStateMutex_'s doc
                    // comment.
                    const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
                    retryStatus = ApplyPatchMessageAndNotifyApp(stashed, manager_, instrumentConfig_, defaultInstrumentConfig_,
                                                    audioDeviceState_, defaultAudioDeviceState_, patchOutputBus_,
                                                    serializationContext_, midiCatalog_.patchCarriesMappings,
                                                    midiCatalog_.patchCarriesMappings ? &loadedInstrument : nullptr);
                }
                StashLoadedPatchInstrument(loadedInstrument);
                LogPatchApplyOutcome(stashed, retryStatus);
                if (retryStatus == PatchApplyStatus::Applied || retryStatus == PatchApplyStatus::Reverted) {
                    DrainPatchInputBus();
                } else if (retryStatus == PatchApplyStatus::ArenaExhausted) {
                    pendingPatchMessage_ = std::move(stashed);
                    arenaGrowPending_.store(true, std::memory_order_release);
                } else {
                    // Serialized/InvalidJSON/OutputQueueFull are terminal for
                    // this message; continue draining any newer messages.
                    DrainPatchInputBus();
                }
            }
        } else {
            DrainPatchInputBus();
        }

        realtimeBatchSize_ = 0;
        DrainMessageBus(uiBus_, timestamp);
        DrainMessageBus(midiBus_, timestamp);
        RouteRealtimeBatch();
        const std::uint64_t blockStartSample =
            sampleCounter_.fetch_add(block.numFrames, std::memory_order_relaxed);
        block.startSample = blockStartSample;
        block.clockPlan = masterClock_.CommitBlock(blockStartSample, block.numFrames, timestamp);
        PublishClockDiagnostics();
        if constexpr (HasProcessFrame<App>) {
            app_.ProcessFrame();
        }
        assert(block.numRequestedInputChannels == config_.numAudioInputs);
        app_.ProcessBlock(block);

        if (++blocksSinceUiPublish_ >= uiPublishInterval_) {
            blocksSinceUiPublish_ = 0;
            // ui-state-before-audio claim machine (design "Mechanism",
            // PINNED). Audio never waits and never spins: once latched, this
            // is exactly today's unsynchronized populate; before the latch,
            // a failed CAS just skips this one throttled window (display-
            // grade, sub-millisecond claim, re-audit accepted this as
            // non-blocking — design "Risks").
            if (audioOwnsUiState_) {
                // Latched permanently: no synchronization, identical to the
                // pre-change behavior.
                if (uiState_ != nullptr) {
                    manager_.PopulateUIState(*uiState_);
                }
                if (gridUIState_ != nullptr) {
                    gridManager_.PopulateUIState(*gridUIState_);
                }
            } else {
                UiStatePublisher expected = UiStatePublisher::Quiescent;
                // acq_rel on success (design: "acquire/release on the
                // CAS/store pair"): acquire synchronizes-with the message
                // thread's release store of Quiescent below, so this
                // thread never claims while a message-thread populate is
                // still in flight for a stale Quiescent it hasn't observed
                // yet; release publishes the transition to AudioThread so
                // MessageThreadTick's own CAS (relaxed-failure load of this
                // same atomic) is guaranteed to observe it on or before its
                // very next attempt, making the latch's "never again"
                // property visible promptly rather than merely eventually.
                // relaxed on failure: a failed CAS carries no buffer
                // access here, so no ordering is needed for it.
                if (uiStatePublisher_.compare_exchange_strong(expected, UiStatePublisher::AudioThread,
                                                               std::memory_order_acq_rel,
                                                               std::memory_order_relaxed)) {
                    if (uiState_ != nullptr) {
                        manager_.PopulateUIState(*uiState_);
                    }
                    if (gridUIState_ != nullptr) {
                        gridManager_.PopulateUIState(*gridUIState_);
                    }
                    // One-way latch: uiStatePublisher_ stays AudioThread
                    // forever (never stored back to Quiescent from here),
                    // so no later message-thread CAS out of Quiescent can
                    // ever succeed again (design: "writer sets are disjoint
                    // at every instant by construction").
                    audioOwnsUiState_ = true;
                } else {
                    // CAS failure: the message thread is mid-populate this
                    // tick (uiStatePublisher_ == MessageThread). Skip this
                    // publish window entirely — no wait, no retry-spin,
                    // retried automatically at the next throttle window
                    // (design: "audio NEVER waits, never spins, never
                    // locks").
                }
            }
        }
    }

    // Task 5: message-thread pump. Binding order:
    //   1. parameter storage-batch replies — drain parameterMessageOutBus_
    //      and reply to each ParameterStorageBatchNeeded request, mirroring
    //      the miniapp's processParameterMessages pattern exactly.
    //   2. arena grow (see the tick contract note on GrowSerializationArenaForTick):
    //      MessageThreadTick grows the arena and clears arenaGrowPending_
    //      ONLY (GrowSerializationArenaForTick clears the flag itself, in
    //      both the ordinary-growth and drop-at-cap cases). It must NOT
    //      touch pendingPatchMessage_ (except the documented drop-at-cap
    //      carve-out) and must NOT re-push anything onto patchInputBus_ —
    //      ProcessBlock alone owns retrying/clearing the stash, on the
    //      audio thread, once it observes arenaGrowPending_ cleared.
    //   3. patchManager_.ProcessResponses()
    //   4. each processor in every slot of midiProcessors_'s outputs: Process()
    //      (per-controller rebuild, Task 2: midiProcessors_ is now one
    //      MidiControllerProfileResult per controller slot; every slot's
    //      outputs still get exactly one Process() call per tick).
    void MessageThreadTick() {
        ParameterMessageOut parameterMessage;
        while (parameterMessageOutBus_.Pop(parameterMessage)) {
            // App actions and forwarded encoder presses arrive here from the
            // audio thread and are dispatched to the app's surface on this
            // thread.
            if constexpr (HasMidiCatalog<App>) {
                if (parameterMessage.type == ParameterMessageOut::Type::AppAction) {
                    if (parameterMessage.appActionIx < midiCatalog_.actions.size()) {
                        const MidiAppAction& action = midiCatalog_.actions[parameterMessage.appActionIx];
                        std::string value = action.value;
                        if (action.analogRange.has_value()) {
                            const float min = action.analogRange->first;
                            const float max = action.analogRange->second;
                            value = std::to_string(min + parameterMessage.value * (max - min));
                        }
                        ui::Action dispatched = ui::Action::WithValue(action.action, value);
                        app_.PortableSurface().DispatchAction(dispatched);
                    }
                    continue;
                }
                if (parameterMessage.type == ParameterMessageOut::Type::AppEncoderPress) {
                    ui::Action dispatched = ui::Action::WithValue(midiCatalog_.encoderPressAction,
                                                                    std::to_string(parameterMessage.position));
                    app_.PortableSurface().DispatchAction(dispatched);
                    continue;
                }
            }
            if (parameterMessage.type != ParameterMessageOut::Type::ParameterStorageBatchNeeded ||
                parameterMessage.group == nullptr) {
                continue;
            }
            // slog-7: INFO-log storage-batch provisioning (group pointer +
            // requested count) so a session log shows when/how often groups
            // are reinforced with additional parameter storage.
            INFO("MessageThreadTick: provisioning storage batch for group %p (requested=%zu)",
                 static_cast<const void*>(parameterMessage.group), parameterMessage.requestedParameters);
            parameterMessage.group->AddParameterStorageBatch(MakeParameterStorageBatch(
                parameterMessage.group->Config(), parameterMessage.group->GestureCount(),
                parameterMessage.requestedParameters));
        }

        if (arenaGrowPending_.load(std::memory_order_acquire)) {
            GrowSerializationArenaForTick();
        }

        const PatchCommandResult patchResult = patchManager_.ProcessResponses();
        if (patchResult.status != PatchCommandStatus::NoCompletion) {
            lastTickPatchResult_ = patchResult;
            // slog-7: INFO-log non-NoCompletion patch command results (status
            // name + path) so patch save/load/revert activity is visible in
            // the session log.
            INFO("MessageThreadTick: patch command result status=%s path=%s",
                 PatchCommandStatusName(patchResult.status), patchResult.path.string().c_str());
        }

        // A loaded patch's midiInstrument section (staged by the patch
        // drain through StashLoadedPatchInstrument, only ever set for an app
        // whose catalog declares patchCarriesMappings) replaces the live
        // instrument here, on the message thread, through EditInstrument --
        // never applied by the patch-apply call itself.
        std::optional<MidiInstrumentConfig> instrumentToApply;
        {
            const std::lock_guard<std::mutex> lock(pendingPatchInstrumentMutex_);
            instrumentToApply = std::move(pendingPatchInstrument_);
            pendingPatchInstrument_.reset();
        }
        if (instrumentToApply.has_value()) {
            EditInstrument([&instrumentToApply](MidiInstrumentConfig& live) {
                live = std::move(*instrumentToApply);
            });
        }

        for (MidiControllerProfileResult& processors : midiProcessors_) {
            for (auto& output : processors.outputs) {
                output->Process();
            }
        }

        // ui-state-before-audio (design "Mechanism", PINNED): after every
        // other MessageThreadTick duty, attempt to claim uiState_/
        // gridUIState_ publication for this tick so parameter/encoder UI
        // state is populated even though the audio pump (ProcessBlock) may
        // never have run yet. Only Quiescent (never-claimed, or the audio
        // pump has not latched permanently and no message-thread populate
        // is currently in flight) admits a successful CAS; once the audio
        // pump claims even once, this CAS can never succeed again (one-way
        // latch owned by ProcessBlock's publish site), so message-thread
        // population ends permanently at that point (design: "message-
        // thread population ends permanently the moment audio first
        // publishes").
        UiStatePublisher expected = UiStatePublisher::Quiescent;
        // acq_rel on success (design: "acquire/release on the CAS/store
        // pair"): acquire synchronizes-with a prior release store of
        // Quiescent from an earlier message-thread tick (or the initial
        // value), and release publishes this thread's claim to the audio
        // thread's next CAS attempt so it observes MessageThread promptly
        // and skips its publish window rather than racing the populate
        // below. relaxed on failure: nothing here depends on ordering
        // relative to a failed attempt (this thread does nothing further
        // this tick either way).
        if (uiStatePublisher_.compare_exchange_strong(expected, UiStatePublisher::MessageThread,
                                                       std::memory_order_acq_rel, std::memory_order_relaxed)) {
            // Null-check both buffers exactly as ProcessBlock's publish
            // site does (design 1.1 trace obligation: mirror
            // Engine.hpp:433/:436) — Initialize() may not have run yet, so
            // uiState_/gridUIState_ can still be nullptr here.
            if (uiState_ != nullptr) {
                manager_.PopulateUIState(*uiState_);
            }
            if (gridUIState_ != nullptr) {
                gridManager_.PopulateUIState(*gridUIState_);
            }
            // release: hands the claim back to Quiescent so a later tick
            // (this thread) or the audio thread's next publish window can
            // claim next; synchronizes-with the acquire half of whichever
            // CAS observes Quiescent next.
            uiStatePublisher_.store(UiStatePublisher::Quiescent, std::memory_order_release);
        }
        // CAS failure (state is AudioThread — latched permanently — or, in
        // the sub-millisecond window a prior tick's own populate could
        // still be settling, MessageThread): do nothing this tick, per
        // design ("CAS failure or non-Quiescent load -> do nothing this
        // tick").
    }

    App& Application() { return app_; }
    const MidiAppCatalog& MidiCatalog() const { return midiCatalog_; }
    AppContext& Context() { return context_; }
    ParameterManager& Manager() { return manager_; }
    GridManager& GridManagerForTest() { return gridManager_; }
    const RuntimeUIState& RuntimeUIStateForTest() const { return runtimeUIState_; }
    MessageInBus& UiBus() { return uiBus_; }
    MessageInBus& MidiBus() { return midiBus_; }
    PatchManager& Patches() { return patchManager_; }
    MasterClock& Clock() { return masterClock_; }
    const MasterClock& Clock() const { return masterClock_; }
    bool RequestSyncConfiguration(const SyncConfig& config) noexcept {
        if (!config.IsValid()) {
            return false;
        }
        requestedSyncWord_.store(EncodeSyncConfiguration(config), std::memory_order_release);
        return true;
    }
    SyncConfig SyncConfigurationSnapshot() const noexcept {
        return DecodeSyncConfiguration(requestedSyncWord_.load(std::memory_order_acquire));
    }
    ClockDiagnostics ClockDiagnosticsSnapshot() const noexcept {
        return clockDiagnosticsPublication_.Snapshot();
    }
    void SetScheduledMidiEventSink(IScheduledMidiEventSink* sink) noexcept {
        masterClock_.SetScheduledMidiEventSink(sink);
    }
    std::uint64_t DroppedRealtimeInputCount() const noexcept {
        return droppedRealtimeInputCount_.load(std::memory_order_relaxed);
    }

    static constexpr std::size_t kRealtimeBatchCapacity = 256;

    // Must be called before Initialize(); startup patch/config discovery reads
    // these paths during initialization.
    void SetRuntimeDataPaths(RuntimeDataPaths paths) { dataPaths_ = std::move(paths); }
    const RuntimeDataPaths& DataPaths() const { return dataPaths_; }

    // Startup-only: call before audio/message-thread editing begins so the
    // file IO window cannot overwrite a concurrent configuration edit.
    RuntimeConfigFileStatus LoadRuntimeConfiguration() {
        MidiInstrumentConfig loadedInstrument;
        AudioDeviceState loadedAudioDevice;
        SyncConfig loadedSync = SyncConfigurationSnapshot();
        {
            const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
            loadedInstrument = instrumentConfig_;
            loadedAudioDevice = audioDeviceState_;
        }

        RuntimeConfigFileStatus status =
            LoadRuntimeConfigFile(dataPaths_.configFile, loadedInstrument, loadedAudioDevice, loadedSync);
        if (status == RuntimeConfigFileStatus::Ok) {
            if (!masterClock_.ApplySyncConfig(loadedSync)) {
                status = RuntimeConfigFileStatus::Invalid;
            } else {
                const std::uint64_t loadedWord = EncodeSyncConfiguration(loadedSync);
                requestedSyncWord_.store(loadedWord, std::memory_order_release);
                appliedSyncWord_ = loadedWord;
                const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
                instrumentConfig_ = std::move(loadedInstrument);
                audioDeviceState_ = loadedAudioDevice;
                lastNotifiedAudioDeviceState_ = loadedAudioDevice;
            }
        }
        const std::string path = dataPaths_.configFile.string();
        INFO("Runtime config load status=%s path=%s", RuntimeConfigFileStatusName(status), path.c_str());
        PublishClockDiagnostics();
        return status;
    }

    RuntimeConfigFileStatus SaveRuntimeConfiguration() const {
        MidiInstrumentConfig instrument;
        AudioDeviceState audioDevice;
        {
            const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
            instrument = instrumentConfig_;
            audioDevice = audioDeviceState_;
        }

        const RuntimeConfigFileStatus status =
            SaveRuntimeConfigFile(dataPaths_.configFile, instrument, audioDevice,
                                  SyncConfigurationSnapshot());
        const std::string path = dataPaths_.configFile.string();
        INFO("Runtime config save status=%s path=%s", RuntimeConfigFileStatusName(status), path.c_str());
        return status;
    }

    // Number of per-controller processor chains currently built --
    // == LiveInstrument().controllers.size() at the time of the last
    // RebuildMidiProcessors() call (the two stay in lockstep: every rebuild
    // resizes midiProcessors_ to match the snapshotted controller count).
    std::size_t MidiControllerCount() const { return midiProcessors_.size(); }

    // Input processor chain head for controller slot controllerIx, or nullptr
    // when controllerIx is out of range (>= MidiControllerCount()). Active
    // slots always include the terminal realtime MIDI processor; Blacklisted
    // slots contain only an explicit drop processor.
    MidiInProcessor* MidiInputProcessor(std::size_t controllerIx) {
        if (controllerIx >= midiProcessors_.size()) {
            return nullptr;
        }
        return midiProcessors_[controllerIx].input.get();
    }

    // Unlocked reference to the live instrument (smi-8). LEGAL ONLY: (1)
    // pre-audio initialization -- single-threaded app/engine setup before
    // Initialize() has started any concurrent activity (e.g. the app's own
    // Init() populating its seed instrument); or (2) while the caller already
    // holds audioDeviceStateMutex_ (e.g. inside EditInstrument's own lambda,
    // or a future engine-internal caller taking the lock directly). The audio
    // thread does not mutate instrumentConfig_; the lock is still the boundary
    // for future engine-internal callers that need a coherent instrument
    // snapshot while audio may be live. Any running-state reader that does not
    // already hold the lock must go through InstrumentSnapshot() instead of this
    // accessor -- see that method's doc
    // comment (Task 4 review, Critical: an earlier UI-side endpoint reader --
    // the single-slot MidiPanel component this file's ControllersPage
    // replaced -- read this unlocked from message-thread paths concurrent
    // with running audio, repeating the same race class
    // RebuildMidiProcessors() was fixed for).
    // Test-support code driving the engine single-threaded (e.g.
    // tests/support/SynthRig.hpp's InstallInstrumentForTest, and
    // engine_tests.cpp's direct pokes) has no concurrent audio thread to race
    // and may keep reading/writing through this reference directly.
    MidiInstrumentConfig& LiveInstrument() { return instrumentConfig_; }

    // Message-thread read of the live instrument, safe to call concurrently
    // with future locked engine-internal updates: returns a locked deep copy of
    // instrumentConfig_ rather than a reference into it. This is the
    // running-state counterpart of LiveInstrument() -- use this (not
    // LiveInstrument()) from any message-thread path that can run while audio
    // is live and does not already hold audioDeviceStateMutex_ (e.g.
    // ControllersPageSurface's per-tick RefreshOnTick(), which rebuilds
    // synth::MidiConfigViewModel from a fresh snapshot). Mirrors
    // AudioDeviceSnapshot()'s pattern
    // for audioDeviceState_.
    MidiInstrumentConfig InstrumentSnapshot() const {
        const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
        return instrumentConfig_;
    }

    // Post-Init() snapshot (see Initialize()'s binding-order comment, step
    // 4a): the instrument revert/new-patch restores. Immutable after
    // Initialize() returns.
    const MidiInstrumentConfig& DefaultInstrument() const { return defaultInstrumentConfig_; }

    // Serialized edit entry point (smi-8): applies `edit` to the live
    // instrument under the same lock used for coherent snapshots of instrument
    // and audio-device state. Message-thread only (mirrors
    // MidiControllerProfileConfig's old message-thread-only
    // contract). After the edit is applied, this rebuilds the MIDI
    // processors and fires midiProcessorsRebuiltCallback_ (if set) — the
    // same rebuilt-callback path RebuildMidiProcessors()'s callers already
    // use — so a host reacts to an edited instrument (e.g. re-opening
    // endpoints) after host-initiated instrument edits.
    void EditInstrument(const std::function<void(MidiInstrumentConfig&)>& edit) {
        if (!edit) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
            edit(instrumentConfig_);
        }
        RebuildMidiProcessors();
        if (midiProcessorsRebuiltCallback_) {
            midiProcessorsRebuiltCallback_();
        }
    }

    void SetMidiProcessorsRebuiltCallback(std::function<void()> callback) {
        midiProcessorsRebuiltCallback_ = std::move(callback);
    }
    // Reserved for host notification when engine-owned runtime configuration
    // changes audioDeviceState_ without being initiated by the host. Patch
    // files are parameter-only and never fire this callback.
    void SetAudioDeviceChangedCallback(std::function<void()> callback) {
        audioDeviceChangedCallback_ = std::move(callback);
    }
    // Host lifecycle hook: gives the host a chance to detach any external
    // pointers into the current MIDI processor chain (e.g. device-callback
    // forwarding targets) before the chain is destroyed. Called on the
    // thread performing the rebuild.
    void SetMidiProcessorsWillRebuildCallback(std::function<void()> callback) {
        midiProcessorsWillRebuildCallback_ = std::move(callback);
    }
    // Message-thread only: iterates controller slot controllerIx's outputs
    // calling Reset() on each, clearing ONLY that controller's output caches
    // (a no-op when controllerIx is out of range). Forces a full LED/value
    // resync on that controller's MIDI output hardware (e.g. after
    // opening/reopening an output device) without disturbing any other
    // controller's already-warm caches. Must not be called from the audio
    // thread — midiProcessors_ is only ever replaced on the thread performing
    // a rebuild (Initialize()/MessageThreadTick(), both message-thread-only),
    // so this has no synchronization of its own.
    void ResetMidiOutputProcessors(std::size_t controllerIx) {
        if (controllerIx >= midiProcessors_.size()) {
            return;
        }
        for (auto& output : midiProcessors_[controllerIx].outputs) {
            output->Reset();
        }
    }
    // Host API, message-thread only: records a host-initiated audio device
    // change (e.g. a UI combo selection) into BOTH the live state and the
    // last-notified shadow under audioDeviceStateMutex_. Host-initiated changes
    // are by definition already known to the host that just made them, so this
    // deliberately does NOT invoke audioDeviceChangedCallback_. Replaces the old
    // mutable AudioDevice() accessor, which let a host write audioDeviceState_
    // directly without advancing the shadow or taking the lock.
    void SetAudioDeviceFromHost(const AudioDeviceState& state) {
        const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
        audioDeviceState_ = state;
        lastNotifiedAudioDeviceState_ = state;
    }

    // Host API: returns a locked copy of the current audio device state.
    // Safe to call from the message thread at any time (including
    // concurrently with the audio thread's patch drain, which is exactly
    // what the lock is for). Replaces host-side reads of the old mutable
    // AudioDevice() accessor.
    AudioDeviceState AudioDeviceSnapshot() const {
        const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
        return audioDeviceState_;
    }

    const RuntimeConfig& Config() const { return config_; }
    std::uint64_t SampleCount() const { return sampleCounter_.load(std::memory_order_relaxed); }

    // Test-only accessors for the ProcessBlock drain-barrier state
    // (pendingPatchMessage_/arenaGrowPending_). PatchManager::HasPendingSave()
    // is not a substitute: it reflects PatchManager's own dispatch-time
    // bookkeeping (reset as soon as a new patch command is enqueued, e.g. by
    // RevertPatch()/NewPatch()), not whether the engine's drain has actually
    // applied the queued message yet. Exposed so tests can observe the
    // barrier directly without depending on that unrelated bookkeeping.
    bool HasStashedPatchMessageForTest() const { return pendingPatchMessage_.has_value(); }
    bool IsArenaGrowPendingForTest() const { return arenaGrowPending_.load(std::memory_order_acquire); }

    // Test-only accessors/hooks for the ui-state-before-audio claim machine
    // (design's "Mechanism" section, pinned): a single-slot lock-free CAS
    // claim over uiState_/gridUIState_ publication between the message
    // thread (MessageThreadTick) and the audio thread (ProcessBlock's
    // throttled publish site), with a one-way latch once audio ever
    // publishes. These accessors read/drive uiStatePublisher_ directly so
    // single-threaded test code can force the CAS-collision and post-latch
    // states deterministically (design's Testing section: "the harness
    // controls interleaving at the primitive's own seam"). Never called from
    // production code / the audio or message-thread paths themselves.
    bool UiStatePublisherIsQuiescentForTest() const {
        return uiStatePublisher_.load(std::memory_order_acquire) == UiStatePublisher::Quiescent;
    }
    bool UiStatePublisherIsMessageThreadForTest() const {
        return uiStatePublisher_.load(std::memory_order_acquire) == UiStatePublisher::MessageThread;
    }
    bool UiStatePublisherIsAudioThreadForTest() const {
        return uiStatePublisher_.load(std::memory_order_acquire) == UiStatePublisher::AudioThread;
    }
    bool AudioOwnsUiStateForTest() const { return audioOwnsUiState_; }
    // Forces the claim into MessageThread state without running a populate,
    // simulating "message thread mid-populate" so a test can drive the audio
    // side into the CAS-failure/skip branch on demand (design Testing,
    // assertion (a)). Only valid pre-latch (production code never leaves the
    // claim parked here across calls).
    void HoldUiStatePublisherAsMessageThreadForTest() {
        uiStatePublisher_.store(UiStatePublisher::MessageThread, std::memory_order_release);
    }
    // Releases a test-forced hold back to Quiescent, letting the next
    // audio-thread publish opportunity attempt its CAS normally (design
    // Testing, assertion (b): "claims and latches at the next window").
    void ReleaseUiStatePublisherHoldForTest() {
        uiStatePublisher_.store(UiStatePublisher::Quiescent, std::memory_order_release);
    }

    // Public host API: rebuild midiProcessors_ from the current
    // instrumentConfig_ on demand (e.g. after a host mutates it via
    // EditInstrument, such as ControllersPageSurface adding/editing a
    // controller). Runs midiProcessorsWillRebuildCallback_ (if set)
    // synchronously, BEFORE the current midiProcessors_ chains are
    // destroyed/replaced, then constructs one fresh chain per controller slot
    // via CreateMidiControllerProfile against midiBus_/runtimeUIState_.
    // Initialize() binds both snapshot pointers before this first rebuild;
    // the profile factory still tolerates null facade members. This is
    // the single call site for the midiProcessors_ assignment, so it covers
    // every rebuild: Initialize()'s silent first rebuild and any host-initiated
    // rebuild (e.g. adding a controller). Does NOT itself invoke
    // midiProcessorsRebuiltCallback_; EditInstrument does that after applying
    // the edit (the path every current caller uses -- e.g. ControllersPage's
    // Commit(), MidiConnectionManager's UpdateRef()). A
    // caller rebuilding directly on the message thread outside EditInstrument
    // would need to invoke midiProcessorsRebuiltCallback_ itself, or
    // otherwise handle the endpoint-reopen consequences of a fresh chain, but
    // there is no such caller currently.
    //
    // Per-controller rebuild (Task 2): midiProcessors_ now holds one
    // MidiControllerProfileResult per controller slot, index-for-index with
    // instrumentConfig_.controllers. Slot i's output processors are built
    // with sink index i (CreateMidiControllerProfile's sinkIx parameter),
    // routing that slot's feedback onto MidiSender's sink i (see
    // MidiSender::SetSink/Enqueue's kMaxSinks routing) -- independent of
    // every other slot's sink, while ALL slots' input chains still feed the
    // SAME midiBus_ (the single MIDI input bus; sar-7/the plan's global
    // constraint is unchanged, only routing on the way OUT is per-slot). A
    // zero-controller instrument yields an empty midiProcessors_ (size 0),
    // matching the previous single-chain empty-profile case's "no
    // processors" behavior but via an empty vector instead of one
    // no-op-configured result.
    //
    // instrumentConfig_ is read under audioDeviceStateMutex_ (Task 3-style
    // review fix, Critical, preserved here): EditInstrument releases that
    // lock before calling this, and the audio-thread patch drain
    // (DrainPatchInputBus, the ProcessBlock stashed-message retry,
    // ApplyPendingPatchMessages) mutates instrumentConfig_ WHILE holding it --
    // see audioDeviceStateMutex_'s doc comment, which already lists
    // instrumentConfig_ among the members it guards. Reading
    // instrumentConfig_.controllers here without the lock would race that
    // drain. The lock is only held long enough to copy the WHOLE controllers
    // vector (every slot's config, not just one) into a local snapshot; the
    // actual processor construction via CreateMidiControllerProfile --
    // heavier now that it runs once per slot -- happens entirely outside the
    // lock, since that work is heavier than the sanctioned patch-boundary
    // non-RT window is meant to cover.
    void RebuildMidiProcessors() {
        if (midiProcessorsWillRebuildCallback_) {
            midiProcessorsWillRebuildCallback_();
        }
        std::vector<MidiControllerSlot> controllers;
        {
            const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
            controllers = instrumentConfig_.controllers;
        }
        std::vector<MidiControllerProfileResult> rebuilt;
        rebuilt.reserve(controllers.size());
        for (std::size_t ix = 0; ix < controllers.size(); ++ix) {
            if (controllers[ix].disposition == MidiControllerDisposition::Blacklisted) {
                rebuilt.push_back(CreateBlacklistedMidiControllerProfile());
                continue;
            }
            MidiControllerProfileConfig config = controllers[ix].config;
            if constexpr (HasMidiCatalog<App>) {
                ResolveAppActionsAgainstCatalog(config);
            }
            // Instrument controller slots are stable route identities, and
            // MidiSender uses the same slot ordinal as its sink index. Keep
            // both arguments explicit even while their values are equal.
            rebuilt.push_back(CreateMidiControllerProfile(config, &midiBus_, &midiSender_,
                                                           &runtimeUIState_, timestampProvider_, ix,
                                                           &absoluteFeedbackCoordinator_, ix,
                                                           controllers[ix].kind));
        }
        midiProcessors_ = std::move(rebuilt);
    }

    // Test-only alias for RebuildMidiProcessors(), kept for existing test
    // call sites (e.g. SynthRig::InstallInstrumentForTest). Prefer calling
    // RebuildMidiProcessors() directly in new code.
    void RebuildMidiProcessorsForTest() { RebuildMidiProcessors(); }

    // Rig/test support: last non-NoCompletion patch response observed by
    // MessageThreadTick. Reading clears it. The JUCE runtime shell reports
    // patch results through its own PatchManager calls and does not use this.
    std::optional<PatchCommandResult> ConsumeLastTickPatchResult() {
        std::optional<PatchCommandResult> result = std::move(lastTickPatchResult_);
        lastTickPatchResult_.reset();
        return result;
    }

    // Iterate immediate subdirectories of root; for each, LatestPatchVersion.
    // Select the directory whose latest version FILENAME is lexicographically
    // greatest; ties break on lexicographically greater directory name. No
    // candidates (or a non-existent root) yields std::nullopt.
    static std::optional<std::filesystem::path> LatestPatchDirectory(const std::filesystem::path& root) {
        std::error_code ec;
        if (root.empty() || !std::filesystem::is_directory(root, ec) || ec) {
            return std::nullopt;
        }

        std::optional<std::filesystem::path> bestDir;
        std::string bestVersionName;
        std::string bestDirName;

        for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
            if (ec) {
                break;
            }
            std::error_code isDirEc;
            if (!entry.is_directory(isDirEc) || isDirEc) {
                continue;
            }
            const auto version = LatestPatchVersion(entry.path());
            if (!version.has_value()) {
                continue;
            }
            const std::string versionName = version->filename().string();
            const std::string dirName = entry.path().filename().string();
            if (!bestDir.has_value() || versionName > bestVersionName ||
                (versionName == bestVersionName && dirName > bestDirName)) {
                bestDir = entry.path();
                bestVersionName = versionName;
                bestDirName = dirName;
            }
        }
        return bestDir;
    }

private:
    void PublishClockDiagnostics() noexcept {
        clockDiagnosticsPublication_.Publish(masterClock_.DiagnosticsSnapshot());
    }

    // Resolves every AppAction row of one controller's profile config
    // against midiCatalog_, called from RebuildMidiProcessors on a copy of
    // the slot's persisted config (never on instrumentConfig_ itself, so an
    // action the running catalog does not know about stays in the saved
    // instrument and comes back once a later app version adds it). A row
    // whose (action, value) the catalog has sets its resolved index; a row
    // it does not have is dropped from the copy, logged once by name.
    void ResolveAppActionsAgainstCatalog(MidiControllerProfileConfig& config) {
        for (auto it = config.systemMessages.begin(); it != config.systemMessages.end();) {
            if (it->press.type != MessageIn::Type::AppAction) {
                ++it;
                continue;
            }
            if (const auto ix = FindMidiAppAction(midiCatalog_, it->appAction, it->appActionValue)) {
                it->press.appActionIx = *ix;
                ++it;
            } else {
                INFO("RebuildMidiProcessors: dropping unresolved app action action=%s value=%s",
                     it->appAction.c_str(), it->appActionValue.c_str());
                it = config.systemMessages.erase(it);
            }
        }
        if (config.analogInput.has_value()) {
            std::vector<AnalogAppActionMapping>& appActions = config.analogInput->appActions;
            for (auto it = appActions.begin(); it != appActions.end();) {
                if (const auto ix = FindMidiAppAction(midiCatalog_, it->appAction, it->appActionValue)) {
                    it->appActionIx = *ix;
                    ++it;
                } else {
                    INFO("RebuildMidiProcessors: dropping unresolved app action action=%s value=%s",
                         it->appAction.c_str(), it->appActionValue.c_str());
                    it = appActions.erase(it);
                }
            }
        }
    }

    static bool IsRealtimeMessage(const MessageIn& message) noexcept {
        switch (message.type) {
        case MessageIn::Type::Start:
        case MessageIn::Type::Continue:
        case MessageIn::Type::Stop:
        case MessageIn::Type::Clock:
            return true;
        default:
            return false;
        }
    }

    static bool RealtimeMessageLess(const MessageIn& lhs, const MessageIn& rhs) noexcept {
        if (lhs.timestamp != rhs.timestamp) {
            return lhs.timestamp < rhs.timestamp;
        }
        if (lhs.origin != rhs.origin) {
            return lhs.origin == MessageIn::Origin::Internal;
        }
        if (lhs.origin == MessageIn::Origin::ExternalMidi &&
            lhs.externalControllerSlot != rhs.externalControllerSlot) {
            return lhs.externalControllerSlot < rhs.externalControllerSlot;
        }
        return false;
    }

    void InsertRealtimeMessage(const MessageIn& message) noexcept {
        if (realtimeBatchSize_ == kRealtimeBatchCapacity &&
            !RealtimeMessageLess(message, realtimeBatch_[realtimeBatchSize_ - 1])) {
            droppedRealtimeInputCount_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        std::size_t insertAt = realtimeBatchSize_;
        if (insertAt == kRealtimeBatchCapacity) {
            --insertAt;
            droppedRealtimeInputCount_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++realtimeBatchSize_;
        }
        while (insertAt > 0 && RealtimeMessageLess(message, realtimeBatch_[insertAt - 1])) {
            realtimeBatch_[insertAt] = realtimeBatch_[insertAt - 1];
            --insertAt;
        }
        realtimeBatch_[insertAt] = message;
    }

    void DrainMessageBus(MessageInBus& bus, std::uint64_t timestamp) noexcept {
        MessageIn message;
        while (bus.Pop(message, timestamp)) {
            if (IsRealtimeMessage(message)) {
                InsertRealtimeMessage(message);
            } else {
                bus.Apply(message);
            }
        }
    }

    void RouteRealtimeBatch() noexcept {
        for (std::size_t ix = 0; ix < realtimeBatchSize_; ++ix) {
            const MessageIn& message = realtimeBatch_[ix];
            if (message.type == MessageIn::Type::Clock) {
                if (message.origin == MessageIn::Origin::ExternalMidi) {
                    (void)masterClock_.HandleExternalClock(
                        message.timestamp, message.externalControllerSlot);
                }
                continue;
            }

            ClockTransportCommand command = ClockTransportCommand::Stop;
            switch (message.type) {
            case MessageIn::Type::Start:
                command = ClockTransportCommand::Start;
                break;
            case MessageIn::Type::Continue:
                command = ClockTransportCommand::Continue;
                break;
            case MessageIn::Type::Stop:
                command = ClockTransportCommand::Stop;
                break;
            default:
                continue;
            }
            if (message.origin == MessageIn::Origin::ExternalMidi) {
                (void)masterClock_.HandleExternalTransport(
                    command, message.timestamp, message.externalControllerSlot);
            } else {
                (void)masterClock_.HandleInternalTransport(command);
            }
        }
    }

    // Audio-thread drain loop shared by ProcessBlock's no-stash path and its
    // post-retry continuation. Drains patchInputBus_ via ApplyPatchMessage;
    // Applied/Reverted patch messages update parameter values only.
    // ArenaExhausted stashes the popped message in pendingPatchMessage_, sets
    // arenaGrowPending_, and stops draining for this block (never grows the
    // arena on the audio path — see the ArenaExhausted handling note above
    // ApplyPendingPatchMessages).
    //
    // audioDeviceStateMutex_ is acquired ONLY inside the loop body, after a
    // message has actually been popped -- never around the
    // patchInputBus_.Pop() call/loop condition itself. In steady state (no
    // pending patch messages) Pop() returns false immediately and the lock
    // is never touched, so this stays lock-free on the hot per-block path;
    // the lock is only ever taken within the rare, user-initiated
    // patch-message-application window (see audioDeviceStateMutex_'s doc
    // comment).
    void DrainPatchInputBus() {
        PatchMessageIn patchMessage;
        while (patchInputBus_.Pop(patchMessage)) {
            PatchApplyStatus status;
            std::optional<MidiInstrumentConfig> loadedInstrument;
            {
                const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
                status = ApplyPatchMessageAndNotifyApp(patchMessage, manager_, instrumentConfig_, defaultInstrumentConfig_,
                                           audioDeviceState_, defaultAudioDeviceState_, patchOutputBus_,
                                           serializationContext_, midiCatalog_.patchCarriesMappings,
                                           midiCatalog_.patchCarriesMappings ? &loadedInstrument : nullptr);
            }
            StashLoadedPatchInstrument(loadedInstrument);
            LogPatchApplyOutcome(patchMessage, status);
            if (status == PatchApplyStatus::ArenaExhausted) {
                pendingPatchMessage_ = std::move(patchMessage);
                arenaGrowPending_.store(true, std::memory_order_release);
                break;
            }
        }
    }

    // slog-7: INFO-log each ApplyPatchMessage outcome (message type + apply
    // status name) from the audio-thread patch drain (ProcessBlock's no-stash
    // drain loop and its stashed-message retry). This runs on the audio
    // thread, but the logger's producer path (AsyncLogQueue::Log) is
    // audio-safe (slog-3: no locks, no heap allocation, no IO), and patch
    // commands are rare/user-initiated, so the extra INFO call per message is
    // negligible relative to block-processing cost.
    void LogPatchApplyOutcome(const PatchMessageIn& message, PatchApplyStatus status) {
        INFO("ProcessBlock: patch message type=%s apply-status=%s", PatchMessageInTypeName(message.type),
             PatchApplyStatusName(status));
    }

    // MessageThreadTick's (Task 5) sole responsibility for the drain
    // barrier: grow serializationArena_ off the audio thread. Heap
    // allocation here is safe because this never runs on the audio thread.
    // Growth doubles the arena's current capacity, capped at
    // serializationContext_.maxArenaCapacity. In the ordinary case (still
    // under the cap) this must NOT touch pendingPatchMessage_ — see the
    // tick contract note on MessageThreadTick. The one carve-out: if the
    // arena is already at the cap (currentCapacity >= maxArenaCapacity),
    // growing further is pointless (the stash would just exhaust again
    // forever), so this drops the stashed message, clears both the stash
    // and arenaGrowPending_, and INFO-logs the failure instead of growing.
    // A capacity that is merely below the cap but would double past it is
    // NOT dropped: it still grows once more, clamped to maxArenaCapacity.
    void GrowSerializationArenaForTick() {
        const std::size_t currentCapacity = serializationArena_.Capacity();
        if (currentCapacity >= serializationContext_.maxArenaCapacity) {
            INFO("MessageThreadTick: serialization arena at max capacity %zu; dropping stashed patch message",
                 serializationContext_.maxArenaCapacity);
            pendingPatchMessage_.reset();
            arenaGrowPending_.store(false, std::memory_order_release);
            return;
        }

        const std::size_t doubled = currentCapacity * 2;
        const std::size_t nextCapacity = std::min(doubled, serializationContext_.maxArenaCapacity);
        serializationArena_.Init(nextCapacity);
        arenaGrowPending_.store(false, std::memory_order_release);
    }

    // Pre-audio-only synchronous drain, used by Initialize(). Drains
    // patchInputBus_ via ApplyPatchMessage using the engine's serialization
    // context. Patch messages are parameter-only, so applying/reverting here
    // does not schedule MIDI rebuilds or audio-device callbacks.
    //
    // ArenaExhausted handling: during Initialize, audio has not started, so
    // on ArenaExhausted we simply grow serializationArena_ synchronously
    // (heap allocation is safe pre-audio) and retry that message once. This
    // growth is illegal once the audio thread is running: ProcessBlock has
    // its own inline drain loop (not this helper) that stashes the message
    // and defers growth to MessageThreadTick (Task 5) instead.
    //
    // audioDeviceStateMutex_ is held around each ApplyPatchMessage call (Task
    // 3 review finding: this used to mutate audioDeviceState_ without holding
    // the lock, contradicting the invariant documented on
    // audioDeviceStateMutex_ even though nothing can race it here --
    // Initialize() runs pre-audio, single-threaded). The lock is uncontended
    // in that window; held anyway for uniformity with every other touch point
    // of audioDeviceState_.
    // Every patch-message apply in this class goes through here, so the revert
    // hook cannot be wired at three of the four call sites and missed at the
    // fourth. Forwards verbatim and adds exactly one thing: an app that
    // declares HasRestoreStartupState is told when a revert has just rebuilt the
    // parameter manager from registered defaults, which is the point at which
    // any startup state the app established itself has been discarded.
    //
    // Called on whichever thread applied the message -- the pre-audio drain
    // during Initialize(), or ProcessBlock's own drain. Apps implementing the
    // hook are subject to the same audio-thread constraints as the drain
    // itself.
    template <typename... Args>
    PatchApplyStatus ApplyPatchMessageAndNotifyApp(Args&&... args) {
        const PatchApplyStatus status = ApplyPatchMessage(std::forward<Args>(args)...);
        if constexpr (HasRestoreStartupState<App>) {
            if (status == PatchApplyStatus::Reverted) {
                app_.RestoreStartupState();
            }
        }
        return status;
    }

    // Publishes a loaded patch's instrument (populated by the
    // ApplyPatchMessageAndNotifyApp call this local optional was just passed
    // to) into pendingPatchInstrument_ for MessageThreadTick to apply. A
    // no-op when nothing was staged -- a revert/serialize message, a patch
    // without a midiInstrument section, or an app whose catalog does not set
    // patchCarriesMappings. Safe to call from the audio thread: this is the
    // same rare, user-initiated patch-boundary window audioDeviceStateMutex_
    // is already documented to accept a lock in on the audio thread (see its
    // doc comment, and the ProcessBlock retry-stash branch above it).
    void StashLoadedPatchInstrument(std::optional<MidiInstrumentConfig>& loaded) {
        if (!loaded.has_value()) {
            return;
        }
        const std::lock_guard<std::mutex> lock(pendingPatchInstrumentMutex_);
        pendingPatchInstrument_ = std::move(loaded);
    }

    void ApplyPendingPatchMessages() {
        PatchMessageIn message;
        while (patchInputBus_.Pop(message)) {
            PatchApplyStatus status;
            std::optional<MidiInstrumentConfig> loadedInstrument;
            {
                const std::lock_guard<std::mutex> lock(audioDeviceStateMutex_);
                status = ApplyPatchMessageAndNotifyApp(message, manager_, instrumentConfig_, defaultInstrumentConfig_,
                                           audioDeviceState_, defaultAudioDeviceState_, patchOutputBus_,
                                           serializationContext_, midiCatalog_.patchCarriesMappings,
                                           midiCatalog_.patchCarriesMappings ? &loadedInstrument : nullptr);
                if (status == PatchApplyStatus::ArenaExhausted) {
                    // Pre-audio only: growing here is safe because the audio
                    // thread has not started running ProcessBlock yet.
                    serializationArena_.GrowAndReset();
                    status = ApplyPatchMessageAndNotifyApp(message, manager_, instrumentConfig_, defaultInstrumentConfig_,
                                               audioDeviceState_, defaultAudioDeviceState_, patchOutputBus_,
                                               serializationContext_, midiCatalog_.patchCarriesMappings,
                                               midiCatalog_.patchCarriesMappings ? &loadedInstrument : nullptr);
                }
            }
            StashLoadedPatchInstrument(loadedInstrument);
        }
    }

    // Members are declared in dependency order: buses reference both managers,
    // and PatchManager references the patch buses.
    ParameterManager manager_;
    GridManager gridManager_;
    MessageInBus uiBus_;
    MessageInBus midiBus_;
    ParameterMessageOutBus parameterMessageOutBus_;
    PatchMessageInBus patchInputBus_;
    MessageOutBus patchOutputBus_;
    MidiSender midiSender_;
    // Runtime-lifetime causal state shared by rebuilt absolute input/output
    // processor chains. Route records retain keys and pending expectations;
    // processors keep only non-owning pointers back to this stable owner.
    AbsoluteFeedbackCoordinator absoluteFeedbackCoordinator_;
    PatchManager patchManager_;
    MasterClock masterClock_;
    std::atomic<std::uint64_t> requestedSyncWord_{EncodeSyncConfiguration(SyncConfig{})};
    std::uint64_t appliedSyncWord_ = EncodeSyncConfiguration(SyncConfig{});
    ClockDiagnosticsPublication clockDiagnosticsPublication_;
    // Engine-owned MIDI instrument (sar-3): the source
    // RebuildMidiProcessors() builds midiProcessors_ from (one
    // MidiControllerProfileResult per controller slot, index-for-index -- see
    // RebuildMidiProcessors()'s doc comment). LiveInstrument()/EditInstrument()
    // are the pre-audio/locked read and message-thread write surface;
    // InstrumentSnapshot() is the locked running-state read surface (use this
    // one once audio may be live). ApplyPatchMessage/LoadPatchJSON never write
    // this member directly, whether or not the app's patch files carry an
    // instrument section. When they do (midiCatalog_.patchCarriesMappings) and
    // a load parses one, the parsed instrument is staged in
    // pendingPatchInstrument_ and applied here only later, on the message
    // thread, through EditInstrument.
    MidiInstrumentConfig instrumentConfig_;
    // Default = the app's Init-configured instrument; revert/new restore
    // this. Snapshotted from instrumentConfig_ in Initialize(), immediately
    // after app_.Init(&context_) returns and before any startup patch applies
    // (see the Initialize() binding-order comment, step 4a). Exposed
    // read-only via DefaultInstrument().
    MidiInstrumentConfig defaultInstrumentConfig_;

    // Guards audioDeviceState_ + lastNotifiedAudioDeviceState_ (the two members
    // below) AND instrumentConfig_ for coherent host reads/writes. AppContext no
    // longer exposes a mutable pointer into audioDeviceState_; the only current
    // writers are SetAudioDeviceFromHost and EditInstrument, both of which hold
    // this lock.
    //
    // Patch-message application still takes this lock while passing the
    // instrument/audio state into compatibility APIs, but parameter-only patch
    // load/revert does not mutate either member. The lock is never touched on
    // the steady-state pump path when there is no pending patch message to apply
    // (patchInputBus_.Pop() returning false costs nothing extra; see
    // DrainPatchInputBus's doc comment).
    mutable std::mutex audioDeviceStateMutex_;

    // Guards pendingPatchInstrument_ (below) only. A loaded patch's
    // midiInstrument section, once parsed, is staged here by whichever
    // thread drains patchInputBus_ (audio-thread ProcessBlock/
    // DrainPatchInputBus, or the pre-audio ApplyPendingPatchMessages) and
    // taken by MessageThreadTick to apply through EditInstrument. Separate
    // from audioDeviceStateMutex_ because that lock is already held for the
    // duration of the ApplyPatchMessage call itself; this one is taken only
    // afterward, briefly, to publish the result -- the same rare,
    // user-initiated patch-boundary window audioDeviceStateMutex_ is
    // documented to accept a lock in on the audio thread.
    mutable std::mutex pendingPatchInstrumentMutex_;
    // Set only when a loaded patch's midiInstrument section parsed
    // (implies midiCatalog_.patchCarriesMappings, the only case
    // ApplyPatchMessage is asked to parse one). Cleared once
    // MessageThreadTick takes it.
    std::optional<MidiInstrumentConfig> pendingPatchInstrument_;

    // Engine-owned audio device selection. Hosts update it through
    // SetAudioDeviceFromHost; runtime configuration loading seeds it during
    // Initialize(). There is no mutable pointer into this member on AppContext,
    // so all reads/writes must hold audioDeviceStateMutex_. See
    // SetAudioDeviceFromHost/AudioDeviceSnapshot for the public API.
    AudioDeviceState audioDeviceState_;
    // Default = the app's Init-configured audio device selection; revert/new
    // restore this. Snapshotted from audioDeviceState_ alongside
    // defaultInstrumentConfig_ in Initialize().
    AudioDeviceState defaultAudioDeviceState_;
    // Shadow of the last audioDeviceState_ value the host was told about.
    // Host-driven changes advance this immediately because the host already
    // knows about its own selection. Runtime configuration loading advances
    // the same shadow before the device is opened. Patch files are
    // parameter-only and never update audioDeviceState_.
    AudioDeviceState lastNotifiedAudioDeviceState_;
    JsonArena serializationArena_;
    PatchSerializationContext serializationContext_;
    RuntimeConfig config_;
    RuntimeDataPaths dataPaths_;
    AppContext context_;
    App app_;
    // Empty unless App declares MidiCatalog() (HasMidiCatalog<App>); read
    // once, in the constructor.
    MidiAppCatalog midiCatalog_;
    std::unique_ptr<ParameterManager::UIState> uiState_;
    std::unique_ptr<GridManager::UIState> gridUIState_;
    RuntimeUIState runtimeUIState_;
    // One MidiControllerProfileResult per controller slot, index-for-index
    // with instrumentConfig_.controllers as of the last RebuildMidiProcessors()
    // call (see that method's doc comment for the per-controller sink-routing
    // rebuild contract). Empty when the instrument has zero controllers.
    std::vector<MidiControllerProfileResult> midiProcessors_;
    TimestampProvider timestampProvider_;
    std::atomic<std::uint64_t> sampleCounter_{0};
    std::array<MessageIn, kRealtimeBatchCapacity> realtimeBatch_{};
    std::size_t realtimeBatchSize_ = 0;
    std::atomic<std::uint64_t> droppedRealtimeInputCount_{0};
    std::function<void()> midiProcessorsRebuiltCallback_;
    // Invoked synchronously immediately BEFORE midiProcessors_ is
    // destroyed/replaced, from RebuildMidiProcessors() (the sole assignment
    // site). See SetMidiProcessorsWillRebuildCallback's doc comment.
    std::function<void()> midiProcessorsWillRebuildCallback_;
    // See SetAudioDeviceChangedCallback's doc comment.
    std::function<void()> audioDeviceChangedCallback_;

    double sampleRate_ = 0.0;
    int blockSize_ = 0;

    // UI-state publish throttle (Task 4): PopulateUIState runs every
    // uiPublishInterval_ blocks. Prepare() computes uiPublishInterval_ =
    // max(1, round(sampleRate / (uiFrameHz * blockSize))); the default of 1
    // (publish every block) applies before Prepare runs.
    int uiPublishInterval_ = 1;
    int blocksSinceUiPublish_ = 0;

    // Audio-path ArenaExhausted handling / drain barrier (Task 4/5):
    // ProcessBlock never grows serializationArena_ on the audio thread. On
    // ArenaExhausted it stashes the popped message here and sets
    // arenaGrowPending_, which bars the ENTIRE patch-drain phase (not just
    // growth) for subsequent blocks: while pendingPatchMessage_ holds a
    // value, ProcessBlock does not pop any further messages from
    // patchInputBus_, preventing a second exhaustion from clobbering the
    // stash and preventing newer messages from applying out of order ahead
    // of it. ProcessBlock retries the stash itself, first, as soon as
    // arenaGrowPending_ reads false.
    //
    // Tick contract: MessageThreadTick grows the arena and clears
    // arenaGrowPending_; it must NOT touch pendingPatchMessage_, except the
    // documented drop-at-cap carve-out in GrowSerializationArenaForTick
    // (arena already at serializationContext_.maxArenaCapacity: the stash
    // is dropped there instead of retried forever). Outside that one case,
    // only ProcessBlock (audio thread) reads, retries, or clears the stash.
    std::optional<PatchMessageIn> pendingPatchMessage_;
    std::atomic<bool> arenaGrowPending_{false};

    // ui-state-before-audio (design's "Mechanism" section, PINNED — do not
    // substitute a different synchronization shape): a single-slot
    // lock-free CAS claim over uiState_/gridUIState_ publication, owned by
    // Engine because Engine owns the buffers (design §8 sibling
    // enumeration: sampleCounter_/BrowserRuntime's lifecycle
    // flags/the plugin heartbeat are all host-layer or otherwise not a
    // writer-exclusion primitive over these buffers, so none is reused;
    // this is a new Engine-internal primitive at the owning layer).
    //
    // Quiescent: nobody has published UI state yet this engine lifetime, or
    //   the message thread has just released after a populate. AudioThread
    //   is free to claim, and so is the message thread.
    // MessageThread: the message thread is mid-populate this tick. The
    //   audio thread's CAS to AudioThread will fail while this holds, so
    //   the audio thread skips that publish window rather than blocking.
    // AudioThread: the audio thread has published at least once, ever. This
    //   is a ONE-WAY LATCH — MessageThreadTick's CAS out of Quiescent can
    //   never succeed again once this state is reached, so message-thread
    //   population ends permanently and audio populates lock-free forever
    //   after via audioOwnsUiState_ below (design: "writer sets are
    //   disjoint at every instant by construction").
    enum class UiStatePublisher : std::uint8_t { Quiescent, MessageThread, AudioThread };
    std::atomic<UiStatePublisher> uiStatePublisher_{UiStatePublisher::Quiescent};
    // Audio-thread-private one-way latch (plain bool, per design: read/set
    // ONLY on the audio thread, never contended, so no atomic is needed).
    // Once true, ProcessBlock's publish site never touches
    // uiStatePublisher_ again — the CAS above already made AudioThread
    // permanent, and this flag lets the audio thread skip even attempting
    // the CAS on every subsequent throttle window (todays's exact
    // no-synchronization populate cost once the transition is behind it).
    bool audioOwnsUiState_ = false;

    // Rig/test support: last non-NoCompletion PatchCommandResult observed by
    // MessageThreadTick's patchManager_.ProcessResponses() call. See
    // ConsumeLastTickPatchResult().
    std::optional<PatchCommandResult> lastTickPatchResult_;
};

}  // namespace synth
