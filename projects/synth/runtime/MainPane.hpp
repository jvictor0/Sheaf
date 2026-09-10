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
        // sprs-17: mirrors RuntimeMainComponent's RuntimeMainPage::AppPage
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
        // Task 8 fix round 1 (sprs-13 finding 1): feed the pane's live JUCE
        // bounds to the shell before the next RefreshFromSurface() rebuilds
        // the tree, so an ExtentAwareSurface app is offered the real window
        // size instead of only ever resolving at its compiled-in default.
        // mainComponent_ is held directly (not through a `ui::Surface&`), so
        // this calls its existing public SetContentExtent() setter (task
        // 8.1) with no dynamic_cast/interface needed at this layer.
        //
        // Fix round 2 (sprs-13 Task 8 re-review): the pane's own bounds are
        // the composite footprint (app content + the runtime sidebar strip
        // placed beside it, RuntimeMainComponent.hpp's BuildTree()), not the
        // app's content area alone. Offering the full pane width let an
        // extent-aware app resolve as wide as the whole pane, which then
        // placed the sidebar at that same width -- past the pane's own
        // right edge, clipped off-screen. The offered extent is the app
        // CONTENT area: pane bounds with the sidebar's width subtracted,
        // height unchanged -- matching liveContentExtent_'s sidebar-free
        // constructor-time default (RuntimeMainComponent.hpp:64-73) and the
        // composition's own layout (content root sits at x 0, sidebar root
        // at x == resolved app width, RuntimeMainComponent.hpp:156). A pane
        // narrower than the sidebar floors at width 0 rather than going
        // negative, the same inset-then-floor idiom already used for
        // exactly this "extent minus a fixed inset" shape elsewhere in this
        // codebase (e.g. `std::max(0.0f, containerExtent - padding * 2.0f)`,
        // PortableUILayout.hpp:179/:325/:435) -- not a new clamping rule.
        synth::ui::Bounds contentExtent = synth_juce::JuceToUiBounds(getLocalBounds().toFloat());
        contentExtent.width =
            std::max(0.0f, contentExtent.width - synth::runtime_ui::Layout::kSidebarWidth);
        mainComponent_.SetContentExtent(contentExtent);
        renderer_.setBounds(getLocalBounds());
        renderer_.RefreshFromSurface();
    }

    synth::ui::Bounds IntrinsicBounds() const
    {
        return mainComponent_.IntrinsicBounds();
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
