#pragma once

#include "PortableJuceBackend.hpp"

#include "synth/ControllerWizardDiscoveryCache.hpp"
#include "synth/ControllersPageUI.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace synth_runtime::test {

struct ControllersHarnessState
{
    synth::MidiInstrumentConfig instrument;
    synth::MidiConnectionState connection;
    synth::MidiDeviceList devices;
    std::string status;
    int commits = 0;
};

inline synth::MidiControllerSlot MakeHarnessWrldBldrSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::WrldBldr;
    slot.config = synth::WrldBldrDefaultProfileConfig();
    slot.input.identifier = "wrldbldr-in-id";
    slot.input.name = "WRLD.Bldr In";
    slot.output.identifier = "wrldbldr-out-id";
    slot.output.name = "WRLD.Bldr Out";
    return slot;
}

inline synth::MidiControllerSlot MakeHarnessLaunchpadSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::Launchpad;
    slot.config = synth::LaunchpadDefaultProfileConfig();
    slot.input.identifier = "launchpad-in-id";
    slot.input.name = "Launchpad X In";
    slot.output.identifier = "launchpad-out-id";
    slot.output.name = "Launchpad X Out";
    return slot;
}

inline synth::MidiControllerSlot MakeHarnessGenericSlot(const char* name)
{
    synth::MidiControllerSlot slot;
    slot.name = name;
    slot.kind = synth::MidiProfileKind::Generic;
    return slot;
}

class ControllersHarnessFixture
{
public:
    ControllersHarnessFixture()
    {
        state.instrument.AddController(MakeHarnessWrldBldrSlot("wrld"));
        state.instrument.AddController(MakeHarnessLaunchpadSlot("pads"));
        state.instrument.AddController(MakeHarnessGenericSlot("blank"));
        SyncConnectionSize();
        state.connection.controllers[0].input.status = synth::MidiEndpointStatus::Online;
        state.connection.controllers[0].output.status = synth::MidiEndpointStatus::Online;
        state.connection.controllers[1].input.status = synth::MidiEndpointStatus::Offline;
        state.connection.controllers[1].output.status = synth::MidiEndpointStatus::Unconfigured;
        AddDefaultDevices();
    }

    synth::runtime_ui::ControllersPageSurface MakeSurface()
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return state.instrument; };
        callbacks.connectionState = [this] { return state.connection; };
        callbacks.enumerateDevices = [this] { return state.devices; };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig out) {
            state.instrument = std::move(out);
            SyncConnectionSize();
            ++state.commits;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [] { return true; };
        callbacks.setStatus = [this](std::string text) { state.status = std::move(text); };
        return synth::runtime_ui::ControllersPageSurface(std::move(callbacks));
    }

    void AddDefaultDevices()
    {
        state.devices.inputs.push_back({"wrldbldr-in-id", "WRLD.Bldr In"});
        state.devices.inputs.push_back({"launchpad-in-id", "Launchpad X In"});
        state.devices.inputs.push_back({"twister-in-id", "Twister In"});
        state.devices.outputs.push_back({"wrldbldr-out-id", "WRLD.Bldr Out"});
        state.devices.outputs.push_back({"launchpad-out-id", "Launchpad X Out"});
    }

    void SyncConnectionSize()
    {
        state.connection.controllers.resize(state.instrument.controllers.size());
    }

    ControllersHarnessState state;
};

// Exact MF Twister descriptor alias. Discovery matches endpoint names
// case-insensitively against descriptor-local aliases, so the fixture must
// present the same string the browser's Web MIDI helper does.
inline constexpr const char* kTwisterDeviceName = "Midi Fighter Twister";

inline synth::MidiDeviceInfoRef MakeTwisterInput(int ordinal)
{
    return {"twister-in-" + std::to_string(ordinal), kTwisterDeviceName};
}

inline synth::MidiDeviceInfoRef MakeTwisterOutput(int ordinal)
{
    return {"twister-out-" + std::to_string(ordinal), kTwisterDeviceName};
}

// Mirrors JuceRuntimeMainServices' Controllers refresh contract without a live
// JUCE runtime: the host owns a ControllerWizardDiscoveryCache, updates it only
// when it learns the device list changed or an instrument commit landed, and
// hands the surface the cached device list plus the derived classification.
// The surface's own staleness recheck reads the same cached list through the
// enumerateDevices callback, exactly as the JUCE runtime's does.
class TwisterWizardHarness
{
public:
    TwisterWizardHarness()
        : surface_(MakeCallbacks())
    {
        RefreshHost();
    }

    TwisterWizardHarness(const TwisterWizardHarness&) = delete;
    TwisterWizardHarness& operator=(const TwisterWizardHarness&) = delete;

    synth::runtime_ui::ControllersPageSurface& Surface() { return surface_; }
    const synth::MidiInstrumentConfig& Instrument() const { return instrument_; }
    const synth::ControllerWizardDiscoveryCache& Cache() const { return cache_; }
    int Commits() const { return commits_; }
    int Saves() const { return saves_; }
    const std::string& Status() const { return status_; }

    void AddTwisterPair(int ordinal)
    {
        devices_.inputs.push_back(MakeTwisterInput(ordinal));
        devices_.outputs.push_back(MakeTwisterOutput(ordinal));
    }

    void RemoveTwisterPair(int ordinal)
    {
        const std::string inputId = MakeTwisterInput(ordinal).identifier;
        const std::string outputId = MakeTwisterOutput(ordinal).identifier;
        const auto drop = [](std::vector<synth::MidiDeviceInfoRef>& devices,
                             const std::string& identifier) {
            devices.erase(std::remove_if(devices.begin(),
                                         devices.end(),
                                         [&](const synth::MidiDeviceInfoRef& device) {
                                             return device.identifier == identifier;
                                         }),
                          devices.end());
        };
        drop(devices_.inputs, inputId);
        drop(devices_.outputs, outputId);
    }

    // The host's device-list signal. Returns whether the cached classification
    // was recomputed, so tests can pin that an unchanged list stays a no-op.
    bool NoteDeviceListChanged()
    {
        return cache_.UpdateDeviceList(devices_);
    }

    // One host UI tick: learn about device changes, publish the cached list and
    // classification, and let the page rebuild its view model.
    void RefreshHost()
    {
        NoteDeviceListChanged();
        surface_.SetEnumerateDevices(cache_.DeviceList());
        surface_.SetDiscovery(cache_.Discovery());
        surface_.RefreshOnTick();
    }

private:
    synth::runtime_ui::ControllersPageCallbacks MakeCallbacks()
    {
        synth::runtime_ui::ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return instrument_; };
        callbacks.connectionState = [this] { return connection_; };
        callbacks.enumerateDevices = [this] { return cache_.DeviceList(); };
        callbacks.commitInstrument = [this](synth::MidiInstrumentConfig out) {
            instrument_ = std::move(out);
            connection_.controllers.resize(instrument_.controllers.size());
            cache_.UpdateInstrumentSnapshot(instrument_);
            ++commits_;
            return true;
        };
        callbacks.saveRuntimeConfiguration = [this] {
            ++saves_;
            return true;
        };
        callbacks.setStatus = [this](std::string text) { status_ = std::move(text); };
        return callbacks;
    }

    synth::MidiInstrumentConfig instrument_;
    synth::MidiConnectionState connection_;
    synth::MidiDeviceList devices_;
    synth::ControllerWizardDiscoveryCache cache_;
    std::string status_;
    int commits_ = 0;
    int saves_ = 0;
    synth::runtime_ui::ControllersPageSurface surface_;
};

}  // namespace synth_runtime::test
