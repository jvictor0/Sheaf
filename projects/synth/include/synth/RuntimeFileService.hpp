#pragma once

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

}  // namespace synth::runtime_ui
