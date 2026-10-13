#pragma once

// The Controllers page's engine-derived callbacks, discovery cache and dirty
// flags, shared by JuceRuntimeMainServices, BrowserRuntimeMainServices and a
// plugin host. EngineType is a template parameter, never Engine<App> spelled
// out, so this header stays free of the concrete engine's own dependencies.

#include "synth/ControllerWizard.hpp"
#include "synth/ControllerWizardDiscoveryCache.hpp"
#include "synth/ControllersPageUI.hpp"
#include "synth/MidiConfigViewModel.hpp"
#include "synth/MidiReconcile.hpp"

#include <functional>
#include <string>
#include <utility>

namespace synth::runtime_ui {

template <typename EngineType>
class ControllersPageBinding final
{
public:
    explicit ControllersPageBinding(EngineType& engine) : engine_(engine) {}

    ControllersPageBinding(const ControllersPageBinding&) = delete;
    ControllersPageBinding& operator=(const ControllersPageBinding&) = delete;

    // Fills every field of ControllersPageCallbacks the engine can answer on
    // its own; the three the engine cannot (leaving the page, the host's own
    // connection state, and whether the host's runtime configuration write
    // succeeded) are the caller's.
    ControllersPageCallbacks MakeCallbacks(std::function<void()> onBack,
                                           std::function<MidiConnectionState()> connectionState,
                                           std::function<bool()> saveRuntimeConfiguration)
    {
        ControllersPageCallbacks callbacks;
        callbacks.instrumentSnapshot = [this] { return engine_.InstrumentSnapshot(); };
        callbacks.connectionState = std::move(connectionState);
        callbacks.enumerateDevices = [this] { return discoveryCache_.DeviceList(); };
        callbacks.commitInstrument = [this](MidiInstrumentConfig instrument) {
            engine_.EditInstrument([&instrument](MidiInstrumentConfig& current) {
                current = std::move(instrument);
            });
            discoveryCache_.UpdateInstrumentSnapshot(engine_.InstrumentSnapshot());
            controllersDirty_ = true;
            instrumentSnapshotDirty_ = false;
            return true;
        };
        callbacks.saveRuntimeConfiguration = std::move(saveRuntimeConfiguration);
        callbacks.setStatus = [](std::string) {};
        callbacks.onBack = std::move(onBack);
        callbacks.messageCatalog = MakeUISystemMessageChoices(engine_.MidiCatalog());
        callbacks.analogActionCatalog = MakeAnalogAppActionChoices(engine_.MidiCatalog());
        callbacks.layouts = MakeControllerWizardRegistry(engine_.MidiCatalog());
        callbacks.gestureCount = engine_.Manager().GestureCount();
        discoveryCache_.SetRegistry(callbacks.layouts);
        return callbacks;
    }

    // The engine rebuilt its MIDI processors out from under this page: the
    // device discovery it shows and the instrument snapshot it reads at the
    // next Refresh() are both stale.
    void MarkInstrumentRebuilt()
    {
        controllersDirty_ = true;
        instrumentSnapshotDirty_ = true;
    }

    bool HasDeviceList() const
    {
        return discoveryCache_.HasDeviceList();
    }

    void UpdateDeviceList(MidiDeviceList devices)
    {
        discoveryCache_.UpdateDeviceList(std::move(devices));
    }

    void Refresh(ControllersPageSurface& surface)
    {
        surface.SetEnumerateDevices(discoveryCache_.DeviceList());
        if (instrumentSnapshotDirty_)
        {
            discoveryCache_.UpdateInstrumentSnapshot(engine_.InstrumentSnapshot());
            instrumentSnapshotDirty_ = false;
        }
        if (controllersDirty_)
        {
            surface.MarkDirty();
            controllersDirty_ = false;
        }
        surface.SetDiscovery(discoveryCache_.Discovery());
        surface.RefreshOnTick();
    }

private:
    EngineType& engine_;
    ControllerWizardDiscoveryCache discoveryCache_;
    bool controllersDirty_ = true;
    bool instrumentSnapshotDirty_ = true;
};

}  // namespace synth::runtime_ui
