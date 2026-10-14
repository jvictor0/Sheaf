#pragma once

#include "Runtime.hpp"
#include "MidiConnectionManager.hpp"

#include "synth/ControllersPageBinding.hpp"
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
        , controllersBinding_(runtime_.GetEngine())
        , fileService_(MakeFileCallbacks())
    {
        runtime_.SetAudioStatusHook([this](const juce::String& text) {
            audioStatus_ = text.toStdString();
        });
        runtime_.SetAudioSyncHook([this] { audioSyncPending_ = true; });
        runtime_.SetMidiProcessorsRebuiltHook([this] {
            controllersBinding_.MarkInstrumentRebuilt();
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
        return controllersBinding_.MakeCallbacks(
            std::move(onBack),
            [this] { return runtime_.MidiConnections().State(); },
            [this] {
                return runtime_.SaveRuntimeConfiguration() ==
                       synth::RuntimeConfigFileStatus::Ok;
            });
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
        FeedControllersDeviceList(controllersBinding_, runtime_.MidiConnections());
        controllersBinding_.Refresh(surface);
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
        return synth::runtime_ui::MakeEngineFileCallbacks(runtime_.GetEngine(), &LogPatchCommand);
    }

    Runtime<App>& runtime_;
    synth::runtime_ui::ControllersPageBinding<synth::Engine<App>> controllersBinding_;
    synth::runtime_ui::RuntimeFileService fileService_;
    std::function<bool()> focusGuard_;
    std::optional<std::string> audioStatus_;
    bool audioSyncPending_ = true;
};

}  // namespace synth_runtime
