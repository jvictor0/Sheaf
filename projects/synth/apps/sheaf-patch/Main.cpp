#include "Launcher.hpp"
#include "Braid4Registration.hpp"
#include "HostDataPaths.hpp"
#include "LauncherWindow.hpp"
#include "MiniAppRegistration.hpp"
#include "OneSecondDelayRegistration.hpp"
#include "synth/ThreadId.hpp"

// Optional out-of-tree application supplied by the EXTRA_APP_* build variables.
#ifdef SHEAF_PATCH_EXTRA_APP_TYPE
#include SHEAF_PATCH_EXTRA_APP_HEADER
#endif

#include <juce_gui_extra/juce_gui_extra.h>

#include <exception>
#include <filesystem>
#include <memory>
#include <vector>

namespace synth_sheaf_patch {

class SheafPatchApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Sheaf Patch"; }
    const juce::String getApplicationVersion() override { return "0.1"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override {
        synth::SetCurrentThreadId(synth::ThreadId::Message);

        try {
            dataRoot_ = synth_runtime::SheafUserApplicationDataRoot();

            window_ = std::make_unique<synth_runtime::MainWindow>("Sheaf Patch");

            std::vector<synth::SynthAppRegistration> apps;
            apps.push_back(synth_miniapp::MakeMiniAppRegistration([this](synth::RuntimeDataPaths paths) {
                synth_runtime::LaunchRegisteredApp<synth_miniapp::MiniApp>(
                    *window_, activeSession_, std::move(paths));
            }));
            apps.push_back(synth_braid4::MakeBraid4Registration([this](synth::RuntimeDataPaths paths) {
                synth_runtime::LaunchRegisteredApp<synth_braid4::Braid4>(
                    *window_, activeSession_, std::move(paths));
            }));
            apps.push_back(synth_one_second_delay::MakeOneSecondDelayRegistration(
                [this](synth::RuntimeDataPaths paths) {
                    synth_runtime::LaunchRegisteredApp<synth_one_second_delay::OneSecondDelay>(
                        *window_, activeSession_, std::move(paths));
                }));
#ifdef SHEAF_PATCH_EXTRA_APP_TYPE
            apps.push_back(SHEAF_PATCH_EXTRA_APP_REGISTRAR([this](synth::RuntimeDataPaths paths) {
                synth_runtime::LaunchRegisteredApp<SHEAF_PATCH_EXTRA_APP_TYPE>(
                    *window_, activeSession_, std::move(paths));
            }));
#endif

            if (const auto* directLaunch = ResolveDirectLaunchApp(apps, commandLine.trim().toStdString());
                directLaunch != nullptr) {
                directLaunch->launch(synth::SheafPatchDataPathsForApp(dataRoot_, directLaunch->manifest.appId));
            } else {
                launcher_ = std::make_unique<LauncherComponent>(std::move(apps), dataRoot_);
                window_->ShowContent(*launcher_, 720, 420);
            }
        } catch (const std::exception& e) {
            INFO("SheafPatchApplication::initialise failed: %s", e.what());
            setApplicationReturnValue(1);
            quit();
        }
    }

    void shutdown() override {
        window_.reset();
        activeSession_.reset();
        launcher_.reset();
    }

    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted(const juce::String&) override {}

private:
    std::filesystem::path dataRoot_;
    std::unique_ptr<synth_runtime::MainWindow> window_;
    std::unique_ptr<LauncherComponent> launcher_;
    std::unique_ptr<synth_runtime::RuntimeSessionOwner> activeSession_;
};

}  // namespace synth_sheaf_patch

START_JUCE_APPLICATION(synth_sheaf_patch::SheafPatchApplication)
