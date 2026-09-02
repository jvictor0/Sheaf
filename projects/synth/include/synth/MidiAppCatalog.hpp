#pragma once

// An app's MIDI catalog: what it offers the Controllers page. The actions
// it exposes for a control to dispatch, the library kinds it keeps around
// (rather than the Controllers page inventing its own), whether one of
// those actions is the app's own ParamPush target, whether a saved patch
// carries controller mappings along with it, and the device defaults a
// wizard can pre-fill from.

#include "synth/MidiConfigViewModel.hpp"
#include "synth/MidiController.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth {

struct MidiAppAction {
    std::string action;      // a ui::Action name the app's surface routes
    std::string value;       // that action's value ("3" for Bank 4); empty when none
    std::string label;       // what the Controllers page shows
    std::optional<std::pair<float, float>> analogRange;
                             // set: an analog control drives it, and the
                             // dispatched value is min + v * (max - min)
};

struct MidiAppDeviceDefault {
    std::string id;          // wizard id, stored in MidiControllerSlot::wizardId
    std::string displayName; // dropdown label
    MidiProfileKind kind = MidiProfileKind::Generic;
    std::vector<std::string> inputAliases;
    std::vector<std::string> outputAliases;
    MidiControllerProfileConfig config;
};

struct MidiAppCatalog {
    std::vector<MidiAppAction> actions;
    std::vector<UISystemMessage> libraryKinds;
    std::string encoderPressAction;   // ui::Action a ParamPush dispatches, with the
                                      // position as its value; empty = the library's
                                      // HandlePress, as today
    bool patchCarriesMappings = false;
    std::vector<MidiAppDeviceDefault> deviceDefaults;
};

// Index of the first action in the catalog whose (action, value) pair
// matches, or nullopt if none does.
inline std::optional<std::size_t> FindMidiAppAction(const MidiAppCatalog& catalog, std::string_view action,
                                                     std::string_view value) {
    for (std::size_t ix = 0; ix < catalog.actions.size(); ++ix) {
        if (catalog.actions[ix].action == action && catalog.actions[ix].value == value) {
            return ix;
        }
    }
    return std::nullopt;
}

}  // namespace synth
