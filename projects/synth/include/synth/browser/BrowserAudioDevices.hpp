#pragma once

#include "synth/PatchPersistence.hpp"
#include "synth/RuntimePages.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace synth_browser {

// One browser-enumerated audio device, submitted by JS through
// `synth_browser_submit_audio_devices` (BrowserRuntime.hpp). `deviceId` is the
// browser's opaque, privacy-scoped `MediaDeviceInfo.deviceId`; `label` is the
// human-readable name, empty for an entry the page has no permission to name
// (see the empty-label rule on `BuildBrowserAudioSnapshot` below).
enum class BrowserAudioDeviceKind : std::uint32_t { Input = 0, Output = 1 };

struct BrowserAudioDevice {
    std::string deviceId;
    std::string label;
    BrowserAudioDeviceKind kind = BrowserAudioDeviceKind::Input;
};

// Consumed alongside `synth_browser_consume_pending_audio_request`
// (BrowserRuntime.hpp / BrowserRuntimeAbi.cpp), which reports this index
// together with a `BrowserAudioDeviceKind` saying which control (audio input
// or audio output) it applies to. One pending slot is shared by every control:
// retrying input, selecting an input device, and selecting an output device
// all arm the same slot with different arguments, so at most one request is
// ever outstanding. Mirrored manually by `browser/src/audio.ts`, the same
// convention `BrowserAudioInputStatus` below already follows (its own mirror
// there is `AudioInputStatusCode`): -1 means nothing is pending, -2 means
// release the armed control (release capture for input -- the user
// selected/retried into No Input -- or revert to the platform default sink
// for output), and any value >= 0 is an index into the device list most
// recently submitted through `synth_browser_submit_audio_devices`, filtered
// to entries of the armed control's kind -- JS already holds that exact
// array, so an index alone identifies the device to route to (getUserMedia
// for input, setSinkId for output).
inline constexpr std::int32_t kNoPendingAudioRequest = -1;
inline constexpr std::int32_t kReleaseAudioRequest = -2;
// Asks for capture permission without naming a device. An unpermitted page
// enumerates input devices with empty labels, so there is no index to arm and
// nothing to release; this sentinel is the only pending request that reaches a
// permission prompt, and the stream it opens is closed before it is observed.
inline constexpr std::int32_t kRequestPermissionAudioRequest = -3;

// The browser capture states the launcher realm publishes through
// `synth_browser_set_audio_input_source` / `synth_browser_clear_audio_input_source`.
// The numeric order IS the ABI status code and is mirrored by
// `browser/src/audio.ts`, so entries may be appended but never reordered.
//
// `InsecureContext` through `AudioContextUnavailable` were appended after the
// generic `PrerequisiteBlocked` so sbw-10's "report the missing prerequisite by
// name" is answered on the Audio page itself rather than only in a JavaScript
// diagnostic. That widened the accepted status range from 0-7 to 0-10, which a
// module built against the narrower range rejects, so browser ABI version 3
// became version 4 -- version equality alone would not have protected an old
// module, because the launcher would have kept advertising the version the
// module also declared. The generic `PrerequisiteBlocked` stays valid as a
// retained ABI value: an already shipped code is not withdrawn, even though the
// current host names its causes individually and has no producer for the
// generic value.
enum class BrowserAudioInputStatus : std::uint32_t {
    NotRequested,
    Requesting,
    Online,
    PermissionDenied,
    ApiUnavailable,
    PrerequisiteBlocked,
    StreamEnded,
    ChannelCountUnreported,
    InsecureContext,
    PermissionsPolicyBlocked,
    AudioContextUnavailable
};

inline bool BrowserAudioInputStatusCodeValid(std::uint32_t statusCode) noexcept
{
    switch (static_cast<BrowserAudioInputStatus>(statusCode))
    {
        case BrowserAudioInputStatus::NotRequested:
        case BrowserAudioInputStatus::Requesting:
        case BrowserAudioInputStatus::Online:
        case BrowserAudioInputStatus::PermissionDenied:
        case BrowserAudioInputStatus::ApiUnavailable:
        case BrowserAudioInputStatus::PrerequisiteBlocked:
        case BrowserAudioInputStatus::StreamEnded:
        case BrowserAudioInputStatus::ChannelCountUnreported:
        case BrowserAudioInputStatus::InsecureContext:
        case BrowserAudioInputStatus::PermissionsPolicyBlocked:
        case BrowserAudioInputStatus::AudioContextUnavailable:
            return true;
    }
    return false;
}

// What the Audio page knows about browser capture: the application's own
// request, the count the host actually published (already clamped to the
// request), and the last state the launcher realm reported. Every field is
// observable outside the realtime callback only.
struct BrowserAudioInputState
{
    std::size_t requestedChannels = 0;
    std::size_t activeChannels = 0;
    BrowserAudioInputStatus status = BrowserAudioInputStatus::NotRequested;
};

// Capture is feeding the worklet input bus. A live-but-short capture is a
// shortfall, not an outage, so it reports its counts without offering retry.
inline bool BrowserAudioInputCaptureLive(BrowserAudioInputStatus status) noexcept
{
    return status == BrowserAudioInputStatus::Online ||
           status == BrowserAudioInputStatus::ChannelCountUnreported;
}

// Capture is offline and only a user gesture may re-run it (sbw-4, sbw-10):
// there is no automatic or realtime retry, so `Retry Input` is the only way
// back from any of these states. `Requesting` is deliberately excluded -- a
// request already in flight must not offer a second prompt.
inline bool BrowserAudioInputOffline(BrowserAudioInputStatus status) noexcept
{
    switch (status)
    {
        case BrowserAudioInputStatus::NotRequested:
        case BrowserAudioInputStatus::PermissionDenied:
        case BrowserAudioInputStatus::ApiUnavailable:
        case BrowserAudioInputStatus::PrerequisiteBlocked:
        case BrowserAudioInputStatus::StreamEnded:
        case BrowserAudioInputStatus::InsecureContext:
        case BrowserAudioInputStatus::PermissionsPolicyBlocked:
        case BrowserAudioInputStatus::AudioContextUnavailable:
            return true;
        case BrowserAudioInputStatus::Requesting:
        case BrowserAudioInputStatus::Online:
        case BrowserAudioInputStatus::ChannelCountUnreported:
            return false;
    }
    return true;
}

inline std::string BrowserAudioInputStatusText(BrowserAudioInputStatus status)
{
    switch (status)
    {
        case BrowserAudioInputStatus::NotRequested:
            return "microphone capture not started";
        case BrowserAudioInputStatus::Requesting:
            return "requesting microphone access";
        case BrowserAudioInputStatus::Online:
            return {};
        case BrowserAudioInputStatus::PermissionDenied:
            return "microphone permission denied";
        case BrowserAudioInputStatus::ApiUnavailable:
            return "microphone capture unavailable";
        case BrowserAudioInputStatus::PrerequisiteBlocked:
            return "microphone prerequisite unavailable";
        case BrowserAudioInputStatus::StreamEnded:
            return "microphone stream ended";
        case BrowserAudioInputStatus::ChannelCountUnreported:
            return "microphone channel count unreported";
        case BrowserAudioInputStatus::InsecureContext:
            return "microphone requires a secure context";
        case BrowserAudioInputStatus::PermissionsPolicyBlocked:
            return "microphone blocked by permissions policy";
        case BrowserAudioInputStatus::AudioContextUnavailable:
            return "microphone requires the launch-owned AudioContext";
    }
    return {};
}

// Reported once by the browser host through the same dispatch-action channel
// BrowserAudioInputStatusText's codes travel, when this browser exposes no
// way to route to a specific output device at all (no Web Audio Output
// Devices API). Folded into the Audio page's status line the same way an
// input diagnostic is (RefreshAudio, BrowserRuntimeMainServices.hpp), so the
// operator sees why the output combo only ever offers System Default instead
// of the combo silently going quiet.
inline constexpr const char* kOutputRoutingUnsupportedText = "output device selection unavailable";

// The diagnostic half of the status line. Shortfall is orthogonal to the
// capture state -- a device can be online, or online with an unreported count,
// and still supply fewer channels than the application asked for -- so it is
// appended rather than folded into a single state.
inline std::string BrowserAudioInputDetail(const BrowserAudioInputState& input)
{
    // An application that asked for no input makes no input claim at all, so it
    // gets no capture detail either: "capture not started" is only news to an
    // application that wanted capture. Guarding here keeps the page builder and
    // the services status line -- which composes this detail itself -- agreed.
    if (input.requestedChannels == 0)
    {
        return {};
    }
    std::string detail = BrowserAudioInputStatusText(input.status);
    if (BrowserAudioInputCaptureLive(input.status) && input.activeChannels < input.requestedChannels)
    {
        detail = detail.empty() ? "input channel shortfall" : detail + ", input channel shortfall";
    }
    return detail;
}

// The same stable requested/active line the JUCE runtime publishes (sru-3): the
// counts always lead, and whatever diagnostic is current follows them instead of
// displacing them. A zero-input application makes no input claim at all and gets
// exactly the detail it was given.
inline std::string ComposeBrowserAudioStatusLine(const BrowserAudioInputState& input,
                                                 const std::string& detail)
{
    if (input.requestedChannels == 0)
    {
        return detail;
    }
    std::string text = "Input requested " + std::to_string(input.requestedChannels) +
                       " / active " + std::to_string(input.activeChannels);
    if (!detail.empty())
    {
        text += " - " + detail;
    }
    return text;
}

// Both option lists derive from the devices JS most recently submitted
// through `synth_browser_submit_audio_devices`, built through the same
// `Layout::BuildDeviceOptions` the JUCE hosts use (JuceRuntimeMainServices.hpp,
// RuntimePagesJuce.hpp) -- an option's id and label are both the device's
// `label`, matching how a JUCE device's name doubles as its persisted
// identity. An entry with an empty label carries no identity to offer (a
// page with no permission for a device enumerates it with both `deviceId`
// and `label` empty) and is filtered out before the option list is built.
inline synth::runtime_ui::AudioPageSnapshot BuildBrowserAudioSnapshot(
    const synth::AudioDeviceState& state,
    const BrowserAudioInputState& input = {},
    const std::vector<BrowserAudioDevice>& devices = {})
{
    synth::runtime_ui::AudioPageSnapshot snapshot;

    std::vector<std::string> outputNames;
    std::vector<std::string> inputNames;
    std::size_t unnamedInputCount = 0;
    for (const BrowserAudioDevice& device : devices)
    {
        if (device.label.empty())
        {
            if (device.kind == BrowserAudioDeviceKind::Input)
            {
                ++unnamedInputCount;
            }
            continue;
        }
        if (device.kind == BrowserAudioDeviceKind::Output)
        {
            outputNames.push_back(device.label);
        }
        else
        {
            inputNames.push_back(device.label);
        }
    }

    snapshot.outputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        outputNames,
        {synth::runtime_ui::kSystemDefaultOptionId, synth::runtime_ui::kSystemDefaultOptionLabel});
    snapshot.selectedOutputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
        state.outputDeviceName, snapshot.outputOptions, synth::runtime_ui::kSystemDefaultOptionId);

    snapshot.showInputCombo = input.requestedChannels > 0;
    snapshot.inputOptions.clear();
    if (!snapshot.showInputCombo)
    {
        return snapshot;
    }
    // No Input is always the first (and default/startup) entry; a stored
    // selection naming a device absent from the current list falls back to
    // it via SelectedDeviceOptionId rather than claiming that device.
    snapshot.inputOptions = synth::runtime_ui::Layout::BuildDeviceOptions(
        inputNames,
        {synth::runtime_ui::kNoInputOptionId, synth::runtime_ui::kNoInputOptionLabel});
    snapshot.selectedInputId = synth::runtime_ui::Layout::SelectedDeviceOptionId(
        state.inputDeviceName, snapshot.inputOptions, synth::runtime_ui::kNoInputOptionId);
    snapshot.showInputRetry = BrowserAudioInputOffline(input.status);
    // Devices are present but none can be named, which is what a page holding
    // no capture permission enumerates. Selecting is impossible (there is no
    // option to select) and retry re-requests the current selection, which is
    // No Input, so asking for access is the only move that changes anything.
    // Named devices, or no input devices at all, both leave this false: the
    // first has nothing to ask for and the second has nothing to ask about.
    snapshot.showInputPermissionRequest = unnamedInputCount > 0 && inputNames.empty();
    snapshot.statusLineText = ComposeBrowserAudioStatusLine(input, BrowserAudioInputDetail(input));
    return snapshot;
}

// Resolves a selected output option id against the devices JS most recently
// submitted, returning the device name to persist (empty for System Default,
// or for an id naming a device no longer in the submitted list -- a stale id
// falls back rather than claiming a device this host cannot currently name).
inline std::string BrowserOutputDeviceName(const std::string& optionId,
                                           const std::vector<BrowserAudioDevice>& devices)
{
    if (optionId == synth::runtime_ui::kSystemDefaultOptionId)
    {
        return {};
    }
    for (const BrowserAudioDevice& device : devices)
    {
        if (device.kind == BrowserAudioDeviceKind::Output && !device.label.empty() &&
            device.label == optionId)
        {
            return device.label;
        }
    }
    return {};
}

// Resolves a selected input option id the same way; No Input and a stale id
// both persist as the empty input name.
inline std::string BrowserInputDeviceName(const std::string& optionId,
                                          const std::vector<BrowserAudioDevice>& devices)
{
    if (optionId == synth::runtime_ui::kNoInputOptionId)
    {
        return {};
    }
    for (const BrowserAudioDevice& device : devices)
    {
        if (device.kind == BrowserAudioDeviceKind::Input && !device.label.empty() &&
            device.label == optionId)
        {
            return device.label;
        }
    }
    return {};
}

}  // namespace synth_browser
