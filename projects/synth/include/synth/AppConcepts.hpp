#pragma once
#include "synth/AppContext.hpp"
#include "synth/MidiAppCatalog.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"
#include <concepts>
#include <functional>
#include <string>
#include <utility>

namespace synth {

// JUCE-free application core contract (sar-4). The engine and the test rig
// require only this; the JUCE runtime additionally requires SynthApplication.
template <typename T>
concept SynthApplicationCore = requires(T app, AppContext* context, AudioBlock& block) {
    { T::Config() } -> std::convertible_to<RuntimeConfig>;
    { app.Init(context) } -> std::same_as<void>;
    { app.ProcessBlock(block) } -> std::same_as<void>;
};

// Full application contract: core plus the portable UI surface hook.
template <typename T>
concept SynthApplication = SynthApplicationCore<T> && requires(T app) {
    { app.PortableSurface() } -> std::same_as<synth::ui::Surface&>;
};

// Optional hooks, detected at compile time and skipped when absent.
template <typename T>
concept HasPrepareToPlay = requires(T app, double sampleRate, int blockSize) {
    { app.PrepareToPlay(sampleRate, blockSize) } -> std::same_as<void>;
};

// Optional once-per-block control-rate hook. When present, synth::Engine's
// ProcessBlock invokes app.ProcessFrame() exactly once per block, after
// message drains and before the app's own ProcessBlock(block) call (so any
// control-rate state ProcessFrame updates is visible there). Intended for
// application-level work that only needs to run once per block rather than
// once per sample, e.g. control-rate modulation bookkeeping.
template <typename T>
concept HasProcessFrame = requires(T app) {
    { app.ProcessFrame() } -> std::same_as<void>;
};

// Optional revert hook. When present, synth::Engine invokes
// app.RestoreStartupState() immediately after a patch message reverts the
// parameter manager to defaults, on whichever thread applied that message.
//
// A revert rebuilds every parameter from its REGISTERED default
// (ParameterConfig::defaultValue). That is the whole of what registration can
// express, and it cannot express a modulation DEPTH, which is a relationship
// between a target parameter and a source slot rather than a value on one
// parameter. An app whose startup state includes such depths -- materialized
// in its own Init() -- therefore does not get them back from a revert, and
// this hook is where it re-applies them.
//
// Named for the state it restores rather than for the revert that triggers it,
// because Parameter::RevertToDefault(SceneState) already means something else
// in this codebase -- reverting ONE parameter -- and a second concept under
// that name would make either of them harder to trace by grep.
//
// Apps that establish nothing beyond registered defaults need not implement
// it: for them a revert already reproduces startup exactly, and the absence of
// the hook is the correct answer rather than an omission.
//
// The hook re-invokes the app's own definition of its startup state rather
// than restoring a snapshot of it, so launch and revert cannot drift apart.
template <typename T>
concept HasRestoreStartupState = requires(T app) {
    { app.RestoreStartupState() } -> std::same_as<void>;
};

// Optional MIDI catalog: an app that declares one is offering the
// Controllers page its actions, library kinds, and device defaults.
// Detected at compile time, same as the hooks above. The catalog is data,
// read once at start-up; the engine dispatches its actions to the app's own
// PortableSurface().
template <typename T>
concept HasMidiCatalog = requires(const T app) {
    { app.MidiCatalog() } -> std::same_as<MidiAppCatalog>;
};

// Optional UI capability (sprs-17): an app may register exactly one
// additional sidebar page -- id, title, and a layout-preserving tree
// builder using the std::function<ui::Subtree(ui::Bounds)> graft idiom Task
// 9 established for AudioPageSnapshot::appSection (RuntimePages.hpp) and its
// design amendment (Splice(Subtree), not Splice(NodeTree), so a nested
// container's declared LayoutOptions survive the splice instead of
// re-resolving with defaults).
struct RegisteredPage {
    // Currently unused by Sheaf: routing and mounting go through Sheaf's own
    // structural NodeIds (NodeIds::kAppRoot etc., RuntimePages.hpp), never
    // through this id. It is the app's own bookkeeping -- reserved for
    // whatever cross-referencing the app itself wants to do with its
    // registered page.
    std::string id;
    std::string title;
    std::function<ui::Subtree(ui::Bounds)> buildTree;
};

// Detected the same way as HasPrepareToPlay/HasProcessFrame above: every
// call site that needs this (RuntimeMainComponent<App, Services> holds
// `App&` directly) has App as a concrete, non-erased template parameter,
// exactly like Engine<App> does for the hooks above -- so the same
// compile-time optional-method concept applies here as cleanly as it does
// to those audio-thread hooks. Two other existing conventions were
// considered and rejected: ui::ExtentAwareSurface's runtime dynamic_cast
// idiom (PortableUI.hpp) exists only because THAT seam's accessor
// (SynthApplication::PortableSurface()) is constrained to return the
// already-erased `ui::Surface&`, which this seam never does, so the
// dynamic_cast machinery would add nothing; a RuntimeConfig field
// (AppContext.hpp) was rejected because the tree builder is a std::function
// that typically closes over live app state (the app instance itself), not
// a plain data value a static `App::Config()` accessor can hold.
template <typename T>
concept HasRegisteredPage = requires(T app) {
    { app.RegisteredPage() } -> std::same_as<RegisteredPage>;
};

}  // namespace synth
