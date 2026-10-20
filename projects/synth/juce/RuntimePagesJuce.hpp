#pragma once

// JUCE desktop backend for runtime page portable surfaces (OpenSpec tasks 4.1–4.3).

#include "synth/RuntimePages.hpp"

#include "PortableJuceBackend.hpp"
#include "Runtime.hpp"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace synth_runtime {

template <synth::SynthApplication App>
class Runtime;

class SidebarHost final : public juce::Component
{
public:
    SidebarHost()
        : m_renderer(m_surface)
    {
        m_surface.SetActionHandler([this](const synth::ui::Action& action) {
            if (onAction)
            {
                onAction(action);
            }
        });
        addAndMakeVisible(m_renderer);
        m_renderer.RefreshFromSurface();
    }

    void SetDeadlinePercent(float percent)
    {
        m_surface.SetDeadlinePercent(percent);
        m_renderer.RefreshFromSurface();
    }

    void RefreshFromSurface()
    {
        m_renderer.RefreshFromSurface();
    }

    void resized() override
    {
        m_renderer.setBounds(getLocalBounds());
        m_renderer.RefreshFromSurface();
    }

    std::function<void(const synth::ui::Action&)> onAction;

private:
    synth::runtime_ui::SidebarSurface m_surface;
    synth_juce::PortableComponent m_renderer;
};

template <synth::SynthApplication App>
class AudioPageHost final : public juce::Component
{
public:
    explicit AudioPageHost(Runtime<App>& runtime)
        : m_runtime(runtime)
        , m_renderer(m_surface)
    {
        m_surface.Snapshot().showInputCombo = App::Config().numAudioInputs > 0;

        m_surface.SetActionHandler([this](const synth::ui::Action& action) {
            HandleAction(action);
        });

        addAndMakeVisible(m_renderer);

        m_runtime.SetAudioStatusHook([this](const juce::String& text) { SetStatus(text); });
        m_runtime.SetAudioSyncHook([this] { Refresh(); });

        Refresh();
    }

    ~AudioPageHost() override
    {
        m_runtime.SetAudioStatusHook({});
        m_runtime.SetAudioSyncHook({});
    }

    AudioPageHost(const AudioPageHost&) = delete;
    AudioPageHost& operator=(const AudioPageHost&) = delete;

    void RefreshOnTick()
    {
        RefreshStatus();
        m_renderer.RefreshFromSurface();
    }

    void Refresh()
    {
        juce::AudioDeviceManager& deviceManager = m_runtime.DeviceManager();

        m_outputNames.clear();
        std::vector<std::string> outputNames;
        if (juce::AudioIODeviceType* deviceType = deviceManager.getCurrentDeviceTypeObject();
            deviceType != nullptr)
        {
            const juce::StringArray names = deviceType->getDeviceNames(false);
            for (const juce::String& name : names)
            {
                outputNames.push_back(name.toStdString());
            }
        }
        m_outputNames = outputNames;

        m_inputNames.clear();
        std::vector<std::string> inputNames;
        if (m_surface.Snapshot().showInputCombo)
        {
            if (juce::AudioIODeviceType* deviceType = deviceManager.getCurrentDeviceTypeObject();
                deviceType != nullptr)
            {
                const juce::StringArray names = deviceType->getDeviceNames(true);
                for (const juce::String& name : names)
                {
                    inputNames.push_back(name.toStdString());
                }
            }
        }
        m_inputNames = inputNames;

        synth::runtime_ui::AudioPageSnapshot& snapshot = m_surface.Snapshot();
        snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
            m_outputNames,
            {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
        snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
            m_inputNames,
            {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
        SyncSelection();
        RefreshStatus();
        m_renderer.RefreshFromSurface();
    }

    void SyncSelection()
    {
        const synth::AudioDeviceState state = m_runtime.GetEngine().AudioDeviceSnapshot();
        synth::runtime_ui::AudioPageSnapshot& snapshot = m_surface.Snapshot();
        snapshot.selectedOutputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
            state.outputDeviceName, snapshot.outputOptions, synth::runtime_ui::kSystemDefaultOptionId);
        snapshot.selectedInputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
            state.inputDeviceName, snapshot.inputOptions, synth::runtime_ui::kNoInputOptionId);
    }

    void SetStatus(const juce::String& text)
    {
        m_surface.SetStatusLine(text.toStdString());
        m_renderer.RefreshFromSurface();
    }

    void RefreshStatus()
    {
        juce::AudioIODevice* device = m_runtime.DeviceManager().getCurrentAudioDevice();
        synth::runtime_ui::AudioPageSnapshot& snapshot = m_surface.Snapshot();
        if (device == nullptr)
        {
            snapshot.deviceLineText = "No audio device";
            return;
        }
        snapshot.deviceLineText = synth::runtime_ui::Layout::BuildNegotiatedDeviceLine(
            device->getName().toStdString(),
            device->getCurrentSampleRate(),
            device->getCurrentBufferSizeSamples());
    }

    void resized() override
    {
        const auto area = getLocalBounds();
        m_surface.SetContentBounds(synth_juce::JuceToUiBounds(area.toFloat()));
        m_renderer.setBounds(area);
        m_renderer.RefreshFromSurface();
    }

    std::function<void()> onBack;

private:
    void HandleAction(const synth::ui::Action& action)
    {
        if (action.name == synth::runtime_ui::Actions::kAudioBack)
        {
            if (onBack)
            {
                onBack();
            }
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kAudioOutputSelect)
        {
            const juce::String outputName =
                juce::String(synth::runtime_ui::Layout::DeviceNameFromOptionId(action.value));
            m_runtime.ApplyAudioDeviceSelection(outputName);
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kAudioInputSelect)
        {
            const juce::String inputName =
                juce::String(synth::runtime_ui::Layout::DeviceNameFromOptionId(action.value));
            m_runtime.ApplyAudioDeviceInputSelection(inputName);
        }
    }

    Runtime<App>& m_runtime;
    synth::runtime_ui::AudioPageSurface m_surface;
    synth_juce::PortableComponent m_renderer;
    std::vector<std::string> m_outputNames;
    std::vector<std::string> m_inputNames;
};

template <synth::SynthApplication App>
class FilePageHost final : public juce::Component
{
public:
    explicit FilePageHost(Runtime<App>& runtime)
        : m_runtime(runtime)
        , m_renderer(m_surface)
    {
        m_surface.SetActionHandler([this](const synth::ui::Action& action) {
            HandleAction(action);
        });
        addAndMakeVisible(m_renderer);
        RefreshPatchNameLabel();
        m_renderer.RefreshFromSurface();
    }

    FilePageHost(const FilePageHost&) = delete;
    FilePageHost& operator=(const FilePageHost&) = delete;

    void RefreshOnTick()
    {
        RefreshPatchNameLabel();
        m_renderer.RefreshFromSurface();
    }

    void resized() override
    {
        const auto area = getLocalBounds();
        m_surface.SetContentBounds(synth_juce::JuceToUiBounds(area.toFloat()));
        m_renderer.setBounds(area);
        m_renderer.RefreshFromSurface();
    }

    std::function<void()> onBack;

private:
    void HandleAction(const synth::ui::Action& action)
    {
        if (action.name == synth::runtime_ui::Actions::kFileBack)
        {
            if (onBack)
            {
                onBack();
            }
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileNew)
        {
            m_runtime.NewPatch();
            SetStatus("New patch created");
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileSave)
        {
            RefreshPatchNameLabel();
            m_runtime.SavePatch();
            SetStatus("Save requested");
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileConfirmedSaveAs)
        {
            const std::filesystem::path path(action.value);
            m_runtime.SavePatchAs(path);
            SetStatus("Save As requested: " + action.value);
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileConfirmedOverwriteSaveAs)
        {
            const std::filesystem::path path(action.value);
            m_runtime.SavePatchAsOverwrite(path);
            SetStatus("Save As requested: " + action.value);
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileConfirmedLoad)
        {
            const std::filesystem::path path(action.value);
            m_runtime.LoadPatch(path);
            SetStatus("Load requested: " + action.value);
            return;
        }

        if (action.name == synth::runtime_ui::Actions::kFileRevert)
        {
            m_runtime.RevertPatch();
            SetStatus("Revert requested");
        }
    }

    void SetStatus(const juce::String& text)
    {
        m_surface.SetStatus(text.toStdString());
        m_renderer.RefreshFromSurface();
    }

    void RefreshPatchNameLabel()
    {
        const auto& currentPatchDirectory = m_runtime.GetEngine().Patches().CurrentPatchDirectory();
        synth::runtime_ui::FilePageSnapshot& snapshot = m_surface.Snapshot();
        snapshot.hasCurrentPatch = currentPatchDirectory.has_value();
        snapshot.patchNameText = currentPatchDirectory.has_value()
                                     ? currentPatchDirectory->filename().string()
                                     : "(no patch)";
        snapshot.patchesRoot = m_runtime.DataPaths().patchesRoot.string();
    }

    Runtime<App>& m_runtime;
    synth::runtime_ui::FilePageSurface m_surface;
    synth_juce::PortableComponent m_renderer;
};

}  // namespace synth_runtime
