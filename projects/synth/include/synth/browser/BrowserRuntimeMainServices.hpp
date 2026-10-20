#pragma once

#include "synth/ControllersPageUI.hpp"
#include "synth/ControllerWizardDiscoveryCache.hpp"
#include "synth/Engine.hpp"
#include "synth/RuntimeFileService.hpp"
#include "synth/RuntimePages.hpp"
#include "synth/browser/BrowserAudioDevices.hpp"
#include "synth/browser/BrowserMidiBridge.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace synth_browser {

template <synth::SynthApplication App>
class BrowserRuntimeMainServices final
{
public:
    using EngineType = synth::Engine<App>;
    using MidiBridge = BrowserMidiBridge<EngineType>;

    BrowserRuntimeMainServices(EngineType& engine,
                               MidiBridge& midiBridge,
                               std::vector<BrowserAudioDevice>& audioDevices,
                               std::function<float()> deadlineSampleProvider = {},
                               std::function<BrowserAudioInputState()> audioInputStateProvider = {})
        : engine_(engine)
        , midiBridge_(midiBridge)
        , audioDevices_(audioDevices)
        , fileService_(MakeFileCallbacks())
        , deadlineSampleProvider_(std::move(deadlineSampleProvider))
        , audioInputStateProvider_(std::move(audioInputStateProvider))
    {
    }

    BrowserRuntimeMainServices(const BrowserRuntimeMainServices&) = delete;
    BrowserRuntimeMainServices& operator=(const BrowserRuntimeMainServices&) = delete;
    BrowserRuntimeMainServices(BrowserRuntimeMainServices&&) = delete;
    BrowserRuntimeMainServices& operator=(BrowserRuntimeMainServices&&) = delete;

    synth::runtime_ui::ControllersPageCallbacks MakeControllersCallbacks(
        std::function<void()> onBack)
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] {
            return engine_.InstrumentSnapshot();
        };
        callbacks.connectionState = [this] {
            return midiBridge_.ConnectionState();
        };
        callbacks.enumerateDevices = [this] {
            return wizardDiscoveryCache_.DeviceList();
        };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig instrument) {
            engine_.EditInstrument([&instrument](synth::MidiInstrumentConfig& current) {
                current = std::move(instrument);
            });
            wizardDiscoveryCache_.UpdateInstrumentSnapshot(engine_.InstrumentSnapshot());
            controllersDirty_ = true;
            instrumentSnapshotDirty_ = false;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [this] {
            if (engine_.SaveRuntimeConfiguration() !=
                synth::RuntimeConfigFileStatus::Ok)
            {
                return false;
            }
            persistenceDirty_ = true;
            return true;
        };
        callbacks.setStatus = [](std::string) {};
        callbacks.onBack = std::move(onBack);
        return callbacks;
    }

    void RecordAudioNegotiation(double sampleRate, std::size_t blockSize)
    {
        negotiatedSampleRate_ = sampleRate;
        negotiatedBlockSize_ = static_cast<int>(blockSize);
    }

    void RefreshAudio(synth::runtime_ui::AudioPageSnapshot& snapshot)
    {
        const BrowserAudioInputState input = AudioInputState();
        snapshot = BuildBrowserAudioSnapshot(engine_.AudioDeviceSnapshot(), input, audioDevices_);
        if (negotiatedSampleRate_.has_value() && negotiatedBlockSize_.has_value())
        {
            snapshot.deviceLineText = synth::runtime_ui::Layout::BuildNegotiatedDeviceLine(
                synth::runtime_ui::kSystemDefaultOptionLabel,
                *negotiatedSampleRate_,
                *negotiatedBlockSize_);
        }
        else
        {
            snapshot.deviceLineText = "No audio device";
        }
        // A live capture diagnostic outranks the last selection acknowledgement:
        // the acknowledgement is what the user just did, the diagnostic is what
        // the host can currently deliver. Neither displaces the requested/active
        // counts (sru-3) -- both compose after them.
        std::string detail = BrowserAudioInputDetail(input);
        if (detail.empty() && audioStatus_.has_value())
        {
            detail = *audioStatus_;
        }
        // An unroutable output control is a standing fact about this browser,
        // not a one-shot acknowledgement, so it appends to whatever detail is
        // already showing rather than competing with it for the one slot.
        if (outputRoutingUnsupported_)
        {
            detail = detail.empty() ? kOutputRoutingUnsupportedText
                                     : detail + ", " + kOutputRoutingUnsupportedText;
        }
        snapshot.statusLineText = ComposeBrowserAudioStatusLine(input, detail);
    }

    void DispatchAudio(const synth::ui::Action& action)
    {
        // Reacquiring is the launcher realm's work: it needs DOM/media APIs
        // this side never touches, and it must not be initiated by anything
        // but the user (sbw-4). Retry re-requests whatever is currently
        // selected, so it arms from the persisted input name rather than an
        // action value. There is no output equivalent of retry: an output
        // selection arms immediately below, and System Default is always
        // reachable without a stream to lose.
        if (action.name == synth::runtime_ui::Actions::kAudioInputRetry)
        {
            ArmPendingAudioRequest(BrowserAudioDeviceKind::Input,
                                   engine_.AudioDeviceSnapshot().inputDeviceName);
            return;
        }

        // Retry re-requests the current selection, so on a page holding no
        // permission -- where the selection is No Input and the list offers
        // nothing else -- it arms the release sentinel and prompts for
        // nothing. This action is the way out of that state: it names no
        // device, so it does not go through ArmPendingAudioRequest's
        // name-to-index resolution at all.
        if (action.name == synth::runtime_ui::Actions::kAudioInputPermission)
        {
            pendingAudioRequestControl_ = BrowserAudioDeviceKind::Input;
            pendingAudioRequestIndex_ = kRequestPermissionAudioRequest;
            return;
        }

        // JS reports a rejected setSinkId here rather than leaving the failed
        // device selected with nothing routed to it: the selection reverts to
        // System Default, the one output that is always actually reachable.
        // This is a display correction only -- the route was never claimed
        // (JS's own setSinkId call is what would route it), so nothing here
        // arms a new pending request. A report naming a device the operator
        // has since replaced with a newer selection is stale and dropped,
        // since outputDeviceName no longer matches it.
        if (action.name == synth::runtime_ui::Actions::kAudioOutputRouteFailed)
        {
            synth::AudioDeviceState state = engine_.AudioDeviceSnapshot();
            if (state.outputDeviceName == action.value)
            {
                state.outputDeviceName.clear();
                engine_.SetAudioDeviceFromHost(state);
            }
            return;
        }

        // JS reports this once it discovers the browser exposes no way to
        // route to a specific output device at all; RefreshAudio folds it
        // into the status line so the reason reaches the operator instead of
        // the output combo just quietly offering nothing but System Default.
        if (action.name == synth::runtime_ui::Actions::kAudioOutputRoutingUnsupported)
        {
            outputRoutingUnsupported_ = true;
            return;
        }

        const bool selectsOutput = action.name == synth::runtime_ui::Actions::kAudioOutputSelect;
        if (!selectsOutput && action.name != synth::runtime_ui::Actions::kAudioInputSelect)
        {
            return;
        }
        synth::AudioDeviceState state = engine_.AudioDeviceSnapshot();
        if (selectsOutput)
        {
            state.outputDeviceName = BrowserOutputDeviceName(action.value, audioDevices_);
            // Selecting an output device arms the same pending request an
            // input selection does, generalized by control: "route what was
            // just selected" is one mechanism with different arguments (only
            // JS can call setSinkId, so the actual routing happens there).
            ArmPendingAudioRequest(BrowserAudioDeviceKind::Output, state.outputDeviceName);
        }
        else
        {
            state.inputDeviceName = BrowserInputDeviceName(action.value, audioDevices_);
            // Selecting arms the same pending request retry does: "acquire
            // what was just selected" and "reacquire what is selected" are one
            // operation with different arguments (only JS can call
            // getUserMedia, so the actual acquisition/release happens there).
            ArmPendingAudioRequest(BrowserAudioDeviceKind::Input, state.inputDeviceName);
        }
        engine_.SetAudioDeviceFromHost(state);
        audioStatus_ = "Using System Default";
    }

    // Reports the pending index together with which control (input or
    // output) it was armed for -- see BrowserAudioDevices.hpp for the
    // sentinel values this index may hold.
    std::int32_t ConsumePendingAudioRequest(BrowserAudioDeviceKind& outControl)
    {
        outControl = pendingAudioRequestControl_;
        const std::int32_t pending = pendingAudioRequestIndex_;
        pendingAudioRequestIndex_ = kNoPendingAudioRequest;
        return pending;
    }

    void RefreshFile(synth::runtime_ui::FilePageSnapshot& snapshot)
    {
        fileService_.Refresh(snapshot);
    }

    void DispatchFile(const synth::ui::Action& action)
    {
        fileService_.Dispatch(action);
    }

    // Recompute the cached classification as soon as the host learns the device
    // list changed, instead of waiting for the next frame build. Submit
    // rechecks its candidate through the enumerateDevices callback, so a
    // controller unplugged between two frames must already be gone from the
    // cache by the time that recheck runs. An unchanged list is a no-op, and
    // this neither enumerates devices nor reconciles endpoints.
    void NoteMidiDeviceListChanged()
    {
        const std::uint64_t deviceListRevision = midiBridge_.DeviceListRevision();
        if (wizardDiscoveryCache_.HasDeviceList() && deviceListRevision == cachedDeviceListRevision_)
        {
            return;
        }
        wizardDiscoveryCache_.UpdateDeviceList(midiBridge_.LatestDeviceList());
        cachedDeviceListRevision_ = deviceListRevision;
    }

    void RefreshControllers(synth::runtime_ui::ControllersPageSurface& surface)
    {
        NoteMidiDeviceListChanged();
        surface.SetEnumerateDevices(wizardDiscoveryCache_.DeviceList());
        if (instrumentSnapshotDirty_)
        {
            wizardDiscoveryCache_.UpdateInstrumentSnapshot(engine_.InstrumentSnapshot());
            instrumentSnapshotDirty_ = false;
        }
        if (controllersDirty_)
        {
            surface.MarkDirty();
            controllersDirty_ = false;
        }
        surface.SetDiscovery(wizardDiscoveryCache_.Discovery());
        surface.RefreshOnTick();
    }

    synth::SyncConfig SnapshotSyncConfiguration()
    {
        return engine_.SyncConfigurationSnapshot();
    }

    void RefreshSyncStatus(synth::runtime_ui::SyncPageStatus& status)
    {
        status = synth::runtime_ui::BuildSyncPageStatus(
            engine_.ClockDiagnosticsSnapshot(), engine_.InstrumentSnapshot());
    }

    bool CommitSyncConfiguration(const synth::SyncConfig& config)
    {
        return engine_.RequestSyncConfiguration(config);
    }

    float DeadlineSamplePercent() const
    {
        return deadlineSampleProvider_ ? deadlineSampleProvider_() : 0.0f;
    }

    void SaveRuntimeConfiguration()
    {
        if (engine_.SaveRuntimeConfiguration() == synth::RuntimeConfigFileStatus::Ok)
        {
            persistenceDirty_ = true;
        }
    }

    bool ConsumePersistenceDirty()
    {
        const bool dirty = persistenceDirty_;
        persistenceDirty_ = false;
        return dirty;
    }

private:
    BrowserAudioInputState AudioInputState() const
    {
        return audioInputStateProvider_ ? audioInputStateProvider_() : BrowserAudioInputState{};
    }

    // Resolves the wanted device name (empty means No Input / System Default)
    // against the most recently submitted list, filtered to the armed
    // control's kind, and arms the single pending request with its index, or
    // with kReleaseAudioRequest if the name is empty or no longer present in
    // that list -- an absent device falls back to release rather than
    // claiming a device this host cannot currently name. One slot is shared
    // by every control; arming one control's request implicitly supersedes
    // whatever the other control last armed and never consumed.
    void ArmPendingAudioRequest(BrowserAudioDeviceKind control, const std::string& deviceName)
    {
        pendingAudioRequestControl_ = control;
        if (!deviceName.empty())
        {
            for (std::size_t ix = 0; ix < audioDevices_.size(); ++ix)
            {
                if (audioDevices_[ix].kind == control && audioDevices_[ix].label == deviceName)
                {
                    pendingAudioRequestIndex_ = static_cast<std::int32_t>(ix);
                    return;
                }
            }
        }
        pendingAudioRequestIndex_ = kReleaseAudioRequest;
    }

    synth::runtime_ui::RuntimeFileCallbacks MakeFileCallbacks()
    {
        synth::runtime_ui::RuntimeFileCallbacks callbacks;
        callbacks.currentPatchDirectory = [this] {
            return engine_.Patches().CurrentPatchDirectory();
        };
        callbacks.patchesRoot = [this] {
            return engine_.DataPaths().patchesRoot;
        };
        callbacks.newPatch = [this] { engine_.Patches().NewPatch(); };
        callbacks.savePatch = [this] { engine_.Patches().SavePatch(); };
        callbacks.savePatchAs = [this](const std::filesystem::path& path) {
            engine_.Patches().SavePatchAs(path);
        };
        callbacks.savePatchAsOverwrite = [this](const std::filesystem::path& path) {
            engine_.Patches().SavePatchAsOverwrite(path);
        };
        callbacks.loadPatch = [this](const std::filesystem::path& path) {
            engine_.Patches().LoadPatch(path);
        };
        callbacks.revertPatch = [this] { engine_.Patches().RevertPatch(); };
        return callbacks;
    }

    EngineType& engine_;
    MidiBridge& midiBridge_;
    // The devices JS most recently submitted, owned by the enclosing Runtime
    // beside its midiBridge_ (BrowserRuntime.hpp); referenced here the same
    // way midiBridge_ is.
    std::vector<BrowserAudioDevice>& audioDevices_;
    synth::runtime_ui::RuntimeFileService fileService_;
    std::function<float()> deadlineSampleProvider_;
    std::function<BrowserAudioInputState()> audioInputStateProvider_;
    std::optional<double> negotiatedSampleRate_;
    std::optional<int> negotiatedBlockSize_;
    std::optional<std::string> audioStatus_;
    // Latches true once JS reports this browser has no way to route to a
    // specific output device at all (kAudioOutputRoutingUnsupported); never
    // reset, since the capability a page-lifetime AudioContext exposes does
    // not change back within that lifetime.
    bool outputRoutingUnsupported_ = false;
    // One pending request slot shared by input retry, input selection, and
    // output selection: BrowserAudioDevices.hpp's kNoPendingAudioRequest /
    // kReleaseAudioRequest, or a nonnegative index into audioDevices_, plus
    // which control that index was armed for. Meaningless when the index is
    // kNoPendingAudioRequest.
    std::int32_t pendingAudioRequestIndex_ = kNoPendingAudioRequest;
    BrowserAudioDeviceKind pendingAudioRequestControl_ = BrowserAudioDeviceKind::Input;
    synth::ControllerWizardDiscoveryCache wizardDiscoveryCache_;
    std::uint64_t cachedDeviceListRevision_ = 0;
    bool controllersDirty_ = true;
    bool instrumentSnapshotDirty_ = true;
    bool persistenceDirty_ = false;
};

}  // namespace synth_browser
