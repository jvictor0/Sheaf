#include "synth/RuntimeFileService.hpp"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

class Harness
{
public:
    synth::runtime_ui::RuntimeFileService MakeService()
    {
        synth::runtime_ui::RuntimeFileCallbacks callbacks;
        callbacks.currentPatchDirectory = [this] { return currentPatchDirectory; };
        callbacks.patchesRoot = [this] { return root; };
        callbacks.newPatch = [this] { calls.push_back("new"); };
        callbacks.savePatch = [this] { calls.push_back("save"); };
        callbacks.savePatchAs = [this](const std::filesystem::path& path) {
            calls.push_back("save_as:" + path.string());
        };
        callbacks.savePatchAsOverwrite = [this](const std::filesystem::path& path) {
            calls.push_back("overwrite_save_as:" + path.string());
        };
        callbacks.loadPatch = [this](const std::filesystem::path& path) {
            calls.push_back("load:" + path.string());
        };
        return synth::runtime_ui::RuntimeFileService(std::move(callbacks));
    }

    std::filesystem::path root = "/tmp/sheaf-patches";
    std::optional<std::filesystem::path> currentPatchDirectory;
    std::vector<std::string> calls;
};

void TestRefreshProjectsPatchState()
{
    Harness harness;
    synth::runtime_ui::RuntimeFileService service = harness.MakeService();

    synth::runtime_ui::FilePageSnapshot snapshot;
    service.Refresh(snapshot);
    Require(!snapshot.hasCurrentPatch, "initial snapshot has no patch");
    Require(snapshot.patchNameText == "(no patch)", "initial patch name");
    Require(snapshot.patchesRoot == "/tmp/sheaf-patches", "patches root projected");

    harness.currentPatchDirectory = harness.root / "Patch A";
    service.Refresh(snapshot);
    Require(snapshot.hasCurrentPatch, "current patch is projected");
    Require(snapshot.patchNameText == "Patch A", "current patch name projected");
}

void TestDispatchMapsDirectAndConfirmedActions()
{
    Harness harness;
    synth::runtime_ui::RuntimeFileService service = harness.MakeService();
    synth::runtime_ui::FilePageSnapshot snapshot;

    service.Dispatch(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileNew));
    service.Refresh(snapshot);
    Require(harness.calls.back() == "new", "new patch operation");
    Require(snapshot.statusText == "New patch created", "new status");

    service.Dispatch(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSave));
    service.Refresh(snapshot);
    Require(harness.calls.back() == "save", "save patch operation");
    Require(snapshot.statusText == "Save requested", "save status");

    service.Dispatch(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kFileConfirmedSaveAs,
        "/tmp/sheaf-patches/Patch B"));
    service.Refresh(snapshot);
    Require(harness.calls.back() == "save_as:/tmp/sheaf-patches/Patch B",
            "save as patch operation");
    Require(snapshot.statusText == "Save As requested: /tmp/sheaf-patches/Patch B",
            "save as status");

    service.Dispatch(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kFileConfirmedOverwriteSaveAs,
        "/tmp/sheaf-patches/Patch C"));
    service.Refresh(snapshot);
    Require(harness.calls.back() == "overwrite_save_as:/tmp/sheaf-patches/Patch C",
            "overwrite save as patch operation");
    Require(snapshot.statusText == "Save As requested: /tmp/sheaf-patches/Patch C",
            "overwrite save as status");

    service.Dispatch(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kFileConfirmedLoad,
        "/tmp/sheaf-patches/Patch D"));
    service.Refresh(snapshot);
    Require(harness.calls.back() == "load:/tmp/sheaf-patches/Patch D",
            "load patch operation");
    Require(snapshot.statusText == "Load requested: /tmp/sheaf-patches/Patch D",
            "load status");
}

void TestRawBrowserActionsRemainSurfaceOwned()
{
    Harness harness;
    synth::runtime_ui::RuntimeFileService service = harness.MakeService();

    service.Dispatch(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSaveAs));
    service.Dispatch(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));

    Require(harness.calls.empty(), "raw Save As and Load do not invoke patch operations");
}

}  // namespace

int main()
{
    TestRefreshProjectsPatchState();
    TestDispatchMapsDirectAndConfirmedActions();
    TestRawBrowserActionsRemainSurfaceOwned();
    return 0;
}
