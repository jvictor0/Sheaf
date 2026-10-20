#pragma once

#include "synth/AppConcepts.hpp"
#include "synth/AppContext.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace synth_browser::test {

class FakeBrowserSurface final : public synth::ui::Surface {
public:
    // The root extent must match what the app's `Config()` declares: the shell
    // sizes the host from the declared surface and the app publishes its own
    // root, and a disagreement between the two aborts the runtime at startup.
    // Found the hard way while adding the taller fixture below -- this was
    // hardcoded at 480 while `Config()` said otherwise, and every height except
    // 480 died in `Aborted(undefined)` before rendering a node.
    explicit FakeBrowserSurface(float height) : height_(height) {}

    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Builder builder;
        builder.Root("fake-browser-root", {0.0f, 0.0f, 640.0f, height_})
            .Button("fake-browser-button", "Trigger", synth::ui::Action::Named("fake.trigger"), {})
            .Slider("fake-browser-slider", "Level", 0.5f, 0.0f, 1.0f, 0.001f,
                    synth::ui::Action::Named("fake.level"), {})
            .DrawInteractive(
                "fake-browser-draw", {24.0f, 120.0f, 320.0f, 120.0f},
                {synth::ui::DrawCommand::Fill(synth::Color::Rgb(20, 24, 32)),
                 synth::ui::DrawCommand::Line({0.0f, 60.0f}, {320.0f, 60.0f},
                                              synth::Color::Rgb(96, 220, 180), 2.0f)},
                synth::ui::Action::WithValue("fake.drag", "axis:0"),
                synth::ui::Action::Named("fake.double_click"),
                {})
            .StatusText("fake-browser-action-status",
                        "Actions: " + std::to_string(actionCount_) + " " + lastActionName_, {})
            .Button("fake-browser-export", "Export", synth::ui::Action::Named("fake.export"), {});
        return builder.Build({0.0f, 0.0f, 640.0f, height_});
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        ++actionCount_;
        lastActionName_ = action.name;
        if (action.name == "fake.export")
        {
            pendingExport_ = synth::FileExport{
                .fileName = "fixture-export.txt",
                .mediaType = "text/plain",
                .bytes = std::vector<std::uint8_t>{'f', 'i', 'x', 't', 'u', 'r', 'e', ' ', 'e', 'x', 'p',
                                                    'o', 'r', 't', '\n'},
                .note = "",
            };
        }
        if (handler_)
        {
            handler_(action);
        }
    }

    std::optional<synth::FileExport> TakePendingFileExport()
    {
        std::optional<synth::FileExport> fileExport = std::move(pendingExport_);
        pendingExport_.reset();
        return fileExport;
    }

private:
    float height_;
    std::size_t actionCount_ = 0;
    std::string lastActionName_;
    ActionHandler handler_;
    std::optional<synth::FileExport> pendingExport_;
};

class FakeBrowserApp {
public:
    static constexpr int kUiHeight = 480;

    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{
            .appName = "FakeBrowserApp",
            .uiWidth = 640,
            .uiHeight = kUiHeight,
        };
    }

    FakeBrowserApp() : FakeBrowserApp(static_cast<float>(kUiHeight)) {}

    void Init(synth::AppContext*) {}

    void ProcessBlock(synth::AudioBlock& block)
    {
        constexpr float kSampleRate = 48000.0f;
        constexpr float kFrequencyHz = 440.0f;
        constexpr float kTwoPi = 6.28318530717958647692f;
        const float phaseIncrement = kTwoPi * kFrequencyHz / kSampleRate;

        for (std::size_t frame = 0; frame < block.numFrames; ++frame)
        {
            const float sample = 0.2f * std::sin(phase_);
            for (int channel = 0; channel < block.numOutputChannels; ++channel)
            {
                if (block.outputs != nullptr && block.outputs[channel] != nullptr)
                {
                    block.outputs[channel][frame] = sample;
                }
            }
            phase_ += phaseIncrement;
            if (phase_ >= kTwoPi)
            {
                phase_ -= kTwoPi;
            }
        }
    }

    synth::ui::Surface& PortableSurface()
    {
        return surface_;
    }

    std::optional<synth::FileExport> TakePendingFileExport()
    {
        return surface_.TakePendingFileExport();
    }

protected:
    explicit FakeBrowserApp(float surfaceHeight) : surface_(surfaceHeight) {}

private:
    float phase_ = 0.0f;
    FakeBrowserSurface surface_;
};

// The same fixture app, declaring a TALLER surface, and it exists for exactly
// one reason: sru-48 requires the criteria to be evaluated at a second root
// extent in the RENDERED DOM, and nothing a test can do to a browser window
// changes the root extent of this shell.
//
// The chain is worth stating because it is not obvious and it defeated the
// first attempt at this test. A page's content bounds are set once, in
// `RuntimeMainComponent`'s constructor, from `App::Config().uiWidth/uiHeight`
// -- a per-app compile-time declaration. `BrowserUiBackend.fitSurface` then
// applies a shrink-only *width* scale and sizes the host element from the
// surface's own height. So growing the viewport re-fits an already-resolved
// surface; it never asks the resolver for a different root extent. A test that
// resizes the window and compares measurements is comparing a layout to itself.
//
// Varying `uiHeight` is therefore the only honest way to put the same page
// producer in front of the resolver at two root extents and compare what the
// backend built. 720 is chosen to be well clear of 480 and still inside the
// verification viewport, so neither surface is scaled.
class TallFakeBrowserApp : public FakeBrowserApp {
public:
    static constexpr int kUiHeight = 720;

    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config = FakeBrowserApp::Config();
        config.appName = "TallFakeBrowserApp";
        config.uiHeight = kUiHeight;
        return config;
    }

    // The declared height has to reach the surface too, not just the config --
    // see `FakeBrowserSurface`'s constructor.
    TallFakeBrowserApp() : FakeBrowserApp(static_cast<float>(kUiHeight)) {}
};

}  // namespace synth_browser::test
