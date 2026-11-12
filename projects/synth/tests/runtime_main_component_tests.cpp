#include "synth/RuntimeMainComponent.hpp"
#include "synth/ControllerWizard.hpp"
#include "synth/ControllerWizardDiscoveryCache.hpp"

#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef JUCE_MAJOR_VERSION
#error "runtime main component tests must not see JUCE"
#endif

namespace {

void Require(bool condition, const char* label)
{
    if (!condition)
    {
        throw std::runtime_error(label);
    }
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const char* id)
{
    for (const synth::ui::Node& node : tree.nodes)
    {
        if (node.id == synth::ui::NodeId(id))
        {
            return &node;
        }
    }
    return nullptr;
}

void RequireBounds(const synth::ui::Bounds& actual,
                   float x,
                   float y,
                   float width,
                   float height,
                   const char* label)
{
    Require(actual.x == x && actual.y == y && actual.width == width && actual.height == height,
            label);
}

bool NearlyEqual(float actual, float expected)
{
    const float delta = actual - expected;
    return delta <= 0.001f && delta >= -0.001f;
}

const synth::ui::Node& FindNode(const synth::ui::NodeTree& tree, const char* id)
{
    const synth::ui::Node* node = FindNodeById(tree, id);
    Require(node != nullptr, id);
    return *node;
}

bool IsSidebarDescendant(const synth::ui::NodeTree& tree, const synth::ui::NodeId& id)
{
    if (id.value == synth::runtime_ui::NodeIds::kSidebarRoot)
    {
        return false;
    }

    std::unordered_map<std::string, std::string> parentByChild;
    for (const synth::ui::Node& node : tree.nodes)
    {
        for (const synth::ui::NodeId& child : node.children)
        {
            parentByChild.emplace(child.value, node.id.value);
        }
    }

    std::string current = id.value;
    while (true)
    {
        const auto parentIt = parentByChild.find(current);
        if (parentIt == parentByChild.end())
        {
            return false;
        }
        if (parentIt->second == synth::runtime_ui::NodeIds::kSidebarRoot)
        {
            return true;
        }
        current = parentIt->second;
    }
}

bool IsDeliberatelyZeroExtent(const synth::ui::Node& node)
{
    // TODO(tasks 11-13): return true for nodes the layout resolver intentionally
    // collapses to zero extent (for example an empty widget bay), so
    // TestSubtreesArriveFullyResolved does not treat that as a missing layout.
    //
    (void)node;
    return false;
}

synth::ui::NodeTree MakeValidAppTree()
{
    synth::ui::Node root;
    root.id = "app.root";
    root.kind = synth::ui::NodeKind::Root;
    root.bounds = {0.0f, 0.0f, 900.0f, 560.0f};
    return synth::ui::NodeTree{{std::move(root)}};
}

class FakeAppSurface final : public synth::ui::Surface
{
public:
    synth::ui::NodeTree tree = MakeValidAppTree();
    int dispatchCount = 0;
    std::string lastAction;

    synth::ui::NodeTree BuildTree() override
    {
        return tree;
    }

    void SetActionHandler(ActionHandler handler) override
    {
        observer_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        ++dispatchCount;
        lastAction = action.name;
        if (observer_)
        {
            observer_(action);
        }
    }

private:
    ActionHandler observer_;
};

struct FakeApp
{
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{.appName = "RuntimeMainTest", .uiWidth = 900, .uiHeight = 560};
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }

    FakeAppSurface surface;
};

// An app whose own vocabulary already uses "Audio" -- a synth with an Audio
// parameter bank -- and so renames the runtime's Audio page through
// RuntimeConfig::audioPageTitle. Nothing else about it differs from FakeApp.
struct RenamedAudioPageApp
{
    static synth::RuntimeConfig Config()
    {
        synth::RuntimeConfig config{.appName = "RenamedAudioPageTest", .uiWidth = 900, .uiHeight = 560};
        config.audioPageTitle = "Audio I/O";
        return config;
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }

    FakeAppSurface surface;
};

// An app that registers one additional sidebar page. The builder
// declares a Row with non-default padding and a non-even weight split, the
// same nested-layout shape
// TestAudioPageAppSectionNestedLayoutSurvivesTheSplice (portable_ui_tests.cpp)
// uses to prove Splice(Subtree) -- not Splice(NodeTree) -- carries a nested
// container's declared LayoutOptions through the splice instead of letting
// them re-resolve at defaults.
struct RegisteredPageApp
{
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{.appName = "RegisteredPageAppTest", .uiWidth = 900, .uiHeight = 560};
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }

    synth::RegisteredPage RegisteredPage()
    {
        return synth::RegisteredPage{
            .id = "app.custom",
            .title = "Custom",
            .buildTree = [](synth::ui::Bounds) {
                synth::ui::Builder appBuilder;
                appBuilder.Rootless();
                synth::ui::LayoutOptions rowLayout;
                rowLayout.padding = 40.0f;  // default LayoutOptions padding is not 40
                appBuilder.Row("app.custom.row", rowLayout, [](synth::ui::Builder& row) {
                    synth::ui::ControlStyle heavy;
                    heavy.layout.main = synth::ui::Extent::Weight(3.0f);
                    row.Label("app.custom.heavy", "Heavy", heavy);
                    synth::ui::ControlStyle light;
                    light.layout.main = synth::ui::Extent::Weight(1.0f);
                    row.Label("app.custom.light", "Light", light);
                });
                return appBuilder.BuildSubtree();
            },
        };
    }

    FakeAppSurface surface;
};

// A surface that additionally implements
// ui::ExtentAwareSurface and resolves its BuildTree() root against whatever
// extent it was last offered, instead of a compiled-in size.
class ExtentAwareAppSurface final : public synth::ui::Surface, public synth::ui::ExtentAwareSurface
{
public:
    int dispatchCount = 0;
    int extentOffers = 0;
    std::string lastAction;

    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Node root;
        root.id = "extent.app.root";
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = extent_;
        return synth::ui::NodeTree{{std::move(root)}};
    }

    void SetActionHandler(ActionHandler handler) override
    {
        observer_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        ++dispatchCount;
        lastAction = action.name;
        if (observer_)
        {
            observer_(action);
        }
    }

    void SetContentExtent(synth::ui::Bounds extent) override
    {
        ++extentOffers;
        extent_ = extent;
    }

private:
    synth::ui::Bounds extent_{0.0f, 0.0f, 900.0f, 560.0f};
    ActionHandler observer_;
};

struct ExtentAwareApp
{
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{.appName = "ExtentAwareAppTest", .uiWidth = 900, .uiHeight = 560};
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }

    ExtentAwareAppSurface surface;
};

// A surface that additionally implements ui::SelfSizedSurface (sprs-19): it
// declares its own root bounds (independent of App::Config()) and names a
// slot node nested two levels deep -- "selfsized.app.bottom" at (0, 500)
// relative to the root, itself containing "selfsized.app.slot" at (363, 0)
// relative to that -- so a test that reads the slot's composite position
// exercises summing parent-relative offsets across more than one level, not
// just a direct child's own bounds. `declaredRootBounds` is separate from
// the bounds the built tree's root actually carries so a test can make them
// disagree; `includeSlot` and `slotWidth`/`slotHeight` let a test remove the
// slot or shrink it below the sidebar's own size.
class SelfSizedAppSurface final : public synth::ui::Surface, public synth::ui::SelfSizedSurface
{
public:
    synth::ui::Bounds rootBounds{0.0f, 0.0f, 459.0f, 700.0f};
    synth::ui::Bounds declaredRootBounds{0.0f, 0.0f, 459.0f, 700.0f};
    float slotWidth = 96.0f;
    float slotHeight = 200.0f;
    bool includeSlot = true;

    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Node root;
        root.id = "selfsized.app.root";
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = rootBounds;
        root.children = {synth::ui::NodeId("selfsized.app.top"),
                         synth::ui::NodeId("selfsized.app.bottom")};

        synth::ui::Node top;
        top.id = "selfsized.app.top";
        top.kind = synth::ui::NodeKind::Section;
        top.bounds = {0.0f, 0.0f, rootBounds.width, 500.0f};

        synth::ui::Node bottom;
        bottom.id = "selfsized.app.bottom";
        bottom.kind = synth::ui::NodeKind::Section;
        bottom.bounds = {0.0f, 500.0f, rootBounds.width, rootBounds.height - 500.0f};
        bottom.children = {synth::ui::NodeId("selfsized.app.slot")};

        synth::ui::Node slot;
        slot.id = "selfsized.app.slot";
        slot.kind = synth::ui::NodeKind::Section;
        slot.bounds = {rootBounds.width - slotWidth, 0.0f, slotWidth, slotHeight};

        synth::ui::NodeTree tree;
        tree.nodes = {std::move(root), std::move(top), std::move(bottom), std::move(slot)};
        return tree;
    }

    void SetActionHandler(ActionHandler handler) override
    {
        observer_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        if (observer_)
        {
            observer_(action);
        }
    }

    synth::ui::Bounds RootBounds() const override
    {
        return declaredRootBounds;
    }

    std::optional<synth::ui::NodeId> SidebarSlot() const override
    {
        if (!includeSlot)
        {
            return std::nullopt;
        }
        return synth::ui::NodeId("selfsized.app.slot");
    }

private:
    ActionHandler observer_;
};

struct SelfSizedApp
{
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{.appName = "SelfSizedAppTest", .uiWidth = 900, .uiHeight = 560};
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface()
    {
        return surface;
    }

    SelfSizedAppSurface surface;
};

struct FakeServices
{
    int audioRefreshCount = 0;
    int audioDispatchCount = 0;
    int fileRefreshCount = 0;
    int fileDispatchCount = 0;
    int controllersRefreshCount = 0;
    int controllerCommitCount = 0;
    int syncSnapshotCount = 0;
    int syncRefreshCount = 0;
    int syncCommitCount = 0;
    int saveCount = 0;
    bool controllerCommitSucceeds = true;
    bool controllerSaveSucceeds = true;
    std::vector<std::string> controllerPersistenceEvents;
    std::string lastAudioAction;
    std::string lastFileAction;
    float deadlinePercent = 12.4f;
    bool syncCommitSucceeds = true;
    synth::SyncConfig currentSync{};
    synth::SyncConfig lastCommittedSync{};
    synth::runtime_ui::SyncPageStatus syncStatus{};
    synth::MidiInstrumentConfig instrument;
    synth::MidiDeviceList controllerDevices;

    synth::runtime_ui::ControllersPageCallbacks MakeControllersCallbacks(std::function<void()> onBack)
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return instrument; };
        callbacks.connectionState = [] { return synth::MidiConnectionState{}; };
        callbacks.enumerateDevices = [this] { return controllerDevices; };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig next) {
            ++controllerCommitCount;
            controllerPersistenceEvents.push_back("commit");
            if (!controllerCommitSucceeds)
            {
                return false;
            }
            instrument = std::move(next);
            return true;
        };
        callbacks.saveRuntimeConfiguration = [this] {
            ++saveCount;
            controllerPersistenceEvents.push_back("save");
            return controllerSaveSucceeds;
        };
        callbacks.setStatus = [](std::string) {};
        callbacks.onBack = std::move(onBack);
        return callbacks;
    }

    void RefreshAudio(synth::runtime_ui::AudioPageSnapshot&)
    {
        ++audioRefreshCount;
    }

    void DispatchAudio(const synth::ui::Action& action)
    {
        ++audioDispatchCount;
        lastAudioAction = action.name;
    }

    void RefreshFile(synth::runtime_ui::FilePageSnapshot&)
    {
        ++fileRefreshCount;
    }

    void DispatchFile(const synth::ui::Action& action)
    {
        ++fileDispatchCount;
        lastFileAction = action.name;
    }

    void RefreshControllers(synth::runtime_ui::ControllersPageSurface& surface)
    {
        ++controllersRefreshCount;
        surface.SetEnumerateDevices(controllerDevices);
        surface.SetDiscovery(synth::DiscoverControllerWizards(
            controllerDevices, instrument, synth::MakeControllerWizardRegistry(synth::MidiAppCatalog{})));
    }

    synth::SyncConfig SnapshotSyncConfiguration()
    {
        ++syncSnapshotCount;
        return currentSync;
    }

    void RefreshSyncStatus(synth::runtime_ui::SyncPageStatus& status)
    {
        ++syncRefreshCount;
        status = syncStatus;
    }

    bool CommitSyncConfiguration(const synth::SyncConfig& config)
    {
        ++syncCommitCount;
        lastCommittedSync = config;
        if (syncCommitSucceeds)
        {
            currentSync = config;
        }
        return syncCommitSucceeds;
    }

    float DeadlineSamplePercent()
    {
        return deadlinePercent;
    }

    void SaveRuntimeConfiguration()
    {
        ++saveCount;
    }
};

using MainComponent = synth::runtime_ui::RuntimeMainComponent<FakeApp, FakeServices>;

static_assert(!std::is_copy_constructible_v<MainComponent>);
static_assert(!std::is_copy_assignable_v<MainComponent>);
static_assert(!std::is_move_constructible_v<MainComponent>);
static_assert(!std::is_move_assignable_v<MainComponent>);

struct Fixture
{
    FakeApp app;
    FakeServices services;
    MainComponent component{app, services};
};

// An application declaring a surface shorter than the runtime sidebar. 160 is
// below the sidebar's five fixed 40px rows: the shell places already-resolved
// subtree roots rather than resolving them, and nothing used to catch an app
// that overran the window.
struct ShortSurfaceApp
{
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{.appName = "ShortSurfaceApp", .uiWidth = 900, .uiHeight = 160};
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}

    synth::ui::Surface& PortableSurface() { return surface; }

    FakeAppSurface surface{};
};

synth::ui::NodeTree BuildCompositeTree()
{
    Fixture fixture;
    return fixture.component.BuildTree();
}

void TestPlacingASubtreeRootPlacesEveryDescendant()
{
    const synth::ui::NodeTree composite = BuildCompositeTree();
    Require(NearlyEqual(FindNode(composite, "runtime.main.root").bounds.width, 996.0f),
            "runtime chrome is additive: 900 + 96");
    Require(NearlyEqual(FindNode(composite, "runtime.sidebar.root").bounds.x, 900.0f),
            "the sidebar root sits at x 900 relative to the composite root");
    for (const synth::ui::Node& node : composite.nodes)
    {
        if (IsSidebarDescendant(composite, node.id))
        {
            Require(node.bounds.x < 96.0f,
                    "every sidebar descendant carries coordinates relative to its own "
                    "parent, never translated into the 900-996 band");
        }
    }
}

void TestSubtreesArriveFullyResolved()
{
    const synth::ui::NodeTree composite = BuildCompositeTree();
    for (const synth::ui::Node& node : composite.nodes)
    {
        if (node.kind == synth::ui::NodeKind::Root)
        {
            continue;
        }
        Require(node.bounds.width > 0.0f || IsDeliberatelyZeroExtent(node),
                "every node arrives laid out by the library, not by a backend");
    }
}

void TestCompositeBoundsPreserveAppAndAddSidebar()
{
    Fixture fixture;
    const synth::ui::NodeTree tree = fixture.component.BuildTree();

    Require(tree.nodes.size() >= 7, "composite contains main, app, and sidebar trees");
    Require(tree.nodes[0].id == synth::ui::NodeId("runtime.main.root"), "composite root is first");
    Require(tree.nodes[1].id == synth::ui::NodeId("app.root"), "application root is second");
    RequireBounds(tree.nodes[0].bounds, 0.0f, 0.0f, 996.0f, 560.0f, "composite bounds");
    RequireBounds(tree.nodes[1].bounds, 0.0f, 0.0f, 900.0f, 560.0f, "application bounds");

    const synth::ui::Node* sidebar =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarRoot);
    const synth::ui::Node* audio =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarAudio);
    const synth::ui::Node* sync =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarSync);
    Require(sidebar != nullptr, "sidebar root exists");
    Require(audio != nullptr, "sidebar audio node exists");
    Require(sync != nullptr, "sidebar sync node exists");
    RequireBounds(sidebar->bounds, 900.0f, 0.0f, 96.0f, 200.0f, "sidebar root placed");
    RequireBounds(audio->bounds, 0.0f, 0.0f, 96.0f, 40.0f, "sidebar descendant stays parent-relative");
    RequireBounds(sync->bounds, 0.0f, 80.0f, 96.0f, 40.0f, "sidebar sync stays parent-relative");
    RequireBounds(fixture.component.IntrinsicBounds(),
                  0.0f,
                  0.0f,
                  996.0f,
                  560.0f,
                  "intrinsic bounds");
}

// A self-sized surface's root is validated against RootBounds() instead of
// App::Config() -- 900x560 for SelfSizedApp -- so a built root that does not
// match the surface's OWN declared bounds fails composition, the same
// generic application-root diagnostic TestRejectsRootSizeMismatch checks for
// a legacy app (ValidateApplicationTree has one definition of that check;
// only what it compares against differs per sprs-19).
void TestSelfSizedRootIsValidatedAgainstItsDeclaredBounds()
{
    SelfSizedApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<SelfSizedApp, FakeServices> component{app, services};
    app.surface.declaredRootBounds = {0.0f, 0.0f, 500.0f, 700.0f};  // built root stays 459 wide

    bool threw = false;
    std::string message;
    try
    {
        component.BuildTree();
    }
    catch (const std::invalid_argument& error)
    {
        threw = true;
        message = error.what();
    }
    Require(threw, "a self-sized root that disagrees with RootBounds() fails composition");
    Require(message.find("configured bounds") != std::string::npos,
            "the diagnostic names the application-root contract");
}

// A self-sized surface's declared slot receives the sidebar, at the slot's
// composite position (summed across the two nesting levels
// SelfSizedAppSurface builds), and the composite is exactly the declared app
// root rather than the app root plus an additive sidebar column.
void TestSidebarIsPlacedAtTheDeclaredSlot()
{
    SelfSizedApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<SelfSizedApp, FakeServices> component{app, services};

    const synth::ui::NodeTree tree = component.BuildTree();
    RequireBounds(FindNode(tree, "runtime.main.root").bounds,
                  0.0f, 0.0f, 459.0f, 700.0f, "composite equals the declared app root exactly");
    RequireBounds(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  363.0f, 500.0f, 96.0f, 200.0f,
                  "sidebar sits at the slot's composite position: bottom's own (0, 500) plus "
                  "the slot's (363, 0) relative to bottom");
}

// A slot smaller than the sidebar root fails composition naming the slot,
// rather than silently overlapping or clipping it.
void TestSidebarLargerThanItsSlotFailsComposition()
{
    SelfSizedApp app;
    app.surface.slotHeight = 150.0f;  // sidebar needs 200 for its five default rows
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<SelfSizedApp, FakeServices> component{app, services};

    bool threw = false;
    std::string message;
    try
    {
        component.BuildTree();
    }
    catch (const std::invalid_argument& error)
    {
        threw = true;
        message = error.what();
    }
    Require(threw, "a slot smaller than the sidebar fails composition");
    Require(message.find("selfsized.app.slot") != std::string::npos,
            "the diagnostic names the slot node");
}

// While Audio, Controllers, Sync and File are open in turn beside a slotted
// (narrow) app, each page takes the app root's width less the sidebar and the
// app root's height, so page plus sidebar is exactly the 459x700 app root and
// the shell scales both screens alike. Returning to the application
// recomposes the slotted tree.
void TestEveryRuntimePageOpensBesideASlottedApp()
{
    SelfSizedApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<SelfSizedApp, FakeServices> component{app, services};

    const char* sidebarActions[] = {
        "runtime.sidebar.audio",
        "runtime.sidebar.controllers",
        "runtime.sidebar.sync",
        "runtime.sidebar.file",
    };
    const char* backActions[] = {
        "runtime.audio.back",
        "runtime.controllers.back",
        "runtime.sync.back",
        "runtime.file.back",
    };
    for (std::size_t index = 0; index < 4; ++index)
    {
        component.DispatchAction(synth::ui::Action::Named(sidebarActions[index]));
        Require(component.CurrentPage() != synth::runtime_ui::RuntimeMainPage::Application,
                "the sidebar action opens a runtime page");
        const synth::ui::NodeTree tree = component.BuildTree();
        RequireBounds(tree.nodes[1].bounds, 0.0f, 0.0f, 363.0f, 700.0f,
                      "the page root is the app root's width less the sidebar, at its height");
        RequireBounds(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                      363.0f, 0.0f, 96.0f, 200.0f,
                      "the sidebar sits at the page root's own right edge");
        RequireBounds(FindNode(tree, "runtime.main.root").bounds, 0.0f, 0.0f, 459.0f, 700.0f,
                      "page plus sidebar is exactly the app root");
        component.DispatchAction(synth::ui::Action::Named(backActions[index]));
        Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
                "Back restores the application page");
    }

    const synth::ui::NodeTree backToApp = component.BuildTree();
    RequireBounds(FindNode(backToApp, "runtime.main.root").bounds,
                  0.0f, 0.0f, 459.0f, 700.0f, "the slotted composite returns");
    RequireBounds(FindNode(backToApp, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  363.0f, 500.0f, 96.0f, 200.0f, "the sidebar is back in its slot");
}

// A self-sized app that declares no slot is not laid out for a narrow window,
// so a runtime page opened over it keeps the configured content bounds.
void TestRuntimePageKeepsConfiguredBoundsBesideAnUnslottedApp()
{
    SelfSizedApp app;
    app.surface.includeSlot = false;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<SelfSizedApp, FakeServices> component{app, services};

    component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.audio"));
    const synth::ui::NodeTree tree = component.BuildTree();
    RequireBounds(tree.nodes[1].bounds, 0.0f, 0.0f, 900.0f, 560.0f,
                  "the page root keeps the configured content bounds");
    RequireBounds(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  900.0f, 0.0f, 96.0f, 200.0f,
                  "the sidebar sits at the page root's own right edge");
}

// An extent-aware app surface resolves against whatever
// live extent the shell offers, and the sidebar tracks the resolved app
// root width rather than a compiled-in one.
void TestExtentAwareAppTracksResizedContentExtent()
{
    ExtentAwareApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<ExtentAwareApp, FakeServices> component{app, services};

    // Never explicitly resized: the shell still offers the extent-aware
    // surface its content extent (the hook is exercised every BuildTree()),
    // and because that default equals config.uiWidth/uiHeight, the baseline
    // composition matches the legacy, hook-free values exactly.
    const synth::ui::NodeTree initial = component.BuildTree();
    Require(app.surface.extentOffers >= 1,
            "shell offers the extent-aware surface its content extent before BuildTree");
    RequireBounds(FindNode(initial, "runtime.main.root").bounds,
                  0.0f, 0.0f, 996.0f, 560.0f, "default composite bounds match legacy");
    RequireBounds(FindNode(initial, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  900.0f, 0.0f, 96.0f, 200.0f, "default sidebar placement matches legacy");

    // The window resizes wider: the shell offers the new live extent, the
    // app resolves its tree against it, and the sidebar tracks the resolved
    // width -- no dead space between the app's new edge and the sidebar.
    component.SetContentExtent({0.0f, 0.0f, 1200.0f, 560.0f});
    const synth::ui::NodeTree resized = component.BuildTree();
    RequireBounds(FindNode(resized, "extent.app.root").bounds,
                  0.0f, 0.0f, 1200.0f, 560.0f, "app resolves against the newly offered extent");
    RequireBounds(FindNode(resized, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  1200.0f, 0.0f, 96.0f, 200.0f,
                  "sidebar sits at the resolved root width, no dead space");
    RequireBounds(FindNode(resized, "runtime.main.root").bounds,
                  0.0f, 0.0f, 1296.0f, 560.0f, "composite root grows with the resolved app width");

    // A vertical resize too -- the composite root's
    // height must track the resolved app height (900x560 -> 1200x700), not
    // stay pinned to the compiled-in config.uiHeight (560). root.bounds.height
    // used to stay hardcoded to config.uiHeight even on this extent-aware
    // branch, so this resize would validate (the validator checks the app
    // root, not the composite root) and then throw inside
    // RequireCompositionHolds because the 700-tall app root no longer fit a
    // 560-tall composite root.
    component.SetContentExtent({0.0f, 0.0f, 1200.0f, 700.0f});
    const synth::ui::NodeTree resizedTaller = component.BuildTree();
    RequireBounds(FindNode(resizedTaller, "extent.app.root").bounds,
                  0.0f, 0.0f, 1200.0f, 700.0f, "app resolves against the newly offered taller extent");
    RequireBounds(FindNode(resizedTaller, synth::runtime_ui::NodeIds::kSidebarRoot).bounds,
                  1200.0f, 0.0f, 96.0f, 200.0f,
                  "sidebar x still tracks the resolved width; the fixed 200px sidebar column "
                  "keeps fitting under a 700-tall composite root");
    RequireBounds(FindNode(resizedTaller, "runtime.main.root").bounds,
                  0.0f, 0.0f, 1296.0f, 700.0f,
                  "composite root height tracks the resolved app height (700), not the "
                  "compiled-in config.uiHeight (560) -- BuildTree() completing without throwing "
                  "also proves the validator accepted the resized app root and "
                  "RequireCompositionHolds held for both children");
}

void TestSidebarOpensEachPageAndBackRestoresApp()
{
    Fixture fixture;

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.audio"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Audio,
            "audio page opens");
    Require(fixture.component.BuildTree().nodes[1].id ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kAudioRoot),
            "audio page root replaces app root");
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.audio.back"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "audio back restores app");

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.controllers"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Controllers,
            "controllers page opens");
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.controllers.back"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "controllers back restores app");

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.file"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::File,
            "file page opens");
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.file.back"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "file back restores app");

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.sync"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Sync,
            "sync page opens");
    Require(fixture.services.syncSnapshotCount == 1,
            "sync opening snapshots requested configuration once");
    Require(fixture.component.BuildTree().nodes[1].id ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kSyncRoot),
            "sync page root replaces app root");
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sync.back"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "sync back restores app");
}

// A plugin host declares a narrower page set: the sidebar shows only the
// declared entries, sized to match, each opens its page and Back returns, and
// dispatching an undeclared page's sidebar action leaves the application
// page showing.
void TestSidebarShowsOnlyTheDeclaredPages()
{
    FakeApp app;
    FakeServices services;
    const synth::runtime_ui::RuntimeSidebarPages pages{
        .audio = false, .controllers = true, .sync = false, .file = true, .loadReadout = false};
    MainComponent component{app, services, pages};

    const synth::ui::NodeTree tree = component.BuildTree();
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarControllers) != nullptr,
            "the declared Controllers entry is built");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarFile) != nullptr,
            "the declared File entry is built");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarAudio) == nullptr,
            "the undeclared Audio entry is not built");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarSync) == nullptr,
            "the undeclared Sync entry is not built");
    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarDeadline) == nullptr,
            "the undeclared load readout is not built");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).bounds.height == 80.0f,
            "the sidebar is as tall as its two declared rows");

    component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.controllers"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Controllers,
            "the declared Controllers entry opens its page");
    component.DispatchAction(synth::ui::Action::Named("runtime.controllers.back"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "Controllers Back restores the application");

    component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.file"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::File,
            "the declared File entry opens its page");
    component.DispatchAction(synth::ui::Action::Named("runtime.file.back"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "File Back restores the application");

    component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.audio"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "dispatching the undeclared Audio action leaves the application page showing");
    component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.sync"));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "dispatching the undeclared Sync action leaves the application page showing");
}

void TestAppActionsRouteOnlyToAppSurface()
{
    Fixture fixture;
    fixture.component.DispatchAction(synth::ui::Action::WithValue("app.change", "42"));

    Require(fixture.app.surface.dispatchCount == 1, "app action reaches app surface once");
    Require(fixture.app.surface.lastAction == "app.change", "app receives original action");
    Require(fixture.services.audioDispatchCount == 0, "app action does not reach audio services");
    Require(fixture.services.fileDispatchCount == 0, "app action does not reach file services");
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "app action does not navigate");
}

void TestRuntimeActionsRouteOnlyToOwningPageOrServices()
{
    Fixture fixture;

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.sidebar.audio"));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Audio,
            "sidebar action navigates");
    Require(fixture.app.surface.dispatchCount == 0, "sidebar action does not reach app");

    fixture.component.DispatchAction(
        synth::ui::Action::WithValue("runtime.audio.output.select", "system_default"));
    Require(fixture.services.audioDispatchCount == 1, "audio action reaches audio services");
    Require(fixture.services.fileDispatchCount == 0, "audio action does not reach file services");
    Require(fixture.app.surface.dispatchCount == 0, "audio action does not reach app");

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.file.new"));
    Require(fixture.services.fileDispatchCount == 1, "file action reaches file services");
    Require(fixture.services.audioDispatchCount == 1, "file action does not return to audio services");
    Require(fixture.app.surface.dispatchCount == 0, "file action does not reach app");

    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.unknown"));
    Require(fixture.app.surface.dispatchCount == 0, "unknown runtime action stays reserved");
}

void TestControllerDraftActionsReachControllerSurface()
{
    Fixture fixture;

    fixture.component.ShowPage(synth::runtime_ui::RuntimeMainPage::Controllers);
    fixture.component.DispatchAction(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAddPresetDraft, "custom"));

    const synth::ui::NodeTree tree = fixture.component.BuildTree();
    const synth::ui::Node* addPreset =
        FindNodeById(tree, synth::runtime_ui::NodeIds::kAddPreset);
    Require(addPreset != nullptr && addPreset->selectedOption == "custom",
            "controller add-preset draft routes through runtime component");
}

void TestBackFromConfigurationPageSavesRuntimeConfiguration()
{
    Fixture fixture;

    fixture.component.ShowPage(synth::runtime_ui::RuntimeMainPage::Audio);
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.audio.back"));
    Require(fixture.services.saveCount == 1, "audio back saves runtime configuration");

    fixture.component.ShowPage(synth::runtime_ui::RuntimeMainPage::Controllers);
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.controllers.back"));
    Require(fixture.services.saveCount == 1, "controllers back adds no save");

    fixture.component.ShowPage(synth::runtime_ui::RuntimeMainPage::File);
    fixture.component.DispatchAction(synth::ui::Action::Named("runtime.file.back"));
    Require(fixture.services.saveCount == 1, "file back does not save runtime configuration");
}

void TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot()
{
    Fixture fixture;
    fixture.services.currentSync = {.sendClock = false,
                                    .receiveClock = true,
                                    .sendTransport = false,
                                    .receiveTransport = true,
                                    .ppqn = 24};
    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarSync));

    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncSendClock, "1"));
    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncReceiveClock, "0"));
    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncSendTransport, "1"));
    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncReceiveTransport, "0"));
    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "96"));
    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarSync));
    Require(fixture.services.syncSnapshotCount == 1,
            "clicking the already-open Sync tab does not begin a second edit");
    Require(FindNodeById(fixture.component.BuildTree(),
                         synth::runtime_ui::NodeIds::kSyncPpqn)->text == "96",
            "clicking the already-open Sync tab preserves staged PPQN");

    fixture.services.syncStatus = {.currentBpm = 123.5,
                                   .lockState = "Locked",
                                   .sourceName = "Clock Controller",
                                   .outputLatencyMicros = 8'000,
                                   .ignoredInputCount = 2,
                                   .lateEventCount = 3,
                                   .droppedOutputCount = 4};
    fixture.component.Refresh();
    Require(fixture.services.syncRefreshCount == 1, "refresh requests sync diagnostics");
    const synth::ui::NodeTree stagedTree = fixture.component.BuildTree();
    Require(FindNodeById(stagedTree, synth::runtime_ui::NodeIds::kSyncSendClock)->checked,
            "refresh does not overwrite staged send-clock");
    Require(!FindNodeById(stagedTree, synth::runtime_ui::NodeIds::kSyncReceiveClock)->checked,
            "refresh does not overwrite staged receive-clock");
    Require(FindNodeById(stagedTree, synth::runtime_ui::NodeIds::kSyncPpqn)->text == "96",
            "refresh does not overwrite staged PPQN");
    Require(FindNodeById(stagedTree, synth::runtime_ui::NodeIds::kSyncWarning)->text.find(
                "nonstandard") != std::string::npos,
            "staged nonstandard PPQN warning remains visible");
    Require(FindNodeById(stagedTree, synth::runtime_ui::NodeIds::kSyncSource)->text.find(
                "Clock Controller") != std::string::npos,
            "refresh updates source diagnostics");

    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSyncBack));
    Require(fixture.services.syncCommitCount == 1, "Sync Back commits exactly once");
    Require(fixture.services.saveCount == 1, "successful Sync Back saves exactly once");
    Require(fixture.services.lastCommittedSync ==
                synth::SyncConfig{true, false, true, false, 96},
            "Sync Back commits one complete staged config");
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "successful Sync Back returns to application");

    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarSync));
    const synth::ui::NodeTree reopenedTree = fixture.component.BuildTree();
    Require(fixture.services.syncSnapshotCount == 2,
            "reopening Sync obtains one fresh committed snapshot");
    Require(FindNodeById(reopenedTree, synth::runtime_ui::NodeIds::kSyncPpqn)->text == "96",
            "reopening Sync starts from committed PPQN");

    fixture.services.syncCommitSucceeds = false;
    fixture.component.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "48"));
    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSyncBack));
    Require(fixture.services.syncCommitCount == 2, "rejected Sync Back attempts one commit");
    Require(fixture.services.saveCount == 1, "rejected Sync Back does not save");
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Sync,
            "rejected Sync Back stays on Sync");
    Require(FindNodeById(fixture.component.BuildTree(),
                         synth::runtime_ui::NodeIds::kSyncValidation)->text.find("apply") !=
                std::string::npos,
            "rejected Sync Back shows an apply error");
}

void TestRefreshUpdatesRuntimePageModelsAndRollingDeadline()
{
    Fixture fixture;

    fixture.component.Refresh();
    fixture.services.deadlinePercent = 4.0f;
    fixture.component.Refresh();

    Require(fixture.services.audioRefreshCount == 2, "refresh updates audio snapshot");
    Require(fixture.services.fileRefreshCount == 2, "refresh updates file snapshot");
    Require(fixture.services.controllersRefreshCount == 2, "refresh updates controllers surface");
    Require(fixture.services.syncRefreshCount == 2, "refresh updates sync status only");
    const synth::ui::Node* deadline = FindNodeById(
        fixture.component.BuildTree(), synth::runtime_ui::NodeIds::kSidebarDeadline);
    Require(deadline != nullptr, "deadline node exists after refresh");
    Require(deadline->text == "DSP 12%", "sidebar displays rolling deadline maximum");
}

void TestSidebarWarningReflectsControllersDiscoverySnapshot()
{
    Fixture fixture;
    fixture.services.controllerDevices.inputs.push_back(
        {.identifier = "twister-input", .name = "Midi Fighter Twister"});
    fixture.services.controllerDevices.outputs.push_back(
        {.identifier = "twister-output", .name = "Midi Fighter Twister"});

    fixture.component.Refresh();

    Require(FindNodeById(fixture.component.BuildTree(),
                         "runtime.sidebar.controllers.warning") != nullptr,
            "unclaimed recognized cached pair warns while application is open");
}

// An app that never defines RegisteredPage() (FakeApp) must produce
// exactly the sidebar and routing that existed before app-registered pages -- "optional
// means optional, assert don't assume" (design constraint), checked both as
// a tree shape and as a dispatched-action no-op.
void TestSidebarWithNoRegistrationHasNoAppButtonAndIgnoresItsAction()
{
    Fixture fixture;
    const synth::ui::NodeTree tree = fixture.component.BuildTree();

    Require(FindNodeById(tree, synth::runtime_ui::NodeIds::kSidebarApp) == nullptr,
            "no app-page button exists without registration");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).bounds.height == 200.0f,
            "the sidebar stays five fixed 40px rows tall without registration");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot).children.size() == 5,
            "the sidebar contains exactly the four built-in page entries plus the deadline readout");

    fixture.component.DispatchAction(
        synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarApp));
    Require(fixture.component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "dispatching the app-page action without registration does not navigate");
}

// An app that sets nothing gets the runtime's own name. Asserted against the
// literal rather than against another tree built the same way, so that
// changing the default fails here instead of moving both sides together.
void TestAudioPageButtonKeepsItsRuntimeNameWhenTheAppSetsNothing()
{
    Fixture fixture;
    const synth::ui::NodeTree tree = fixture.component.BuildTree();

    Require(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarAudio).label == "Audio",
            "the Audio page button reads the runtime's own name without an override");
}

// An app that sets one gets it, and gets nothing else: the override renames
// the BUTTON, so the page's own action, position and the rest of the sidebar
// have to come out identical to the un-renamed case.
void TestAudioPageButtonTakesTheAppsOwnNameWhenSet()
{
    RenamedAudioPageApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<RenamedAudioPageApp, FakeServices> component{app, services};

    const synth::ui::NodeTree tree = component.BuildTree();
    const synth::ui::Node& audioButton = FindNode(tree, synth::runtime_ui::NodeIds::kSidebarAudio);
    Require(audioButton.label == "Audio I/O", "the Audio page button reads the app's own name");
    Require(audioButton.action.has_value() &&
                audioButton.action->name == synth::runtime_ui::Actions::kSidebarAudio,
            "renaming the button does not change what it dispatches");

    const synth::ui::Node& sidebarRoot = FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot);
    Require(sidebarRoot.children.size() == 5,
            "renaming a page adds no entry and removes none");
    Require(sidebarRoot.children[0] == synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarAudio),
            "the renamed page stays first");
    Require(sidebarRoot.bounds.height == 200.0f,
            "renaming a page does not resize the sidebar");

    component.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarAudio));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Audio,
            "the renamed button still opens the Audio page");
}

// An app that does define RegisteredPage() gets a button after
// File, and selecting it shows the app-built tree spliced with its declared
// nested layout preserved (the nested-layout assertion pattern, reused
// against the whole registered page rather than an audio-page section).
void TestRegisteredPageButtonRendersAfterFileAndRoutesToSplicedAppTree()
{
    RegisteredPageApp app;
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<RegisteredPageApp, FakeServices> component{app, services};

    const synth::ui::NodeTree tree = component.BuildTree();
    const synth::ui::Node& sidebarRoot = FindNode(tree, synth::runtime_ui::NodeIds::kSidebarRoot);
    Require(sidebarRoot.children.size() == 6,
            "the sidebar gains exactly one more entry when a page is registered");
    Require(sidebarRoot.children[3] == synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarFile) &&
                sidebarRoot.children[4] == synth::ui::NodeId(synth::runtime_ui::NodeIds::kSidebarApp),
            "the registered page's button is placed directly after File");
    Require(FindNode(tree, synth::runtime_ui::NodeIds::kSidebarApp).label == "Custom",
            "the button's label is the app's registered title");

    component.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarApp));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::AppPage,
            "selecting the button navigates to the registered page");

    const synth::ui::NodeTree onPage = component.BuildTree();
    Require(onPage.nodes[1].id == synth::ui::NodeId(synth::runtime_ui::NodeIds::kAppRoot),
            "the app page's own root replaces the app root while it is open");

    const synth::ui::Node& heavy = FindNode(onPage, "app.custom.heavy");
    const synth::ui::Node& light = FindNode(onPage, "app.custom.light");
    Require(NearlyEqual(heavy.bounds.x, 40.0f),
            "the row's explicit 40px padding is honored, not LayoutOptions{}'s default -- proof "
            "the splice carried the app's declared layout rather than dropping it");
    Require(heavy.bounds.width > 0.0f && light.bounds.width > 0.0f,
            "both weighted children resolve to a positive width");
    Require(NearlyEqual(heavy.bounds.width / light.bounds.width, 3.0f),
            "the declared 3:1 weight split is honored, not an even default split");

    // Built-in pages remain reachable and unaffected while the app page exists.
    component.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarAudio));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Audio,
            "built-in pages remain reachable once an app page is registered");
    Require(FindNode(component.BuildTree(), synth::runtime_ui::NodeIds::kAudioRoot).id ==
                synth::ui::NodeId(synth::runtime_ui::NodeIds::kAudioRoot),
            "the audio page still renders exactly as it did before");

    // Back returns to the application without saving runtime configuration:
    // the app page is not one of the four RuntimePageKind values
    // RuntimePageBackSavesConfiguration recognizes (RuntimePagePolicy.hpp),
    // matching File's own no-save behavior.
    component.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kSidebarApp));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::AppPage,
            "the app page reopens");
    component.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kAppBack));
    Require(component.CurrentPage() == synth::runtime_ui::RuntimeMainPage::Application,
            "the app page's Back button returns to the application");
    Require(services.saveCount == 0, "leaving the app page does not save runtime configuration");
}

void TestWizardDiscoveryCacheRecomputesOnlyForCachedSnapshotChanges()
{
    synth::ControllerWizardDiscoveryCache cache;
    const synth::MidiDeviceList twisterPair{
        .inputs = {{.identifier = "twister-input", .name = "Midi Fighter Twister"}},
        .outputs = {{.identifier = "twister-output", .name = "Midi Fighter Twister"}},
    };

    Require(cache.UpdateDeviceList(twisterPair), "first device signal updates cached device snapshot");
    const std::uint64_t afterDeviceSignal = cache.Revision();
    Require(afterDeviceSignal == 1, "first device signal recomputes discovery once");
    Require(!cache.UpdateDeviceList(twisterPair), "unchanged device source does not update cache");
    Require(cache.Revision() == afterDeviceSignal,
            "unchanged device source does not recompute discovery");

    synth::MfTwisterControllerWizard wizard;
    synth::MfTwisterConfigForm form;
    const synth::WizardGenerationResult generated = wizard.GenerateProfile(
        form,
        {.name = "claimed", .input = {"twister-input", "Midi Fighter Twister"},
         .output = {"twister-output", "Midi Fighter Twister"}});
    Require(static_cast<bool>(generated), "create instrument snapshot for successful commit");
    synth::MidiInstrumentConfig committed;
    committed.controllers.push_back(*generated.controller);
    cache.UpdateInstrumentSnapshot(std::move(committed));
    Require(cache.Revision() == afterDeviceSignal + 1,
            "successful instrument commit recomputes against cached devices");
    Require(cache.Discovery().available.empty(), "committed claim clears cached candidate");
}

void RequireInvalidTree(synth::ui::NodeTree tree, const char* expectedMessage)
{
    Fixture fixture;
    fixture.app.surface.tree = std::move(tree);
    try
    {
        static_cast<void>(fixture.component.BuildTree());
    }
    catch (const std::invalid_argument& error)
    {
        Require(std::string(error.what()).find(expectedMessage) != std::string::npos,
                "invalid tree diagnostic");
        return;
    }
    throw std::runtime_error("invalid tree was accepted");
}

void TestRejectsASurfaceTooShortForTheRuntimeSidebar()
{
    ShortSurfaceApp app;
    app.surface.tree.nodes.front().bounds = {0.0f, 0.0f, 900.0f, 160.0f};
    FakeServices services;
    synth::runtime_ui::RuntimeMainComponent<ShortSurfaceApp, FakeServices> component{app, services};

    bool threw = false;
    std::string message;
    try
    {
        component.BuildTree();
    }
    catch (const std::invalid_argument& error)
    {
        threw = true;
        message = error.what();
    }
    Require(threw, "a surface shorter than the sidebar fails composition rather than overrunning");
    Require(message.find("runtime.sidebar.root") != std::string::npos,
            "the diagnostic names the subtree root that does not fit");
    Require(message.find("uiWidth/uiHeight") != std::string::npos,
            "the diagnostic names the declaration the host has to change");

    // The other direction, so the check is a bound rather than a blanket: the
    // ordinary 900x560 surface composes without complaint.
    Fixture ordinary;
    ordinary.component.BuildTree();
}

void TestRejectsRootSizeMismatch()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    tree.nodes.front().bounds.width = 899.0f;
    RequireInvalidTree(std::move(tree), "configured bounds");
}

void TestRejectsDuplicateNodeIds()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    synth::ui::Node duplicate;
    duplicate.id = "app.root";
    duplicate.kind = synth::ui::NodeKind::Label;
    tree.nodes.push_back(std::move(duplicate));
    RequireInvalidTree(std::move(tree), "duplicate node id");
}

void TestRejectsUnknownChild()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    tree.nodes.front().children.push_back(synth::ui::NodeId("app.missing"));
    RequireInvalidTree(std::move(tree), "unknown child");
}

void TestRejectsCycle()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    tree.nodes.front().children.push_back(synth::ui::NodeId("app.child"));
    synth::ui::Node child;
    child.id = "app.child";
    child.kind = synth::ui::NodeKind::Section;
    child.children.push_back(child.id);
    tree.nodes.push_back(std::move(child));
    RequireInvalidTree(std::move(tree), "cycle");
}

void TestRejectsAppRuntimeNamespace()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    tree.nodes.front().id = "runtime.app.root";
    RequireInvalidTree(std::move(tree), "reserved runtime namespace");
}

void TestRejectsDisconnectedGraph()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    synth::ui::Node disconnected;
    disconnected.id = "app.disconnected";
    disconnected.kind = synth::ui::NodeKind::Section;
    tree.nodes.push_back(std::move(disconnected));
    RequireInvalidTree(std::move(tree), "exactly one parentless root");
}

void TestRejectsMultiplyParentedDiamondGraph()
{
    synth::ui::NodeTree tree = MakeValidAppTree();
    tree.nodes.front().children = {synth::ui::NodeId("app.left"),
                                   synth::ui::NodeId("app.right")};

    synth::ui::Node left;
    left.id = "app.left";
    left.kind = synth::ui::NodeKind::Section;
    left.children.push_back(synth::ui::NodeId("app.shared"));
    tree.nodes.push_back(std::move(left));

    synth::ui::Node right;
    right.id = "app.right";
    right.kind = synth::ui::NodeKind::Section;
    right.children.push_back(synth::ui::NodeId("app.shared"));
    tree.nodes.push_back(std::move(right));

    synth::ui::Node shared;
    shared.id = "app.shared";
    shared.kind = synth::ui::NodeKind::Label;
    tree.nodes.push_back(std::move(shared));

    RequireInvalidTree(std::move(tree), "reachable exactly once");
}

int failureCount = 0;

template <typename Test>
void Run(const char* name, Test test)
{
    try
    {
        test();
        std::cout << "PASS " << name << '\n';
    }
    catch (const std::exception& error)
    {
        ++failureCount;
        std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
}

}  // namespace

int main()
{
    static_assert(synth::SynthApplication<FakeApp>);
    static_assert(synth::SynthApplication<ExtentAwareApp>);
    static_assert(synth::SynthApplication<SelfSizedApp>);
    static_assert(synth::SynthApplication<RegisteredPageApp>);
    static_assert(!synth::HasRegisteredPage<FakeApp>, "FakeApp opts out by never defining RegisteredPage()");
    static_assert(synth::HasRegisteredPage<RegisteredPageApp>);
    static_assert(synth::runtime_ui::RuntimeMainServices<FakeServices>);

    Run("TestPlacingASubtreeRootPlacesEveryDescendant",
        TestPlacingASubtreeRootPlacesEveryDescendant);
    Run("TestSubtreesArriveFullyResolved", TestSubtreesArriveFullyResolved);
    Run("TestCompositeBoundsPreserveAppAndAddSidebar", TestCompositeBoundsPreserveAppAndAddSidebar);
    Run("TestSelfSizedRootIsValidatedAgainstItsDeclaredBounds",
        TestSelfSizedRootIsValidatedAgainstItsDeclaredBounds);
    Run("TestSidebarIsPlacedAtTheDeclaredSlot", TestSidebarIsPlacedAtTheDeclaredSlot);
    Run("TestSidebarLargerThanItsSlotFailsComposition",
        TestSidebarLargerThanItsSlotFailsComposition);
    Run("TestEveryRuntimePageOpensBesideASlottedApp", TestEveryRuntimePageOpensBesideASlottedApp);
    Run("TestRuntimePageKeepsConfiguredBoundsBesideAnUnslottedApp",
        TestRuntimePageKeepsConfiguredBoundsBesideAnUnslottedApp);
    Run("TestExtentAwareAppTracksResizedContentExtent", TestExtentAwareAppTracksResizedContentExtent);
    Run("TestSidebarOpensEachPageAndBackRestoresApp", TestSidebarOpensEachPageAndBackRestoresApp);
    Run("TestSidebarShowsOnlyTheDeclaredPages", TestSidebarShowsOnlyTheDeclaredPages);
    Run("TestAppActionsRouteOnlyToAppSurface", TestAppActionsRouteOnlyToAppSurface);
    Run("TestRuntimeActionsRouteOnlyToOwningPageOrServices",
        TestRuntimeActionsRouteOnlyToOwningPageOrServices);
    Run("TestControllerDraftActionsReachControllerSurface",
        TestControllerDraftActionsReachControllerSurface);
    Run("TestBackFromConfigurationPageSavesRuntimeConfiguration",
        TestBackFromConfigurationPageSavesRuntimeConfiguration);
    Run("TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot",
        TestSyncStagesRefreshesCommitsAndReopensFromEngineSnapshot);
    Run("TestRefreshUpdatesRuntimePageModelsAndRollingDeadline",
        TestRefreshUpdatesRuntimePageModelsAndRollingDeadline);
    Run("TestSidebarWarningReflectsControllersDiscoverySnapshot",
        TestSidebarWarningReflectsControllersDiscoverySnapshot);
    Run("TestSidebarWithNoRegistrationHasNoAppButtonAndIgnoresItsAction",
        TestSidebarWithNoRegistrationHasNoAppButtonAndIgnoresItsAction);
    Run("TestAudioPageButtonKeepsItsRuntimeNameWhenTheAppSetsNothing",
        TestAudioPageButtonKeepsItsRuntimeNameWhenTheAppSetsNothing);
    Run("TestAudioPageButtonTakesTheAppsOwnNameWhenSet",
        TestAudioPageButtonTakesTheAppsOwnNameWhenSet);
    Run("TestRegisteredPageButtonRendersAfterFileAndRoutesToSplicedAppTree",
        TestRegisteredPageButtonRendersAfterFileAndRoutesToSplicedAppTree);
    Run("TestWizardDiscoveryCacheRecomputesOnlyForCachedSnapshotChanges",
        TestWizardDiscoveryCacheRecomputesOnlyForCachedSnapshotChanges);
    Run("TestRejectsASurfaceTooShortForTheRuntimeSidebar",
        TestRejectsASurfaceTooShortForTheRuntimeSidebar);
    Run("TestRejectsRootSizeMismatch", TestRejectsRootSizeMismatch);
    Run("TestRejectsDuplicateNodeIds", TestRejectsDuplicateNodeIds);
    Run("TestRejectsUnknownChild", TestRejectsUnknownChild);
    Run("TestRejectsCycle", TestRejectsCycle);
    Run("TestRejectsAppRuntimeNamespace", TestRejectsAppRuntimeNamespace);
    Run("TestRejectsDisconnectedGraph", TestRejectsDisconnectedGraph);
    Run("TestRejectsMultiplyParentedDiamondGraph", TestRejectsMultiplyParentedDiamondGraph);
    return failureCount == 0 ? 0 : 1;
}
