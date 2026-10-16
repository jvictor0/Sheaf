#include "Launcher.hpp"

#include "Braid4Registration.hpp"
#include "FakeAudioDeviceType.hpp"
#include "MiniAppRegistration.hpp"
#include "Shell.hpp"
#include "synth/AppRegistry.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* label) {
    if (!condition) {
        throw std::runtime_error(label);
    }
}

synth::SynthAppRegistration TestRegistration(std::string appId,
                                             std::string displayName,
                                             std::string category) {
    // The harness constructs plain registrations to test launcher sorting
    // independently of the typed app concept gate.
    synth::SynthAppRegistration registration;
    registration.manifest.appId = std::move(appId);
    registration.manifest.displayName = std::move(displayName);
    registration.manifest.author = "Harness";
    registration.manifest.category = std::move(category);
    registration.manifest.hardware.minEncoders = 2;
    registration.launch = [](synth::RuntimeDataPaths) {};
    return registration;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juce;

    {
        using MiniAppOwnerFactoryResult =
            decltype(synth_runtime::MakeRuntimeSessionOwner<synth_miniapp::MiniApp>(
                std::declval<synth::RuntimeDataPaths>()));
        static_assert(std::is_same_v<MiniAppOwnerFactoryResult,
                                     std::unique_ptr<synth_runtime::RuntimeSessionOwner>>);

        std::function<std::unique_ptr<synth_runtime::RuntimeSessionOwner>(synth::RuntimeDataPaths)>
            miniappOwnerFactory = [](synth::RuntimeDataPaths paths) {
                // A real device type would otherwise be created here (JUCE
                // only creates the real platform types when its list is
                // still empty), so a fake one is registered before Start()
                // via MakeRuntimeSessionOwner's beforeStart hook.
                return synth_runtime::MakeRuntimeSessionOwner<synth_miniapp::MiniApp>(
                    std::move(paths), &synth_juce::RegisterFakeAudioDeviceType<synth_miniapp::MiniApp>);
            };

        auto owner = miniappOwnerFactory(synth::RuntimeDataPaths::FromDataRoot(
            std::filesystem::temp_directory_path() / "sheaf-patch-launcher-owner-test"));
        Require(owner != nullptr,
                "miniapp registration can construct through the generic runtime session owner factory");
        Require(dynamic_cast<synth_runtime::ShellComponent<synth_miniapp::MiniApp>*>(&owner->Component()) != nullptr,
                "miniapp registration owner exposes a component through the generic interface");

        // This launcher path must never reach a real platform audio device
        // type -- the same guard RuntimeShellSessionTests.cpp applies to its
        // own session construction.
        auto* miniappOwner = dynamic_cast<synth_runtime::RuntimeSessionOwnerFor<synth_miniapp::MiniApp>*>(owner.get());
        Require(miniappOwner != nullptr, "miniapp owner is the concrete RuntimeSessionOwnerFor");
        juce::AudioIODeviceType* miniappDeviceType = miniappOwner->GetRuntime().DeviceManager().getCurrentDeviceTypeObject();
        Require(miniappDeviceType != nullptr, "the miniapp launcher owner has a current audio device type");
        Require(miniappDeviceType->getTypeName() == synth_juce::FakeAudioDeviceType::kTypeName,
                "the miniapp launcher owner's current audio device type is the fake one, not a real platform type");
    }

    {
        const auto manifest = synth_braid4::Braid4Manifest();
        Require(manifest.appId == "braid-4", "braid manifest exposes stable app id");
        Require(manifest.displayName == "Braid 4", "braid manifest exposes display name");
        Require(manifest.author == "Sheaf", "braid manifest exposes author");
        Require(manifest.category == "synth", "braid manifest exposes category");
        Require(manifest.hardware.minEncoders == 16, "braid manifest exposes minimum encoders");
    }

    {
        using Braid4OwnerFactoryResult =
            decltype(synth_runtime::MakeRuntimeSessionOwner<synth_braid4::Braid4>(
                std::declval<synth::RuntimeDataPaths>()));
        static_assert(std::is_same_v<Braid4OwnerFactoryResult,
                                     std::unique_ptr<synth_runtime::RuntimeSessionOwner>>);

        std::function<std::unique_ptr<synth_runtime::RuntimeSessionOwner>(synth::RuntimeDataPaths)>
            braidOwnerFactory = [](synth::RuntimeDataPaths paths) {
                return synth_runtime::MakeRuntimeSessionOwner<synth_braid4::Braid4>(
                    std::move(paths), &synth_juce::RegisterFakeAudioDeviceType<synth_braid4::Braid4>);
            };

        auto owner = braidOwnerFactory(synth::RuntimeDataPaths::FromDataRoot(
            std::filesystem::temp_directory_path() / "sheaf-patch-braid-owner-test"));
        Require(owner != nullptr,
                "braid registration can construct through the generic runtime session owner factory");
        Require(dynamic_cast<synth_runtime::ShellComponent<synth_braid4::Braid4>*>(&owner->Component()) != nullptr,
                "braid registration owner exposes a component through the generic interface");

        auto* braidOwner = dynamic_cast<synth_runtime::RuntimeSessionOwnerFor<synth_braid4::Braid4>*>(owner.get());
        Require(braidOwner != nullptr, "braid owner is the concrete RuntimeSessionOwnerFor");
        juce::AudioIODeviceType* braidDeviceType = braidOwner->GetRuntime().DeviceManager().getCurrentDeviceTypeObject();
        Require(braidDeviceType != nullptr, "the braid launcher owner has a current audio device type");
        Require(braidDeviceType->getTypeName() == synth_juce::FakeAudioDeviceType::kTypeName,
                "the braid launcher owner's current audio device type is the fake one, not a real platform type");
    }

    {
        std::vector<synth::SynthAppRegistration> apps;
        apps.push_back(TestRegistration("zeta", "Zeta", "test"));
        apps.push_back(TestRegistration("alpha", "Alpha", "tools"));

        synth_sheaf_patch::LauncherComponent launcher(std::move(apps), "data");

        Require(launcher.AppCountForTesting() == 2, "launcher exposes both apps");
        Require(launcher.AppIdForTesting(0) == "alpha", "launcher sorts rows by stable app id");
        Require(launcher.AppIdForTesting(1) == "zeta", "launcher keeps later sorted app id");
        Require(launcher.RowTextForTesting("alpha") ==
                    "Alpha | Author: Harness | Category: tools | Minimum encoders: 2",
                "row shows formatted metadata");
    }

    {
        bool launched = false;
        synth::RuntimeDataPaths launchedPaths;
        auto miniapp = synth_miniapp::MakeMiniAppRegistration([&](synth::RuntimeDataPaths paths) {
            launched = true;
            launchedPaths = std::move(paths);
        });

        synth_sheaf_patch::LauncherComponent launcher({std::move(miniapp)}, "data");
        auto* row = launcher.RowButtonForTesting("miniapp");

        Require(row != nullptr, "miniapp row button exists");
        Require(launcher.RowTextForTesting("miniapp") ==
                    "Mini App | Author: Sheaf | Category: test | Minimum encoders: 16",
                "miniapp row shows formatted metadata");

        row->onClick();

        Require(launched, "row activation invokes selected launch binding");
        Require(launchedPaths.configFile == std::filesystem::path("data/synth/sheaf-patch/config"),
                "launcher passes shared Sheaf Patch config path");
        Require(launchedPaths.patchesRoot == std::filesystem::path("data/synth/sheaf-patch/patches/miniapp"),
                "launcher passes selected app patch root");
    }

    {
        bool braidLaunched = false;
        synth::RuntimeDataPaths braidPaths;
        auto braid = synth_braid4::MakeBraid4Registration([&](synth::RuntimeDataPaths paths) {
            braidLaunched = true;
            braidPaths = std::move(paths);
        });
        auto miniapp = synth_miniapp::MakeMiniAppRegistration([](synth::RuntimeDataPaths) {});

        synth_sheaf_patch::LauncherComponent launcher({std::move(miniapp), std::move(braid)}, "data");
        auto* row = launcher.RowButtonForTesting("braid-4");

        Require(launcher.AppCountForTesting() == 2, "sheaf patch launcher exposes miniapp and braid");
        Require(launcher.AppIdForTesting(0) == "braid-4", "sheaf patch launcher sorts braid before miniapp");
        Require(launcher.AppIdForTesting(1) == "miniapp", "sheaf patch launcher keeps miniapp after braid");
        Require(row != nullptr, "braid row button exists");
        Require(launcher.RowTextForTesting("braid-4") ==
                    "Braid 4 | Author: Sheaf | Category: synth | Minimum encoders: 16",
                "braid row shows formatted metadata");

        row->onClick();

        Require(braidLaunched, "braid row activation invokes selected launch binding");
        Require(braidPaths.configFile == std::filesystem::path("data/synth/sheaf-patch/config"),
                "launcher passes shared Sheaf Patch config path for braid");
        Require(braidPaths.patchesRoot == std::filesystem::path("data/synth/sheaf-patch/patches/braid-4"),
                "launcher passes braid app patch root");
    }

    {
        std::vector<synth::SynthAppRegistration> apps;
        apps.push_back(TestRegistration("alpha", "Alpha", "tools"));
        apps.push_back(TestRegistration("zeta", "Zeta", "test"));

        const auto* known = synth_sheaf_patch::ResolveDirectLaunchApp(apps, "zeta");
        Require(known != nullptr, "known appId resolves to a registration");
        Require(known == &apps[1], "known appId resolves to the matching registration, not a copy");
        Require(known->manifest.appId == "zeta", "resolved registration has the requested appId");

        const auto* unknown = synth_sheaf_patch::ResolveDirectLaunchApp(apps, "not-a-registered-app");
        Require(unknown == nullptr, "unknown appId falls back to the picker sentinel");

        const auto* empty = synth_sheaf_patch::ResolveDirectLaunchApp(apps, "");
        Require(empty == nullptr, "empty argument falls back to the picker sentinel");
    }

    std::cout << "LauncherHarnessTests passed\n";
    return 0;
}
