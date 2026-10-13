#pragma once

#include "synth/PatchPersistence.hpp"
#include "synth/RuntimePages.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace synth::runtime_ui {

struct RuntimeFileCallbacks
{
    std::function<std::optional<std::filesystem::path>()> currentPatchDirectory;
    std::function<std::filesystem::path()> patchesRoot;
    std::function<void()> newPatch;
    std::function<void()> savePatch;
    std::function<void(const std::filesystem::path&)> savePatchAs;
    std::function<void(const std::filesystem::path&)> savePatchAsOverwrite;
    std::function<void(const std::filesystem::path&)> loadPatch;
};

class RuntimeFileService final
{
public:
    explicit RuntimeFileService(RuntimeFileCallbacks callbacks)
        : callbacks_(std::move(callbacks))
    {
    }

    void Refresh(FilePageSnapshot& snapshot) const
    {
        const std::optional<std::filesystem::path> currentPatchDirectory =
            callbacks_.currentPatchDirectory();
        snapshot.hasCurrentPatch = currentPatchDirectory.has_value();
        snapshot.patchNameText = currentPatchDirectory.has_value()
                                     ? currentPatchDirectory->filename().string()
                                     : "(no patch)";
        snapshot.patchesRoot = callbacks_.patchesRoot().string();
        if (fileStatus_.has_value())
        {
            snapshot.statusText = *fileStatus_;
        }
    }

    void Dispatch(const ui::Action& action)
    {
        if (action.name == Actions::kFileNew)
        {
            callbacks_.newPatch();
            fileStatus_ = "New patch created";
        }
        else if (action.name == Actions::kFileSave)
        {
            callbacks_.savePatch();
            fileStatus_ = "Save requested";
        }
        else if (action.name == Actions::kFileConfirmedSaveAs)
        {
            callbacks_.savePatchAs(std::filesystem::path(action.value));
            fileStatus_ = "Save As requested: " + action.value;
        }
        else if (action.name == Actions::kFileConfirmedOverwriteSaveAs)
        {
            callbacks_.savePatchAsOverwrite(std::filesystem::path(action.value));
            fileStatus_ = "Save As requested: " + action.value;
        }
        else if (action.name == Actions::kFileConfirmedLoad)
        {
            callbacks_.loadPatch(std::filesystem::path(action.value));
            fileStatus_ = "Load requested: " + action.value;
        }
    }

private:
    RuntimeFileCallbacks callbacks_;
    std::optional<std::string> fileStatus_;
};

// Binds RuntimeFileCallbacks straight to an engine, shared by
// JuceRuntimeMainServices, BrowserRuntimeMainServices and a plugin host. New
// and Load go through Engine::NewPatch/Engine::LoadPatch so a host that
// records a reopen version keeps recording one; Save, Save As and its
// overwrite go straight to Engine::Patches() since neither changes what the
// host reopens. onCommand, when given, is handed each command's name and its
// PatchCommandResult right after the call that produced it, for a host that
// logs; the JUCE hosts pass synth_runtime::LogPatchCommand for this.
template <typename EngineType>
RuntimeFileCallbacks MakeEngineFileCallbacks(
    EngineType& engine, std::function<void(const char*, const PatchCommandResult&)> onCommand = {})
{
    // Every command below reports through this one call rather than
    // repeating the same if(onCommand) guard five times.
    const auto notify = [onCommand](const char* action, const PatchCommandResult& result) {
        if (onCommand)
        {
            onCommand(action, result);
        }
    };

    RuntimeFileCallbacks callbacks;
    callbacks.currentPatchDirectory = [&engine] {
        return engine.Patches().CurrentPatchDirectory();
    };
    callbacks.patchesRoot = [&engine] {
        return engine.DataPaths().patchesRoot;
    };
    callbacks.newPatch = [&engine, notify] {
        notify("NewPatch", engine.NewPatch());
    };
    callbacks.savePatch = [&engine, notify] {
        notify("SavePatch", engine.Patches().SavePatch());
    };
    callbacks.savePatchAs = [&engine, notify](const std::filesystem::path& path) {
        notify("SavePatchAs", engine.Patches().SavePatchAs(path));
    };
    callbacks.savePatchAsOverwrite = [&engine, notify](const std::filesystem::path& path) {
        notify("SavePatchAsOverwrite", engine.Patches().SavePatchAsOverwrite(path));
    };
    callbacks.loadPatch = [&engine, notify](const std::filesystem::path& path) {
        notify("LoadPatch", engine.LoadPatch(path));
    };
    return callbacks;
}

}  // namespace synth::runtime_ui
