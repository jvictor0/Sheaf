#pragma once

// synth_runtime::MainWindow / LaunchRegisteredApp<App> -- the reusable
// top-level launch window and registered-app launch plumbing shared by any
// host process that offers one or more synth::SynthApplication types
// (Task 6, sprs-14/15). Hoisted out of apps/sheaf-patch/Main.cpp, where
// MainWindow and LaunchRegisteredApp<App> were a private nested class and a
// private member function of SheafPatchApplication, so an out-of-tree
// `main` can launch its registered app on the same window-and-launch code
// path the bundled sheaf-patch launcher uses instead of maintaining its own
// copy.
//
// MainWindow::ShowContent(component) derives the window's size from the
// component's own current bounds. For a synth_runtime::RuntimeShellSession's
// Component(), that is already its IntrinsicBounds() -- config extent plus
// the shared sidebar width (see RuntimeMainComponent.hpp's IntrinsicBounds()
// and MainPane.hpp's forwarding accessor) -- because RuntimeShellSession
// sizes its shell to that before handing it out. The explicit two-argument
// overload remains for content with no intrinsic size of its own, such as
// the sheaf-patch picker.

#include "Shell.hpp"

#include "synth/AppConcepts.hpp"
#include "synth/AppContext.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <exception>
#include <memory>
#include <utility>

namespace synth_runtime {

class MainWindow final : public juce::DocumentWindow {
public:
    explicit MainWindow(juce::String name)
        : DocumentWindow(std::move(name), juce::Colours::black, DocumentWindow::allButtons) {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setVisible(true);
    }

    // Sizes the window explicitly to `width`/`height`. Used for content
    // (like the sheaf-patch picker) that has no intrinsic size of its own.
    void ShowContent(juce::Component& component, int width, int height) {
        setContentNonOwned(&component, false);
        setSize(width, height);
        centreWithSize(width, height);
        setVisible(true);
    }

    // Sizes the window from `component`'s own current bounds rather than
    // any raw app-declared extent. Used for registered-app content (a
    // RuntimeShellSession's Component()), which already sizes itself to its
    // IntrinsicBounds() before this is called.
    void ShowContent(juce::Component& component) {
        ShowContent(component, component.getWidth(), component.getHeight());
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

// Launches `App` with `paths`, replacing `window`'s content and, only on
// success, `activeSession`. On failure (an exception during session
// construction), returns without touching `window` or `activeSession`,
// preserving whatever content/session was showing before the call.
template <synth::SynthApplication App>
void LaunchRegisteredApp(MainWindow& window,
                          std::unique_ptr<RuntimeSessionOwner>& activeSession,
                          synth::RuntimeDataPaths paths) {
    try {
        auto session = MakeRuntimeSessionOwner<App>(std::move(paths));
        const synth::RuntimeConfig config = App::Config();

        window.setName(juce::String(config.appName));
        window.ShowContent(session->Component());
        activeSession = std::move(session);
    } catch (const std::exception& e) {
        INFO("LaunchRegisteredApp failed: %s", e.what());
    }
}

}  // namespace synth_runtime
