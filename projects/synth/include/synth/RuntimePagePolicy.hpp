#pragma once

namespace synth {

enum class RuntimePageKind {
    None,
    Audio,
    Controllers,
    Sync,
    File,
};

inline bool RuntimePageBackSavesConfiguration(RuntimePageKind page) {
    // Controllers saves every committed edit as it is made, so leaving that
    // page by Back has nothing left to save. Audio and Sync do not commit
    // through that same path, so Back still saves for them.
    return page == RuntimePageKind::Audio || page == RuntimePageKind::Sync;
}

}  // namespace synth
