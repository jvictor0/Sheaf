#pragma once

#include "Runtime.hpp"

#include "synth/ControllerWizardDiscoveryCache.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/RuntimeFileService.hpp"
#include "synth/RuntimePages.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace synth_runtime {

template <synth::SynthApplication App>
class JuceRuntimeMainServices final
{
public:
    explicit JuceRuntimeMainServices(Runtime<App>& runtime)
        : runtime_(runtime)
        , fileService_(MakeFileCallbacks())
    {
        runtime_.SetAudioStatusHook([this](const juce::String& text) {
            audioStatus_ = text.toStdString();
        });
        runtime_.SetAudioSyncHook([this] { audioSyncPending_ = true; });
        runtime_.SetMidiProcessorsRebuiltHook([this] {
            controllersDirty_ = true;
            instrumentSnapshotDirty_ = true;
        });
    }

    ~JuceRuntimeMainServices()
    {
        runtime_.SetAudioStatusHook({});
        runtime_.SetAudioSyncHook({});
        runtime_.SetMidiProcessorsRebuiltHook({});
    }

    JuceRuntimeMainServices(const JuceRuntimeMainServices&) = delete;
    JuceRuntimeMainServices& operator=(const JuceRuntimeMainServices&) = delete;

    synth::runtime_ui::ControllersPageCallbacks MakeControllersCallbacks(
        std::function<void()> onBack)
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] {
            return runtime_.GetEngine().InstrumentSnapshot();
        };
        callbacks.connectionState = [this] {
            return runtime_.MidiConnections().State();
        };
        callbacks.enumerateDevices = [this] {
            return wizardDiscoveryCache_.DeviceList();
        };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig instrument) {
            runtime_.GetEngine().EditInstrument(
                [&](synth::MidiInstrumentConfig& current) {
                    current = std::move(instrument);
                });
            wizardDiscoveryCache_.UpdateInstrumentSnapshot(
                runtime_.GetEngine().InstrumentSnapshot());
            controllersDirty_ = true;
            instrumentSnapshotDirty_ = false;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [this] {
            return runtime_.SaveRuntimeConfiguration() ==
                   synth::RuntimeConfigFileStatus::Ok;
        };
        callbacks.setStatus = [](std::string) {};
        callbacks.onBack = std::move(onBack);
        return callbacks;
    }

    void RefreshAudio(synth::runtime_ui::AudioPageSnapshot& snapshot)
    {
        juce::AudioDeviceManager& deviceManager = runtime_.DeviceManager();
        snapshot.showInputCombo = App::Config().numAudioInputs > 0;
        if (audioSyncPending_)
        {
            std::vector<std::string> outputNames;
            if (juce::AudioIODeviceType* deviceType = deviceManager.getCurrentDeviceTypeObject();
                deviceType != nullptr)
            {
                for (const juce::String& name : deviceType->getDeviceNames(false))
                {
                    outputNames.push_back(name.toStdString());
                }
            }

            std::vector<std::string> inputNames;
            if (snapshot.showInputCombo)
            {
                if (juce::AudioIODeviceType* deviceType = deviceManager.getCurrentDeviceTypeObject();
                    deviceType != nullptr)
                {
                    for (const juce::String& name : deviceType->getDeviceNames(true))
                    {
                        inputNames.push_back(name.toStdString());
                    }
                }
            }

            snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
                outputNames,
                {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
            snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
                inputNames,
                {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
            audioSyncPending_ = false;
        }

        const synth::AudioDeviceState state = runtime_.GetEngine().AudioDeviceSnapshot();
        snapshot.selectedOutputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
            state.outputDeviceName, snapshot.outputOptions, synth::runtime_ui::kSystemDefaultOptionId);
        snapshot.selectedInputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
            state.inputDeviceName, snapshot.inputOptions, synth::runtime_ui::kNoInputOptionId);

        if (juce::AudioIODevice* device = deviceManager.getCurrentAudioDevice(); device != nullptr)
        {
            snapshot.deviceLineText = synth::runtime_ui::Layout::BuildNegotiatedDeviceLine(
                device->getName().toStdString(),
                device->getCurrentSampleRate(),
                device->getCurrentBufferSizeSamples());
        }
        else
        {
            snapshot.deviceLineText = "No audio device";
        }

        if (audioStatus_.has_value())
        {
            snapshot.statusLineText = *audioStatus_;
        }
    }

    void DispatchAudio(const synth::ui::Action& action)
    {
        if (action.name == synth::runtime_ui::Actions::kAudioOutputSelect)
        {
            runtime_.ApplyAudioDeviceSelection(juce::String(
                synth::runtime_ui::Layout::DeviceNameFromOptionId(action.value)));
        }
        else if (action.name == synth::runtime_ui::Actions::kAudioInputSelect)
        {
            runtime_.ApplyAudioDeviceInputSelection(juce::String(
                synth::runtime_ui::Layout::DeviceNameFromOptionId(action.value)));
        }
    }

    void RefreshFile(synth::runtime_ui::FilePageSnapshot& snapshot)
    {
        fileService_.Refresh(snapshot);
    }

    void DispatchFile(const synth::ui::Action& action)
    {
        fileService_.Dispatch(action);
    }

    void RefreshControllers(synth::runtime_ui::ControllersPageSurface& surface)
    {
        surface.SetFocusGuard(focusGuard_);
        if (!wizardDiscoveryCache_.HasDeviceList())
        {
            wizardDiscoveryCache_.UpdateDeviceList(runtime_.MidiConnections().DeviceListSnapshot());
        }
        synth::MidiDeviceList changedDevices;
        if (runtime_.MidiConnections().ConsumeDeviceListChange(changedDevices))
        {
            wizardDiscoveryCache_.UpdateDeviceList(std::move(changedDevices));
        }
        surface.SetEnumerateDevices(wizardDiscoveryCache_.DeviceList());
        if (instrumentSnapshotDirty_)
        {
            wizardDiscoveryCache_.UpdateInstrumentSnapshot(runtime_.GetEngine().InstrumentSnapshot());
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
        return runtime_.GetEngine().SyncConfigurationSnapshot();
    }

    void RefreshSyncStatus(synth::runtime_ui::SyncPageStatus& status)
    {
        status = synth::runtime_ui::BuildSyncPageStatus(
            runtime_.GetEngine().ClockDiagnosticsSnapshot(),
            runtime_.GetEngine().InstrumentSnapshot());
    }

    bool CommitSyncConfiguration(const synth::SyncConfig& config)
    {
        return runtime_.GetEngine().RequestSyncConfiguration(config);
    }

    float DeadlineSamplePercent() const
    {
        return runtime_.DeadlineSamplePct();
    }

    void SaveRuntimeConfiguration()
    {
        runtime_.SaveRuntimeConfiguration();
    }

    void SetFocusGuard(std::function<bool()> guard)
    {
        focusGuard_ = std::move(guard);
    }

private:
    synth::runtime_ui::RuntimeFileCallbacks MakeFileCallbacks()
    {
        synth::runtime_ui::RuntimeFileCallbacks callbacks;
        callbacks.currentPatchDirectory = [this] {
            return runtime_.GetEngine().Patches().CurrentPatchDirectory();
        };
        callbacks.patchesRoot = [this] {
            return runtime_.DataPaths().patchesRoot;
        };
        callbacks.newPatch = [this] { runtime_.NewPatch(); };
        callbacks.savePatch = [this] { runtime_.SavePatch(); };
        callbacks.savePatchAs = [this](const std::filesystem::path& path) {
            runtime_.SavePatchAs(path);
        };
        callbacks.savePatchAsOverwrite = [this](const std::filesystem::path& path) {
            runtime_.SavePatchAsOverwrite(path);
        };
        callbacks.loadPatch = [this](const std::filesystem::path& path) {
            runtime_.LoadPatch(path);
        };
        callbacks.revertPatch = [this] { runtime_.RevertPatch(); };
        return callbacks;
    }

    Runtime<App>& runtime_;
    synth::runtime_ui::RuntimeFileService fileService_;
    std::function<bool()> focusGuard_;
    std::optional<std::string> audioStatus_;
    synth::ControllerWizardDiscoveryCache wizardDiscoveryCache_;
    bool audioSyncPending_ = true;
    bool controllersDirty_ = true;
    bool instrumentSnapshotDirty_ = true;
};

}  // namespace synth_runtime
