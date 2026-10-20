#pragma once

// synth_runtime::MidiConnectionManager — per-controller MIDI device lifecycle
// owner. Owns a vector of synth_juce::MidiInHandler/
// MidiOutputHandler parallel to the engine's instrument controller slots,
// plus the MidiConnectionState mirror and the MidiDevicePoller that watches
// for USB device list changes in the background. This is the runtime-side
// binding of the JUCE-free ExecuteReconcilePlan/PlanMidiReconciliation core
// (include/synth/MidiReconcile.hpp) to real JUCE device handlers and
// synth::Engine<App>.
//
// Forwarding-processor swap safety (binding): reopening
// or rebuilding a controller's input device must never leave a MIDI callback
// pointing at a destroyed processor chain. This class follows a
// detach -> rebuild -> reattach discipline (OnMidiProcessorsWillRebuild +
// InstallForwardingProcessor), applied per controller (every slot, including
// slot 0): engine.SetMidiProcessorsWillRebuildCallback must be wired (by the host,
// e.g. Runtime) to call this class's OnMidiProcessorsWillRebuild() BEFORE
// the engine destroys midiProcessors_, and the rebuilt callback should call
// OnInstrumentRebuilt() afterward. Between those two calls, every input
// handler's forwarding processor is detached (SetProcessor(nullptr)); no
// code in this class dereferences a MidiInProcessor* outside that window.
//
// EditInstrument re-entrancy (binding): Update*Ref
// actions in a reconcile plan write the matched device's identifier+name
// back into the engine's live instrument via engine.EditInstrument, and
// EditInstrument ALWAYS rebuilds MIDI processors and fires the
// rebuilt-callback synchronously (Engine::EditInstrument's own doc comment).
// If that rebuilt callback forwarded straight back into this manager's
// reconcile path, an UpdateInputRef/UpdateOutputRef action partway through
// ExecuteReconcilePlan would re-enter reconciliation before the current pass
// has finished applying its own actions. This class guards against that
// with reconciling_: OnInstrumentRebuilt() (the method a host wires as the
// rebuilt-callback target) no-ops when reconciling_ is already true, instead
// of running a nested reconcile pass. This is a guard-flag choice, not a
// batch-and-apply-after choice: the alternative (collect ref updates and
// apply them once after ExecuteReconcilePlan returns) would mean
// UpdateInputRef/UpdateOutputRef's ops callback couldn't call
// engine.EditInstrument synchronously either, which would just move the
// same problem into a hand-rolled batching layer instead of removing it. The
// guard is sufficient because OnInstrumentRebuilt()'s only real job here
// (beyond the resize) is to run exactly one reconcile pass per rebuild
// trigger, and the plan currently executing already IS that pass for this
// trigger -- a nested call has nothing new to reconcile.
//
// Startup double-reconcile gate (binding): the host wires
// OnInstrumentRebuilt() as the rebuilt-callback target unconditionally, in its
// own constructor -- before it ever calls StartupReconcile() (typically from a
// later Start() method). started_ (false until StartupReconcile() sets it)
// gates this: OnInstrumentRebuilt() always resizes handler/state vectors
// (cheap, and StartupReconcile()'s own resize call needs the current controller
// count regardless of what ran before it), but only runs Reconcile() when
// started_ is already true. See OnInstrumentRebuilt()'s doc comment for the
// post-startup behavior once started_ is true.
//
// Sole owner of every device handler (binding): this manager is the sole
// owner of every
// controller slot's MidiInHandler/MidiOutputHandler, including slot 0 -- so
// there is exactly one owner per physical device and no sink/open
// contention. The old single-slot MidiPanel UI was replaced
// with ControllersPageSurface (ControllersPageUI.hpp), a genuinely
// per-controller UI; ControllersPageSurface
// does NOT call into this manager to open/close devices directly -- every
// page edit commits through
// engine.EditInstrument + connectionManager_ reconcile rather than mutating
// config or handlers directly from components, so a device combo selection on
// that page writes the chosen (or cleared) MidiEndpointRef via
// synth::MidiConfigViewModel::SetEndpointRef and commits it through
// engine.EditInstrument; this manager's own rebuilt-callback handler
// (OnInstrumentRebuilt(), wired by the host below) is what actually opens or
// closes the device, via a normal reconcile pass -- the same self-healing path
// persisted runtime configuration takes on load. This manager exposes only
// State()/EnumerateNow() as direct read accessors for a UI; there is no
// UI-facing manual open/close entry point.
#include "synth/AsyncLogger.hpp"
#include "synth/Engine.hpp"
#include "synth/MidiController.hpp"
#include "synth/MidiDevicePoller.hpp"
#include "synth/MidiReconcile.hpp"
#include "synth/ThreadId.hpp"

#include "MidiHandlers.hpp"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace synth_runtime {

namespace detail {

// Forwards MIDI input to whatever the engine's current processor for this
// controller slot is, retargetable via SetProcessor(nullptr) around a
// midiProcessors_ rebuild (see this file's "Forwarding-processor swap
// safety" doc comment above) without ever destroying/recreating the JUCE
// device handler itself. One instance per controller slot (installed by
// InstallForwardingProcessor(ix), below).
class EngineForwardingMidiInProcessor final : public synth::MidiInProcessor {
public:
    explicit EngineForwardingMidiInProcessor(synth::MidiInProcessor* target) : target_(target) {}

    void Process(const synth::BasicMidi& midi) override {
        synth::ScopedThreadId tag(synth::ThreadId::MidiInput);
        if (target_ != nullptr) {
            target_->Process(midi);
        }
    }

private:
    synth::MidiInProcessor* target_ = nullptr;
};

// The single JUCE device-enumeration helper: builds a
// synth::MidiDeviceList from juce::MidiInput::getAvailableDevices() +
// juce::MidiOutput::getAvailableDevices(). This is the function the
// message-thread authoritative re-enumeration (Reconcile(), called from
// StartupReconcile()/OnTimerTick()/OnInstrumentRebuilt(), all of which run on
// the message thread) always calls.
//
// Degraded-mode decision: this
// function must NOT be called off the message thread on macOS. JUCE's
// CoreMidi backend (juce_CoreMidi_mac.mm, CoreMidiHelpers::findDevices(),
// called by both MidiInput::getAvailableDevices() and
// MidiOutput::getAvailableDevices()) opens with an unconditional
// JUCE_ASSERT_MESSAGE_THREAD on every call, with the accompanying comment
// "It seems that OSX can be a bit picky about the thread that's first used to
// search for devices. It's safest to use the message thread for calling
// this." -- i.e. this is not merely a theoretical hazard, it is JUCE's own
// documented+asserted contract. MidiDevicePoller's worker thread runs under
// ThreadId::IoPoll, never the message thread, so handing this function
// directly to poller_.Start() as the injected Enumerate callback would fire
// that assertion on every poll cycle in a debug JUCE build. Per the
// accepted-degraded-mode fallback: the poller's injected enumerate callback
// is ForceDirtyEnumerate() below (a synthetic, ever-changing payload that
// makes MidiDevicePoller's own snapshot-compare see a "change" every cycle,
// unconditionally, without calling any JUCE API off the message thread), and
// OnTimerTick() -- already, independent of this decision -- discards the
// poller's reported `latest` list and re-enumerates authoritatively via this
// function on the message thread before reconciling. That "discard and
// re-enumerate" shape was already the binding message-thread-executor
// contract (see this class's OnTimerTick() doc comment: "never trusts the
// poller's own snapshot for the actual reconcile"), so degraded mode changes
// nothing about the reconcile path itself -- only what the poller thread is
// allowed to touch.
inline synth::MidiDeviceList EnumerateDevices() {
    synth::MidiDeviceList list;
    for (const auto& device : synth_juce::MidiInHandler::AvailableDevices()) {
        list.inputs.push_back({device.identifier.toStdString(), device.name.toStdString()});
    }
    for (const auto& device : synth_juce::MidiOutputHandler::AvailableDevices()) {
        list.outputs.push_back({device.identifier.toStdString(), device.name.toStdString()});
    }
    return list;
}

// Degraded-mode poll-thread enumerate callback (see EnumerateDevices()'s doc
// comment above for why this exists instead of handing the poller
// EnumerateDevices() directly). Never touches any JUCE API -- runs entirely
// on the IoPoll background thread, exactly as MidiDevicePoller's contract
// requires ("bounded/non-blocking", no platform dependency). Returns a
// single-entry synthetic input list whose identifier changes every call (a
// monotonically incrementing counter), which is guaranteed to differ from
// the previous cycle's snapshot under MidiDevicePoller::SnapshotChanged's
// exact vector-equality comparison -- so every poll cycle after the first
// (the first only primes the snapshot; priming is never a change, per
// MidiDevicePoller's own contract) sets the poller's dirty flag
// unconditionally. This is exactly "the poller ticks the dirty flag every
// 5 s unconditionally" from the design D4 fallback description. The
// synthetic payload itself is never read by anything: OnTimerTick() ignores
// ConsumeChange's `latest` output entirely and re-enumerates authoritatively
// via EnumerateDevices() on the message thread instead.
inline synth::MidiDeviceList ForceDirtyEnumerate() {
    static std::atomic<std::uint64_t> counter{0};
    synth::MidiDeviceList list;
    list.inputs.push_back({std::to_string(counter.fetch_add(1, std::memory_order_relaxed)), std::string()});
    return list;
}

}  // namespace detail

template <synth::SynthApplication App>
class MidiConnectionManager {
public:
    explicit MidiConnectionManager(synth::Engine<App>& engine,
                                   synth_juce::RuntimeMidiEpoch midiEpoch = {})
        : engine_(engine), midiEpoch_(midiEpoch) {}

    ~MidiConnectionManager() {
        // Shutdown ordering (binding): stop/join the
        // poller before closing devices, so no in-flight poll cycle races
        // handler teardown.
        poller_.Stop();
        for (auto& handler : inputHandlers_) {
            if (handler) {
                handler->SetProcessor(nullptr);
                handler->Close();
            }
        }
        for (std::size_t ix = 0; ix < outputHandlers_.size(); ++ix) {
            if (outputHandlers_[ix]) {
                // ClearSinkSync (not SetSink(ix, nullptr)) -- blocks until the
                // sender worker is not (and will never again be) mid-Send()
                // on this sink, which is required here because the handler
                // is about to be Close()'d and then destroyed by this
                // destructor. See ClearSinkSync's doc comment and the class
                // doc comment's sink-use-after-free note.
                if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
                    sender->ClearSinkSync(ix);
                }
                outputHandlers_[ix]->Close();
            }
        }
    }

    MidiConnectionManager(const MidiConnectionManager&) = delete;
    MidiConnectionManager& operator=(const MidiConnectionManager&) = delete;

    // Startup order (binding): call once after processors have been
    // built, BEFORE starting the poller. Resizes
    // handler vectors to the current controller count (state_ starts out
    // default-constructed, i.e. every entry status-unconfigured -- an
    // empty `MidiConnectionState{}` as the reconcile plan's `current`), then
    // runs one synchronous reconcile pass against the actually-enumerated
    // device list. Because `current` (state_) starts all-Unconfigured, the
    // planner still opens every configured ref (PlanMidiReconciliation's
    // matching pass does not require a prior Online/Offline connection
    // record -- only the instrument's configured refs and present devices
    // matter for whether an Open* action is planned); an absent device
    // simply goes offline, never a startup failure. Starts the poller
    // afterward. Sets started_ = true (see the class doc comment's
    // ONE-synchronous-startup-reconcile paragraph) so any rebuilt-callback that
    // fired before this point did not also run a reconcile pass of its own.
    void StartupReconcile() {
        ResizeToControllerCount();
        Reconcile(detail::EnumerateDevices());
        started_ = true;
        // Degraded mode (see detail::EnumerateDevices()'s doc comment): the
        // poller's injected enumerate callback is NOT the real JUCE
        // enumeration function -- juce::MidiInput/Output::getAvailableDevices()
        // must only ever be called on the message thread on macOS (JUCE
        // itself asserts this), and the poller's worker runs under
        // ThreadId::IoPoll, never the message thread. detail::ForceDirtyEnumerate()
        // never touches JUCE; it makes the poller report a change every
        // interval unconditionally, and OnTimerTick() below re-enumerates
        // authoritatively via detail::EnumerateDevices() on the message
        // thread when that happens (it already ignored the poller's reported
        // list even before this decision -- see OnTimerTick()'s doc comment).
        poller_.Start([] { return detail::ForceDirtyEnumerate(); });
    }

    // Wired to the runtime's existing message-thread timer tick. Consumes
    // the poller's dirty flag (if any); on a change, re-enumerates
    // authoritatively on the message thread (never trusts the poller's own
    // snapshot for the actual reconcile -- see MidiDevicePoller's class doc
    // comment: the poller exists purely to wake us, not to be the source of
    // truth), then gates whether to run reconciliation planning/execution at
    // all through synth::PlanMidiTickResponse ("WHEN the device list
    // is unchanged from the previous message-thread pass, THE message
    // thread SHALL NOT run reconciliation planning or plan execution").
    //
    // Unchanged-list gate (binding; supersedes the prior
    // always-reconcile design -- see PlanMidiTickResponse's own doc comment
    // for the full rationale and the pinned unit tests in
    // reconcile_executor_tests.cpp's tick_response_* cases): in degraded
    // mode (see detail::ForceDirtyEnumerate()'s doc comment) the poller's
    // dirty flag is set unconditionally every ~5 s, but that alone no longer
    // triggers a reconcile pass -- only a re-enumerated list that actually
    // differs from lastEnumerated_ does. rebuildPending is always false
    // here: Runtime::timerCallback()'s binding order runs
    // engine_.MessageThreadTick() (which drains any pending
    // instrument-rebuild and runs OnInstrumentRebuilt()'s own reconcile pass
    // synchronously) BEFORE OnTimerTick(), so any rebuild-triggered
    // reconcile for this tick has always already completed by the time this
    // method runs; reconciling_ is passed anyway (not a literal `false`) as
    // a defensive belt-and-suspenders read of the actual re-entrancy guard,
    // so this gate can never suppress a reconcile that a rebuild path still
    // has in flight, even if that ordering invariant ever changes.
    void OnTimerTick() {
        synth::MidiDeviceList latest;
        if (!poller_.ConsumeChange(latest)) {
            return;
        }

        const synth::MidiDeviceList present = detail::EnumerateDevices();
        const bool listChanged = !(hasLastEnumerated_ && present == lastEnumerated_);
        const synth::MidiTickResponse response = synth::PlanMidiTickResponse(
            /*pollerDirty=*/true, listChanged, /*rebuildPending=*/reconciling_);
        if (!response.reconcile) {
            lastEnumerated_ = present;
            hasLastEnumerated_ = true;
            return;
        }

        Reconcile(present);
    }

    // Wired to engine.SetMidiProcessorsWillRebuildCallback (forwarded by the
    // host, e.g. Runtime): detaches every input handler's forwarding
    // processor before the engine destroys midiProcessors_. See the class
    // doc comment's forwarding-swap-safety paragraph.
    void OnMidiProcessorsWillRebuild() {
        for (auto& handler : inputHandlers_) {
            if (handler) {
                handler->SetProcessor(nullptr);
            }
        }
    }

    // Wired to engine.SetMidiProcessorsRebuiltCallback (forwarded by the
    // host): resizes handler/state vectors to the (possibly changed)
    // controller count, reinstalls forwarding processors against the fresh
    // chain, and runs one reconcile pass (this is the same executor
    // path OnTimerTick uses). No-ops (beyond the resize) when a reconcile is
    // already in flight -- see the class doc comment's EditInstrument
    // re-entrancy paragraph.
    //
    // started_ gate: the
    // gate decision itself -- resize always, reconcile only once started --
    // is pinned by synth::PlanMidiRebuildResponse, a pure JUCE-free
    // function unit-tested directly in reconcile_executor_tests.cpp
    // (rebuild_response_* cases), instead of living only as inline logic
    // here that only a full JUCE runtime build would exercise. This
    // callback is wired unconditionally in the host's constructor (see
    // Runtime.hpp), i.e. BEFORE Runtime::Start() ever calls
    // StartupReconcile(). Any pre-startup instrument rebuild must NOT itself
    // run a reconcile pass -- only StartupReconcile()'s own call does,
    // immediately afterward, against whatever the resize below just produced.
    // Skipping the reconcile here pre-startup is safe: the resize (handler/state
    // vector sizing + forwarding-processor installation) still happens
    // unconditionally, so by the time StartupReconcile() runs its Reconcile()
    // call, the vectors are already correctly sized -- nothing is lost, only
    // the redundant early reconcile pass is skipped. Once started_ is true,
    // this method reconciles exactly as before. The `response` value
    // itself is used only for its `reconcile` flag here -- the resize
    // execution still goes through ResizeToControllerCount() (which
    // independently calls PlanMidiConnectionResize with the same
    // oldCount/newCount this computes), since PlanMidiRebuildResponse's
    // resizePlan describes JUCE-free bookkeeping only and this method needs
    // the JUCE handler-construction side effects ResizeToControllerCount()
    // performs.
    void OnInstrumentRebuilt() {
        const std::size_t oldCount = inputHandlers_.size();
        const std::size_t newCount = engine_.MidiControllerCount();
        const synth::MidiRebuildResponse response = synth::PlanMidiRebuildResponse(started_, oldCount, newCount);

        ResizeToControllerCount();
        if (!response.reconcile || reconciling_) {
            return;
        }
        Reconcile(detail::EnumerateDevices());
    }

    const synth::MidiConnectionState& State() const { return state_; }

    synth::MidiDeviceList EnumerateNow() const { return detail::EnumerateDevices(); }

    // Message-thread snapshot signal for UI consumers. Reconciliation remains
    // owned by OnTimerTick()/OnInstrumentRebuilt(); consumers only observe the
    // list already used by that path and never force another enumeration.
    const synth::MidiDeviceList& DeviceListSnapshot() const { return lastEnumerated_; }

    bool ConsumeDeviceListChange(synth::MidiDeviceList& latest) {
        if (!deviceListChanged_) {
            return false;
        }
        latest = lastEnumerated_;
        deviceListChanged_ = false;
        return true;
    }

private:
    // Grows/shrinks inputHandlers_/outputHandlers_/state_.controllers to
    // engine_.MidiControllerCount(), preserving existing entries by index
    // (a shrink closes and drops the trailing handlers/sinks; a growth
    // default-constructs new ones, all-Unconfigured in state_). Reinstalls
    // every surviving input handler's forwarding processor against the
    // (possibly rebuilt) MidiInputProcessor(ix) -- safe to call outside the
    // will-rebuild/rebuilt window too (e.g. from StartupReconcile(), where
    // there is no "prior" chain to race).
    //
    // The WHICH-indices-close / WHICH-indices-grow decision itself is
    // delegated to synth::PlanMidiConnectionResize (extracted into a pure,
    // JUCE-free, independently unit-tested helper --
    // see MidiReconcile.hpp's doc comment on that function and
    // reconcile_executor_tests.cpp's PlanMidiConnectionResize cases). This
    // method only executes the plan: for closingIx, ClearSinkSync (not
    // SetSink(ix, nullptr) -- see ClearSinkSync's
    // doc comment) the output sink before Close()ing both handlers, since the
    // vector resize immediately below destroys them; for growingIx (after the
    // resize), construct fresh handlers.
    void ResizeToControllerCount() {
        const std::size_t oldCount = inputHandlers_.size();
        const std::size_t newCount = engine_.MidiControllerCount();
        const synth::MidiConnectionResizePlan resizePlan = synth::PlanMidiConnectionResize(oldCount, newCount);

        for (const std::size_t ix : resizePlan.closingIx) {
            if (ix < inputHandlers_.size() && inputHandlers_[ix]) {
                inputHandlers_[ix]->SetProcessor(nullptr);
                inputHandlers_[ix]->Close();
            }
            if (ix < outputHandlers_.size() && outputHandlers_[ix]) {
                if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
                    sender->ClearSinkSync(ix);
                }
                outputHandlers_[ix]->Close();
            }
        }

        inputHandlers_.resize(newCount);
        outputHandlers_.resize(newCount);
        state_.controllers.resize(newCount);

        for (const std::size_t ix : resizePlan.growingIx) {
            inputHandlers_[ix] = std::make_unique<synth_juce::MidiInHandler>(midiEpoch_);
            outputHandlers_[ix] = std::make_unique<synth_juce::MidiOutputHandler>(midiEpoch_);
        }
        for (std::size_t ix = 0; ix < newCount; ++ix) {
            InstallForwardingProcessor(ix);
        }

        INFO("MidiConnectionManager resized to %zu controller(s)", newCount);
    }

    void InstallForwardingProcessor(std::size_t ix) {
        inputHandlers_[ix]->SetProcessor(
            std::make_unique<detail::EngineForwardingMidiInProcessor>(engine_.MidiInputProcessor(ix)));
    }

    static juce::String ToJuceString(const std::string& text) { return juce::String(text.c_str()); }

    // Builds the MidiEndpointOps binding for this manager's handlers/engine,
    // runs PlanMidiReconciliation + ExecuteReconcilePlan against `present`,
    // stores the result into state_, and logs a summary. reconciling_ guards
    // the whole call (see the class doc comment's re-entrancy paragraph):
    // updateInputRef/updateOutputRef call engine_.EditInstrument, which
    // synchronously fires the rebuilt callback (OnInstrumentRebuilt) --
    // reconciling_ being true there short-circuits that nested call before
    // it can start a second reconcile pass while this one is still applying
    // actions.
    void Reconcile(const synth::MidiDeviceList& present) {
        reconciling_ = true;
        const synth::MidiInstrumentConfig instrument = engine_.InstrumentSnapshot();
        const synth::ReconcilePlan plan = synth::PlanMidiReconciliation(instrument, present, state_);

        synth::MidiEndpointOps ops;
        ops.openInput = [this](std::size_t ix, const std::string& identifier) { return OpenInput(ix, identifier); };
        ops.openOutput = [this](std::size_t ix, const std::string& identifier) {
            return OpenOutput(ix, identifier);
        };
        ops.closeInput = [this](std::size_t ix) { CloseInput(ix); };
        ops.closeOutput = [this](std::size_t ix) { CloseOutput(ix); };
        ops.updateInputRef = [this](std::size_t ix, const std::string& id, const std::string& name) {
            UpdateRef(ix, id, name, /*isInput=*/true);
        };
        ops.updateOutputRef = [this](std::size_t ix, const std::string& id, const std::string& name) {
            UpdateRef(ix, id, name, /*isInput=*/false);
        };
        ops.resync = [this](std::size_t ix) { Resync(ix); };

        std::size_t opens = 0;
        std::size_t closes = 0;
        std::size_t offline = 0;
        std::size_t unconfigured = 0;
        std::size_t resyncs = 0;
        for (const auto& action : plan.actions) {
            switch (action.type) {
                case synth::ReconcileAction::Type::OpenInput:
                case synth::ReconcileAction::Type::OpenOutput:
                    ++opens;
                    break;
                case synth::ReconcileAction::Type::CloseInput:
                case synth::ReconcileAction::Type::CloseOutput:
                    ++closes;
                    break;
                case synth::ReconcileAction::Type::MarkInputOffline:
                case synth::ReconcileAction::Type::MarkOutputOffline:
                    ++offline;
                    break;
                case synth::ReconcileAction::Type::MarkInputUnconfigured:
                case synth::ReconcileAction::Type::MarkOutputUnconfigured:
                    ++unconfigured;
                    break;
                case synth::ReconcileAction::Type::Resync:
                    ++resyncs;
                    break;
                default:
                    break;
            }
        }

        state_ = synth::ExecuteReconcilePlan(plan, state_, ops);
        reconciling_ = false;

        // Log-quiet: skip the summary line only when
        // the plan was empty AND the device list itself is unchanged since
        // the last reconcile pass. Since the unchanged-list gate (see
        // OnTimerTick()'s doc comment and synth::PlanMidiTickResponse),
        // OnTimerTick() itself no longer calls Reconcile() at all on an
        // unchanged list, so this condition now mainly guards
        // rebuild-triggered reconciles (OnInstrumentRebuilt(),
        // StartupReconcile()) that happen to converge against an unchanged
        // list with an empty plan. Any non-empty plan, or any actual list
        // change that happens to converge to an empty plan, so a device
        // appearing/disappearing is never silently dropped from the log.
        const bool listUnchanged = hasLastEnumerated_ && present == lastEnumerated_;
        if (!plan.actions.empty() || !listUnchanged) {
            INFO("MIDI reconcile: plan=%zu opens=%zu closes=%zu offline=%zu unconfigured=%zu resyncs=%zu",
                 plan.actions.size(), opens, closes, offline, unconfigured, resyncs);
        }

        if (!listUnchanged) {
            deviceListChanged_ = true;
        }
        lastEnumerated_ = present;
        hasLastEnumerated_ = true;
    }

    bool OpenInput(std::size_t ix, const std::string& identifier) {
        if (ix >= inputHandlers_.size() || !inputHandlers_[ix]) {
            return false;
        }
        return inputHandlers_[ix]->Open(ToJuceString(identifier));
    }

    bool OpenOutput(std::size_t ix, const std::string& identifier) {
        if (ix >= outputHandlers_.size() || !outputHandlers_[ix]) {
            return false;
        }
        const bool opened = outputHandlers_[ix]->Open(ToJuceString(identifier));
        if (opened) {
            if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
                sender->SetSink(ix, outputHandlers_[ix].get());
            }
        }
        return opened;
    }

    void CloseInput(std::size_t ix) {
        if (ix < inputHandlers_.size() && inputHandlers_[ix]) {
            inputHandlers_[ix]->Close();
        }
    }

    void CloseOutput(std::size_t ix) {
        if (ix < outputHandlers_.size() && outputHandlers_[ix]) {
            if (synth::MidiSender* sender = engine_.Context().midiSender; sender != nullptr) {
                sender->ClearSinkSync(ix);
            }
            outputHandlers_[ix]->Close();
        }
    }

    // Writes the matched device's identifier+name back into the engine's
    // live instrument via EditInstrument -- this marks the patch dirty
    // exactly like a UI edit, from the message thread. EditInstrument rebuilds MIDI processors and fires the
    // rebuilt callback synchronously; OnInstrumentRebuilt()'s reconciling_
    // guard (see the class doc comment) prevents that from recursing back
    // into a nested Reconcile() call.
    void UpdateRef(std::size_t ix, const std::string& id, const std::string& name, bool isInput) {
        engine_.EditInstrument([&](synth::MidiInstrumentConfig& instrument) {
            if (ix >= instrument.controllers.size()) {
                return;
            }
            synth::MidiEndpointRef ref;
            ref.identifier = id;
            ref.name = name;
            if (isInput) {
                instrument.controllers[ix].input = ref;
            } else {
                instrument.controllers[ix].output = ref;
            }
        });
        // EditInstrument's rebuild replaced midiProcessors_ (and thus
        // rebuilt every forwarding target); reinstall this slot's forwarding
        // processor against the fresh chain the same way OnInstrumentRebuilt
        // does for every slot, since that path's own reconcile is
        // suppressed while reconciling_ is true (it never gets to do this
        // itself for the current pass).
        if (ix < inputHandlers_.size()) {
            InstallForwardingProcessor(ix);
        }
    }

    // resync = detach forwarding -> engine.ResetMidiOutputProcessors(ix) ->
    // reinstall forwarding, per the class doc comment / brief. Detaching the
    // input forwarding processor around a call that only touches OUTPUT
    // processors is deliberate defensive symmetry with
    // OnMidiProcessorsWillRebuild/OnInstrumentRebuilt's discipline -- it
    // costs nothing (ResetMidiOutputProcessors never replaces the input
    // chain) and keeps every codepath that could plausibly touch
    // midiProcessors_ bracketed the same way.
    void Resync(std::size_t ix) {
        if (ix < inputHandlers_.size() && inputHandlers_[ix]) {
            inputHandlers_[ix]->SetProcessor(nullptr);
        }
        engine_.ResetMidiOutputProcessors(ix);
        if (ix < inputHandlers_.size()) {
            InstallForwardingProcessor(ix);
        }
    }

    synth::Engine<App>& engine_;
    synth_juce::RuntimeMidiEpoch midiEpoch_;

    std::vector<std::unique_ptr<synth_juce::MidiInHandler>> inputHandlers_;
    std::vector<std::unique_ptr<synth_juce::MidiOutputHandler>> outputHandlers_;
    synth::MidiConnectionState state_;
    synth::MidiDevicePoller poller_;

    // Guards against OnInstrumentRebuilt() starting a nested reconcile pass
    // when EditInstrument (called from within Reconcile()'s own
    // updateInputRef/updateOutputRef ops) fires the rebuilt callback
    // synchronously. See the class doc comment's re-entrancy paragraph.
    bool reconciling_ = false;

    // Last message-thread-authoritative device list Reconcile() ran a plan
    // against, to avoid degraded-mode log noise. In degraded
    // mode (see detail::ForceDirtyEnumerate()'s doc comment) the poller sets
    // the dirty flag every ~5 s unconditionally, so OnTimerTick() calls
    // Reconcile() every tick even when nothing has actually changed on the
    // OS device list -- without this, every one of those ticks would log an
    // INFO line, in perpetuity, on a system with no MIDI activity at all.
    // Reconcile() skips its summary INFO line only when BOTH the plan is
    // empty (no action happened) AND `present` equals this snapshot (the
    // device list itself is unchanged since the last converged pass); any
    // non-empty plan, or any actual device-list change (even one that
    // happens to converge to an empty plan), still logs.
    //
    // hasLastEnumerated_ distinguishes "never reconciled yet" from "last
    // reconcile saw zero devices" -- both leave lastEnumerated_ as a
    // default-constructed (empty) MidiDeviceList, but only the latter should
    // ever suppress a log line; the very first Reconcile() call must always
    // log regardless of whether `present` happens to be empty too. Both
    // members are updated unconditionally at the end of every Reconcile()
    // call.
    bool hasLastEnumerated_ = false;
    synth::MidiDeviceList lastEnumerated_;
    bool deviceListChanged_ = false;

    // True once StartupReconcile() has run its one synchronous startup
    // reconcile pass. Before that,
    // OnInstrumentRebuilt() (wired unconditionally in the host's
    // constructor, so it can fire before Runtime::Start() calls
    // StartupReconcile()) only resizes handler/state vectors and does NOT
    // run a reconcile pass, preserving the binding "ONE synchronous startup
    // reconcile, then poller" ordering. See OnInstrumentRebuilt()'s doc
    // comment.
    bool started_ = false;
};

}  // namespace synth_runtime
