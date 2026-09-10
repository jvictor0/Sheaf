#pragma once

#include "synth/ControllerWizard.hpp"
#include "synth/MidiAppCatalog.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace synth {

// Runtime hosts own their device-list signal and instrument-commit lifecycle.
// This JUCE-free cache owns only the derived classification: callers update a
// snapshot when its source changed, then render Discovery() without doing any
// device enumeration or reconciliation.
class ControllerWizardDiscoveryCache final {
public:
    // Defaults to the Twister-only registry so a cache nobody configures
    // behaves exactly as it did before app device defaults existed. A host
    // with an app catalog calls this once with
    // MakeControllerWizardRegistry(engine.MidiCatalog()).
    void SetRegistry(std::vector<ControllerWizardDescriptor> registry)
    {
        registry_ = std::move(registry);
        Recompute();
    }

    bool UpdateDeviceList(MidiDeviceList devices)
    {
        if (hasDeviceList_ && devices == devices_)
        {
            return false;
        }
        devices_ = std::move(devices);
        hasDeviceList_ = true;
        Recompute();
        return true;
    }

    void UpdateInstrumentSnapshot(MidiInstrumentConfig instrument)
    {
        instrument_ = std::move(instrument);
        Recompute();
    }

    bool HasDeviceList() const { return hasDeviceList_; }
    const MidiDeviceList& DeviceList() const { return devices_; }
    const WizardDiscovery& Discovery() const { return discovery_; }
    std::uint64_t Revision() const { return revision_; }

private:
    void Recompute()
    {
        discovery_ = DiscoverControllerWizards(devices_, instrument_, registry_);
        ++revision_;
    }

    MidiDeviceList devices_;
    MidiInstrumentConfig instrument_;
    WizardDiscovery discovery_;
    std::vector<ControllerWizardDescriptor> registry_ = MakeControllerWizardRegistry(MidiAppCatalog{});
    bool hasDeviceList_ = false;
    std::uint64_t revision_ = 0;
};

}  // namespace synth
