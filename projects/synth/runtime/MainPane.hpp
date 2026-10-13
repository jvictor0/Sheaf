#pragma once

#include "synth/AppConcepts.hpp"
#include "synth/RuntimeMainComponent.hpp"

#include "JuceRuntimeMainServices.hpp"
#include "PortableJuceBackend.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

namespace synth_runtime {

template <synth::SynthApplication App>
class Runtime;

template <synth::SynthApplication App>
class MainPane : public juce::Component
{
public:
    enum class Page
    {
        None,
        Audio,
        Controllers,
        Sync,
        File,
        // Mirrors RuntimeMainComponent's RuntimeMainPage::AppPage
        // (see ToRuntimeMainPage/FromRuntimeMainPage below) the same way
        // every other page value does, regardless of whether the wrapped
        // App actually registers a page.
        AppPage
    };

    explicit MainPane(Runtime<App>& runtime)
        : runtime_(runtime)
        , services_(runtime_)
        , mainComponent_(runtime_.GetEngine().Application(), services_)
        , renderer_(mainComponent_)
    {
        services_.SetFocusGuard([this] { return renderer_.hasKeyboardFocus(true); });
        mainComponent_.SetActionHandler([this](const synth::ui::Action& action) {
            RefreshRendererAfterAction(action);
        });
        addAndMakeVisible(renderer_);
        RefreshOnTick();
    }

    ~MainPane() override
    {
        mainComponent_.SetActionHandler({});
        services_.SetFocusGuard({});
    }

    void ShowPage(Page page)
    {
        mainComponent_.ShowPage(ToRuntimeMainPage(page));
        renderer_.RefreshFromSurface();
    }

    Page CurrentPage() const
    {
        return FromRuntimeMainPage(mainComponent_.CurrentPage());
    }

    void RefreshOnTick()
    {
        mainComponent_.Refresh();
        renderer_.RefreshFromSurface();
    }

    void resized() override
    {
        // The pane's own bounds, offered as the content extent below via
        // OfferContentExtent() -- this is the ONLY thing a host with no
        // shell (the plugin editor, `app/vst/FroggersPluginEditor.hpp`) ever
        // offers, since nothing else calls SetContentExtent() for it. A
        // host with a shell (ShellComponent, sprs-20) also calls
        // OfferContentExtent() itself, with ITS OWN bounds, immediately
        // after every layout of the pane -- overriding whatever this call
        // offers, since the pane's own bounds are no longer the shell's once
        // the pane can be scaled or larger than the shell.
        renderer_.setBounds(getLocalBounds());
        OfferContentExtent(synth_juce::JuceToUiBounds(getLocalBounds().toFloat()));
    }

    // Offers `area` as the app surface's live content extent (see
    // RuntimeMainComponent::SetContentExtent()) and refreshes the renderer
    // against it. `area` is the CONTENT area a caller wants the app to
    // resolve against; the runtime sidebar's fixed width is subtracted here
    // (not by the caller) so every caller states its own bounds in the same
    // terms -- matching liveContentExtent_'s sidebar-free constructor-time
    // default (RuntimeMainComponent's constructor) and the composition's own
    // layout (content root sits at x 0, sidebar root at x == resolved app
    // width, in RuntimeMainComponent::BuildTree()). An area narrower than
    // the sidebar floors at width 0 rather than going negative, the same
    // inset-then-floor idiom already used for exactly this "extent minus a
    // fixed inset" shape elsewhere in this codebase (e.g.
    // `std::max(0.0f, containerExtent - padding * 2.0f)` in AllocateExtents,
    // ResolveCrossExtent, and IntrinsicForWrappingRow in
    // PortableUILayout.hpp) -- not a new clamping rule.
    void OfferContentExtent(synth::ui::Bounds area)
    {
        area.width = std::max(0.0f, area.width - synth::runtime_ui::Layout::kSidebarWidth);
        mainComponent_.SetContentExtent(area);
        renderer_.RefreshFromSurface();
    }

    synth::ui::Bounds IntrinsicBounds() const
    {
        return mainComponent_.IntrinsicBounds();
    }

    // The bounds of the composite root the last RefreshFromSurface() /
    // BuildTree() call produced (RuntimeMainComponent::ComposedBounds(),
    // sprs-19/20): what a host lays itself out around instead of always
    // IntrinsicBounds()'s compiled-in size.
    synth::ui::Bounds ComposedBounds() const
    {
        return mainComponent_.ComposedBounds();
    }

    bool NeedsDeferredRendererRefresh(const synth::ui::Action& action) const
    {
        return mainComponent_.NeedsDeferredDispatch(action);
    }

    bool HasDeferredRendererRefresh() const
    {
        return deferredRendererRefreshPending_;
    }

    void FlushDeferredRendererRefresh()
    {
        if (!deferredRendererRefreshPending_)
        {
            return;
        }
        deferredRendererRefreshPending_ = false;
        renderer_.RefreshFromSurface();
    }

private:
    void RefreshRendererAfterAction(const synth::ui::Action& action)
    {
        if (!NeedsDeferredRendererRefresh(action))
        {
            renderer_.RefreshFromSurface();
            return;
        }
        if (deferredRendererRefreshPending_)
        {
            return;
        }

        deferredRendererRefreshPending_ = true;
        juce::Component::SafePointer<MainPane<App>> safeThis(this);
        if (!juce::MessageManager::callAsync([safeThis] {
                if (safeThis != nullptr)
                {
                    safeThis->FlushDeferredRendererRefresh();
                }
            }))
        {
            // The message queue is shutting down. Do not synchronously rebuild
            // controls inside the active JUCE callback.
            deferredRendererRefreshPending_ = false;
        }
    }

    static synth::runtime_ui::RuntimeMainPage ToRuntimeMainPage(Page page)
    {
        switch (page) {
            case Page::Audio:
                return synth::runtime_ui::RuntimeMainPage::Audio;
            case Page::Controllers:
                return synth::runtime_ui::RuntimeMainPage::Controllers;
            case Page::Sync:
                return synth::runtime_ui::RuntimeMainPage::Sync;
            case Page::File:
                return synth::runtime_ui::RuntimeMainPage::File;
            case Page::AppPage:
                return synth::runtime_ui::RuntimeMainPage::AppPage;
            case Page::None:
                return synth::runtime_ui::RuntimeMainPage::Application;
        }
        return synth::runtime_ui::RuntimeMainPage::Application;
    }

    static Page FromRuntimeMainPage(synth::runtime_ui::RuntimeMainPage page)
    {
        switch (page) {
            case synth::runtime_ui::RuntimeMainPage::Audio:
                return Page::Audio;
            case synth::runtime_ui::RuntimeMainPage::Controllers:
                return Page::Controllers;
            case synth::runtime_ui::RuntimeMainPage::Sync:
                return Page::Sync;
            case synth::runtime_ui::RuntimeMainPage::File:
                return Page::File;
            case synth::runtime_ui::RuntimeMainPage::AppPage:
                return Page::AppPage;
            case synth::runtime_ui::RuntimeMainPage::Application:
                return Page::None;
        }
        return Page::None;
    }

    Runtime<App>& runtime_;
    JuceRuntimeMainServices<App> services_;
    synth::runtime_ui::RuntimeMainComponent<App, JuceRuntimeMainServices<App>> mainComponent_;
    synth_juce::PortableComponent renderer_;
    bool deferredRendererRefreshPending_ = false;
};

}  // namespace synth_runtime
