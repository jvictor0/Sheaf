#pragma once

#include "synth/AppConcepts.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/MidiConfigViewModel.hpp"
#include "synth/RuntimePagePolicy.hpp"
#include "synth/RuntimePages.hpp"

#include <concepts>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace synth::runtime_ui {

enum class RuntimeMainPage
{
    Application,
    Audio,
    Controllers,
    Sync,
    File,
    // the app-registered page. Only ever reached through
    // HandleSidebarAction's registration-gated branch below, so it exists
    // as a routable page regardless of whether any app actually registers
    // one -- the same way the enum itself is unconditional while the
    // sidebar button that reaches this case is not.
    AppPage,
};

template <typename Services>
concept RuntimeMainServices = requires(Services& services,
                                       AudioPageSnapshot& audio,
                                       FilePageSnapshot& file,
                                       SyncPageStatus& syncStatus,
                                       ControllersPageSurface& controllers,
                                       const SyncConfig& syncConfig,
                                       const ui::Action& action,
                                       std::function<void()> onBack) {
    { services.MakeControllersCallbacks(std::move(onBack)) } ->
        std::same_as<ControllersPageCallbacks>;
    { services.RefreshAudio(audio) } -> std::same_as<void>;
    { services.DispatchAudio(action) } -> std::same_as<void>;
    { services.RefreshFile(file) } -> std::same_as<void>;
    { services.DispatchFile(action) } -> std::same_as<void>;
    { services.RefreshControllers(controllers) } -> std::same_as<void>;
    { services.SnapshotSyncConfiguration() } -> std::same_as<SyncConfig>;
    { services.RefreshSyncStatus(syncStatus) } -> std::same_as<void>;
    { services.CommitSyncConfiguration(syncConfig) } -> std::same_as<bool>;
    { services.DeadlineSamplePercent() } -> std::convertible_to<float>;
    { services.SaveRuntimeConfiguration() } -> std::same_as<void>;
};

template <SynthApplication App, RuntimeMainServices Services>
class RuntimeMainComponent final : public ui::Surface
{
public:
    RuntimeMainComponent(App& app, Services& services, RuntimeSidebarPages pages = {})
        : app_(app),
          services_(services),
          pages_(pages),
          deadlineMaximum_(DeadlineWindowCapacity(App::Config().uiFrameHz)),
          controllersSurface_(services_.MakeControllersCallbacks([this] {
              ReturnToApplication(RuntimePageKind::Controllers);
          }))
    {
        const RuntimeConfig config = App::Config();
        const ui::Bounds contentBounds{
            0.0f, 0.0f, static_cast<float>(config.uiWidth), static_cast<float>(config.uiHeight)};
        configuredPageBounds_ = contentBounds;
        pageBounds_ = contentBounds;
        audioSurface_.SetContentBounds(contentBounds);
        fileSurface_.SetContentBounds(contentBounds);
        controllersSurface_.SetContentBounds(contentBounds);
        // Defaults to the compiled-in size, matching what an extent-aware
        // app would resolve to if it were never offered anything else:
        // identical to the legacy, hook-free value.
        liveContentExtent_ = contentBounds;
        lastComposedBounds_ = IntrinsicBounds();
        syncSurface_.SetContentBounds(contentBounds);
        // An application whose own vocabulary already uses "Audio" renames the
        // runtime's Audio page here (RuntimeConfig::audioPageTitle). Unset for
        // every application that does not, which leaves the sidebar unchanged.
        sidebarSurface_.SetAudioPageTitle(config.audioPageTitle);
        sidebarSurface_.SetPages(pages_);

        // an app opts in by defining App::RegisteredPage() (see
        // HasRegisteredPage, AppConcepts.hpp); App is a concrete, non-erased
        // template parameter here, so presence/absence of the method is a
        // compile-time branch, mirroring HasPrepareToPlay/HasProcessFrame's
        // if-constexpr idiom in Engine.hpp. Everything below is skipped for
        // an app that never defines the method: hasRegisteredPage_ stays
        // false, the sidebar snapshot's title stays unset, and the sidebar
        // and routing are exactly what they were before app registration
        // existed.
        if constexpr (HasRegisteredPage<App>)
        {
            RegisteredPage page = app_.RegisteredPage();
            appPageSurface_.SetContentBounds(contentBounds);
            appPageSurface_.SetPage(page);
            sidebarSurface_.SetRegisteredPageTitle(page.title);
            hasRegisteredPage_ = true;
            appPageSurface_.SetActionHandler([this](const ui::Action& action) {
                if (action.name == Actions::kAppBack)
                {
                    ReturnToApplication(RuntimePageKind::None);
                }
            });
        }

        sidebarSurface_.SetActionHandler([this](const ui::Action& action) {
            HandleSidebarAction(action);
        });
        audioSurface_.SetActionHandler([this](const ui::Action& action) {
            if (action.name == Actions::kAudioBack)
            {
                ReturnToApplication(RuntimePageKind::Audio);
                return;
            }
            services_.DispatchAudio(action);
        });
        fileSurface_.SetActionHandler([this](const ui::Action& action) {
            if (action.name == Actions::kFileBack)
            {
                ReturnToApplication(RuntimePageKind::File);
                return;
            }
            services_.DispatchFile(action);
        });
        syncSurface_.SetActionHandler([this](const ui::Action& action) {
            if (action.name != Actions::kSyncBack)
            {
                return;
            }
            if (!services_.CommitSyncConfiguration(syncSurface_.StagedConfiguration()))
            {
                syncSurface_.SetApplyError();
                return;
            }
            ReturnToApplication(RuntimePageKind::Sync);
        });
    }

    RuntimeMainComponent(const RuntimeMainComponent&) = delete;
    RuntimeMainComponent& operator=(const RuntimeMainComponent&) = delete;
    RuntimeMainComponent(RuntimeMainComponent&&) = delete;
    RuntimeMainComponent& operator=(RuntimeMainComponent&&) = delete;

    ui::NodeTree BuildTree() override
    {
        // Offer the app surface the live content extent
        // immediately before BuildTree(), generalizing the SetContentBounds
        // convention. app_.PortableSurface() is constrained by
        // SynthApplication to return exactly `ui::Surface&`
        // (AppConcepts.hpp), erasing the concrete surface type, so each hook
        // is detected with a dynamic_cast against its own separate
        // interface rather than a compile-time trait. A surface that
        // doesn't implement either is left alone and resolves at its own
        // compiled-in size.
        ui::Surface& appSurface = app_.PortableSurface();
        auto* extentAwareApp = dynamic_cast<ui::ExtentAwareSurface*>(&appSurface);
        auto* selfSizedApp = dynamic_cast<ui::SelfSizedSurface*>(&appSurface);
        const bool applicationShown = currentPage_ == RuntimeMainPage::Application;

        // A self-sized surface (sprs-19) controls its own root outright: its
        // RootBounds() is authoritative over both the configured size and
        // whatever an ExtentAwareSurface would also resolve to if it
        // additionally implemented that interface.
        const ui::Bounds expectedAppBounds =
            selfSizedApp != nullptr
                ? selfSizedApp->RootBounds()
                : extentAwareApp != nullptr
                      ? liveContentExtent_
                      : ui::Bounds{0.0f,
                                  0.0f,
                                  static_cast<float>(App::Config().uiWidth),
                                  static_cast<float>(App::Config().uiHeight)};
        if (extentAwareApp != nullptr)
        {
            extentAwareApp->SetContentExtent(liveContentExtent_);
        }

        ui::NodeTree appTree = appSurface.BuildTree();
        const std::size_t appRootIndex = ValidateApplicationTree(appTree, expectedAppBounds);
        const ui::Bounds appRootBounds = appTree.nodes[appRootIndex].bounds;

        // A self-sized surface that declares a sidebar slot is laid out for
        // a narrow window (a phone). A runtime page shown in its place takes
        // the app root's width less the sidebar, and its height, so page plus
        // sidebar is exactly the app root and the shell scales both screens
        // alike; at the configured size it would be shrunk to a fraction of
        // the window width. Every other surface keeps the configured size.
        if (!applicationShown)
        {
            const bool narrow = selfSizedApp != nullptr && selfSizedApp->SidebarSlot().has_value() &&
                                appRootBounds.width > Layout::kSidebarWidth;
            SetPageBounds(narrow ? ui::Bounds{0.0f,
                                              0.0f,
                                              appRootBounds.width - Layout::kSidebarWidth,
                                              appRootBounds.height}
                                 : configuredPageBounds_);
        }

        ui::NodeTree contentTree = applicationShown
                                       ? MoveRootFirst(std::move(appTree), appRootIndex)
                                       : BuildRuntimePageTree();
        ui::NodeTree sidebarTree = sidebarSurface_.BuildTree();
        if (sidebarTree.nodes.empty())
        {
            throw std::invalid_argument("sidebar tree must have a root");
        }

        // A self-sized surface's declared slot governs sidebar placement
        // only while it is itself on screen (sprs-19): while any runtime
        // page is shown instead, the page keeps the content bounds it was
        // constructed with and the sidebar sits at ITS root's right edge,
        // "whatever the application surface declares" (sprs-19), which is
        // also the fix for a surface that never declares a slot at all --
        // both read the sidebar's x from contentTree's own resolved root
        // rather than from the app tree's, so an open page is never sized
        // against an app root it does not contain.
        const std::optional<ui::NodeId> slot =
            applicationShown && selfSizedApp != nullptr ? selfSizedApp->SidebarSlot()
                                                         : std::optional<ui::NodeId>{};

        ui::Node root;
        root.id = "runtime.main.root";
        root.kind = ui::NodeKind::Root;

        if (slot.has_value())
        {
            ui::Bounds slotBounds{};
            if (!ComputeCompositePosition(contentTree, *slot, slotBounds))
            {
                throw std::invalid_argument(
                    "self-sized surface names an absent sidebar slot: " + slot->value);
            }
            const ui::Bounds& sidebarBounds = sidebarTree.nodes.front().bounds;
            if (sidebarBounds.width > slotBounds.width + 0.01f ||
                sidebarBounds.height > slotBounds.height + 0.01f)
            {
                throw std::invalid_argument(
                    "runtime sidebar does not fit its declared slot: " + slot->value);
            }
            sidebarTree.nodes.front().bounds.x = slotBounds.x;
            sidebarTree.nodes.front().bounds.y = slotBounds.y;
            // The composite is exactly the declared app root (sprs-19): the
            // sidebar lives inside it rather than beside it.
            root.bounds = appRootBounds;
        }
        else
        {
            // The legacy, additive composition: the sidebar sits immediately
            // to the right of contentTree's own resolved root -- the app
            // root while the application page is shown, or the page's own
            // constructed root while a runtime page is shown -- rather than
            // always reading the app tree's width (which used to put the
            // sidebar at a resized or narrow app's width even while a
            // differently-sized page was on screen).
            const float contentWidth = contentTree.nodes.front().bounds.width;
            const float contentHeight = contentTree.nodes.front().bounds.height;
            sidebarTree.nodes.front().bounds.x = contentWidth;
            root.bounds = {0.0f, 0.0f, contentWidth + Layout::kSidebarWidth, contentHeight};
        }
        RequireCompositionHolds(root.bounds, contentTree.nodes.front(), sidebarTree.nodes.front());
        root.children = {contentTree.nodes.front().id, sidebarTree.nodes.front().id};

        ui::NodeTree result;
        result.nodes.reserve(1 + contentTree.nodes.size() + sidebarTree.nodes.size());
        // Captured before root moves into result: the last composite this
        // call built, for a caller that lays out around the composite's
        // actual size rather than IntrinsicBounds()'s compiled-in one
        // (sprs-20's shell, which sizes and scales its pane to this).
        lastComposedBounds_ = root.bounds;

        result.nodes.push_back(std::move(root));
        for (ui::Node& node : contentTree.nodes)
        {
            result.nodes.push_back(std::move(node));
        }
        for (ui::Node& node : sidebarTree.nodes)
        {
            result.nodes.push_back(std::move(node));
        }
        return result;
    }

    // The bounds of the composite root the last BuildTree() call produced.
    // Defaults to the compiled-in IntrinsicBounds() before BuildTree() is
    // ever called, matching what the first call will in fact produce for a
    // surface that doesn't resolve against a live-offered or self-declared
    // size.
    ui::Bounds ComposedBounds() const
    {
        return lastComposedBounds_;
    }

    void SetActionHandler(ActionHandler handler) override
    {
        actionHandler_ = std::move(handler);
    }

    bool NeedsDeferredDispatch(const ui::Action& action) const
    {
        return IsControllersAction(action.name) && controllersSurface_.NeedsDeferredDispatch(action);
    }

    void DispatchAction(const ui::Action& action) override
    {
        if (IsSidebarAction(action.name))
        {
            sidebarSurface_.DispatchAction(action);
        }
        else if (IsAudioAction(action.name))
        {
            audioSurface_.DispatchAction(action);
        }
        else if (IsFileAction(action.name))
        {
            fileSurface_.DispatchAction(action);
        }
        else if (IsSyncAction(action.name))
        {
            syncSurface_.DispatchAction(action);
        }
        else if (IsControllersAction(action.name))
        {
            controllersSurface_.DispatchAction(action);
        }
        else if (IsAppPageAction(action.name))
        {
            appPageSurface_.DispatchAction(action);
        }
        else if (!std::string_view(action.name).starts_with("runtime."))
        {
            app_.PortableSurface().DispatchAction(action);
        }

        if (actionHandler_)
        {
            actionHandler_(action);
        }
    }

    void Refresh()
    {
        deadlineMaximum_.Write(static_cast<float>(services_.DeadlineSamplePercent()));
        sidebarSurface_.SetDeadlinePercent(deadlineMaximum_.Max());
        services_.RefreshAudio(audioSurface_.Snapshot());
        services_.RefreshFile(fileSurface_.Snapshot());
        services_.RefreshControllers(controllersSurface_);
        sidebarSurface_.SetControllersWarning(!controllersSurface_.Discovery().available.empty());
        SyncPageStatus syncStatus;
        services_.RefreshSyncStatus(syncStatus);
        syncSurface_.RefreshStatus(syncStatus);
    }

    void ShowPage(RuntimeMainPage page)
    {
        currentPage_ = page;
    }

    // The live content extent the shell will offer the
    // app surface immediately before its next BuildTree(), generalizing the
    // SetContentBounds convention already used by individual runtime pages
    // (RuntimePages.hpp, ControllersPageUI.hpp) to the whole app-surface
    // seam. Callers (the JUCE renderer via its live bounds, the
    // browser host, or a test) call this whenever the live extent changes;
    // BuildTree() always offers whatever is currently stored here. A surface
    // that doesn't implement ui::ExtentAwareSurface never sees this value.
    void SetContentExtent(ui::Bounds extent)
    {
        liveContentExtent_ = extent;
    }

    RuntimeMainPage CurrentPage() const
    {
        return currentPage_;
    }

    ui::Bounds IntrinsicBounds() const
    {
        const RuntimeConfig config = App::Config();
        return {0.0f,
                0.0f,
                static_cast<float>(config.uiWidth) + Layout::kSidebarWidth,
                static_cast<float>(config.uiHeight)};
    }

private:
    // The composition contract. The shell PLACES two already-resolved
    // subtree roots rather than resolving them itself -- but that also means
    // the page-level overflow gate never sees this composition, because the
    // resolver is never invoked on it. The residual
    // that leaves is concrete: the composite root's height follows the app's
    // declared `uiHeight` while the sidebar is as tall as the rows it shows, so
    // an app declaring `uiHeight` shorter than that overruns the window with
    // nothing to catch it. This is that catch, stated as a precondition on the
    // app's declaration rather than as a silent clip.
    static void RequireCompositionHolds(ui::Bounds rootBounds,
                                        const ui::Node& content,
                                        const ui::Node& sidebar)
    {
        const auto fits = [](const ui::Bounds& child, const ui::Bounds& parent) {
            return child.x >= -0.01f && child.y >= -0.01f &&
                   child.x + child.width <= parent.width + 0.01f &&
                   child.y + child.height <= parent.height + 0.01f;
        };
        for (const ui::Node* placed : {&content, &sidebar})
        {
            if (fits(placed->bounds, rootBounds))
            {
                continue;
            }
            throw std::invalid_argument(
                std::string("runtime shell composition does not fit its surface: '") +
                placed->id.value + "' is " + std::to_string(static_cast<int>(placed->bounds.width)) +
                "x" + std::to_string(static_cast<int>(placed->bounds.height)) + " at (" +
                std::to_string(static_cast<int>(placed->bounds.x)) + ", " +
                std::to_string(static_cast<int>(placed->bounds.y)) + ") inside a " +
                std::to_string(static_cast<int>(rootBounds.width)) + "x" +
                std::to_string(static_cast<int>(rootBounds.height)) +
                " surface. The application's declared uiWidth/uiHeight is the surface, and it must "
                "be at least as tall as the runtime sidebar.");
        }
    }

    // Finds `targetId` inside `tree` and sums its own bounds.{x,y} with
    // every ancestor's, up to and including the tree's own root (always
    // `tree.nodes.front()`, since every caller here passes a tree already
    // reordered by MoveRootFirst) -- the composite position the parent-
    // relative coordinate contract (PortableUI.hpp) assigns it. Returns
    // false when no node in the tree carries `targetId`, or when it is not
    // reachable from the root by following parent links (cannot happen for
    // a tree ValidateApplicationTree already accepted, but this is the sole
    // caller-facing contract, not that validator's absence).
    static bool ComputeCompositePosition(const ui::NodeTree& tree,
                                         const ui::NodeId& targetId,
                                         ui::Bounds& outBounds)
    {
        if (tree.nodes.empty())
        {
            return false;
        }

        std::unordered_map<std::string, std::size_t> nodeIndex;
        nodeIndex.reserve(tree.nodes.size());
        for (std::size_t index = 0; index < tree.nodes.size(); ++index)
        {
            nodeIndex.emplace(tree.nodes[index].id.value, index);
        }
        const auto targetIt = nodeIndex.find(targetId.value);
        if (targetIt == nodeIndex.end())
        {
            return false;
        }

        std::unordered_map<std::string, std::string> parentByChild;
        for (const ui::Node& node : tree.nodes)
        {
            for (const ui::NodeId& child : node.children)
            {
                parentByChild.emplace(child.value, node.id.value);
            }
        }

        const std::string rootId = tree.nodes.front().id.value;
        float x = 0.0f;
        float y = 0.0f;
        std::string current = targetId.value;
        while (true)
        {
            const ui::Node& node = tree.nodes[nodeIndex.at(current)];
            x += node.bounds.x;
            y += node.bounds.y;
            if (current == rootId)
            {
                break;
            }
            const auto parentIt = parentByChild.find(current);
            if (parentIt == parentByChild.end())
            {
                return false;
            }
            current = parentIt->second;
        }

        const ui::Node& target = tree.nodes[targetIt->second];
        outBounds = {x, y, target.bounds.width, target.bounds.height};
        return true;
    }

    // Each router below reads the surface's own action array rather than
    // restating it. The restatement is what let a rendered control dispatch
    // into nothing.
    template <std::size_t N>
    static bool IsOneOf(std::string_view action, const std::string_view (&candidates)[N])
    {
        for (const std::string_view candidate : candidates)
        {
            if (action == candidate)
            {
                return true;
            }
        }
        return false;
    }

    static bool IsSidebarAction(std::string_view action)
    {
        return IsOneOf(action, Actions::kSidebarActions);
    }

    static bool IsAudioAction(std::string_view action)
    {
        return IsOneOf(action, Actions::kAudioActions);
    }

    static bool IsFileAction(std::string_view action)
    {
        return IsOneOf(action, Actions::kFileActions);
    }

    static bool IsSyncAction(std::string_view action)
    {
        return IsOneOf(action, Actions::kSyncActions);
    }

    // The prefix rule is not membership: a per-controller action carries an
    // index the page composes at build time, so no fixed set can hold it. The
    // fixed half reads the page's array like every other surface.
    static bool IsControllersAction(std::string_view action)
    {
        return IsOneOf(action, Actions::kControllersActions) ||
               action.starts_with("runtime.controllers.controller.");
    }

    // the app-registered page's own reserved action. Actions the
    // app's own registered-page content emits are not runtime-namespaced
    // (ValidateApplicationTree's rule applies to the app's whole tree, not
    // just its main surface) and fall through DispatchAction's final
    // `!starts_with("runtime.")` branch to app_.PortableSurface() exactly
    // like an app-supplied audio section's actions already do --
    // this predicate exists only for the one action Sheaf itself owns here.
    static bool IsAppPageAction(std::string_view action)
    {
        return action == Actions::kAppBack;
    }

    static ui::NodeTree MoveRootFirst(ui::NodeTree tree, std::size_t rootIndex)
    {
        if (rootIndex == 0)
        {
            return tree;
        }

        ui::NodeTree reordered;
        reordered.nodes.reserve(tree.nodes.size());
        reordered.nodes.push_back(std::move(tree.nodes[rootIndex]));
        for (std::size_t index = 0; index < tree.nodes.size(); ++index)
        {
            if (index != rootIndex)
            {
                reordered.nodes.push_back(std::move(tree.nodes[index]));
            }
        }
        return reordered;
    }

    // `expectedBounds` is the extent the surface actually
    // resolved against -- the offered live extent when the extent-aware hook
    // was accepted, `config.uiWidth/uiHeight` otherwise (BuildTree() decides
    // which). The positive-config guard below stays config-based regardless:
    // it is a sanity check on the app's own declaration, independent of
    // which bounds this call is validating the resolved root against.
    static std::size_t ValidateApplicationTree(const ui::NodeTree& tree,
                                               const ui::Bounds& expectedBounds)
    {
        const RuntimeConfig config = App::Config();
        if (config.uiWidth <= 0 || config.uiHeight <= 0 || tree.nodes.empty())
        {
            throw std::invalid_argument(
                "application root must match positive configured bounds");
        }

        std::unordered_map<std::string, std::size_t> nodeIndex;
        nodeIndex.reserve(tree.nodes.size());
        for (std::size_t index = 0; index < tree.nodes.size(); ++index)
        {
            const ui::Node& node = tree.nodes[index];
            if (!nodeIndex.emplace(node.id.value, index).second)
            {
                throw std::invalid_argument("application tree has duplicate node id: " +
                                            node.id.value);
            }
            if (std::string_view(node.id.value).starts_with("runtime."))
            {
                throw std::invalid_argument(
                    "application tree uses reserved runtime namespace: " + node.id.value);
            }
        }

        std::vector<std::size_t> parentCounts(tree.nodes.size(), 0);
        for (const ui::Node& node : tree.nodes)
        {
            for (const ui::NodeId& child : node.children)
            {
                const auto childIt = nodeIndex.find(child.value);
                if (childIt == nodeIndex.end())
                {
                    throw std::invalid_argument("application tree references unknown child: " +
                                                child.value);
                }
                ++parentCounts[childIt->second];
            }
        }

        enum class VisitState
        {
            Unvisited,
            Visiting,
            Visited,
        };
        std::vector<VisitState> states(tree.nodes.size(), VisitState::Unvisited);
        std::function<void(std::size_t)> visit = [&](std::size_t index) {
            if (states[index] == VisitState::Visiting)
            {
                throw std::invalid_argument("application tree contains a cycle");
            }
            if (states[index] == VisitState::Visited)
            {
                return;
            }

            states[index] = VisitState::Visiting;
            for (const ui::NodeId& child : tree.nodes[index].children)
            {
                visit(nodeIndex.at(child.value));
            }
            states[index] = VisitState::Visited;
        };
        for (std::size_t index = 0; index < tree.nodes.size(); ++index)
        {
            if (states[index] == VisitState::Unvisited)
            {
                visit(index);
            }
        }

        std::size_t rootIndex = tree.nodes.size();
        // In an acyclic graph, one root and one parent per other node imply full reachability.
        for (std::size_t index = 0; index < parentCounts.size(); ++index)
        {
            if (parentCounts[index] != 0)
            {
                continue;
            }
            if (rootIndex != tree.nodes.size())
            {
                throw std::invalid_argument(
                    "application tree must have exactly one parentless root");
            }
            rootIndex = index;
        }
        if (rootIndex == tree.nodes.size() || tree.nodes[rootIndex].kind != ui::NodeKind::Root)
        {
            throw std::invalid_argument("application tree must have exactly one parentless root");
        }

        for (std::size_t index = 0; index < parentCounts.size(); ++index)
        {
            if (index != rootIndex && parentCounts[index] != 1)
            {
                throw std::invalid_argument(
                    "application tree nodes must be reachable exactly once");
            }
        }

        const ui::Bounds& bounds = tree.nodes[rootIndex].bounds;
        if (bounds.x != expectedBounds.x || bounds.y != expectedBounds.y ||
            bounds.width != expectedBounds.width || bounds.height != expectedBounds.height)
        {
            throw std::invalid_argument("application root does not match configured bounds");
        }
        return rootIndex;
    }

    void SetPageBounds(const ui::Bounds& bounds)
    {
        if (bounds == pageBounds_)
        {
            return;
        }
        pageBounds_ = bounds;
        audioSurface_.SetContentBounds(bounds);
        fileSurface_.SetContentBounds(bounds);
        controllersSurface_.SetContentBounds(bounds);
        syncSurface_.SetContentBounds(bounds);
        if (hasRegisteredPage_)
        {
            appPageSurface_.SetContentBounds(bounds);
        }
    }

    ui::NodeTree BuildRuntimePageTree()
    {
        switch (currentPage_)
        {
            case RuntimeMainPage::Audio:
                return audioSurface_.BuildTree();
            case RuntimeMainPage::Controllers:
                return controllersSurface_.BuildTree();
            case RuntimeMainPage::Sync:
                return syncSurface_.BuildTree();
            case RuntimeMainPage::File:
                return fileSurface_.BuildTree();
            case RuntimeMainPage::AppPage:
                return appPageSurface_.BuildTree();
            case RuntimeMainPage::Application:
                break;
        }
        throw std::invalid_argument("application page has no runtime page tree");
    }

    void HandleSidebarAction(const ui::Action& action)
    {
        if (action.name == Actions::kSidebarAudio)
        {
            if (!pages_.audio)
            {
                return;
            }
            ShowPage(RuntimeMainPage::Audio);
        }
        else if (action.name == Actions::kSidebarControllers)
        {
            if (!pages_.controllers)
            {
                return;
            }
            ShowPage(RuntimeMainPage::Controllers);
        }
        else if (action.name == Actions::kSidebarSync)
        {
            if (!pages_.sync)
            {
                return;
            }
            if (currentPage_ != RuntimeMainPage::Sync)
            {
                syncSurface_.BeginEdit(services_.SnapshotSyncConfiguration());
            }
            ShowPage(RuntimeMainPage::Sync);
        }
        else if (action.name == Actions::kSidebarFile)
        {
            if (!pages_.file)
            {
                return;
            }
            ShowPage(RuntimeMainPage::File);
        }
        // gated on hasRegisteredPage_ even though the button (and
        // therefore this action) only ever exists in the UI when a page is
        // registered -- "optional means optional, assert don't assume"
        // (design constraint) applies to a directly dispatched action too,
        // not only to what the sidebar renders.
        else if (action.name == Actions::kSidebarApp && hasRegisteredPage_)
        {
            ShowPage(RuntimeMainPage::AppPage);
        }
    }

    void ReturnToApplication(RuntimePageKind page)
    {
        if (RuntimePageBackSavesConfiguration(page))
        {
            services_.SaveRuntimeConfiguration();
        }
        ShowPage(RuntimeMainPage::Application);
    }

    App& app_;
    Services& services_;
    RuntimeSidebarPages pages_;
    RuntimeMainPage currentPage_ = RuntimeMainPage::Application;
    RollingMax deadlineMaximum_;
    SidebarSurface sidebarSurface_;
    AudioPageSurface audioSurface_;
    FilePageSurface fileSurface_;
    ControllersPageSurface controllersSurface_;
    SyncPageSurface syncSurface_;
    // always present so BuildRuntimePageTree()'s switch and
    // DispatchAction()'s IsAppPageAction branch compile and behave the same
    // regardless of App -- only the constructor's if-constexpr block (and
    // hasRegisteredPage_ below) differ between a registering and a
    // non-registering App.
    AppRegisteredPageSurface appPageSurface_;
    bool hasRegisteredPage_ = false;
    ActionHandler actionHandler_;
    ui::Bounds liveContentExtent_;
    ui::Bounds lastComposedBounds_;
    // The runtime pages' content bounds: the configured size, or a narrow
    // self-sized app's width less the sidebar (BuildTree()).
    ui::Bounds configuredPageBounds_;
    ui::Bounds pageBounds_;
};

}  // namespace synth::runtime_ui
