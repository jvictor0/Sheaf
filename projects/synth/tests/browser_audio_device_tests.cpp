#include "synth/browser/BrowserAudioDevices.hpp"
#include "synth/browser/BrowserMidiBridge.hpp"
#include "synth/browser/BrowserRuntimeMainServices.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
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

using synth_browser::BrowserAudioInputState;
using synth_browser::BrowserAudioInputStatus;

BrowserAudioInputState InputState(std::size_t requested,
                                  std::size_t active,
                                  BrowserAudioInputStatus status)
{
    return BrowserAudioInputState{
        .requestedChannels = requested, .activeChannels = active, .status = status};
}

// The output option list is derived from the submitted device list
// (BuildBrowserAudioSnapshot), not a fixed constant, so this pins the
// no-devices-submitted case specifically -- not "the browser only ever
// exposes System Default output".
void TestZeroInputBrowserExposesOnlySystemDefaultOutputWithNoSubmittedDevices()
{
    const synth::AudioDeviceState state{.outputDeviceName = "Named device", .inputDeviceName = "Ignored input"};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(state);

    Require(snapshot.outputOptions.size() == 1, "one browser output option");
    Require(snapshot.outputOptions.front().id == "system_default", "system default output id");
    Require(snapshot.outputOptions.front().label == "System Default", "system default output label");
    Require(snapshot.selectedOutputId == "system_default", "system default selected");
    Require(!snapshot.showInputCombo, "browser hides input selector");
    Require(snapshot.inputOptions.empty(), "browser has no input options");
    Require(!snapshot.showInputRetry, "browser hides retry for a zero-input application");
    Require(snapshot.statusLineText.empty(), "a zero-input application claims no input status");
}

void TestInputCapableBrowserExposesOneNoInputOption()
{
    const synth::AudioDeviceState state{.outputDeviceName = {}, .inputDeviceName = "Stale Named Input"};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online));

    Require(snapshot.showInputCombo, "an input-capable browser application shows the input selector");
    Require(snapshot.inputOptions.size() == 1, "one browser input option");
    Require(snapshot.inputOptions.front().id == "no_input", "no input id");
    Require(snapshot.inputOptions.front().label == "No Input", "no input label");
    Require(snapshot.selectedInputId == "no_input",
            "a stale persisted input name still selects No Input");
    Require(snapshot.statusLineText == "Input requested 4 / active 4",
            "online capture reports the requested and active counts alone");
    Require(!snapshot.showInputRetry, "online capture hides Retry Input");
}

void TestBrowserInputNoInputSelectionPersistsAsEmptyName()
{
    Require(synth_browser::BrowserInputDeviceName("no_input", {}).empty(),
            "no input persists as empty input name");
}

void TestBrowserInputResolvesAgainstSubmittedListAndFallsBackWhenAbsent()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = "out-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    Require(synth_browser::BrowserInputDeviceName("USB Mic", devices) == "USB Mic",
            "a submitted input device's id resolves to its name");
    Require(synth_browser::BrowserInputDeviceName("named-input", devices).empty(),
            "an id absent from the submitted list falls back to empty rather than being claimed");
}

void TestBrowserInputStatesAreDistinctAndRetryableOnlyWhileOffline()
{
    struct Expectation
    {
        BrowserAudioInputState input;
        std::string statusLineText;
        bool showInputRetry;
    };

    const std::vector<Expectation> expectations{
        {InputState(4, 0, BrowserAudioInputStatus::NotRequested),
         "Input requested 4 / active 0 - microphone capture not started",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::Requesting),
         "Input requested 4 / active 0 - requesting microphone access",
         false},
        {InputState(4, 0, BrowserAudioInputStatus::PermissionDenied),
         "Input requested 4 / active 0 - microphone permission denied",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::ApiUnavailable),
         "Input requested 4 / active 0 - microphone capture unavailable",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::PrerequisiteBlocked),
         "Input requested 4 / active 0 - microphone prerequisite unavailable",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::InsecureContext),
         "Input requested 4 / active 0 - microphone requires a secure context",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::PermissionsPolicyBlocked),
         "Input requested 4 / active 0 - microphone blocked by permissions policy",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::AudioContextUnavailable),
         "Input requested 4 / active 0 - microphone requires the launch-owned AudioContext",
         true},
        {InputState(4, 0, BrowserAudioInputStatus::StreamEnded),
         "Input requested 4 / active 0 - microphone stream ended",
         true},
        {InputState(4, 1, BrowserAudioInputStatus::ChannelCountUnreported),
         "Input requested 4 / active 1 - microphone channel count unreported, input channel shortfall",
         false},
        {InputState(4, 4, BrowserAudioInputStatus::ChannelCountUnreported),
         "Input requested 4 / active 4 - microphone channel count unreported",
         false},
        {InputState(4, 2, BrowserAudioInputStatus::Online),
         "Input requested 4 / active 2 - input channel shortfall",
         false},
    };

    std::vector<std::string> seen;
    for (const Expectation& expectation : expectations)
    {
        const synth::runtime_ui::AudioPageSnapshot snapshot =
            synth_browser::BuildBrowserAudioSnapshot({}, expectation.input);
        Require(snapshot.statusLineText == expectation.statusLineText,
                "browser input status text matches the published capture state");
        Require(snapshot.showInputRetry == expectation.showInputRetry,
                "Retry Input is offered exactly while capture is offline");
        Require(snapshot.showInputCombo, "every input-capable state keeps the input selector");
        seen.push_back(snapshot.statusLineText);
    }
    std::sort(seen.begin(), seen.end());
    Require(std::adjacent_find(seen.begin(), seen.end()) == seen.end(),
            "each browser capture state reads differently");
}

void TestBrowserDefaultSelectionPersistsAsEmptyName()
{
    Require(synth_browser::BrowserOutputDeviceName("system_default", {}).empty(),
            "system default persists as empty output name");
}

void TestBrowserOutputResolvesAgainstSubmittedListAndFallsBackWhenAbsent()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "out-1", .label = "Audio Interface", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    Require(synth_browser::BrowserOutputDeviceName("Audio Interface", devices) == "Audio Interface",
            "a submitted output device's id resolves to its name");
    Require(synth_browser::BrowserOutputDeviceName("named-output", devices).empty(),
            "an id absent from the submitted list falls back to empty rather than being claimed");
}

void TestBrowserSnapshotPresentsEveryDeviceFromAMultiDeviceSubmittedListAndDefaultsToNoInput()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = "in-2", .label = "Built-in Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = "out-1", .label = "USB Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
        {.deviceId = "out-2", .label = "Built-in Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    const synth::AudioDeviceState state{};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online), devices);

    Require(snapshot.outputOptions.size() == 3, "system default plus every submitted output device");
    Require(snapshot.outputOptions[1].label == "USB Speakers", "first submitted output device is presented");
    Require(snapshot.outputOptions[2].label == "Built-in Speakers", "second submitted output device is presented");

    Require(snapshot.inputOptions.size() == 3, "no input plus every submitted input device");
    Require(snapshot.inputOptions.front().id == "no_input",
            "no input remains the first input option in a multi-device list");
    Require(snapshot.inputOptions[1].label == "USB Mic", "first submitted input device is presented");
    Require(snapshot.inputOptions[2].label == "Built-in Mic", "second submitted input device is presented");
    Require(snapshot.selectedInputId == "no_input",
            "no input remains the default and startup selection with no persisted device name");
}

void TestBrowserSnapshotWithEmptySubmittedListExposesOnlyDefaultsAndDoesNotThrow()
{
    const synth::AudioDeviceState state{};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online), {});

    Require(snapshot.outputOptions.size() == 1, "system default alone with no submitted output devices");
    Require(snapshot.outputOptions.front().id == "system_default", "system default output id");
    Require(snapshot.inputOptions.size() == 1, "no input alone with no submitted input devices");
    Require(snapshot.inputOptions.front().id == "no_input", "no input id");
}

// The measured unpermitted-page case: `enumerateDevices` still reports one
// entry per physical device, but with both `deviceId` and `label` empty,
// since the page has never been granted permission to name them.
void TestBrowserSnapshotOmitsSubmittedDevicesWithEmptyLabelAndDeviceId()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    const synth::AudioDeviceState state{};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online), devices);

    Require(snapshot.outputOptions.size() == 1,
            "an unpermitted page's empty-label output entry is not presented as a named device");
    Require(snapshot.inputOptions.size() == 1,
            "an unpermitted page's empty-label input entry is not presented as a named device");
}

void TestUnpermittedPageIsOfferedAWayToAskForAccess()
{
    // The exact shape an unpermitted page enumerates: entries present, both
    // fields empty. Nothing here can be selected and Retry re-requests the
    // empty selection, so without this offer the page has no route to a
    // microphone at all.
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Input},
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    const synth::AudioDeviceState state{};
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(1, 0, BrowserAudioInputStatus::NotRequested), devices);

    Require(snapshot.inputOptions.size() == 1,
            "an unpermitted page still presents no unnamed entry as a device");
    Require(snapshot.showInputPermissionRequest,
            "a page that enumerates inputs it cannot name is offered a way to ask for access");
}

void TestAWayToAskIsNotOfferedWhereItWouldAchieveNothing()
{
    const synth::AudioDeviceState state{};

    // Permission already held: labels populate, so there is nothing to ask for.
    const std::vector<synth_browser::BrowserAudioDevice> named{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    Require(!synth_browser::BuildBrowserAudioSnapshot(
                 state, InputState(1, 0, BrowserAudioInputStatus::NotRequested), named)
                 .showInputPermissionRequest,
            "a page that can already name its inputs is not told to ask");

    // No input devices at all: nothing to ask about.
    const std::vector<synth_browser::BrowserAudioDevice> outputsOnly{
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    Require(!synth_browser::BuildBrowserAudioSnapshot(
                 state, InputState(1, 0, BrowserAudioInputStatus::NotRequested), outputsOnly)
                 .showInputPermissionRequest,
            "a machine with no input device is not invited to ask for one");

    // A zero-input application never reaches the offer.
    const std::vector<synth_browser::BrowserAudioDevice> unnamedInput{
        {.deviceId = {}, .label = {}, .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    Require(!synth_browser::BuildBrowserAudioSnapshot(
                 state, InputState(0, 0, BrowserAudioInputStatus::NotRequested), unnamedInput)
                 .showInputPermissionRequest,
            "an application that requests no input is not offered capture access");
}

void TestBrowserSnapshotFallsBackWhenPersistedInputSelectionIsAbsentFromSubmittedList()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    synth::AudioDeviceState state{};
    state.inputDeviceName = "Unplugged Mic";
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online), devices);

    Require(snapshot.selectedInputId == "no_input",
            "a persisted selection naming a device absent from the submitted list falls back to No Input "
            "rather than resolving to it");
}

void TestBrowserSnapshotResolvesPersistedInputSelectionPresentInSubmittedList()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    synth::AudioDeviceState state{};
    state.inputDeviceName = "USB Mic";
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, InputState(4, 4, BrowserAudioInputStatus::Online), devices);

    Require(snapshot.selectedInputId == "USB Mic",
            "a persisted selection naming a device present in the submitted list resolves to that device");
}

void TestBrowserSnapshotFallsBackWhenPersistedOutputSelectionIsAbsentFromSubmittedList()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "out-1", .label = "USB Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    synth::AudioDeviceState state{};
    state.outputDeviceName = "Unplugged Speakers";
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, {}, devices);

    Require(snapshot.selectedOutputId == "system_default",
            "a persisted output selection naming a device absent from the submitted list falls back to "
            "System Default rather than resolving to it");
}

void TestBrowserSnapshotResolvesPersistedOutputSelectionPresentInSubmittedList()
{
    const std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "out-1", .label = "USB Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    synth::AudioDeviceState state{};
    state.outputDeviceName = "USB Speakers";
    const synth::runtime_ui::AudioPageSnapshot snapshot = synth_browser::BuildBrowserAudioSnapshot(
        state, {}, devices);

    Require(snapshot.selectedOutputId == "USB Speakers",
            "a persisted output selection naming a device present in the submitted list resolves to that device");
}

class ServiceSurface final : public synth::ui::Surface
{
public:
    synth::ui::NodeTree BuildTree() override
    {
        synth::ui::Node root;
        root.id = "service.contract.root";
        root.kind = synth::ui::NodeKind::Root;
        root.bounds = {0.0f, 0.0f, 320.0f, 240.0f};
        return synth::ui::NodeTree{{std::move(root)}};
    }

    void SetActionHandler(ActionHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void DispatchAction(const synth::ui::Action& action) override
    {
        if (handler_)
        {
            handler_(action);
        }
    }

private:
    ActionHandler handler_;
};

class ServiceApp
{
public:
    static synth::RuntimeConfig Config()
    {
        return synth::RuntimeConfig{
            .appName = "BrowserServicesContract",
            .uiWidth = 320,
            .uiHeight = 240,
        };
    }

    void Init(synth::AppContext*) {}
    void ProcessBlock(synth::AudioBlock&) {}
    synth::ui::Surface& PortableSurface() { return surface_; }

private:
    ServiceSurface surface_;
};

void TestBrowserServicesExposeNegotiatedDefaultAudio()
{
    synth::Engine<ServiceApp> engine([] { return std::uint64_t{0}; });
    synth_browser::BrowserMidiBridge<synth::Engine<ServiceApp>> midiBridge(engine);
    std::vector<synth_browser::BrowserAudioDevice> devices;
    synth_browser::BrowserRuntimeMainServices<ServiceApp> services(engine, midiBridge, devices);

    synth::AudioDeviceState persisted;
    persisted.outputDeviceName = "previous-device";
    engine.SetAudioDeviceFromHost(persisted);

    synth::runtime_ui::AudioPageSnapshot snapshot;
    services.RefreshAudio(snapshot);
    Require(snapshot.deviceLineText == "No audio device",
            "browser reports no negotiated audio before prepare");

    services.RecordAudioNegotiation(48000.0, 128);
    services.RefreshAudio(snapshot);
    Require(snapshot.outputOptions.size() == 1, "services retain one output choice");
    Require(snapshot.selectedOutputId == "system_default", "services select system default");
    Require(snapshot.deviceLineText == "System Default: 48000 Hz, 128 frames",
            "services report negotiated browser audio");
    Require(services.DeadlineSamplePercent() == 0.0f,
            "browser deadline sample remains explicitly unavailable");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "system_default"));
    Require(engine.AudioDeviceSnapshot().outputDeviceName.empty(),
            "services persist system default as empty output name");
}

// Regression: the services layer composes its own status line, so a zero-input
// application must not pick up the "capture not started" detail that an
// input-capable application in the same state would show.
void TestZeroInputServicesPublishNoInputStatus()
{
    synth::Engine<ServiceApp> engine([] { return std::uint64_t{0}; });
    synth_browser::BrowserMidiBridge<synth::Engine<ServiceApp>> midiBridge(engine);
    std::vector<synth_browser::BrowserAudioDevice> devices;
    synth_browser::BrowserRuntimeMainServices<ServiceApp> services(
        engine, midiBridge, devices, {},
        [] { return InputState(0, 0, BrowserAudioInputStatus::NotRequested); });

    synth::runtime_ui::AudioPageSnapshot snapshot;
    services.RefreshAudio(snapshot);
    Require(snapshot.statusLineText.empty(),
            "a zero-input application publishes no input status through services");
    Require(!snapshot.showInputCombo, "services hide the input selector for a zero-input app");
    Require(!snapshot.showInputRetry, "services hide Retry Input for a zero-input app");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "system_default"));
    services.RefreshAudio(snapshot);
    Require(snapshot.statusLineText == "Using System Default",
            "a zero-input application still shows its output selection acknowledgement");
}

void TestBrowserServicesPublishCaptureStatusAndUserRetry()
{
    synth::Engine<ServiceApp> engine([] { return std::uint64_t{0}; });
    synth_browser::BrowserMidiBridge<synth::Engine<ServiceApp>> midiBridge(engine);
    std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    BrowserAudioInputState published = InputState(4, 4, BrowserAudioInputStatus::Online);
    synth_browser::BrowserRuntimeMainServices<ServiceApp> services(
        engine, midiBridge, devices, {}, [&published] { return published; });

    synth::AudioDeviceState persisted;
    persisted.inputDeviceName = "previous-input";
    engine.SetAudioDeviceFromHost(persisted);

    synth::runtime_ui::AudioPageSnapshot snapshot;
    services.RefreshAudio(snapshot);
    Require(snapshot.showInputCombo, "services expose the input selector for an input-capable app");
    Require(snapshot.statusLineText == "Input requested 4 / active 4",
            "services report the live capture counts");

    // Poisoned to Output so a passing Require below proves the freshly
    // constructed services instance really reports its Input default, rather
    // than the assertion coincidentally matching an unwritten local.
    synth_browser::BrowserAudioDeviceKind control = synth_browser::BrowserAudioDeviceKind::Output;
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kNoPendingAudioRequest &&
                control == synth_browser::BrowserAudioDeviceKind::Input,
            "no request is pending before the user asks for one, and the control defaults to input");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "USB Mic"));
    Require(engine.AudioDeviceSnapshot().inputDeviceName == "USB Mic",
            "services persist a submitted device's name");
    Require(services.ConsumePendingAudioRequest(control) == 0 &&
                control == synth_browser::BrowserAudioDeviceKind::Input,
            "selecting the one submitted input device arms its index against the input control");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "no_input"));
    Require(engine.AudioDeviceSnapshot().inputDeviceName.empty(),
            "services persist no input as empty input name");
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest,
            "selecting No Input arms a release rather than an index");
    services.RefreshAudio(snapshot);
    Require(snapshot.statusLineText == "Input requested 4 / active 4 - Using System Default",
            "a selection acknowledgement follows the requested and active counts");

    // An id no longer present in the submitted list is not thrown on -- it
    // falls back to empty (No Input), the same way a stale persisted name does.
    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "named-input"));
    Require(engine.AudioDeviceSnapshot().inputDeviceName.empty(),
            "an id absent from the submitted list persists as empty input name");
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest,
            "an id absent from the submitted list arms a release rather than an index");

    published = InputState(4, 0, BrowserAudioInputStatus::PermissionDenied);
    services.RefreshAudio(snapshot);
    Require(snapshot.statusLineText == "Input requested 4 / active 0 - microphone permission denied",
            "a capture diagnostic displaces the stale selection acknowledgement");
    Require(snapshot.showInputRetry, "denied capture offers Retry Input");

    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kNoPendingAudioRequest,
            "no input request is pending before the user asks for one");
    services.DispatchAudio(synth::ui::Action::Named(synth::runtime_ui::Actions::kAudioInputRetry));
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest,
            "retrying while the selection is No Input reacquires that same release, not an index");
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kNoPendingAudioRequest,
            "a consumed request does not repeat itself");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "USB Mic"));
    services.ConsumePendingAudioRequest(control);
    services.DispatchAudio(synth::ui::Action::Named(synth::runtime_ui::Actions::kAudioInputRetry));
    Require(services.ConsumePendingAudioRequest(control) == 0,
            "retrying while a submitted device is selected reacquires that same index");
}

// The output-selection half of the same generalized mechanism: selecting a
// submitted output device arms its index against the output control, System
// Default arms a release, and an id absent from the submitted list falls
// back to that same release -- exactly the input control's precedent, with
// no retry equivalent since output has no capture to lose.
void TestBrowserServicesArmsPendingRequestForOutputSelection()
{
    synth::Engine<ServiceApp> engine([] { return std::uint64_t{0}; });
    synth_browser::BrowserMidiBridge<synth::Engine<ServiceApp>> midiBridge(engine);
    std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "out-1", .label = "USB Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
        {.deviceId = "out-2", .label = "Built-in Speakers", .kind = synth_browser::BrowserAudioDeviceKind::Output},
    };
    synth_browser::BrowserRuntimeMainServices<ServiceApp> services(engine, midiBridge, devices);

    synth_browser::BrowserAudioDeviceKind control = synth_browser::BrowserAudioDeviceKind::Input;
    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "Built-in Speakers"));
    Require(engine.AudioDeviceSnapshot().outputDeviceName == "Built-in Speakers",
            "services persist a submitted output device's name");
    Require(services.ConsumePendingAudioRequest(control) == 1 &&
                control == synth_browser::BrowserAudioDeviceKind::Output,
            "selecting the second submitted output device arms its index against the output control");

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "system_default"));
    Require(engine.AudioDeviceSnapshot().outputDeviceName.empty(),
            "services persist system default as empty output name");
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest &&
                control == synth_browser::BrowserAudioDeviceKind::Output,
            "selecting System Default arms a release against the output control rather than an index");

    // An id no longer present in the submitted list falls back to release,
    // the same way a stale output selection does elsewhere in this file.
    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "named-output"));
    Require(engine.AudioDeviceSnapshot().outputDeviceName.empty(),
            "an output id absent from the submitted list persists as empty output name");
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest &&
                control == synth_browser::BrowserAudioDeviceKind::Output,
            "an output id absent from the submitted list arms a release against the output control");

    // Arming one control supersedes whatever the other control last armed and
    // never consumed: only one request is ever outstanding.
    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "no_input"));
    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioOutputSelect,
        "USB Speakers"));
    Require(services.ConsumePendingAudioRequest(control) == 0 &&
                control == synth_browser::BrowserAudioDeviceKind::Output,
            "the most recently armed control supersedes an unconsumed request from the other control");
}

// A device that is selected while present, then vanishes from a later
// submitted list -- the browser resubmits on every devicechange -- must not
// keep being claimed: the snapshot falls back for display and a subsequent
// Retry arms a release rather than repinning a device this host can no
// longer name. This is the same fallback SelectedDeviceOptionId already
// gives a stale persisted name (see the absent-from-submitted-list cases
// above); this test is the temporal case, where the device was present at
// selection time and only later drops out of the submitted list.
void TestBrowserServicesStopsClaimingAnInputDeviceThatLeavesTheSubmittedList()
{
    synth::Engine<ServiceApp> engine([] { return std::uint64_t{0}; });
    synth_browser::BrowserMidiBridge<synth::Engine<ServiceApp>> midiBridge(engine);
    std::vector<synth_browser::BrowserAudioDevice> devices{
        {.deviceId = "in-1", .label = "USB Mic", .kind = synth_browser::BrowserAudioDeviceKind::Input},
    };
    synth_browser::BrowserRuntimeMainServices<ServiceApp> services(
        engine, midiBridge, devices, {},
        [] { return InputState(4, 4, BrowserAudioInputStatus::Online); });

    services.DispatchAudio(synth::ui::Action::WithValue(
        synth::runtime_ui::Actions::kAudioInputSelect,
        "USB Mic"));
    synth_browser::BrowserAudioDeviceKind control = synth_browser::BrowserAudioDeviceKind::Output;
    Require(services.ConsumePendingAudioRequest(control) == 0,
            "selecting the present device arms its index");

    synth::runtime_ui::AudioPageSnapshot snapshot;
    services.RefreshAudio(snapshot);
    Require(snapshot.selectedInputId == "USB Mic",
            "the snapshot resolves the selection while the device is still submitted");

    // The device is unplugged: the browser's next `devicechange` resubmission
    // no longer includes it. `devices` is the exact vector `services` holds a
    // reference to (BrowserRuntimeMainServices's constructor takes it by
    // reference), so mutating it here reproduces that resubmission.
    devices.clear();

    services.RefreshAudio(snapshot);
    Require(snapshot.selectedInputId == "no_input",
            "an input selection whose device leaves the submitted list falls back to No Input "
            "instead of continuing to claim it");

    services.DispatchAudio(synth::ui::Action::Named(synth::runtime_ui::Actions::kAudioInputRetry));
    Require(services.ConsumePendingAudioRequest(control) == synth_browser::kReleaseAudioRequest,
            "retrying a selection whose device has left the submitted list arms a release "
            "rather than repinning a device this host can no longer name");
}

}  // namespace

int main()
{
    TestZeroInputBrowserExposesOnlySystemDefaultOutputWithNoSubmittedDevices();
    TestBrowserDefaultSelectionPersistsAsEmptyName();
    TestBrowserOutputResolvesAgainstSubmittedListAndFallsBackWhenAbsent();
    TestInputCapableBrowserExposesOneNoInputOption();
    TestBrowserInputNoInputSelectionPersistsAsEmptyName();
    TestBrowserInputResolvesAgainstSubmittedListAndFallsBackWhenAbsent();
    TestBrowserInputStatesAreDistinctAndRetryableOnlyWhileOffline();
    TestBrowserSnapshotPresentsEveryDeviceFromAMultiDeviceSubmittedListAndDefaultsToNoInput();
    TestBrowserSnapshotWithEmptySubmittedListExposesOnlyDefaultsAndDoesNotThrow();
    TestBrowserSnapshotOmitsSubmittedDevicesWithEmptyLabelAndDeviceId();
    TestUnpermittedPageIsOfferedAWayToAskForAccess();
    TestAWayToAskIsNotOfferedWhereItWouldAchieveNothing();
    TestBrowserSnapshotFallsBackWhenPersistedInputSelectionIsAbsentFromSubmittedList();
    TestBrowserSnapshotResolvesPersistedInputSelectionPresentInSubmittedList();
    TestBrowserSnapshotFallsBackWhenPersistedOutputSelectionIsAbsentFromSubmittedList();
    TestBrowserSnapshotResolvesPersistedOutputSelectionPresentInSubmittedList();
    TestBrowserServicesExposeNegotiatedDefaultAudio();
    TestZeroInputServicesPublishNoInputStatus();
    TestBrowserServicesPublishCaptureStatusAndUserRetry();
    TestBrowserServicesArmsPendingRequestForOutputSelection();
    TestBrowserServicesStopsClaimingAnInputDeviceThatLeavesTheSubmittedList();
    return 0;
}
