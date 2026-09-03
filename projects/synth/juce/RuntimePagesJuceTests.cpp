#include "RuntimePagesJuce.hpp"

#include "synth/RuntimePages.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

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

juce::Rectangle<int> SurfaceBoundsOf(const synth_juce::PortableComponent& root,
                                     const juce::Component& component)
{
    return root.getLocalArea(&component, component.getLocalBounds());
}

void RequireInsideRoot(const synth_juce::PortableComponent& root,
                       const juce::Component* component,
                       const char* label)
{
    Require(component != nullptr, label);
    const juce::Rectangle<int> boundsInRoot =
        root.getLocalArea(component->getParentComponent(), component->getBounds());
    Require(root.getLocalBounds().contains(boundsInRoot), label);
}

}  // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    synth_runtime::SidebarHost sidebar;
    sidebar.setSize(static_cast<int>(synth::runtime_ui::Layout::kSidebarWidth), 200);
    sidebar.RefreshFromSurface();

    std::string lastAction;
    sidebar.onAction = [&lastAction](const synth::ui::Action& action) {
        lastAction = action.name;
    };

    synth::runtime_ui::SidebarSurface sidebarSurface;
    sidebarSurface.SetDeadlinePercent(7.5f);
    synth_juce::PortableComponent sidebarRenderer(sidebarSurface);
    sidebarRenderer.setSize(static_cast<int>(synth::runtime_ui::Layout::kSidebarWidth), 200);
    sidebarRenderer.RefreshFromSurface();
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarAudio) != nullptr,
            "sidebar audio control renders");
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarDeadline) != nullptr,
            "sidebar deadline control renders");
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarSync) != nullptr,
            "sidebar Sync control renders");

    auto* audioButton = dynamic_cast<juce::TextButton*>(
        sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarAudio));
    Require(audioButton != nullptr, "sidebar audio is a TextButton");
    sidebarSurface.SetActionHandler([&lastAction](const synth::ui::Action& action) {
        lastAction = action.name;
    });
    audioButton->onClick();
    Require(lastAction == synth::runtime_ui::Actions::kSidebarAudio, "sidebar audio dispatches navigation action");

    sidebarSurface.SetDeadlinePercent(9.0f);
    sidebarRenderer.RefreshFromSurface();
    const synth::ui::NodeTree refreshedSidebar = sidebarSurface.BuildTree();
    Require(FindNodeById(refreshedSidebar, synth::runtime_ui::NodeIds::kSidebarDeadline)->text == "CPU 9%",
            "sidebar deadline refresh updates semantic node");

    // sru-2 / sru-33: the Controllers warning marker is a portable node the
    // JUCE backend renders and drops exactly as Chrome does. The sidebar knows
    // only the boolean the host hands it.
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarControllersWarning) == nullptr,
            "sidebar renders no Controllers warning marker without an available candidate");
    sidebarSurface.SetControllersWarning(true);
    sidebarRenderer.RefreshFromSurface();
    auto* controllersWarning = dynamic_cast<juce::Label*>(
        sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarControllersWarning));
    Require(controllersWarning != nullptr && controllersWarning->getText() == juce::String("!"),
            "sidebar renders the Controllers warning marker while a candidate is available");
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarControllers) != nullptr,
            "the warning marker does not replace the Controllers entry");
    sidebarSurface.SetControllersWarning(false);
    sidebarRenderer.RefreshFromSurface();
    Require(sidebarRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSidebarControllersWarning) == nullptr,
            "sidebar drops the Controllers warning marker when the last candidate clears");

    synth::runtime_ui::AudioPageSurface audioSurface;
    synth::runtime_ui::AudioPageSnapshot& audioSnapshot = audioSurface.Snapshot();
    audioSnapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Speakers"},
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    audioSnapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        {"Microphone"},
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    audioSnapshot.deviceLineText = "Speakers: 48000 Hz, 512 frames";
    audioSnapshot.statusLineText = "Audio: Speakers";
    audioSnapshot.selectedOutputId = "Speakers";
    audioSnapshot.selectedInputId = "Microphone";
    audioSnapshot.showInputCombo = true;
    audioSurface.SetContentBounds({0.0f, 0.0f, 640.0f, 200.0f});

    synth_juce::PortableComponent audioRenderer(audioSurface);
    audioRenderer.setSize(640, 200);
    audioRenderer.RefreshFromSurface();
    Require(audioRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioOutput) != nullptr,
            "audio output combo renders");
    Require(audioRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioInput) != nullptr,
            "audio input combo renders when configured");
    auto* outputCaption = dynamic_cast<juce::Label*>(
        audioRenderer.FindByNodeId((std::string(synth::runtime_ui::NodeIds::kAudioOutput) + ".caption").c_str()));
    Require(outputCaption != nullptr && outputCaption->getText() == juce::String("Output device"),
            "JUCE audio output caption remains visible while a device is selected");
    auto* inputCaption = dynamic_cast<juce::Label*>(
        audioRenderer.FindByNodeId((std::string(synth::runtime_ui::NodeIds::kAudioInput) + ".caption").c_str()));
    Require(inputCaption != nullptr && inputCaption->getText() == juce::String("Input device"),
            "JUCE audio input caption remains visible while a device is selected");
    Require(audioRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioDeviceLine) != nullptr,
            "audio device line renders");

    audioSnapshot.showInputCombo = false;
    audioRenderer.RefreshFromSurface();
    Require(audioRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioInput) == nullptr,
            "JUCE audio input combo is removed when the input selector is hidden");
    Require(audioRenderer.FindByNodeId((std::string(synth::runtime_ui::NodeIds::kAudioInput) + ".caption").c_str()) == nullptr,
            "JUCE audio input caption is removed with the hidden input selector");

    audioSnapshot.showInputCombo = true;
    audioSnapshot.deviceLineText = "Speakers: 44100 Hz, 256 frames";
    audioRenderer.RefreshFromSurface();
    const synth::ui::NodeTree refreshedAudioTree = audioSurface.BuildTree();
    const synth::ui::Node* deviceLine =
        FindNodeById(refreshedAudioTree, synth::runtime_ui::NodeIds::kAudioDeviceLine);
    Require(deviceLine != nullptr && deviceLine->text == "Speakers: 44100 Hz, 256 frames",
            "audio page state refresh updates device line");

    // sru-3: the requested/active input diagnostic the JUCE runtime publishes
    // reaches the user through the existing status line, verbatim.
    audioSnapshot.statusLineText = "Input requested 2 / active 1";
    audioRenderer.RefreshFromSurface();
    auto* inputStatusLine = dynamic_cast<juce::Label*>(
        audioRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kAudioStatusLine));
    Require(inputStatusLine != nullptr &&
                inputStatusLine->getText() == juce::String("Input requested 2 / active 1"),
            "JUCE audio status line renders the requested/active input diagnostic");

    synth::runtime_ui::SyncPageSurface syncSurface;
    syncSurface.BeginEdit({.sendClock = true,
                           .receiveClock = false,
                           .sendTransport = true,
                           .receiveTransport = false,
                           .ppqn = 96});
    syncSurface.RefreshStatus({.currentBpm = 121.5,
                               .lockState = "Locked",
                               .sourceName = "Clock Controller",
                               .outputLatencyMicros = 5'500,
                               .ignoredInputCount = 2,
                               .lateEventCount = 3,
                               .droppedOutputCount = 4});
    syncSurface.SetContentBounds({0.0f, 0.0f, 240.0f, 560.0f});
    synth_juce::PortableComponent syncRenderer(syncSurface);
    syncRenderer.setSize(240, 560);
    syncRenderer.RefreshFromSurface();
    auto* syncSendClock = dynamic_cast<juce::ToggleButton*>(
        syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncSendClock));
    Require(syncSendClock != nullptr,
            "Sync send-clock renders as ToggleButton");
    Require(dynamic_cast<juce::ToggleButton*>(
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncReceiveClock)) != nullptr,
            "Sync receive-clock renders as ToggleButton");
    Require(dynamic_cast<juce::ToggleButton*>(
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncSendTransport)) != nullptr,
            "Sync send-transport renders as ToggleButton");
    Require(dynamic_cast<juce::ToggleButton*>(
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncReceiveTransport)) != nullptr,
            "Sync receive-transport renders as ToggleButton");
    auto* ppqnEditor = dynamic_cast<juce::TextEditor*>(
        syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncPpqn));
    Require(ppqnEditor != nullptr && ppqnEditor->getText() == "96",
            "Sync PPQN renders as TextEditor with staged text");
    syncSendClock->setToggleState(false, juce::dontSendNotification);
    syncSendClock->onClick();
    Require(!syncSurface.StagedConfiguration().sendClock,
            "JUCE Sync toggle dispatches the portable zero-valued action");
    ppqnEditor->setText("48");
    ppqnEditor->onReturnKey();
    Require(syncSurface.StagedConfiguration().ppqn == 48,
            "JUCE Sync editor dispatches the portable PPQN action");
    // The two diagnostic lines are emitted only when they have something to
    // say (sru-48, task 6.2), so they are checked in the states that produce
    // them rather than in the always-on list below. Asserting presence AND
    // absence pins the new contract in JUCE, not just the Label mapping.
    // A standard PPQN first, so neither diagnostic has anything to say and the
    // absence assertions below mean something.
    syncSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "24"));
    syncRenderer.RefreshFromSurface();
    Require(syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncValidation) == nullptr &&
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncWarning) == nullptr,
            "neither Sync diagnostic is emitted while a standard PPQN is staged");
    syncSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "96x"));
    syncRenderer.RefreshFromSurface();
    Require(dynamic_cast<juce::Label*>(
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncValidation)) != nullptr,
            "Sync validation renders as Label while the staged PPQN is invalid");
    Require(syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncWarning) == nullptr,
            "an unparseable entry leaves the staged value alone, so it raises no warning");
    syncSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "96"));
    syncRenderer.RefreshFromSurface();
    Require(dynamic_cast<juce::Label*>(
                syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncWarning)) != nullptr,
            "Sync warning renders as Label while the staged PPQN is nonstandard");
    Require(syncRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kSyncValidation) == nullptr,
            "Sync validation is removed once the staged PPQN parses");
    syncSurface.DispatchAction(
        synth::ui::Action::WithValue(synth::runtime_ui::Actions::kSyncPpqn, "48"));
    syncRenderer.RefreshFromSurface();
    for (const char* statusId : {synth::runtime_ui::NodeIds::kSyncBpm,
                                 synth::runtime_ui::NodeIds::kSyncLock,
                                 synth::runtime_ui::NodeIds::kSyncSource,
                                 synth::runtime_ui::NodeIds::kSyncOutputLatency,
                                 synth::runtime_ui::NodeIds::kSyncIgnoredInput,
                                 synth::runtime_ui::NodeIds::kSyncLateEvents,
                                 synth::runtime_ui::NodeIds::kSyncDroppedOutput})
    {
        Require(dynamic_cast<juce::Label*>(syncRenderer.FindByNodeId(statusId)) != nullptr,
                "Sync read-only status renders as Label");
        RequireInsideRoot(syncRenderer, syncRenderer.FindByNodeId(statusId),
                          "Sync narrow status remains inside root bounds");
    }

    synth::runtime_ui::FilePageSurface fileSurface;
    synth::runtime_ui::FilePageSnapshot& fileSnapshot = fileSurface.Snapshot();
    fileSnapshot.patchNameText = "(no patch)";
    fileSnapshot.statusText = "Ready";
    const std::filesystem::path patchRoot =
        std::filesystem::temp_directory_path() / "sheaf_runtime_pages_juce_test";
    std::filesystem::remove_all(patchRoot);
    std::filesystem::create_directories(patchRoot / "DemoPatch");
    fileSnapshot.patchesRoot = patchRoot.string();
    fileSurface.SetContentBounds({0.0f, 0.0f, 640.0f, 200.0f});

    synth_juce::PortableComponent fileRenderer(fileSurface);
    fileRenderer.setSize(640, 200);
    fileRenderer.RefreshFromSurface();
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileSave) != nullptr, "file save renders");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFilePatchName) != nullptr,
            "file patch name renders");

    fileSnapshot.patchNameText = "demo_patch";
    fileRenderer.RefreshFromSurface();
    const synth::ui::NodeTree refreshedFileTree = fileSurface.BuildTree();
    const synth::ui::Node* patchName =
        FindNodeById(refreshedFileTree, synth::runtime_ui::NodeIds::kFilePatchName);
    Require(patchName != nullptr && patchName->text == "demo_patch", "file page state refresh updates patch name");

    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileLoad));
    fileSurface.SetContentBounds({0.0f, 0.0f, 760.0f, 420.0f});
    fileRenderer.setSize(760, 420);
    fileRenderer.RefreshFromSurface();
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowser) != nullptr,
            "browser viewer component renders");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserTitle) != nullptr,
            "browser title renders");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserCurrentPath) == nullptr,
            "flat browser omits path control");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserParent) == nullptr,
            "flat browser omits parent control");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::FileBrowserEntryOpen(0)) == nullptr,
            "flat browser omits per-row open control");
    Require(fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::FileBrowserEntry(0)) != nullptr,
            "browser row renders");
    const synth::ui::NodeTree openBrowserTree = fileSurface.BuildTree();
    const std::string firstBrowserEntryId = synth::runtime_ui::NodeIds::FileBrowserEntry(0);
    const synth::ui::Node* browserEntry =
        FindNodeById(openBrowserTree, firstBrowserEntryId.c_str());
    Require(browserEntry != nullptr &&
                browserEntry->doubleClickAction.has_value() &&
                browserEntry->doubleClickAction->name == synth::runtime_ui::Actions::kFileBrowserAccept,
            "flat browser row double-click loads");
    juce::Component* browser = fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowser);
    juce::Component* row = fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::FileBrowserEntry(0));
    Require(browser != nullptr && row != nullptr &&
                SurfaceBoundsOf(fileRenderer, *browser).contains(SurfaceBoundsOf(fileRenderer, *row)),
            "browser row is inside browser viewer bounds");

    fileSurface.DispatchAction(synth::ui::Action::Named(synth::runtime_ui::Actions::kFileSaveAs));
    fileSurface.SetContentBounds({0.0f, 0.0f, 360.0f, 360.0f});
    fileRenderer.setSize(360, 360);
    fileRenderer.RefreshFromSurface();
    RequireInsideRoot(fileRenderer,
                      fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserSaveName),
                      "narrow save-name field remains inside root bounds");
    RequireInsideRoot(fileRenderer,
                      fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileStatus),
                      "narrow status remains inside root bounds");
    RequireInsideRoot(fileRenderer,
                      fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserConfirm),
                      "narrow confirm remains inside root bounds");
    RequireInsideRoot(fileRenderer,
                      fileRenderer.FindByNodeId(synth::runtime_ui::NodeIds::kFileBrowserCancel),
                      "narrow cancel remains inside root bounds");
    std::filesystem::remove_all(patchRoot);

    std::cout << "RuntimePagesJuceTests passed\n";
    return 0;
}
