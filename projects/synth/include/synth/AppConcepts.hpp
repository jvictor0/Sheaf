#pragma once
#include "synth/AppContext.hpp"
#include "synth/PortableUI.hpp"
#include <concepts>
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

}  // namespace synth
