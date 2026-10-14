#include "MiniApp.hpp"
#include "MiniAppCore.hpp"
#include "MiniAppRegistration.hpp"
#include "MiniAppUI.hpp"
#include "MiniAppUiModel.hpp"
#include "support/SynthRig.hpp"

#include "synth/AppConcepts.hpp"
#include "synth/DspRandomLfo.hpp"
#include "synth/Json.hpp"
#include "synth/PatchBrowser.hpp"
#include "synth/PortableUI.hpp"
#include "synth/StandardModulators.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth miniapp system tests must not see JUCE headers -- MiniAppCore must stay JUCE-free"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Register {
    Register(const char* name, void (*fn)()) {
        Registry().push_back({name, fn});
    }
};

#define TEST_CASE(name) \
    void name(); \
    Register reg_##name(#name, &name); \
    void name()

#define REQUIRE_TRUE(expr) \
    do { \
        if (!(expr)) { \
            std::ostringstream oss; \
            oss << __FILE__ << ":" << __LINE__ << " requirement failed: " #expr; \
            throw std::runtime_error(oss.str()); \
        } \
    } while (false)

void RequireNear(float actual, float expected, float tolerance, const char* expr) {
    if (std::fabs(actual - expected) > tolerance) {
        std::ostringstream oss;
        oss << expr << " expected " << expected << " got " << actual;
        throw std::runtime_error(oss.str());
    }
}

#define REQUIRE_NEAR(actual, expected, tolerance) RequireNear((actual), (expected), (tolerance), #actual)

// Volume is the fourth parameter WavetableVcoModule<2>::RegisterParameters
// registers (Tune, Phase, Shape, Volume, in that order -- see
// synth/Modules.hpp), and RegisterToBank(vcoBank, /*offset=*/0) maps them onto
// bank positions offset+0..offset+3 in the same order, so Volume lands on
// bank position 3.
constexpr std::size_t kSlotIx = 0;
constexpr std::size_t kLfoBankIx = 1;
constexpr std::size_t kTunePosition = 0;
constexpr std::size_t kPhasePosition = 1;
constexpr std::size_t kShapePosition = 2;
constexpr std::size_t kVolumePosition = 3;
constexpr std::size_t kFilterCutoffPosition = 4;
constexpr std::size_t kFilterResonancePosition = 5;
constexpr std::size_t kFilterBlendPosition = 6;
constexpr std::size_t kLfoFrequencyPosition = 0;
constexpr std::size_t kLfoShapePosition = 1;
constexpr std::size_t kLfoPhaseOffsetPosition = 2;
constexpr std::size_t kLfoSkewPosition = 3;
constexpr std::size_t kLfoExponentPosition = 4;
constexpr std::size_t kAttackPosition = 5;
constexpr std::size_t kDecayPosition = 6;
constexpr std::size_t kSustainPosition = 7;
constexpr std::size_t kReleasePosition = 8;
constexpr std::size_t kTempoPosition = 9;

// Builds fresh runtime-owned scratch data paths unique to the calling test.
// Every test below injects these paths through SynthRig so startup patch
// loading cannot observe a shared production location or another test's data.
synth::RuntimeDataPaths UseScratchRuntimeDataPaths(const char* testName) {
    const std::filesystem::path dataRoot =
        std::filesystem::temp_directory_path() / "sheaf-patch-miniapp-system-tests" / testName;
    std::filesystem::remove_all(dataRoot);
    synth::RuntimeDataPaths paths = synth::RuntimeDataPaths::FromDataRoot(dataRoot);
    std::filesystem::create_directories(paths.patchesRoot);
    return paths;
}

// Snapshot of a settled output window, used to prove a Turn actually changes
// the AUDIBLE output (not just a tracked parameter value). Captured via
// rig.ClearOutput() + rig.RunBlocks(count) + rig.Output(), copying every
// frame/channel sample out before the rig's capture ring can evict them.
using OutputWindow = std::vector<synth_rig::SynthRig<synth_miniapp::MiniAppCore>::OutputFrame>;

OutputWindow CaptureSettledOutputWindow(synth_rig::SynthRig<synth_miniapp::MiniAppCore>& rig, std::size_t numBlocks) {
    rig.ClearOutput();
    rig.RunBlocks(numBlocks);
    return rig.Output();
}

// Assert that two windows have exactly equal shape: same frame count and equal
// per-frame channel counts. Used to guard value comparisons and fail clearly
// on shape mismatches.
void RequireEqualWindowShapes(const OutputWindow& a, const OutputWindow& b, const char* context) {
    if (a.size() != b.size()) {
        std::ostringstream oss;
        oss << context << " frame count mismatch: " << a.size() << " vs " << b.size();
        throw std::runtime_error(oss.str());
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].channels.size() != b[i].channels.size()) {
            std::ostringstream oss;
            oss << context << " frame " << i << " channel count mismatch: "
                << a[i].channels.size() << " vs " << b[i].channels.size();
            throw std::runtime_error(oss.str());
        }
    }
}

// True if any frame/channel sample differs by more than tolerance between the
// two windows. Requires equal shapes first (fails clearly on shape mismatch).
// Used to assert a Turn produces a materially different output signal, not
// just a changed parameter value that never reaches the audio path.
bool OutputWindowsDifferMaterially(const OutputWindow& before, const OutputWindow& after, float tolerance) {
    RequireEqualWindowShapes(before, after, "OutputWindowsDifferMaterially");
    for (std::size_t frame = 0; frame < before.size(); ++frame) {
        const auto& beforeChannels = before[frame].channels;
        const auto& afterChannels = after[frame].channels;
        for (std::size_t ch = 0; ch < beforeChannels.size(); ++ch) {
            if (std::fabs(afterChannels[ch] - beforeChannels[ch]) > tolerance) {
                return true;
            }
        }
    }
    return false;
}

bool AllSamplesFinite(const OutputWindow& window) {
    for (const auto& frame : window) {
        for (const float sample : frame.channels) {
            if (!std::isfinite(sample)) {
                return false;
            }
        }
    }
    return true;
}

bool ValuesDifferMaterially(const std::vector<float>& values, float tolerance) {
    if (values.empty()) {
        return false;
    }
    const float first = values.front();
    for (const float value : values) {
        if (std::fabs(value - first) > tolerance) {
            return true;
        }
    }
    return false;
}

float OutputWindowPeak(const OutputWindow& window) {
    float peak = 0.0f;
    for (const auto& frame : window) {
        for (const float sample : frame.channels) {
            peak = std::max(peak, std::fabs(sample));
        }
    }
    return peak;
}

void RequireBankPosition(synth::BankSlot& slot, const synth::Bank& bank, std::size_t position,
                         synth::ParameterId expectedId, const char* expectedName) {
    synth::PhysicalEncoderId encoderId = 0;
    REQUIRE_TRUE(slot.ResolvePosition(position, encoderId));
    const synth::Parameter* parameter = bank.VisibleParameter(encoderId);
    REQUIRE_TRUE(parameter != nullptr);
    REQUIRE_TRUE(parameter->Id() == expectedId);
    REQUIRE_TRUE(parameter->Name() == expectedName);
}

void RequirePagePosition(const synth::Page& page, std::size_t position,
                         synth::ParameterId expectedId, const char* expectedName) {
    REQUIRE_TRUE(position < page.parameters.size());
    const synth::Parameter* parameter = page.parameters[position];
    REQUIRE_TRUE(parameter != nullptr);
    REQUIRE_TRUE(parameter->Id() == expectedId);
    REQUIRE_TRUE(parameter->Name() == expectedName);
}

void RequireUnboundBankPosition(synth::BankSlot& slot, const synth::Bank& bank, std::size_t position) {
    synth::PhysicalEncoderId encoderId = 0;
    REQUIRE_TRUE(slot.ResolvePosition(position, encoderId));
    REQUIRE_TRUE(bank.VisibleParameter(encoderId) == nullptr);
}

template <typename Rig>
void RequireAdsrTempoAccessors(Rig& rig, double expectedSampleRate) {
    using Core = std::remove_reference_t<decltype(rig.Application())>;
    if constexpr (requires(Core& core, const Core& constCore) {
                      core.AdsrParameterIds();
                      core.AdsrModuleInstance();
                      constCore.AdsrModuleInstance();
                      core.TempoParameterId();
                      core.TempoParameter();
                      constCore.TempoParameter();
                      core.AdsrModulationMirror();
                      constCore.AdsrModulationMirror();
                  }) {
        Core& core = rig.Application();
        const Core& constCore = core;
        const auto ids = constCore.AdsrParameterIds();
        REQUIRE_TRUE(ids.attack == 12);
        REQUIRE_TRUE(ids.decay == 13);
        REQUIRE_TRUE(ids.sustain == 14);
        REQUIRE_TRUE(ids.release == 15);
        REQUIRE_TRUE(constCore.TempoParameterId() == 16);
        REQUIRE_TRUE(constCore.TempoParameter().Id() == 16);
        REQUIRE_TRUE(core.TempoParameter().Id() == 16);
        REQUIRE_NEAR(constCore.AdsrModuleInstance().SampleRate(), expectedSampleRate, 0.0);

        const float* const mirrorAddress = constCore.AdsrModulationMirror().data();
        REQUIRE_TRUE(mirrorAddress != nullptr);
        REQUIRE_TRUE(core.AdsrModulationMirror().data() == mirrorAddress);
        rig.RunBlocks(3);
        REQUIRE_TRUE(constCore.AdsrModulationMirror().data() == mirrorAddress);
    } else {
        REQUIRE_TRUE(false);
    }
}

template <typename Core>
void RequireAdsrClockDebug(const Core& core,
                           std::size_t framesProcessed,
                           bool firstGate,
                           bool lastGate,
                           std::size_t risingEdges,
                           std::size_t fallingEdges,
                           std::uint64_t lastRisingSample,
                           std::uint64_t lastFallingSample) {
    if constexpr (requires { core.AdsrClockDebug(); }) {
        const auto& debug = core.AdsrClockDebug();
        REQUIRE_TRUE(debug.framesProcessed == framesProcessed);
        REQUIRE_TRUE(debug.firstGate == firstGate);
        REQUIRE_TRUE(debug.lastGate == lastGate);
        REQUIRE_TRUE(debug.risingEdgeCount == risingEdges);
        REQUIRE_TRUE(debug.fallingEdgeCount == fallingEdges);
        REQUIRE_TRUE(debug.lastRisingSample == lastRisingSample);
        REQUIRE_TRUE(debug.lastFallingSample == lastFallingSample);
    } else {
        REQUIRE_TRUE(false);
    }
}

template <typename Core>
void RequireTempoRequestDebug(const Core& core,
                              std::uint64_t requestCount,
                              float lastRequestedBpm,
                              bool lastAcceptance) {
    if constexpr (requires { core.TempoRequestDebug(); }) {
        const auto& debug = core.TempoRequestDebug();
        REQUIRE_TRUE(debug.requestCount == requestCount);
        REQUIRE_NEAR(debug.lastRequestedBpm, lastRequestedBpm, 0.000001f);
        REQUIRE_TRUE(debug.lastAcceptance == lastAcceptance);
    } else {
        REQUIRE_TRUE(false);
    }
}

void SetNormalizedParameter(synth::ParameterManager& manager,
                            synth::Parameter& parameter,
                            float normalized) {
    for (std::size_t sceneIx = 0; sceneIx < parameter.Group().Config().numScenes; ++sceneIx) {
        parameter.SceneCenter(sceneIx) = normalized;
    }
    manager.ComputeAllParameters();
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const char* id) {
    for (const synth::ui::Node& node : tree.nodes) {
        if (node.id == synth::ui::NodeId(id)) {
            return &node;
        }
    }
    return nullptr;
}

const synth::ui::Node* FindNodeById(const synth::ui::NodeTree& tree, const std::string& id) {
    return FindNodeById(tree, id.c_str());
}

bool HasOuterEncoderFrame(const synth::ui::Node& encoder) {
    constexpr float kTolerance = 0.0001f;
    const synth::ui::Bounds expected{
        5.0f,
        5.0f,
        encoder.bounds.width - 10.0f,
        encoder.bounds.height - 10.0f,
    };
    return std::any_of(encoder.drawCommands.begin(), encoder.drawCommands.end(),
                       [&](const synth::ui::DrawCommand& command) {
                           return command.kind == synth::ui::DrawCommand::Kind::StrokeRoundedRect &&
                                  std::fabs(command.bounds.x - expected.x) <= kTolerance &&
                                  std::fabs(command.bounds.y - expected.y) <= kTolerance &&
                                  std::fabs(command.bounds.width - expected.width) <= kTolerance &&
                                  std::fabs(command.bounds.height - expected.height) <= kTolerance &&
                                  std::fabs(command.cornerRadius - 6.0f) <= kTolerance;
                       });
}

std::size_t NodeIndexById(const synth::ui::NodeTree& tree, const std::string& id) {
    for (std::size_t ix = 0; ix < tree.nodes.size(); ++ix) {
        if (tree.nodes[ix].id == synth::ui::NodeId(id)) {
            return ix;
        }
    }
    throw std::runtime_error("missing node " + id);
}

void RequireNodeId(const synth::ui::NodeTree& tree, const char* id) {
    REQUIRE_TRUE(FindNodeById(tree, id) != nullptr);
}

void RequireNodeKind(const synth::ui::NodeTree& tree, const char* id, synth::ui::NodeKind kind) {
    const synth::ui::Node* node = FindNodeById(tree, id);
    REQUIRE_TRUE(node != nullptr);
    REQUIRE_TRUE(node->kind == kind);
}

void RequireAction(const std::optional<synth::ui::Action>& action, const char* expectedName)
{
    REQUIRE_TRUE(action.has_value());
    REQUIRE_TRUE(action->name == expectedName);
}

bool PopNextMessage(synth::MessageInBus& uiBus, synth::MessageIn& message) {
    return uiBus.Pop(message, std::numeric_limits<std::uint64_t>::max());
}

bool BoundsInside(synth::ui::Bounds inner, synth::ui::Bounds outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

std::optional<std::string> ParentIdOf(const synth::ui::NodeTree& tree, const std::string& id) {
    for (const synth::ui::Node& node : tree.nodes) {
        for (const synth::ui::NodeId& child : node.children) {
            if (child.value == id) {
                return node.id.value;
            }
        }
    }
    return std::nullopt;
}

// Node bounds are parent-relative, so a claim about where two nodes sit
// relative to each other is a claim about their bounds folded over their
// ancestor origins.
synth::ui::Bounds SurfaceBoundsOf(const synth::ui::NodeTree& tree, const std::string& id) {
    const synth::ui::Node* node = FindNodeById(tree, id);
    REQUIRE_TRUE(node != nullptr);
    synth::ui::Bounds bounds = node->bounds;
    std::optional<std::string> parent = ParentIdOf(tree, id);
    while (parent.has_value()) {
        const synth::ui::Node* parentNode = FindNodeById(tree, *parent);
        REQUIRE_TRUE(parentNode != nullptr);
        bounds.x += parentNode->bounds.x;
        bounds.y += parentNode->bounds.y;
        parent = ParentIdOf(tree, *parent);
    }
    return bounds;
}

bool IsDescendantOf(const synth::ui::NodeTree& tree, const std::string& id, const std::string& ancestorId) {
    std::optional<std::string> parent = ParentIdOf(tree, id);
    while (parent.has_value()) {
        if (*parent == ancestorId) {
            return true;
        }
        parent = ParentIdOf(tree, *parent);
    }
    return false;
}

bool BoundsOverlap(synth::ui::Bounds a, synth::ui::Bounds b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

bool NoTwoNodesOverlap(const synth::ui::NodeTree& tree, const std::vector<std::string>& ids) {
    for (std::size_t i = 0; i < ids.size(); ++i) {
        for (std::size_t j = i + 1; j < ids.size(); ++j) {
            if (BoundsOverlap(SurfaceBoundsOf(tree, ids[i]), SurfaceBoundsOf(tree, ids[j]))) {
                return false;
            }
        }
    }
    return true;
}

// The test binary runs from projects/synth while the plan names repository
// relative paths; a path that exists in neither is an error, never a silent
// pass.
bool SourceContains(const std::string& repoRelativePath, const std::string& needle) {
    for (const std::string& candidate : {repoRelativePath, "../../" + repoRelativePath}) {
        std::ifstream stream(candidate);
        if (stream) {
            std::ostringstream contents;
            contents << stream.rdbuf();
            return contents.str().find(needle) != std::string::npos;
        }
    }
    throw std::runtime_error("missing source file: " + repoRelativePath);
}

// A context-free surface build: the layout claims below are about the
// resolver, not about any particular engine state.
synth::ui::NodeTree BuildMiniAppTreeAt(float width, float height) {
    synth::RuntimeConfig config = synth_miniapp::MiniAppCore::Config();
    config.uiWidth = static_cast<int>(width);
    config.uiHeight = static_cast<int>(height);
    synth::AppContext context;
    context.config = &config;
    synth_miniapp::MiniAppUiSurface surface;
    surface.Attach(&context, nullptr);
    return surface.BuildTree();
}

struct TestVisualizer final : synth::ui::Visualizer
{
    std::vector<synth::ui::DrawCommand> DrawVisible() const override
    {
        const synth::ui::Bounds bounds = GetBounds();
        return {synth::ui::DrawCommand::Fill({0.0f, 0.0f, bounds.width, bounds.height},
                                             synth::Color::Cyan)};
    }
};

synth::ui::NodeTree BuildMiniAppTree(synth_rig::SynthRig<synth_miniapp::MiniAppCore>& rig) {
    rig.UIState();
    synth::AppContext context = rig.Engine().Context();
    synth_miniapp::MiniAppUiSurface surface;
    surface.Attach(&context, &rig.Engine().Application());
    return surface.BuildTree();
}

}  // namespace

TEST_CASE(miniapp_ratio_grid_declares_independent_set_only_rows_and_feedback) {
    synth_rig::SynthRig<synth_miniapp::MiniApp> rig(
        64, UseScratchRuntimeDataPaths("ratio_grid_topology_and_interaction"));
    REQUIRE_TRUE(rig.Engine().Context().uiState == rig.Engine().RuntimeUIStateForTest().parameters);
    const auto& grids = *rig.Engine().RuntimeUIStateForTest().grids;
    REQUIRE_TRUE(grids.slots.size() == 1);
    REQUIRE_TRUE(grids.slots[0]->range == *synth::GridRange::Create(0, 8, 0, 2));
    REQUIRE_TRUE(grids.slots[0]->colors.size() == 16);
    REQUIRE_TRUE(rig.Engine().GridManagerForTest().SlotAt(0)->SelectedGrid() != nullptr);
    REQUIRE_TRUE(rig.Engine().GridManagerForTest().Finalized());
    REQUIRE_TRUE(rig.Application().Context()->uiState == rig.Engine().Context().uiState);

    const auto& range = grids.slots[0]->range;
    auto colorAt = [&grids, &range](int x, int y) {
        return grids.slots[0]->colors[*range.IndexOf(x, y)].Load();
    };
    auto requireNonBlackRgb = [](synth::Color color) {
        REQUIRE_TRUE(color.r != 0 || color.g != 0 || color.b != 0);
    };

    // Engine publishes initial grid feedback during Initialize(), before the
    // first audio block. Both rows start at the unity (x=3) ratio.
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const synth::Color color = colorAt(x, y);
            requireNonBlackRgb(color);
            REQUIRE_TRUE(color.a == (x == 3 ? 1 : 0));
        }
    }

    const synth::Color initialUnityFull = colorAt(3, 0);
    const synth::Color initialUnityFullOtherRow = colorAt(3, 1);
    REQUIRE_TRUE(initialUnityFull == initialUnityFullOtherRow);

    rig.Engine().GridManagerForTest().HandlePress(0, 0, 0, 100);
    rig.Engine().GridManagerForTest().HandlePress(0, 6, 1, 100);
    // The rig prepares MiniApp at 48 kHz/64 frames and publishes UI at 30 Hz,
    // so one published UI frame takes 25 audio blocks.
    rig.RunBlocks(25);

    for (int y = 0; y < 2; ++y) {
        const int selectedX = y == 0 ? 0 : 6;
        for (int x = 0; x < 8; ++x) {
            const synth::Color color = colorAt(x, y);
            requireNonBlackRgb(color);
            REQUIRE_TRUE(color.a == (x == selectedX ? 1 : 0));
        }
    }

    // A selected cell is full brightness, and the matching unselected row is
    // the same hue family at lower RGB brightness (not alpha dimming).
    const synth::Color row0Selected = colorAt(0, 0);
    const synth::Color row0Unselected = colorAt(0, 1);
    const synth::Color row1Selected = colorAt(6, 1);
    const synth::Color row1Unselected = colorAt(6, 0);
    for (const auto& [full, dim] : {std::pair{row0Selected, row0Unselected},
                                   std::pair{row1Selected, row1Unselected},
                                   std::pair{initialUnityFull, colorAt(3, 0)},
                                   std::pair{initialUnityFullOtherRow, colorAt(3, 1)}}) {
        REQUIRE_TRUE(full.a == 1);
        REQUIRE_TRUE(dim.a == 0);
        REQUIRE_TRUE(dim.r <= full.r && dim.g <= full.g && dim.b <= full.b);
        REQUIRE_TRUE(dim.r != full.r || dim.g != full.g || dim.b != full.b);
    }

    const std::array<synth::Color, 16> afterPress{
        colorAt(0, 0), colorAt(1, 0), colorAt(2, 0), colorAt(3, 0),
        colorAt(4, 0), colorAt(5, 0), colorAt(6, 0), colorAt(7, 0),
        colorAt(0, 1), colorAt(1, 1), colorAt(2, 1), colorAt(3, 1),
        colorAt(4, 1), colorAt(5, 1), colorAt(6, 1), colorAt(7, 1),
    };
    rig.Engine().GridManagerForTest().HandleRelease(0, 0, 0);
    rig.Engine().GridManagerForTest().HandlePressureChange(0, 6, 1, 127);
    rig.RunBlocks(25);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            REQUIRE_TRUE(colorAt(x, y) == afterPress[*range.IndexOf(x, y)]);
        }
    }
}

TEST_CASE(miniapp_ratio_grid_applies_independent_voice_pitch_offsets_without_mutating_tune) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64, UseScratchRuntimeDataPaths("ratio_grid_independent_pitch_offsets"));

    rig.RunBlocks(1);
    const auto& vcoBeforeSelection = rig.Application().VcoModuleInstance().CurrentInput();
    const float baseVoice0Frequency = vcoBeforeSelection.voices[0].vco.freq;
    const float baseVoice1Frequency = vcoBeforeSelection.voices[1].vco.freq;
    REQUIRE_NEAR(baseVoice0Frequency, baseVoice1Frequency, 1e-7f);

    const synth::ParameterId tuneId = rig.Application().VcoParameterIds().tune;
    const float tuneBeforeGridPress = rig.ParameterValue(tuneId);
    rig.Engine().GridManagerForTest().HandlePress(0, 0, 0, 100);  // 1/2
    rig.Engine().GridManagerForTest().HandlePress(0, 7, 1, 100);  // 2/1
    REQUIRE_NEAR(rig.ParameterValue(tuneId), tuneBeforeGridPress, 0.0f);

    rig.RunBlocks(1);
    const auto& vcoAfterSelection = rig.Application().VcoModuleInstance().CurrentInput();
    REQUIRE_NEAR(vcoAfterSelection.voices[0].vco.freq, baseVoice0Frequency * 0.5f, 1e-7f);
    REQUIRE_NEAR(vcoAfterSelection.voices[1].vco.freq, baseVoice1Frequency * 2.0f, 1e-7f);
    REQUIRE_TRUE(vcoAfterSelection.voices[0].vco.freq != vcoAfterSelection.voices[1].vco.freq);
    REQUIRE_NEAR(rig.ParameterValue(tuneId), tuneBeforeGridPress, 0.0f);
}

TEST_CASE(miniapp_registration_declares_launcher_metadata_and_launch_callable) {
    bool launched = false;
    synth::RuntimeDataPaths launchedPaths;
    const auto registration = synth_miniapp::MakeMiniAppRegistration(
        [&](synth::RuntimeDataPaths paths) {
            launched = true;
            launchedPaths = std::move(paths);
        });

    REQUIRE_TRUE(registration.manifest.appId == "miniapp");
    REQUIRE_TRUE(registration.manifest.displayName == "Mini App");
    REQUIRE_TRUE(registration.manifest.author == "Sheaf");
    REQUIRE_TRUE(registration.manifest.category == "test");
    REQUIRE_TRUE(registration.manifest.hardware.minEncoders == 16);
    REQUIRE_TRUE(static_cast<bool>(registration.launch));

    registration.launch(synth::RuntimeDataPaths::FromDataRoot("/tmp/sheaf-miniapp-registration-test"));
    REQUIRE_TRUE(launched);
    REQUIRE_TRUE(launchedPaths.dataRoot == std::filesystem::path("/tmp/sheaf-miniapp-registration-test"));
}

TEST_CASE(miniapp_portable_surface_exposes_stable_ids_and_routes_actions) {
    (void)UseScratchRuntimeDataPaths("portable_surface_exposes_stable_ids_and_routes_actions");

    synth::ParameterManager manager;
    synth::MessageInBus uiBus(&manager);
    synth::GridManager gridManager;
    synth::RuntimeConfig config = synth_miniapp::MiniAppCore::Config();
    synth::MidiInstrumentConfig instrument;
    synth::AppContext context;
    context.parameterManager = &manager;
    context.uiBus = &uiBus;
    context.config = &config;
    context.instrument = &instrument;
    context.gridManager = &gridManager;

    std::uint64_t timestamp = 1000;
    context.now = [&timestamp]() { return timestamp++; };

    synth_miniapp::MiniApp app;
    app.Init(&context);
    synth::ui::Surface& surface = app.PortableSurface();

    const synth::ui::NodeTree tree = surface.BuildTree();
    RequireNodeId(tree, "miniapp.root");
    RequireNodeId(tree, "miniapp.title");
    RequireNodeId(tree, "miniapp.encoder.0");
    RequireNodeId(tree, "miniapp.encoder.15");
    RequireNodeId(tree, "miniapp.vco.scope");
    RequireNodeId(tree, "miniapp.lfo.scope");
    REQUIRE_TRUE(FindNodeById(tree, "miniapp.ganged_random_lfo.round") == nullptr);
    RequireNodeId(tree, "miniapp.bank.vco");
    RequireNodeId(tree, "miniapp.bank.lfo");
    RequireNodeId(tree, "miniapp.gesture.toggle");
    RequireNodeId(tree, "miniapp.scene.0");
    RequireNodeId(tree, "miniapp.scene.1");
    RequireNodeId(tree, "miniapp.scene.2");
    RequireNodeId(tree, "miniapp.reset");
    RequireNodeId(tree, "miniapp.random");
    RequireNodeId(tree, "miniapp.random_mod");
    RequireNodeId(tree, "miniapp.start");
    RequireNodeId(tree, "miniapp.stop");
    RequireNodeId(tree, "miniapp.gesture.value");
    RequireNodeId(tree, "miniapp.scene.blend");

    const synth::ui::Node* encoder0 = FindNodeById(tree, "miniapp.encoder.0");
    REQUIRE_TRUE(encoder0 != nullptr);
    REQUIRE_TRUE(encoder0->kind == synth::ui::NodeKind::Draw);
    RequireAction(encoder0->pointerDragAction, synth_miniapp::MiniAppActions::kEncoderDrag);
    RequireAction(encoder0->doubleClickAction, synth_miniapp::MiniAppActions::kEncoderPush);
    std::size_t encodedSlot = 999;
    std::size_t encodedPosition = 999;
    float encodedDelta = 999.0f;
    REQUIRE_TRUE(synth_miniapp::ParseEncoderGestureValue(
        synth_miniapp::FormatEncoderGestureValue(0, 0, 0.25f),
        encodedSlot,
        encodedPosition,
        encodedDelta));
    REQUIRE_TRUE(encodedSlot == 0);
    REQUIRE_TRUE(encodedPosition == 0);
    REQUIRE_NEAR(encodedDelta, 0.25f, 1e-6f);

    const std::size_t queueBefore = uiBus.Size();
    surface.DispatchAction(synth::ui::Action::Named("miniapp.start"));
    REQUIRE_TRUE(uiBus.Size() == queueBefore + 1);
    synth::MessageIn message;
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::Start);
    REQUIRE_TRUE(message.timestamp == 1000);

    surface.DispatchAction(synth::ui::Action::Named("miniapp.stop"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::Stop);
    REQUIRE_TRUE(message.timestamp == 1001);

    surface.DispatchAction(synth::ui::Action::WithValue("miniapp.bank.select", "1"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.bankIx == 1);
    REQUIRE_TRUE(message.timestamp == 1002);

    surface.DispatchAction(synth::ui::Action::WithValue("miniapp.gesture.value", "0.42"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_NEAR(message.value, 0.42f, 1e-4f);
    REQUIRE_TRUE(message.timestamp == 1003);

    surface.DispatchAction(synth::ui::Action::Named("miniapp.gesture.toggle"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleGestureSelect);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_TRUE(message.timestamp == 1004);

    surface.DispatchAction(synth::ui::Action::WithValue("miniapp.scene.select", "2"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 2);
    REQUIRE_TRUE(message.timestamp == 1005);

    surface.DispatchAction(synth::ui::Action::WithValue("miniapp.scene.blend", "0.75"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetSceneBlend);
    REQUIRE_NEAR(message.value, 0.75f, 1e-4f);
    REQUIRE_TRUE(message.timestamp == 1006);

    surface.DispatchAction(synth::ui::Action::Named("miniapp.reset"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.timestamp == 1007);

    surface.DispatchAction(synth::ui::Action::Named("miniapp.random"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleRandom);
    REQUIRE_TRUE(message.timestamp == 1008);

    surface.DispatchAction(synth::ui::Action::Named("miniapp.random_mod"));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleRandomMod);
    REQUIRE_TRUE(message.timestamp == 1009);

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth_miniapp::MiniAppActions::kEncoderDrag,
        synth_miniapp::FormatEncoderGestureValue(0, 3, 0.125f)));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_NEAR(message.delta, 0.125f, 1e-6f);
    REQUIRE_TRUE(message.timestamp == 1010);

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth_miniapp::MiniAppActions::kEncoderPush,
        synth_miniapp::FormatEncoderGestureValue(0, 3, 0.0f)));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_TRUE(message.timestamp == 1011);

    surface.DispatchAction(synth::ui::Action::WithValue(
        synth_miniapp::MiniAppActions::kEncoderPush,
        synth_miniapp::FormatEncoderGestureValue(0, 15, 0.0f)));
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.position == 15);
    REQUIRE_TRUE(message.timestamp == 1012);

    static_assert(synth::SynthApplication<synth_miniapp::MiniApp>);
}

TEST_CASE(miniapp_main_layout_draws_bounded_scope_stack_and_complete_encoder_grid) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("main_layout_draws_bounded_scope_stack_and_complete_encoder_grid"));
    rig.RunBlocks(2);

    const auto requireCompleteLayout = [&](int width, int height) {
        rig.UIState();
        synth::RuntimeConfig sizedConfig = *rig.Engine().Context().config;
        sizedConfig.uiWidth = width;
        sizedConfig.uiHeight = height;
        synth::AppContext context = rig.Engine().Context();
        context.config = &sizedConfig;
        synth_miniapp::MiniAppUiSurface surface;
        surface.Attach(&context, &rig.Engine().Application());
        const synth::ui::NodeTree tree = surface.BuildTree();

        const synth::ui::Bounds root = synth_miniapp::MiniAppPageLayout::RootBounds(&context);
        std::array<synth::ui::Bounds, 16> encoders{};
        for (std::size_t ix = 0; ix < encoders.size(); ++ix) {
            const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(ix);
            REQUIRE_TRUE(FindNodeById(tree, encoderId) != nullptr);
            encoders[ix] = SurfaceBoundsOf(tree, encoderId);
            REQUIRE_TRUE(BoundsInside(encoders[ix], root));
        }
        REQUIRE_TRUE(encoders[0].y == encoders[3].y);
        REQUIRE_TRUE(encoders[0].x < encoders[3].x);
        REQUIRE_TRUE(encoders[0].x == encoders[12].x);
        REQUIRE_TRUE(encoders[0].y < encoders[12].y);
        REQUIRE_TRUE(encoders[3].x == encoders[15].x);
        REQUIRE_TRUE(encoders[12].y == encoders[15].y);

        REQUIRE_TRUE(FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kVcoScope) != nullptr);
        REQUIRE_TRUE(FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kLfoScope) != nullptr);
        const synth::ui::Bounds vco = SurfaceBoundsOf(tree, synth_miniapp::MiniAppNodeIds::kVcoScope);
        const synth::ui::Bounds lfo = SurfaceBoundsOf(tree, synth_miniapp::MiniAppNodeIds::kLfoScope);
        REQUIRE_TRUE(BoundsInside(vco, root));
        REQUIRE_TRUE(BoundsInside(lfo, root));
        REQUIRE_TRUE(vco.y + vco.height <= lfo.y);
        REQUIRE_TRUE(vco.x + vco.width <= encoders[0].x);
        REQUIRE_TRUE(lfo.x + lfo.width <= encoders[12].x);

        // The slot component fills whatever extent the slot resolved to, at
        // either root extent, without the app computing the region.
        const synth::ui::Node* vcoNode = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kVcoScope);
        REQUIRE_TRUE(!vcoNode->drawCommands.empty());
        RequireNear(vcoNode->drawCommands.front().bounds.width, vcoNode->bounds.width, 0.0001f,
                    "vco waveform fills its slot width");
        RequireNear(vcoNode->drawCommands.front().bounds.height, vcoNode->bounds.height, 0.0001f,
                    "vco waveform fills its slot height");
        REQUIRE_TRUE(FindNodeById(tree, "miniapp.ganged_random_lfo.round") == nullptr);
    };

    requireCompleteLayout(synth_miniapp::MiniAppCore::Config().uiWidth,
                          synth_miniapp::MiniAppCore::Config().uiHeight);
    requireCompleteLayout(640, 480);

    rig.Press(kSlotIx, kTunePosition);
    rig.RunBlocks(1);
    const synth::ui::NodeTree modulationTree = BuildMiniAppTree(rig);
    const std::string underlayId = synth_miniapp::MiniAppNodeIds::Encoder(0) + ".visualizer";
    const synth::ui::Node* underlay = FindNodeById(modulationTree, underlayId);
    REQUIRE_TRUE(underlay != nullptr);
    // The underlay covers exactly the encoder it sits beneath, and the retained
    // visualizer is handed that cell's NODE-LOCAL extent: draw geometry is
    // node-local (sru-46), so a visualizer's own origin is always (0, 0).
    const synth::ui::Node* underlaidEncoder =
        FindNodeById(modulationTree, synth_miniapp::MiniAppNodeIds::Encoder(0));
    REQUIRE_TRUE(underlaidEncoder != nullptr);
    REQUIRE_TRUE(underlay->bounds.x == underlaidEncoder->bounds.x);
    REQUIRE_TRUE(underlay->bounds.y == underlaidEncoder->bounds.y);
    REQUIRE_TRUE(underlay->bounds.width == underlaidEncoder->bounds.width);
    REQUIRE_TRUE(underlay->bounds.height == underlaidEncoder->bounds.height);
    REQUIRE_TRUE(underlay->bounds.width > 0.0f && underlay->bounds.height > 0.0f);
    const auto& retained = rig.Application().StandardModulatorsInstance().RandomVisualizer(0);
    REQUIRE_TRUE(retained.GetBounds().x == 0.0f);
    REQUIRE_TRUE(retained.GetBounds().y == 0.0f);
    REQUIRE_TRUE(retained.GetBounds().width == underlay->bounds.width);
    REQUIRE_TRUE(retained.GetBounds().height == underlay->bounds.height);
}

TEST_CASE(miniapp_ui_model_exposes_layout_scene_labels_and_dispatch) {
    (void)UseScratchRuntimeDataPaths("ui_model_exposes_layout_scene_labels_and_dispatch");

    REQUIRE_TRUE(synth_miniapp::SceneLabel(0) == std::string("S1"));
    REQUIRE_TRUE(synth_miniapp::SceneLabel(1) == std::string("S2"));
    REQUIRE_TRUE(synth_miniapp::SceneLabel(2) == std::string("S3"));
    REQUIRE_TRUE(synth_miniapp::SceneButtonLabel(0, 0, 1) == std::string("S1 L"));
    REQUIRE_TRUE(synth_miniapp::SceneButtonLabel(1, 0, 1) == std::string("S2 R"));
    REQUIRE_TRUE(synth_miniapp::SceneButtonLabel(2, 2, 2) == std::string("S3 L R"));

    // The grid no longer computes cell geometry: it declares four rows of four
    // and the resolver divides the region it is given. Resolved against a
    // 410x330 region with the grid's 8 gaps, the cells are the same
    // 96.5 x 76.5 the retired arithmetic produced, at the same offsets.
    REQUIRE_TRUE(synth_miniapp::EncoderGridLayout::kEncoderCount == 16);
    {
        synth::ui::Builder builder;
        builder.Root("grid.root", {0.0f, 0.0f, 410.0f, 330.0f});
        synth::ui::LayoutOptions region;
        region.main = synth::ui::Extent::Weight(1.0f);
        region.padding = 0.0f;
        builder.Section("grid.region", region, [](synth::ui::Builder& builder) {
            synth_miniapp::EncoderGridLayout::Emit(
                builder, "grid", [](synth::ui::Builder& builder, std::size_t ix) {
                    builder.Draw("grid.cell." + std::to_string(ix),
                                 synth_miniapp::EncoderGridLayout::CellLayout(),
                                 [](synth::ui::Bounds) { return std::vector<synth::ui::DrawCommand>{}; });
                });
        });
        const synth::ui::NodeTree gridTree = builder.Build({0.0f, 0.0f, 410.0f, 330.0f});
        const std::array<std::size_t, 4> cornerIndexes{0, 3, 12, 15};
        const std::array<synth::ui::Bounds, 4> expectedCorners{{
            {0.0f, 0.0f, 96.5f, 76.5f},
            {313.5f, 0.0f, 96.5f, 76.5f},
            {0.0f, 253.5f, 96.5f, 76.5f},
            {313.5f, 253.5f, 96.5f, 76.5f},
        }};
        for (std::size_t ix = 0; ix < cornerIndexes.size(); ++ix) {
            const synth::ui::Bounds actual =
                SurfaceBoundsOf(gridTree, "grid.cell." + std::to_string(cornerIndexes[ix]));
            RequireNear(actual.x, expectedCorners[ix].x, 0.0001f, "encoder corner x");
            RequireNear(actual.y, expectedCorners[ix].y, 0.0001f, "encoder corner y");
            RequireNear(actual.width, expectedCorners[ix].width, 0.0001f, "encoder corner width");
            RequireNear(actual.height, expectedCorners[ix].height, 0.0001f, "encoder corner height");
        }
    }

    synth::ParameterManager manager;
    synth::MessageInBus uiBus(&manager);
    std::uint64_t timestamp = 42;
    bool dispatched = synth_miniapp::DispatchMiniAppAction(
        nullptr,
        timestamp,
        synth::ui::Action::Named(synth_miniapp::MiniAppActions::kStart),
        [](const synth::MessageIn&) {});
    REQUIRE_TRUE(!dispatched);

    synth::AppContext context;
    context.uiBus = &uiBus;
    dispatched = synth_miniapp::DispatchMiniAppAction(
        &context,
        timestamp,
        synth::ui::Action::Named(synth_miniapp::MiniAppActions::kStart),
        [&uiBus](const synth::MessageIn& message) { uiBus.Push(message); });
    REQUIRE_TRUE(dispatched);
    synth::MessageIn message;
    REQUIRE_TRUE(PopNextMessage(uiBus, message));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::Start);
    REQUIRE_TRUE(message.timestamp == 42);
}

TEST_CASE(miniapp_ui_snapshot_reflects_runtime_state_in_tree) {
    (void)UseScratchRuntimeDataPaths("ui_snapshot_reflects_runtime_state_in_tree");

    synth_rig::SynthRig<synth_miniapp::MiniApp> rig;
    rig.SetReset(true);
    rig.SetRandom(true);
    rig.SetRandomMod(true);
    rig.SelectGesture(0, true);
    rig.SetGestureValue(0, 0.33f);
    rig.SetSceneBlend(0.66f);
    rig.SelectScene(2);
    rig.RunBlocks(1);
    rig.UIState();

    synth::ui::Surface& surface = rig.Application().PortableSurface();
    const synth::ui::NodeTree tree = surface.BuildTree();

    const synth::ui::Node* gestureToggle = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kGestureToggle);
    REQUIRE_TRUE(gestureToggle != nullptr);
    REQUIRE_TRUE(gestureToggle->kind == synth::ui::NodeKind::Toggle);
    REQUIRE_TRUE(gestureToggle->checked);

    const synth::ui::Node* resetToggle = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kReset);
    REQUIRE_TRUE(resetToggle != nullptr);
    REQUIRE_TRUE(resetToggle->kind == synth::ui::NodeKind::Toggle);
    REQUIRE_TRUE(resetToggle->checked);

    const synth::ui::Node* randomToggle = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kRandom);
    REQUIRE_TRUE(randomToggle != nullptr);
    REQUIRE_TRUE(randomToggle->checked);

    const synth::ui::Node* randomModToggle = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kRandomMod);
    REQUIRE_TRUE(randomModToggle != nullptr);
    REQUIRE_TRUE(randomModToggle->checked);

    const synth::ui::Node* gestureSlider = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kGestureValue);
    REQUIRE_TRUE(gestureSlider != nullptr);
    REQUIRE_NEAR(gestureSlider->value, 0.33f, 1e-4f);

    const synth::ui::Node* blendSlider = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::kSceneBlend);
    REQUIRE_TRUE(blendSlider != nullptr);
    REQUIRE_NEAR(blendSlider->value, 0.66f, 1e-4f);

    RequireNodeKind(tree, synth_miniapp::MiniAppNodeIds::SceneButton(0).c_str(), synth::ui::NodeKind::Button);
    const synth::ui::Node* sceneOne = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::SceneButton(0).c_str());
    REQUIRE_TRUE(sceneOne != nullptr);
    REQUIRE_TRUE(sceneOne->label == "S1");
    const synth::ui::Node* sceneTwo = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::SceneButton(1).c_str());
    REQUIRE_TRUE(sceneTwo != nullptr);
    REQUIRE_TRUE(sceneTwo->label == "S2 R");
    const synth::ui::Node* sceneThree = FindNodeById(tree, synth_miniapp::MiniAppNodeIds::SceneButton(2).c_str());
    REQUIRE_TRUE(sceneThree != nullptr);
    REQUIRE_TRUE(sceneThree->label == "S3 L");
}

TEST_CASE(miniapp_modulation_view_draws_visualizer_beneath_encoder) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("miniapp_modulation_view_draws_visualizer_beneath_encoder"));
    rig.RunBlocks(4);
    rig.Press(kSlotIx, kTunePosition);
    rig.RunBlocks(1);

    auto& manager = rig.Engine().Manager();
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    TestVisualizer injectedVisualizer;
    ui->slots[0].cells[kTunePosition].visualizer.store(&injectedVisualizer, std::memory_order_relaxed);

    synth::AppContext context = rig.Engine().Context();
    context.uiState = ui.get();
    synth_miniapp::MiniAppUiSurface surface;
    surface.Attach(&context, &rig.Engine().Application());
    const synth::ui::NodeTree tree = surface.BuildTree();

    const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(kTunePosition);
    const std::string visualizerId = encoderId + ".visualizer";
    const synth::ui::Node* encoder = FindNodeById(tree, encoderId);
    const synth::ui::Node* visualizer = FindNodeById(tree, visualizerId);
    REQUIRE_TRUE(encoder != nullptr);
    REQUIRE_TRUE(visualizer != nullptr);
    REQUIRE_TRUE(visualizer->kind == synth::ui::NodeKind::Draw);
    REQUIRE_TRUE(visualizer->pointerDragAction == std::nullopt);
    REQUIRE_TRUE(visualizer->doubleClickAction == std::nullopt);
    REQUIRE_TRUE(encoder->pointerDragAction.has_value());
    REQUIRE_TRUE(encoder->doubleClickAction.has_value());
    REQUIRE_TRUE(!encoder->drawCommands.empty());
    REQUIRE_TRUE(encoder->drawCommands.front().kind == synth::ui::DrawCommand::Kind::FillEllipse);
    REQUIRE_TRUE(encoder->drawCommands.front().color.a > 0);
    REQUIRE_TRUE(encoder->drawCommands.front().color.a < 255);
    REQUIRE_TRUE(HasOuterEncoderFrame(*encoder));
    REQUIRE_TRUE(visualizer->bounds.x == encoder->bounds.x);
    REQUIRE_TRUE(visualizer->bounds.y == encoder->bounds.y);
    REQUIRE_TRUE(visualizer->bounds.width == encoder->bounds.width);
    REQUIRE_TRUE(visualizer->bounds.height == encoder->bounds.height);
    REQUIRE_TRUE(NodeIndexById(tree, visualizerId) < NodeIndexById(tree, encoderId));

    const std::array<float, 2> constantValues{0.0f, 1.0f};
    synth::ui::ConstantBarVisualizer constantVisualizer(constantValues, synth::Color::Yellow);
    ui->slots[0].cells[kTunePosition].visualizer.store(
        &constantVisualizer, std::memory_order_relaxed);
    const synth::ui::NodeTree constantTree = surface.BuildTree();
    const synth::ui::Node* constantEncoder = FindNodeById(constantTree, encoderId);
    REQUIRE_TRUE(constantEncoder != nullptr);
    REQUIRE_TRUE(!HasOuterEncoderFrame(*constantEncoder));
}

TEST_CASE(miniapp_top_level_parameter_rendering_remains_encoder_only_without_visualizer) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("miniapp_top_level_parameter_rendering_remains_encoder_only_without_visualizer"));
    rig.RunBlocks(1);

    const synth::ui::NodeTree tree = BuildMiniAppTree(rig);
    const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(kTunePosition);

    REQUIRE_TRUE(FindNodeById(tree, encoderId) != nullptr);
    REQUIRE_TRUE(FindNodeById(tree, encoderId + ".visualizer") == nullptr);
    const synth::ui::Node* encoder = FindNodeById(tree, encoderId);
    REQUIRE_TRUE(encoder != nullptr);
    REQUIRE_TRUE(!encoder->drawCommands.empty());
    REQUIRE_TRUE(encoder->drawCommands.front().kind == synth::ui::DrawCommand::Kind::FillEllipse);
    REQUIRE_TRUE(encoder->drawCommands.front().color.a == 255);
}

TEST_CASE(miniapp_hidden_visualizer_keeps_encoder_opaque_and_encoder_only) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("miniapp_hidden_visualizer_keeps_encoder_opaque_and_encoder_only"));
    rig.RunBlocks(4);
    rig.Press(kSlotIx, kTunePosition);
    rig.RunBlocks(1);

    auto& manager = rig.Engine().Manager();
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    TestVisualizer hiddenVisualizer;
    hiddenVisualizer.SetVisible(false);
    ui->slots[0].cells[kTunePosition].visualizer.store(&hiddenVisualizer, std::memory_order_relaxed);

    synth::AppContext context = rig.Engine().Context();
    context.uiState = ui.get();
    synth_miniapp::MiniAppUiSurface surface;
    surface.Attach(&context, &rig.Engine().Application());
    const synth::ui::NodeTree tree = surface.BuildTree();

    const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(kTunePosition);
    const synth::ui::Node* encoder = FindNodeById(tree, encoderId);
    REQUIRE_TRUE(encoder != nullptr);
    REQUIRE_TRUE(FindNodeById(tree, encoderId + ".visualizer") == nullptr);
    REQUIRE_TRUE(!encoder->drawCommands.empty());
    REQUIRE_TRUE(encoder->drawCommands.front().kind == synth::ui::DrawCommand::Kind::FillEllipse);
    REQUIRE_TRUE(encoder->drawCommands.front().color.a == 255);
}

TEST_CASE(miniapp_bank_transition_clears_modulation_visualizer_underlay) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("miniapp_bank_transition_clears_modulation_visualizer_underlay"));
    rig.RunBlocks(1);
    rig.Press(kSlotIx, kTunePosition);
    rig.RunBlocks(1);

    const std::string encoderId = synth_miniapp::MiniAppNodeIds::Encoder(0);
    const synth::ui::NodeTree modulationTree = BuildMiniAppTree(rig);
    REQUIRE_TRUE(FindNodeById(modulationTree, encoderId) != nullptr);
    REQUIRE_TRUE(FindNodeById(modulationTree, encoderId + ".visualizer") != nullptr);

    rig.SelectBank(kSlotIx, kLfoBankIx);
    rig.RunBlocks(1);

    const synth::ui::NodeTree lfoTree = BuildMiniAppTree(rig);
    REQUIRE_TRUE(FindNodeById(lfoTree, encoderId) != nullptr);
    REQUIRE_TRUE(FindNodeById(lfoTree, encoderId + ".visualizer") == nullptr);
}

TEST_CASE(miniapp_registers_distinct_scope_visualizers_for_modulators) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("miniapp_registers_distinct_scope_visualizers_for_modulators"));
    auto* group = rig.Engine().Application().Group();
    REQUIRE_TRUE(group != nullptr);

    const auto& modulators = group->GetModulators();
    synth::ui::Visualizer* mod4 = modulators.Metadata(4).visualizer;
    synth::ui::Visualizer* mod5 = modulators.Metadata(5).visualizer;
    synth::ui::Visualizer* mod6 = modulators.Metadata(6).visualizer;

    REQUIRE_TRUE(mod4 != nullptr);
    REQUIRE_TRUE(mod5 != nullptr);
    REQUIRE_TRUE(mod6 != nullptr);
    REQUIRE_TRUE(mod5 != mod4);
    REQUIRE_TRUE(mod6 != mod4);
    REQUIRE_TRUE(mod6 != mod5);
    REQUIRE_TRUE(mod4->Visible());
    REQUIRE_TRUE(mod5->Visible());
    REQUIRE_TRUE(mod6->Visible());
}

TEST_CASE(miniapp_registers_clocked_adsr_tempo_topology_without_changing_performer_topology) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("registers_ganged_random_lfo_without_changing_performer_topology"));

    auto& core = rig.Application();
    auto& manager = *core.Context()->parameterManager;
    auto& group = *core.Group();
    const auto& groupConfig = group.Config();
    const auto& modulators = group.GetModulators();

    REQUIRE_TRUE(groupConfig.numVoices == 2);
    REQUIRE_TRUE(groupConfig.numModulators == 15);
    REQUIRE_TRUE(groupConfig.numScenes == 3);
    REQUIRE_TRUE(groupConfig.maxParameters == 272);
    REQUIRE_TRUE(group.ParameterCount() == 17);
    REQUIRE_TRUE(group.TopLevelParameterCount() == 17);
    REQUIRE_TRUE(group.GestureCount() == 1);
    REQUIRE_TRUE(manager.ParameterCount() == 17);
    REQUIRE_TRUE(manager.NumGroups() == 1);
    REQUIRE_TRUE(manager.GestureCount() == 1);
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 1);
    REQUIRE_TRUE(manager.BankAt(0) == core.VcoBank());
    REQUIRE_TRUE(manager.BankAt(1) == core.LfoBank());
    REQUIRE_TRUE(manager.BankAt(2) == nullptr);
    REQUIRE_TRUE(manager.BankSlotAt(0) == core.Slot());
    REQUIRE_TRUE(manager.BankSlotAt(1) == nullptr);

    REQUIRE_TRUE(modulators.NumVoices() == 2);
    REQUIRE_TRUE(modulators.NumModulators() == 15);
    REQUIRE_TRUE(core.Slot()->PhysicalEncoders().size() == 16);
    for (std::size_t position = 0; position < 16; ++position) {
        REQUIRE_TRUE(core.Slot()->PhysicalEncoders()[position] == 10 + position);
    }
    const std::array<const char*, 4> randomNames{
        "Random 500 ms", "Random 2 s", "Random 6 s", "Random 16 s"};
    const std::array<const char*, 4> randomShortNames{"Rnd .5", "Rnd 2", "Rnd 6", "Rnd 16"};
    const std::array<synth::Color, 4> randomColors{
        synth::Color::Cyan, synth::Color::Blue, synth::Color::Indigo, synth::Color::Orange};
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(modulators.Metadata(random).connected);
        REQUIRE_TRUE(modulators.Metadata(random).name == randomNames[random]);
        REQUIRE_TRUE(modulators.Metadata(random).shortName == randomShortNames[random]);
        REQUIRE_TRUE(modulators.Metadata(random).sourceColor == randomColors[random]);
    }
    REQUIRE_TRUE(modulators.Metadata(4).name == "VCO Direct");
    REQUIRE_TRUE(modulators.Metadata(4).sourceColor == synth::Color::Cyan);
    REQUIRE_TRUE(modulators.Metadata(5).name == "VCO Swapped");
    REQUIRE_TRUE(modulators.Metadata(5).sourceColor == synth::Color::Orange);
    REQUIRE_TRUE(modulators.Metadata(6).name == "LFO");
    REQUIRE_TRUE(modulators.Metadata(6).sourceColor == synth::Color::Green);
    REQUIRE_TRUE(modulators.Metadata(7).connected);
    REQUIRE_TRUE(modulators.Metadata(7).name == "ADSR");
    REQUIRE_TRUE(modulators.Metadata(7).shortName == "ADSR");
    REQUIRE_TRUE(modulators.Metadata(7).sourceColor == synth::Color::Blue);
    REQUIRE_TRUE(modulators.Metadata(7).visualizer == nullptr);
    REQUIRE_TRUE(modulators.Metadata(11).connected);
    REQUIRE_TRUE(modulators.Metadata(11).name == "Constant");
    REQUIRE_TRUE(modulators.Metadata(11).shortName == "Const");
    REQUIRE_TRUE(modulators.Metadata(11).sourceColor == synth::Color::Yellow);
    REQUIRE_TRUE(modulators.Metadata(14).connected);
    REQUIRE_TRUE(modulators.Metadata(14).name == "Noise");
    REQUIRE_TRUE(modulators.Metadata(14).shortName == "Noise");
    REQUIRE_TRUE(modulators.Metadata(14).sourceColor == synth::Color::White);
    for (const std::size_t gap : {8u, 9u, 10u, 12u, 13u}) {
        REQUIRE_TRUE(!modulators.Metadata(gap).connected);
        REQUIRE_TRUE(modulators.Metadata(gap).visualizer == nullptr);
    }

    auto& standard = core.StandardModulatorsInstance();
    const auto& input = static_cast<const synth::StandardModulators<2>&>(standard).RandomInput(0);
    REQUIRE_TRUE(input.waiting.muSeconds == 0.5);
    REQUIRE_TRUE(input.waiting.sigmaSeconds == 0.15);
    REQUIRE_TRUE(input.waiting.internalSigmaHz == 0.4);
    REQUIRE_TRUE(input.moving.muSeconds == 0.25);
    REQUIRE_TRUE(input.moving.sigmaSeconds == 0.075);
    REQUIRE_TRUE(input.moving.internalSigmaHz == 0.8);
    REQUIRE_TRUE(input.targetInternalSigma == 0.1f);
    REQUIRE_TRUE(standard.RandomProcessor(0).VoiceColor(0) == synth::Color::Cyan);
    REQUIRE_TRUE(standard.RandomProcessor(0).VoiceColor(1) == synth::Color::Orange);

    synth::ui::Visualizer* const retainedVisualizer =
        &standard.RandomVisualizer(0);
    REQUIRE_TRUE(modulators.Metadata(0).visualizer == retainedVisualizer);
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(modulators.Metadata(random).visualizer == &standard.RandomVisualizer(random));
        standard.RandomOutputRow(random)[0] = 0.05f + static_cast<float>(random) * 0.1f;
        standard.RandomOutputRow(random)[1] = 0.55f + static_cast<float>(random) * 0.1f;
    }
    group.UpdateModValues();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(modulators.Value(0, random) == standard.RandomOutputRow(random)[0]);
        REQUIRE_TRUE(modulators.Value(1, random) == standard.RandomOutputRow(random)[1]);
    }
    REQUIRE_TRUE(modulators.Metadata(11).visualizer == &standard.ConstantVisualizer());
    REQUIRE_TRUE(modulators.Metadata(14).visualizer == &standard.NoiseVisualizer());
    rig.RunBlocks(1);
    REQUIRE_TRUE(modulators.Metadata(0).visualizer == retainedVisualizer);

    rig.Press(kSlotIx, kTunePosition);
    rig.RunBlocks(1);
    const synth::ParameterManager::UIState& uiState = rig.UIState();
    REQUIRE_TRUE(group.ParameterCount() == 27);
    REQUIRE_TRUE(uiState.slots[0].showingModulationView.load());
    for (const std::size_t connected : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u, 14u}) {
        synth::Parameter* const depth = core.Parameters()[0]->ModulationDepthParameter(connected);
        REQUIRE_TRUE(depth != nullptr);
        REQUIRE_TRUE(core.VcoBank()->VisibleParameter(10 + connected) == depth);
        REQUIRE_TRUE(uiState.slots[0].cells[connected].connected.load());
    }
    for (const std::size_t gap : {8u, 9u, 10u, 12u, 13u}) {
        REQUIRE_TRUE(core.Parameters()[0]->ModulationDepthParameter(gap) == nullptr);
        REQUIRE_TRUE(core.VcoBank()->VisibleParameter(10 + gap) == nullptr);
        REQUIRE_TRUE(!uiState.slots[0].cells[gap].connected.load());
    }
    REQUIRE_TRUE(core.VcoBank()->VisibleParameter(25) == core.Parameters()[0]);
    REQUIRE_TRUE(uiState.slots[0].cells[0].visualizer.load(std::memory_order_relaxed) ==
                 retainedVisualizer);
    REQUIRE_TRUE(modulators.Metadata(0).visualizer == retainedVisualizer);
    const synth::ui::NodeTree tree = BuildMiniAppTree(rig);
    for (const std::size_t connected : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 11u, 14u}) {
        REQUIRE_TRUE(uiState.slots[0].cells[connected].visualizer.load(std::memory_order_relaxed) ==
                     modulators.Metadata(connected).visualizer);
    }
    for (const std::size_t visibleConnected : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
        REQUIRE_TRUE(FindNodeById(
                         tree, synth_miniapp::MiniAppNodeIds::Encoder(visibleConnected) + ".visualizer") != nullptr);
    }
    REQUIRE_TRUE(uiState.slots[0].cells[7].visualizer.load(std::memory_order_relaxed) == nullptr);
    REQUIRE_TRUE(FindNodeById(
                     tree, synth_miniapp::MiniAppNodeIds::Encoder(7) + ".visualizer") == nullptr);
    for (const std::size_t gap : {8u, 9u, 10u, 12u, 13u}) {
        REQUIRE_TRUE(uiState.slots[0].cells[gap].visualizer.load(std::memory_order_relaxed) == nullptr);
        REQUIRE_TRUE(FindNodeById(
                         tree, synth_miniapp::MiniAppNodeIds::Encoder(gap) + ".visualizer") == nullptr);
    }
    REQUIRE_TRUE(FindNodeById(
                     tree, synth_miniapp::MiniAppNodeIds::Encoder(15) + ".visualizer") == nullptr);
}

TEST_CASE(miniapp_registers_exact_adsr_and_tempo_parameter_contract) {
    constexpr double kNegotiatedSampleRate = 44100.0;
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("registers_exact_adsr_and_tempo_parameter_contract"),
        {.sampleRate = kNegotiatedSampleRate, .blockSize = 37});
    auto& core = rig.Application();
    auto& manager = *core.Context()->parameterManager;
    const auto& parameters = core.Parameters();
    const std::array<const char*, 17> expectedNames{
        "Tune", "Phase", "Shape", "Volume", "Filter Cutoff", "Filter Resonance", "Filter Blend",
        "LFO Frequency", "LFO Shape", "LFO Phase Offset", "LFO Skew", "LFO Exponent",
        "Attack", "Decay", "Sustain", "Release", "Tempo",
    };
    REQUIRE_TRUE(parameters.size() == expectedNames.size());
    for (std::size_t parameterIx = 0; parameterIx < parameters.size(); ++parameterIx) {
        REQUIRE_TRUE(parameters[parameterIx] != nullptr);
        REQUIRE_TRUE(parameters[parameterIx]->Id() == parameterIx);
        REQUIRE_TRUE(parameters[parameterIx]->Name() == expectedNames[parameterIx]);
    }

    synth::Parameter* const tempo = manager.FindParameterByName("Tempo");
    REQUIRE_TRUE(tempo != nullptr);
    REQUIRE_TRUE(tempo->Id() == 16);
    REQUIRE_TRUE(tempo->ShortName() == "BPM");
    REQUIRE_TRUE(tempo->Range() == synth::RangeKind::Unipolar);
    REQUIRE_TRUE(tempo->BaseColor() == synth::Color::White);
    REQUIRE_TRUE(tempo->IndicatorColor(0) == synth::Color::Cyan);
    REQUIRE_TRUE(tempo->IndicatorColor(1) == synth::Color::Orange);
    REQUIRE_TRUE(tempo->GetRaw(0) == (120.0f - 30.0f) / (300.0f - 30.0f));
    REQUIRE_TRUE(manager.GetLinear(30.0f, 300.0f, 0, tempo->Id()) == 120.0f);

    for (std::size_t sceneIx = 0; sceneIx < 3; ++sceneIx) {
        tempo->SceneCenter(sceneIx) = 0.0f;
    }
    manager.ComputeAllParameters();
    REQUIRE_TRUE(manager.GetLinear(30.0f, 300.0f, 0, tempo->Id()) == 30.0f);
    for (std::size_t sceneIx = 0; sceneIx < 3; ++sceneIx) {
        tempo->SceneCenter(sceneIx) = 1.0f;
    }
    manager.ComputeAllParameters();
    REQUIRE_TRUE(manager.GetLinear(30.0f, 300.0f, 0, tempo->Id()) == 300.0f);

    RequireAdsrTempoAccessors(rig, kNegotiatedSampleRate);
}

TEST_CASE(miniapp_clock_plan_drives_exact_adsr_gate_boundaries_across_nondivisor_blocks) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("clock_plan_drives_exact_adsr_gate_boundaries"),
        {.sampleRate = 8.0, .blockSize = 3});

    rig.StartAt(0);
    rig.RunBlockAt(1);
    const synth::ClockBlockPlan* firstPlan = rig.CurrentClockPlan();
    REQUIRE_TRUE(firstPlan != nullptr);
    REQUIRE_TRUE(firstPlan->StartSample() == 0);
    REQUIRE_TRUE(firstPlan->EndSample() == 3);
    REQUIRE_TRUE(firstPlan->TransportState() == synth::ClockTransportState::Running);
    REQUIRE_TRUE(firstPlan->TransportQuarterNotesAt(0.0) == 0.0);
    REQUIRE_TRUE(firstPlan->TransportQuarterNotesAt(2.0) == 0.5);
    RequireAdsrClockDebug(rig.Application(), 3, true, false, 1, 1, 0, 2);
    for (std::size_t voiceIx = 0; voiceIx < synth_miniapp::MiniAppCore::kVoiceCount; ++voiceIx) {
        REQUIRE_TRUE(!rig.Application().AdsrModuleInstance().CurrentInput().voices[voiceIx].gate);
    }

    rig.RunBlockAt(2);
    const synth::ClockBlockPlan* secondPlan = rig.CurrentClockPlan();
    REQUIRE_TRUE(secondPlan != nullptr);
    REQUIRE_TRUE(secondPlan->StartSample() == 3);
    REQUIRE_TRUE(secondPlan->EndSample() == 6);
    REQUIRE_TRUE(secondPlan->TransportQuarterNotesAt(3.0) == 0.75);
    REQUIRE_TRUE(secondPlan->TransportQuarterNotesAt(4.0) == 1.0);
    RequireAdsrClockDebug(rig.Application(), 3, false, true, 1, 0, 4, 0);
    for (std::size_t voiceIx = 0; voiceIx < synth_miniapp::MiniAppCore::kVoiceCount; ++voiceIx) {
        REQUIRE_TRUE(rig.Application().AdsrModuleInstance().CurrentInput().voices[voiceIx].gate);
    }

    rig.StopAt(3);
    rig.RunBlockAt(3);
    const synth::ClockBlockPlan* stoppedPlan = rig.CurrentClockPlan();
    REQUIRE_TRUE(stoppedPlan != nullptr);
    REQUIRE_TRUE(stoppedPlan->StartSample() == 6);
    REQUIRE_TRUE(stoppedPlan->TransportState() == synth::ClockTransportState::Stopped);
    RequireAdsrClockDebug(rig.Application(), 3, false, false, 0, 1, 0, 6);
    for (std::size_t voiceIx = 0; voiceIx < synth_miniapp::MiniAppCore::kVoiceCount; ++voiceIx) {
        REQUIRE_TRUE(!rig.Application().AdsrModuleInstance().CurrentInput().voices[voiceIx].gate);
    }
}

TEST_CASE(miniapp_publishes_current_adsr_outputs_to_both_voices_in_the_same_frame) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("publishes_current_adsr_outputs_in_same_frame"),
        {.sampleRate = 1000.0, .blockSize = 1});
    rig.StartAt(0);
    rig.RunBlockAt(1);

    const auto& core = rig.Application();
    const auto outputs = core.AdsrModuleInstance().Outputs();
    const auto& mirror = core.AdsrModulationMirror();
    const auto& modulators = core.Group()->GetModulators();
    REQUIRE_TRUE(outputs.size() == 2);
    REQUIRE_TRUE(mirror.size() == 2);
    for (std::size_t voiceIx = 0; voiceIx < 2; ++voiceIx) {
        REQUIRE_TRUE(outputs[voiceIx] > 0.0f);
        REQUIRE_TRUE(mirror[voiceIx] == outputs[voiceIx]);
        REQUIRE_TRUE(modulators.Value(voiceIx, 7) == mirror[voiceIx]);
    }
}

TEST_CASE(miniapp_tempo_requests_respect_committed_plans_and_external_authority) {
    constexpr double kSampleRate = 48000.0;
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("tempo_requests_respect_committed_plans_and_external_authority"),
        {.sampleRate = kSampleRate, .blockSize = 7});
    auto& core = rig.Application();
    auto& manager = *core.Context()->parameterManager;

    rig.RunBlockAt(1);
    RequireTempoRequestDebug(core, 0, 0.0f, false);
    REQUIRE_TRUE(rig.Engine().Clock().TempoBpm() == 120.0);

    SetNormalizedParameter(manager, core.TempoParameter(), (180.0f - 30.0f) / (300.0f - 30.0f));
    rig.RunBlockAt(2);
    const synth::ClockBlockPlan* committedBeforeManualRequest = rig.CurrentClockPlan();
    REQUIRE_TRUE(committedBeforeManualRequest != nullptr);
    REQUIRE_NEAR(committedBeforeManualRequest->QuarterNotesPerSample(),
                 120.0 / (60.0 * kSampleRate), 1.0e-15);
    RequireTempoRequestDebug(core, 1, 180.0f, true);
    REQUIRE_TRUE(rig.Engine().Clock().TempoBpm() == 180.0);

    rig.RunBlockAt(3);
    const synth::ClockBlockPlan* firstManualTempoPlan = rig.CurrentClockPlan();
    REQUIRE_TRUE(firstManualTempoPlan != nullptr);
    REQUIRE_NEAR(firstManualTempoPlan->QuarterNotesPerSample(),
                 180.0 / (60.0 * kSampleRate), 1.0e-15);
    RequireTempoRequestDebug(core, 1, 180.0f, true);

    synth::SyncConfig externalConfig;
    externalConfig.receiveClock = true;
    REQUIRE_TRUE(rig.SetSyncConfig(externalConfig));
    rig.ExternalClockAt(0, 1000);
    rig.RunBlockAt(1000);
    rig.ExternalClockAt(0, 22000);
    rig.RunBlockAt(22000);
    const double externallyAuthoritativeBpm = rig.Engine().Clock().TempoBpm();
    REQUIRE_TRUE(std::isfinite(externallyAuthoritativeBpm));
    REQUIRE_TRUE(externallyAuthoritativeBpm > 0.0);

    SetNormalizedParameter(manager, core.TempoParameter(), (90.0f - 30.0f) / (300.0f - 30.0f));
    rig.RunBlockAt(23000);
    RequireTempoRequestDebug(core, 2, 90.0f, false);
    REQUIRE_TRUE(rig.Engine().Clock().TempoBpm() == externallyAuthoritativeBpm);

    synth::SyncConfig manualConfig;
    REQUIRE_TRUE(rig.SetSyncConfig(manualConfig));
    REQUIRE_TRUE(rig.Engine().Clock().TempoBpm() == 180.0);
    rig.RunBlockAt(24000);
    RequireTempoRequestDebug(core, 2, 90.0f, false);
    REQUIRE_TRUE(rig.Engine().Clock().TempoBpm() == 180.0);
}

TEST_CASE(miniapp_loads_old_six_index_depth_data_without_alias_or_translation) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64, UseScratchRuntimeDataPaths("loads_old_six_index_depth_data_without_alias_or_translation"));
    auto& core = rig.Application();
    synth::JsonArena arena(65536);
    synth::JSON values = arena.Object();
    synth::JSON tune = arena.Object();
    synth::JSON oldDepths = arena.Object();
    const std::array<float, 6> oldIndexValues{0.1f, 0.2f, 0.3f, 0.4f, 0.6f, 0.7f};
    for (std::size_t oldIndex = 0; oldIndex < oldIndexValues.size(); ++oldIndex) {
        synth::JSON depth = arena.Object();
        synth::JSON centers = arena.Array();
        for (std::size_t scene = 0; scene < 3; ++scene) {
            centers.AppendNew(arena.Real(oldIndexValues[oldIndex]));
        }
        depth.SetNew("sceneCenters", centers);
        oldDepths.SetNew(std::to_string(oldIndex).c_str(), depth);
    }
    tune.SetNew("modDepths", oldDepths);
    values.SetNew("Tune", tune);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(core.Context()->parameterManager->LoadParameterValuesFromJSON(values));

    synth::Parameter* tuneParameter = core.Parameters()[0];
    for (std::size_t oldIndex = 0; oldIndex < oldIndexValues.size(); ++oldIndex) {
        REQUIRE_TRUE(tuneParameter->ModulationDepthParameter(oldIndex) != nullptr);
        REQUIRE_NEAR(tuneParameter->ModulationDepthParameter(oldIndex)->SceneCenter(0),
                     oldIndexValues[oldIndex], 1e-6f);
    }
    for (std::size_t newIndex = 6; newIndex < 15; ++newIndex) {
        REQUIRE_TRUE(tuneParameter->ModulationDepthParameter(newIndex) == nullptr);
    }
    REQUIRE_TRUE(core.Group()->GetModulators().Metadata(0).name == "Random 500 ms");
    REQUIRE_TRUE(core.Group()->GetModulators().Metadata(4).name == "VCO Direct");
    REQUIRE_NEAR(tuneParameter->ModulationDepthParameter(0)->SceneCenter(0), 0.1f, 1e-6f);
    REQUIRE_NEAR(tuneParameter->ModulationDepthParameter(4)->SceneCenter(0), 0.6f, 1e-6f);
}

TEST_CASE(miniapp_processes_and_publishes_ganged_random_lfo_at_audio_block_boundaries) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64,
        UseScratchRuntimeDataPaths("processes_and_publishes_ganged_random_lfo_at_audio_block_boundaries"));
    constexpr double negotiatedSampleRate = 44100.0;
    rig.Engine().Prepare(negotiatedSampleRate, synth_miniapp::MiniAppCore::Config().preferredBlockSize);

    auto& core = rig.Application();
    auto& standard = core.StandardModulatorsInstance();
    auto& gang = standard.RandomProcessor(0);
    auto& sources = standard.RandomOutputRow(0);
    auto& modulators = core.Group()->GetModulators();
    REQUIRE_TRUE(gang.SampleRate() == negotiatedSampleRate);

    synth::GangedRandomLfoSnapshot<synth_miniapp::MiniAppCore::kVoiceCount> beforeBlock;
    REQUIRE_TRUE(gang.ReadSnapshot(beforeBlock));
    REQUIRE_TRUE(beforeBlock.sampleRate == 0.0);

    // Sentinels distinguish process-before-update from update-before-process:
    // the audio loop must replace these before UpdateModValues observes them.
    sources[0] = -0.25f;
    sources[1] = -0.75f;
    rig.RunBlocks(1);

    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(standard.RandomProcessor(random).RoundElapsedSamples() == 255.0);
        for (std::size_t voice = 0; voice < synth_miniapp::MiniAppCore::kVoiceCount; ++voice) {
            REQUIRE_TRUE(modulators.Value(voice, random) == standard.RandomOutputRow(random)[voice]);
        }
    }
    for (std::size_t voice = 0; voice < synth_miniapp::MiniAppCore::kVoiceCount; ++voice) {
        REQUIRE_TRUE(sources[voice] == gang.Output(voice));
        REQUIRE_TRUE(modulators.Value(voice, 0) == sources[voice]);
    }

    synth::GangedRandomLfoSnapshot<synth_miniapp::MiniAppCore::kVoiceCount> published;
    REQUIRE_TRUE(gang.ReadSnapshot(published));
    REQUIRE_TRUE(published.sampleRate == negotiatedSampleRate);
    REQUIRE_TRUE(published.roundElapsedSamples == gang.RoundElapsedSamples());
    REQUIRE_TRUE(published.voices[0].color == synth::Color::Cyan);
    REQUIRE_TRUE(published.voices[1].color == synth::Color::Orange);
    for (std::size_t voice = 0; voice < synth_miniapp::MiniAppCore::kVoiceCount; ++voice) {
        REQUIRE_TRUE(published.voices[voice].output == gang.Output(voice));
        REQUIRE_TRUE(published.voices[voice].waitingIncrement == gang.VoiceInputs()[voice].waitingIncrement);
        REQUIRE_TRUE(published.voices[voice].movingIncrement == gang.VoiceInputs()[voice].movingIncrement);
        REQUIRE_TRUE(published.voices[voice].shape == gang.VoiceInputs()[voice].shape);
    }

    const double publishedElapsed = published.roundElapsedSamples;
    gang.Process(static_cast<const synth::StandardModulators<2>&>(standard).RandomInput(0));
    synth::GangedRandomLfoSnapshot<synth_miniapp::MiniAppCore::kVoiceCount> stillPublished;
    REQUIRE_TRUE(gang.ReadSnapshot(stillPublished));
    REQUIRE_TRUE(stillPublished.roundElapsedSamples == publishedElapsed);

    rig.RunBlocks(1);
    synth::GangedRandomLfoSnapshot<synth_miniapp::MiniAppCore::kVoiceCount> nextPublished;
    REQUIRE_TRUE(gang.ReadSnapshot(nextPublished));
    REQUIRE_TRUE(nextPublished.roundElapsedSamples == gang.RoundElapsedSamples());
    REQUIRE_TRUE(nextPublished.roundElapsedSamples > publishedElapsed);
}

TEST_CASE(miniapp_registers_noise_at_standard_index_fourteen) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        1, UseScratchRuntimeDataPaths("miniapp_registers_noise_as_the_fifth_modulator"));
    auto* group = rig.Engine().Application().Group();
    REQUIRE_TRUE(group != nullptr);
    REQUIRE_TRUE(group->Config().numVoices == 2);
    REQUIRE_TRUE(group->Config().numModulators == 15);

    const auto& modulators = group->GetModulators();
    const auto metadata = modulators.Metadata();
    REQUIRE_TRUE(metadata.size() == 15);
    REQUIRE_TRUE(metadata[14].connected);
    REQUIRE_TRUE(metadata[14].name == "Noise");
    REQUIRE_TRUE(metadata[14].shortName == "Noise");
    REQUIRE_TRUE(metadata[14].sourceColor == synth::Color::White);
    REQUIRE_TRUE(metadata[14].visualizer == &rig.Application().StandardModulatorsInstance().NoiseVisualizer());
}

TEST_CASE(miniapp_registers_constant_at_standard_index_eleven_without_sample_work) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        8, UseScratchRuntimeDataPaths("miniapp_registers_constant_as_the_sixth_modulator"));
    auto& core = rig.Application();
    auto& group = *core.Group();
    const auto& modulators = group.GetModulators();
    const auto metadata = modulators.Metadata();

    REQUIRE_TRUE(group.Config().numVoices == 2);
    REQUIRE_TRUE(group.Config().numModulators == 15);
    REQUIRE_TRUE(group.Config().maxParameters == 272);
    REQUIRE_TRUE(metadata.size() == 15);
    REQUIRE_TRUE(metadata[11].connected);
    REQUIRE_TRUE(metadata[11].name == "Constant");
    REQUIRE_TRUE(metadata[11].shortName == "Const");
    REQUIRE_TRUE(metadata[11].sourceColor == synth::Color::Yellow);
    REQUIRE_TRUE(metadata[11].visualizer == &core.StandardModulatorsInstance().ConstantVisualizer());

    const auto pointers = core.StandardModulatorsInstance().ConstantProcessor().SourcePointers();
    REQUIRE_TRUE(pointers.size() == 2);
    REQUIRE_TRUE(*pointers[0] == 0.0f);
    REQUIRE_TRUE(*pointers[1] == 1.0f);
    float* const pointer0 = pointers[0];
    float* const pointer1 = pointers[1];

    rig.RunBlocks(1);
    REQUIRE_TRUE(modulators.Value(0, 11) == 0.0f);
    REQUIRE_TRUE(modulators.Value(1, 11) == 1.0f);
    rig.RunBlocks(2);
    REQUIRE_TRUE(core.StandardModulatorsInstance().ConstantProcessor().SourcePointers()[0] == pointer0);
    REQUIRE_TRUE(core.StandardModulatorsInstance().ConstantProcessor().SourcePointers()[1] == pointer1);
    REQUIRE_TRUE(*pointer0 == 0.0f && *pointer1 == 1.0f);
    REQUIRE_TRUE(modulators.Value(0, 11) == 0.0f);
    REQUIRE_TRUE(modulators.Value(1, 11) == 1.0f);
}

TEST_CASE(miniapp_publishes_new_noise_values_before_each_modulation_update) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        1, UseScratchRuntimeDataPaths("miniapp_publishes_new_noise_values_before_each_modulation_update"));
    auto& modulators = rig.Engine().Application().Group()->GetModulators();
    rig.RunBlocks(1);
    const float first0 = modulators.Value(0, 14);
    const float first1 = modulators.Value(1, 14);
    rig.RunBlocks(1);
    const float second0 = modulators.Value(0, 14);
    const float second1 = modulators.Value(1, 14);
    REQUIRE_TRUE(first0 > 0.0f && first0 < 1.0f);
    REQUIRE_TRUE(first1 > 0.0f && first1 < 1.0f);
    REQUIRE_TRUE(second0 > 0.0f && second0 < 1.0f);
    REQUIRE_TRUE(second1 > 0.0f && second1 < 1.0f);
    REQUIRE_TRUE(first0 != second0 || first1 != second1);
}

TEST_CASE(miniapp_color_flow_keeps_semantic_roles_independent) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64, UseScratchRuntimeDataPaths("color_flow_keeps_semantic_roles_independent"));
    rig.RunBlocks(1);

    const std::array<synth::Color, 17> expectedBaseColors{
        synth::Color::Cyan,
        synth::Color::Indigo,
        synth::Color::Orange,
        synth::Color::Green,
        synth::Color::Cyan,
        synth::Color::Yellow,
        synth::Color::Orange,
        synth::Color::Green,
        synth::Color::Cyan,
        synth::Color::Indigo,
        synth::Color::Orange,
        synth::Color::Yellow,
        synth::Color::Cyan,
        synth::Color::Blue,
        synth::Color::Green,
        synth::Color::Orange,
        synth::Color::White,
    };
    const auto& parameters = rig.Application().Parameters();
    REQUIRE_TRUE(parameters.size() == expectedBaseColors.size());
    for (std::size_t parameterIx = 0; parameterIx < parameters.size(); ++parameterIx) {
        REQUIRE_TRUE(parameters[parameterIx]->BaseColor() == expectedBaseColors[parameterIx]);
        REQUIRE_TRUE(parameters[parameterIx]->IndicatorColor(0) == synth::Color::Cyan);
        REQUIRE_TRUE(parameters[parameterIx]->IndicatorColor(1) == synth::Color::Orange);
    }

    REQUIRE_TRUE(rig.Application().VcoBank()->BankColor() == synth::Color::Cyan);
    REQUIRE_TRUE(rig.Application().LfoBank()->BankColor() == synth::Color::Green);
    const auto modulatorMetadata = rig.Application().Group()->GetModulators().Metadata();
    REQUIRE_TRUE(modulatorMetadata.size() == 15);
    REQUIRE_TRUE(modulatorMetadata[0].sourceColor == synth::Color::Cyan);
    REQUIRE_TRUE(modulatorMetadata[1].sourceColor == synth::Color::Blue);
    REQUIRE_TRUE(modulatorMetadata[2].sourceColor == synth::Color::Indigo);
    REQUIRE_TRUE(modulatorMetadata[3].sourceColor == synth::Color::Orange);
    REQUIRE_TRUE(modulatorMetadata[4].sourceColor == synth::Color::Cyan);
    REQUIRE_TRUE(modulatorMetadata[5].sourceColor == synth::Color::Orange);
    REQUIRE_TRUE(modulatorMetadata[6].sourceColor == synth::Color::Green);
    REQUIRE_TRUE(modulatorMetadata[7].sourceColor == synth::Color::Blue);
    REQUIRE_TRUE(modulatorMetadata[11].sourceColor == synth::Color::Yellow);
    REQUIRE_TRUE(modulatorMetadata[14].sourceColor == synth::Color::White);
    REQUIRE_TRUE(rig.Application().Context()->parameterManager->GestureMetadataAt(0).gestureColor ==
                 synth::Color::Orange);

    const auto vcoScope = synth_miniapp::VcoWaveformDrawStateFromCore(rig.Application());
    REQUIRE_TRUE(vcoScope.layers.size() == 2);
    REQUIRE_TRUE(vcoScope.layers[0].scopeColor == synth::Color::Cyan);
    REQUIRE_TRUE(vcoScope.layers[1].scopeColor == synth::Color::Orange);
    const auto lfoScope = synth_miniapp::LfoWaveformDrawStateFromCore(rig.Application());
    REQUIRE_TRUE(lfoScope.layers.size() == 2);
    REQUIRE_TRUE(lfoScope.layers[0].scopeColor == synth::Color::Green);
    REQUIRE_TRUE(lfoScope.layers[1].scopeColor == synth::Color::Yellow);

    const auto requireVisibleCellColors = [&](std::size_t visibleCellCount,
                                              std::span<const synth::Color> expectedCellBaseColors) {
        const synth::ParameterManager::UIState& uiState = rig.UIState();
        for (std::size_t cellIx = 0; cellIx < visibleCellCount; ++cellIx) {
            const synth::Parameter::UIState& visibleCell = uiState.slots[0].cells[cellIx];
            const synth::ui::EncoderDrawState encoder =
                synth::ui::EncoderDrawStateFromParameter(visibleCell);
            REQUIRE_TRUE(encoder.baseColor == expectedCellBaseColors[cellIx]);
            REQUIRE_TRUE(encoder.voices.size() == 2);
            REQUIRE_TRUE(encoder.voices[0].indicatorColor == synth::Color::Cyan);
            REQUIRE_TRUE(encoder.voices[1].indicatorColor == synth::Color::Orange);
            REQUIRE_TRUE(encoder.modulatorColors.size() == 15);
            REQUIRE_TRUE(encoder.modulatorColors[0] == synth::Color::Cyan);
            REQUIRE_TRUE(encoder.modulatorColors[1] == synth::Color::Blue);
            REQUIRE_TRUE(encoder.modulatorColors[2] == synth::Color::Indigo);
            REQUIRE_TRUE(encoder.modulatorColors[3] == synth::Color::Orange);
            REQUIRE_TRUE(encoder.modulatorColors[4] == synth::Color::Cyan);
            REQUIRE_TRUE(encoder.modulatorColors[5] == synth::Color::Orange);
            REQUIRE_TRUE(encoder.modulatorColors[6] == synth::Color::Green);
            REQUIRE_TRUE(encoder.modulatorColors[11] == synth::Color::Yellow);
            REQUIRE_TRUE(encoder.modulatorColors[14] == synth::Color::White);
            REQUIRE_TRUE(encoder.gestureColors == std::vector<synth::Color>{synth::Color::Orange});
        }
    };
    requireVisibleCellColors(7, std::span<const synth::Color>(expectedBaseColors).first(7));

    rig.SelectBank(kSlotIx, kLfoBankIx);
    rig.RunBlocks(1);
    requireVisibleCellColors(10, std::span<const synth::Color>(expectedBaseColors).subspan(7));
}

TEST_CASE(miniapp_rig_initializes_headlessly_and_runs) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("initializes_headlessly_and_runs"));
    const float expectedDefaultAlpha = 0.1226942309f;  // one-pole 1 kHz cutoff at 48 kHz
    REQUIRE_NEAR(rig.Application().Group()->Config().processLiteAlpha, expectedDefaultAlpha, 0.000001f);
    REQUIRE_TRUE(rig.Application().Group()->Config().targetComputeIntervalSamples == 16);
    rig.RunBlocks(1);
    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(miniapp_rig_run_seconds_produces_finite_output) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("run_seconds_produces_finite_output"));
    rig.RunSeconds(0.1);
    REQUIRE_TRUE(!rig.SawNaN());
    REQUIRE_TRUE(!rig.Output().empty());
}

TEST_CASE(miniapp_rig_raising_volume_yields_nonzero_output_peak) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("raising_volume_yields_nonzero_output_peak"));

    // Volume defaults to 1.0 (see Modules.cpp), so peak should already be
    // nonzero after a short run; still exercise Turn on the production bus
    // to prove the volume encoder position actually reaches the parameter.
    rig.Turn(kSlotIx, kVolumePosition, 0.3f);
    rig.RunSeconds(0.1);

    REQUIRE_TRUE(!rig.SawNaN());
    REQUIRE_TRUE(rig.OutputPeak() > 0.0f);
}

TEST_CASE(miniapp_rig_lfo_bank_exposes_lfo_adsr_and_tempo_parameters) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(
        64, UseScratchRuntimeDataPaths("lfo_bank_exposes_lfo_adsr_and_tempo_parameters"));
    rig.RunBlocks(1);

    REQUIRE_TRUE(rig.Application().Parameters().size() == 17);
    const auto lfoIds = rig.Application().LfoParameterIds();
    synth::ParameterManager& manager = *rig.Application().Context()->parameterManager;
    synth::Parameter* const attack = manager.FindParameterByName("Attack");
    synth::Parameter* const decay = manager.FindParameterByName("Decay");
    synth::Parameter* const sustain = manager.FindParameterByName("Sustain");
    synth::Parameter* const release = manager.FindParameterByName("Release");
    synth::Parameter* const tempo = manager.FindParameterByName("Tempo");
    REQUIRE_TRUE(attack != nullptr && decay != nullptr && sustain != nullptr && release != nullptr && tempo != nullptr);
    const float beforeFrequency = rig.ParameterValue(lfoIds.frequency);
    const float beforeShape = rig.ParameterValue(lfoIds.shape);
    const float beforePhaseOffset = rig.ParameterValue(lfoIds.phaseOffset);
    const float beforeSkew = rig.ParameterValue(lfoIds.skew);
    const float beforeExponent = rig.ParameterValue(lfoIds.exponent);

    rig.SelectBank(kSlotIx, kLfoBankIx);
    rig.RunBlocks(1);

    const synth::Page* activePage = rig.Application().Context()->parameterManager->ActivePage();
    REQUIRE_TRUE(activePage != nullptr);
    REQUIRE_TRUE(activePage->name == "LFO");
    REQUIRE_TRUE(activePage->parameters.size() == 10);
    RequirePagePosition(*activePage, kLfoFrequencyPosition, lfoIds.frequency, "LFO Frequency");
    RequirePagePosition(*activePage, kLfoShapePosition, lfoIds.shape, "LFO Shape");
    RequirePagePosition(*activePage, kLfoPhaseOffsetPosition, lfoIds.phaseOffset, "LFO Phase Offset");
    RequirePagePosition(*activePage, kLfoSkewPosition, lfoIds.skew, "LFO Skew");
    RequirePagePosition(*activePage, kLfoExponentPosition, lfoIds.exponent, "LFO Exponent");
    RequirePagePosition(*activePage, kAttackPosition, attack->Id(), "Attack");
    RequirePagePosition(*activePage, kDecayPosition, decay->Id(), "Decay");
    RequirePagePosition(*activePage, kSustainPosition, sustain->Id(), "Sustain");
    RequirePagePosition(*activePage, kReleasePosition, release->Id(), "Release");
    RequirePagePosition(*activePage, kTempoPosition, tempo->Id(), "Tempo");

    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kLfoFrequencyPosition,
                        lfoIds.frequency, "LFO Frequency");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kLfoShapePosition,
                        lfoIds.shape, "LFO Shape");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kLfoPhaseOffsetPosition,
                        lfoIds.phaseOffset, "LFO Phase Offset");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kLfoSkewPosition,
                        lfoIds.skew, "LFO Skew");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kLfoExponentPosition,
                        lfoIds.exponent, "LFO Exponent");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kAttackPosition,
                        attack->Id(), "Attack");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kDecayPosition,
                        decay->Id(), "Decay");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kSustainPosition,
                        sustain->Id(), "Sustain");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kReleasePosition,
                        release->Id(), "Release");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), kTempoPosition,
                        tempo->Id(), "Tempo");
    for (std::size_t position = 10; position < 16; ++position) {
        RequireUnboundBankPosition(*rig.Application().Slot(), *rig.Application().LfoBank(), position);
    }

    rig.Turn(kSlotIx, kLfoFrequencyPosition, 0.10f);
    rig.Turn(kSlotIx, kLfoShapePosition, -0.10f);
    rig.Turn(kSlotIx, kLfoPhaseOffsetPosition, 0.20f);
    rig.Turn(kSlotIx, kLfoSkewPosition, -0.20f);
    rig.Turn(kSlotIx, kLfoExponentPosition, 0.15f);
    rig.RunBlocks(16);

    REQUIRE_NEAR(rig.ParameterValue(lfoIds.frequency), beforeFrequency + 0.10f, 1e-3f);
    REQUIRE_NEAR(rig.ParameterValue(lfoIds.shape), beforeShape - 0.10f, 1e-3f);
    REQUIRE_NEAR(rig.ParameterValue(lfoIds.phaseOffset), beforePhaseOffset + 0.20f, 1e-3f);
    REQUIRE_NEAR(rig.ParameterValue(lfoIds.skew), beforeSkew - 0.20f, 1e-3f);
    REQUIRE_NEAR(rig.ParameterValue(lfoIds.exponent), beforeExponent + 0.15f, 1e-3f);
    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(miniapp_rig_vco_bank_exposes_vco_and_filter_parameters) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("vco_bank_exposes_vco_and_filter_parameters"));
    rig.RunBlocks(1);

    REQUIRE_TRUE(rig.Application().Parameters().size() == 17);
    const auto vcoIds = rig.Application().VcoParameterIds();
    const auto filterIds = rig.Application().FilterParameterIds();

    const synth::Page* activePage = rig.Application().Context()->parameterManager->ActivePage();
    REQUIRE_TRUE(activePage != nullptr);
    REQUIRE_TRUE(activePage->name == "VCO");
    REQUIRE_TRUE(activePage->parameters.size() == 7);
    RequirePagePosition(*activePage, kTunePosition, vcoIds.tune, "Tune");
    RequirePagePosition(*activePage, kPhasePosition, vcoIds.phase, "Phase");
    RequirePagePosition(*activePage, kShapePosition, vcoIds.shape, "Shape");
    RequirePagePosition(*activePage, kVolumePosition, vcoIds.volume, "Volume");
    RequirePagePosition(*activePage, kFilterCutoffPosition, filterIds.cutoff, "Filter Cutoff");
    RequirePagePosition(*activePage, kFilterResonancePosition, filterIds.resonance, "Filter Resonance");
    RequirePagePosition(*activePage, kFilterBlendPosition, filterIds.blend, "Filter Blend");

    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kTunePosition,
                        vcoIds.tune, "Tune");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kPhasePosition,
                        vcoIds.phase, "Phase");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kShapePosition,
                        vcoIds.shape, "Shape");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kVolumePosition,
                        vcoIds.volume, "Volume");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kFilterCutoffPosition,
                        filterIds.cutoff, "Filter Cutoff");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kFilterResonancePosition,
                        filterIds.resonance, "Filter Resonance");
    RequireBankPosition(*rig.Application().Slot(), *rig.Application().VcoBank(), kFilterBlendPosition,
                        filterIds.blend, "Filter Blend");
    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(miniapp_rig_lfo_modulation_source_changes_from_module_processing) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("lfo_modulation_source_changes_from_module_processing"));
    std::vector<float> voice0Values;
    std::vector<float> voice1Values;

    for (int i = 0; i < 8; ++i) {
        rig.RunBlocks(8);
        voice0Values.push_back(rig.Application().Group()->GetModulators().Value(0, 6));
        voice1Values.push_back(rig.Application().Group()->GetModulators().Value(1, 6));
    }

    REQUIRE_TRUE(ValuesDifferMaterially(voice0Values, 1e-4f));
    REQUIRE_TRUE(ValuesDifferMaterially(voice1Values, 1e-4f));
    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(miniapp_rig_zero_volume_yields_silence_and_turning_up_restores_signal) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("zero_volume_yields_silence_and_turning_up_restores_signal"));

    // Drive Volume down to (near) zero via repeated turns on its bank
    // position, then confirm the output peak collapses -- proving the
    // production Turn(slot, position) path for kVolumePosition actually
    // reaches WavetableVcoModule<2>'s Volume parameter and audibly changes
    // the mixed output, not just that output happens to be nonzero already.
    for (int i = 0; i < 40; ++i) {
        rig.Turn(kSlotIx, kVolumePosition, -0.1f);
    }
    rig.RunBlocks(8);
    const OutputWindow quietWindow = CaptureSettledOutputWindow(rig, 10);
    const float quietPeak = OutputWindowPeak(quietWindow);
    REQUIRE_TRUE(quietPeak < 0.02f);

    for (int i = 0; i < 40; ++i) {
        rig.Turn(kSlotIx, kVolumePosition, 0.1f);
    }
    rig.RunBlocks(8);
    rig.RunSeconds(0.1);

    REQUIRE_TRUE(rig.OutputPeak() > quietPeak + 0.05f);
    REQUIRE_TRUE(!rig.SawNaN());
}

// These "Turn changes output" tests must not compare two free-running output
// windows captured at different times against the SAME rig: the VCOs are
// free-running oscillators, so phase alone drifts the waveform from one
// window to the next even when a Turn has zero effect on the audio path.
// That would let a broken parameter->audio wire pass the test for the wrong
// reason.
//
// Instead this leans on the engine's proven determinism (see
// rig_two_identical_runs_are_deterministic in rig_tests.cpp): build TWO
// rigs, script them through an IDENTICAL sequence of blocks/turns for N
// blocks, then apply the Turn under test to ONLY rig B, run BOTH rigs for
// the SAME further M blocks, and compare their captured output over that
// same final-M-block range. With no turn applied, two identically-scripted
// rigs produce bit-identical output (determinism), so this test's baseline
// sanity check asserts the pre-turn windows from A and B match; any material
// post-turn difference between A and B is then attributable ONLY to the
// turn -- phase drift cannot explain it, because both rigs drift identically
// from the same identical history.
TEST_CASE(miniapp_rig_tune_turn_changes_output) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigA(64, UseScratchRuntimeDataPaths("tune_turn_changes_output"));
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigB(64, UseScratchRuntimeDataPaths("tune_turn_changes_output_b"));

    rigA.RunBlocks(4);
    rigB.RunBlocks(4);

    const synth::ParameterId tuneIdA = rigA.Application().VcoParameterIds().tune;
    const synth::ParameterId tuneIdB = rigB.Application().VcoParameterIds().tune;
    const float beforeA = rigA.ParameterValue(tuneIdA);
    const float beforeB = rigB.ParameterValue(tuneIdB);

    // Baseline sanity: with both rigs scripted identically so far and no
    // turn applied yet, their settled output windows must be bit-identical
    // (determinism). This proves the twin-rig setup itself is apples-to-
    // apples before the turn is introduced.
    const OutputWindow baselineA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow baselineB = CaptureSettledOutputWindow(rigB, 8);
    REQUIRE_TRUE(AllSamplesFinite(baselineA));
    REQUIRE_TRUE(AllSamplesFinite(baselineB));
    // Rigs are deterministic, so baseline windows must be EXACTLY equal.
    RequireEqualWindowShapes(baselineA, baselineB, "miniapp_rig_tune_turn_changes_output baseline");
    REQUIRE_TRUE(baselineA.size() == baselineB.size());
    for (std::size_t i = 0; i < baselineA.size(); ++i) {
        REQUIRE_TRUE(baselineA[i].channels == baselineB[i].channels);  // bit-identical
    }

    // Apply the Turn under test to rig B only.
    rigB.Turn(kSlotIx, kTunePosition, 0.3f);

    rigA.RunBlocks(16);  // let the parameter slew settle before capturing
    rigB.RunBlocks(16);

    const OutputWindow afterA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow afterB = CaptureSettledOutputWindow(rigB, 8);

    // Primary assertion (the brief's actual requirement): Tune's Turn must
    // audibly change the OUTPUT signal, not just the tracked parameter.
    // Because rigA and rigB were scripted identically up to this point,
    // determinism guarantees any material difference here comes from the
    // turn, not from oscillator phase drift.
    REQUIRE_TRUE(AllSamplesFinite(afterA));
    REQUIRE_TRUE(AllSamplesFinite(afterB));
    REQUIRE_TRUE(OutputWindowsDifferMaterially(afterA, afterB, 1e-4f));

    // Secondary (parameter-value) checks, kept for regression coverage of
    // the production Turn(slot, position) -> parameter routing path.
    const float afterValueA = rigA.ParameterValue(tuneIdA);
    const float afterValueB = rigB.ParameterValue(tuneIdB);
    REQUIRE_NEAR(afterValueA, beforeA, 1e-3f);
    REQUIRE_TRUE(afterValueB != beforeB);
    REQUIRE_NEAR(afterValueB, beforeB + 0.3f, 1e-3f);
    REQUIRE_TRUE(!rigA.SawNaN());
    REQUIRE_TRUE(!rigB.SawNaN());
}

TEST_CASE(miniapp_rig_shape_turn_changes_output) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigA(64, UseScratchRuntimeDataPaths("shape_turn_changes_output"));
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigB(64, UseScratchRuntimeDataPaths("shape_turn_changes_output_b"));

    rigA.RunBlocks(4);
    rigB.RunBlocks(4);

    const synth::ParameterId shapeIdA = rigA.Application().VcoParameterIds().shape;
    const synth::ParameterId shapeIdB = rigB.Application().VcoParameterIds().shape;
    const float beforeA = rigA.ParameterValue(shapeIdA);
    const float beforeB = rigB.ParameterValue(shapeIdB);

    // Baseline sanity: with both rigs scripted identically so far and no
    // turn applied yet, their settled output windows must be bit-identical
    // (determinism). This proves the twin-rig setup itself is apples-to-
    // apples before the turn is introduced.
    const OutputWindow baselineA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow baselineB = CaptureSettledOutputWindow(rigB, 8);
    REQUIRE_TRUE(AllSamplesFinite(baselineA));
    REQUIRE_TRUE(AllSamplesFinite(baselineB));
    // Rigs are deterministic, so baseline windows must be EXACTLY equal.
    RequireEqualWindowShapes(baselineA, baselineB, "miniapp_rig_shape_turn_changes_output baseline");
    REQUIRE_TRUE(baselineA.size() == baselineB.size());
    for (std::size_t i = 0; i < baselineA.size(); ++i) {
        REQUIRE_TRUE(baselineA[i].channels == baselineB[i].channels);  // bit-identical
    }

    // Apply the Turn under test to rig B only.
    rigB.Turn(kSlotIx, kShapePosition, 0.4f);

    rigA.RunBlocks(16);  // let the parameter slew settle before capturing
    rigB.RunBlocks(16);

    const OutputWindow afterA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow afterB = CaptureSettledOutputWindow(rigB, 8);

    // Primary assertion (the brief's actual requirement): Shape's Turn must
    // audibly change the OUTPUT signal (waveshape), not just the tracked
    // parameter. Because rigA and rigB were scripted identically up to this
    // point, determinism guarantees any material difference here comes from
    // the turn, not from oscillator phase drift.
    REQUIRE_TRUE(AllSamplesFinite(afterA));
    REQUIRE_TRUE(AllSamplesFinite(afterB));
    REQUIRE_TRUE(OutputWindowsDifferMaterially(afterA, afterB, 1e-4f));

    // Secondary (parameter-value) checks, kept for regression coverage of
    // the production Turn(slot, position) -> parameter routing path.
    const float afterValueA = rigA.ParameterValue(shapeIdA);
    const float afterValueB = rigB.ParameterValue(shapeIdB);
    REQUIRE_NEAR(afterValueA, beforeA, 1e-3f);
    REQUIRE_TRUE(afterValueB != beforeB);
    REQUIRE_NEAR(afterValueB, beforeB + 0.4f, 1e-3f);
    REQUIRE_TRUE(!rigA.SawNaN());
    REQUIRE_TRUE(!rigB.SawNaN());
}

TEST_CASE(miniapp_rig_filter_blend_turn_changes_output) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigA(64, UseScratchRuntimeDataPaths("filter_blend_turn_changes_output"));
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rigB(64, UseScratchRuntimeDataPaths("filter_blend_turn_changes_output_b"));

    rigA.RunBlocks(4);
    rigB.RunBlocks(4);

    const synth::ParameterId blendIdA = rigA.Application().FilterParameterIds().blend;
    const synth::ParameterId blendIdB = rigB.Application().FilterParameterIds().blend;
    const float beforeA = rigA.ParameterValue(blendIdA);
    const float beforeB = rigB.ParameterValue(blendIdB);

    const OutputWindow baselineA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow baselineB = CaptureSettledOutputWindow(rigB, 8);
    REQUIRE_TRUE(AllSamplesFinite(baselineA));
    REQUIRE_TRUE(AllSamplesFinite(baselineB));
    RequireEqualWindowShapes(baselineA, baselineB, "miniapp_rig_filter_blend_turn_changes_output baseline");
    for (std::size_t i = 0; i < baselineA.size(); ++i) {
        REQUIRE_TRUE(baselineA[i].channels == baselineB[i].channels);
    }

    rigB.Turn(kSlotIx, kFilterBlendPosition, 1.0f);

    rigA.RunBlocks(32);
    rigB.RunBlocks(32);

    const OutputWindow afterA = CaptureSettledOutputWindow(rigA, 8);
    const OutputWindow afterB = CaptureSettledOutputWindow(rigB, 8);

    REQUIRE_TRUE(AllSamplesFinite(afterA));
    REQUIRE_TRUE(AllSamplesFinite(afterB));
    REQUIRE_TRUE(OutputWindowsDifferMaterially(afterA, afterB, 1e-4f));

    const float afterValueA = rigA.ParameterValue(blendIdA);
    const float afterValueB = rigB.ParameterValue(blendIdB);
    REQUIRE_NEAR(afterValueA, beforeA, 1e-3f);
    REQUIRE_TRUE(afterValueB != beforeB);
    REQUIRE_TRUE(afterValueB > beforeB + 0.5f);
    REQUIRE_TRUE(!rigA.SawNaN());
    REQUIRE_TRUE(!rigB.SawNaN());
}

TEST_CASE(miniapp_rig_patch_save_perturb_load_round_trip) {
    const synth::RuntimeDataPaths paths = UseScratchRuntimeDataPaths("patch_save_perturb_load_round_trip");

    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, paths);
    rig.RunBlocks(4);

    const synth::ParameterId tuneId = rig.Application().VcoParameterIds().tune;
    const synth::ParameterId shapeId = rig.Application().VcoParameterIds().shape;

    rig.Turn(kSlotIx, kTunePosition, 0.25f);
    rig.Turn(kSlotIx, kShapePosition, -0.15f);
    rig.RunBlocks(16);

    const float savedTune = rig.ParameterValue(tuneId);
    const float savedShape = rig.ParameterValue(shapeId);

    const std::filesystem::path patchDir = paths.patchesRoot / "Take1";
    REQUIRE_TRUE(rig.SavePatchAs(patchDir) == synth_rig::RigPatchStatus::Written);
    rig.Engine().EditInstrument([](synth::MidiInstrumentConfig& instrument) {
        REQUIRE_TRUE(instrument.RenameController(0, "runtime-edited"));
    });
    rig.Engine().SetAudioDeviceFromHost(synth::AudioDeviceState{
        .outputDeviceName = "Runtime Out",
        .inputDeviceName = "Runtime In",
    });

    // Perturb: move both parameters away from the saved values.
    rig.Turn(kSlotIx, kTunePosition, -0.4f);
    rig.Turn(kSlotIx, kShapePosition, 0.4f);
    rig.RunBlocks(16);

    REQUIRE_TRUE(std::fabs(rig.ParameterValue(tuneId) - savedTune) > 1e-3f);
    REQUIRE_TRUE(std::fabs(rig.ParameterValue(shapeId) - savedShape) > 1e-3f);

    REQUIRE_TRUE(rig.LoadPatch(patchDir) == synth_rig::RigPatchStatus::Ok);
    rig.RunBlocks(16);

    REQUIRE_NEAR(rig.ParameterValue(tuneId), savedTune, 1e-3f);
    REQUIRE_NEAR(rig.ParameterValue(shapeId), savedShape, 1e-3f);
    REQUIRE_TRUE(rig.Engine().InstrumentSnapshot().controllers.size() == 1);
    REQUIRE_TRUE(rig.Engine().InstrumentSnapshot().controllers[0].name == "runtime-edited");
    REQUIRE_TRUE(rig.Engine().AudioDeviceSnapshot().outputDeviceName == "Runtime Out");
    REQUIRE_TRUE(rig.Engine().AudioDeviceSnapshot().inputDeviceName == "Runtime In");
    REQUIRE_TRUE(!rig.SawNaN());

    std::filesystem::remove_all(paths.dataRoot);
}

TEST_CASE(miniapp_patch_browser_save_as_path_writes_patch_and_load_selection_reads_it) {
    const synth::RuntimeDataPaths paths = UseScratchRuntimeDataPaths("patch_browser_save_load");
    synth::PatchBrowser browser(paths.patchesRoot);

    const auto savePath = browser.ResolveSaveAsPath("Browser Patch");
    REQUIRE_TRUE(savePath.has_value());

    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, paths);
    rig.RunBlocks(4);

    const synth::ParameterId volumeId = rig.Application().VcoParameterIds().volume;
    rig.Turn(kSlotIx, kVolumePosition, -0.35f);
    rig.RunBlocks(16);
    const float savedVolume = rig.ParameterValue(volumeId);

    REQUIRE_TRUE(rig.SavePatchAs(*savePath) == synth_rig::RigPatchStatus::Written);
    REQUIRE_TRUE(std::filesystem::is_directory(*savePath));

    REQUIRE_TRUE(browser.Refresh());
    REQUIRE_TRUE(browser.Entries().size() == 1);
    REQUIRE_TRUE(browser.Entries()[0].name == "Browser Patch");
    browser.Select(0);

    rig.Turn(kSlotIx, kVolumePosition, 0.35f);
    rig.RunBlocks(16);
    REQUIRE_TRUE(std::fabs(rig.ParameterValue(volumeId) - savedVolume) > 1e-3f);

    const auto loadPath = browser.SelectedLoadPath();
    REQUIRE_TRUE(loadPath.has_value());
    REQUIRE_TRUE(rig.LoadPatch(*loadPath) == synth_rig::RigPatchStatus::Ok);
    rig.RunBlocks(16);
    REQUIRE_NEAR(rig.ParameterValue(volumeId), savedVolume, 1e-3f);

    std::filesystem::remove_all(paths.dataRoot);
}

// spm-45: post-Init the default instrument (Engine::LiveInstrument(), which
// Engine::Initialize() snapshots into DefaultInstrument() right after
// MiniAppCore::Init() returns -- see MiniAppCore.hpp's comment on the
// controller-seeding block) must contain EXACTLY ONE controller slot, named
// "wrldbldr", kind WrldBldr, built from the shared DefaultMidiInstrumentConfig().
// The profile deliberately carries all 16 encoder positions and 8 scene
// selector messages even when a specific app exposes fewer scenes or backing
// cells. Spot-check the encoder input mapping rather than comparing the whole
// config: turns live on channel 0, pushes on channel 1, and CCs map onto
// positions 1:1 (EncoderPositionToCC(position) == position for position < 16).
TEST_CASE(miniapp_rig_default_instrument_has_single_wrldbldr_controller) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("default_instrument_has_single_wrldbldr_controller"));

    // Pin the post-Init DEFAULT instrument (revert/new-patch restore value).
    const synth::MidiInstrumentConfig& defaultInstrument = rig.Engine().DefaultInstrument();
    REQUIRE_TRUE(defaultInstrument.controllers.size() == 1);

    // Assert the live instrument equals the default at startup.
    const synth::MidiInstrumentConfig& liveInstrument = rig.Engine().LiveInstrument();
    REQUIRE_TRUE(liveInstrument.controllers.size() == defaultInstrument.controllers.size());

    const synth::MidiControllerSlot& slot = defaultInstrument.controllers.front();
    REQUIRE_TRUE(slot.name == "wrldbldr");
    REQUIRE_TRUE(slot.kind == synth::MidiProfileKind::WrldBldr);

    // The default WRLD.Bldr profile maps all 16 physical encoders presented by
    // the MiniApp surface.
    constexpr std::size_t kVisibleEncoderCount = 16;
    synth::WrldBldrDefaultProfileOptions expectedOptions;
    const synth::MidiControllerProfileConfig expectedConfig = synth::WrldBldrDefaultProfileConfig(expectedOptions);

    REQUIRE_TRUE(slot.config.encoderInput.has_value());
    REQUIRE_TRUE(expectedConfig.encoderInput.has_value());
    REQUIRE_TRUE(slot.config.encoderInput->turns.size() == expectedConfig.encoderInput->turns.size());
    REQUIRE_TRUE(slot.config.encoderInput->pushes.size() == expectedConfig.encoderInput->pushes.size());
    REQUIRE_TRUE(slot.config.encoderInput->turns.size() == kVisibleEncoderCount);

    // Assert system association count. WrldBldrDefaultProfileConfig produces:
    // 3 (reset/random/random-mod modifiers) + sceneCount (8) + bankButtonCount (16)
    // + gestureSelectorCount (1) = 28
    REQUIRE_TRUE(slot.config.systemMessages.size() == expectedConfig.systemMessages.size());
    REQUIRE_TRUE(slot.config.systemMessages.size() == 28);

    // Encoder-mapping spot-checks (brief Step 1): turn channel 0, push
    // channel 1, CCs 0..(visibleEncoderCount-1) -> positions
    // 0..(visibleEncoderCount-1).
    for (std::size_t position = 0; position < kVisibleEncoderCount; ++position) {
        const auto turnIt = std::find_if(
            slot.config.encoderInput->turns.begin(), slot.config.encoderInput->turns.end(),
            [position](const synth::EncoderMidiMapping& mapping) { return mapping.position == position; });
        REQUIRE_TRUE(turnIt != slot.config.encoderInput->turns.end());
        REQUIRE_TRUE(turnIt->control.channel == 0);
        REQUIRE_TRUE(turnIt->control.cc == static_cast<std::uint8_t>(position));

        const auto pushIt = std::find_if(
            slot.config.encoderInput->pushes.begin(), slot.config.encoderInput->pushes.end(),
            [position](const synth::EncoderMidiMapping& mapping) { return mapping.position == position; });
        REQUIRE_TRUE(pushIt != slot.config.encoderInput->pushes.end());
        REQUIRE_TRUE(pushIt->control.channel == 1);
        REQUIRE_TRUE(pushIt->control.cc == static_cast<std::uint8_t>(position));
    }
}

TEST_CASE(miniapp_rig_no_nan_across_extended_run) {
    synth_rig::SynthRig<synth_miniapp::MiniAppCore> rig(64, UseScratchRuntimeDataPaths("no_nan_across_extended_run"));

    rig.Turn(kSlotIx, kTunePosition, 0.2f);
    rig.Turn(kSlotIx, kShapePosition, 0.3f);
    rig.Turn(kSlotIx, kPhasePosition, 0.5f);
    rig.Turn(kSlotIx, kVolumePosition, -0.2f);
    rig.RunSeconds(0.5);

    REQUIRE_TRUE(!rig.SawNaN());
}

TEST_CASE(miniapp_composes_the_standard_application_layout) {
    const synth::ui::NodeTree tree = BuildMiniAppTreeAt(900.0f, 560.0f);
    for (const char* suffix : {".title", ".body", ".visualizers", ".slot.upper", ".slot.lower",
                               ".encoders", ".bay"}) {
        REQUIRE_TRUE(FindNodeById(tree, std::string("miniapp") + suffix) != nullptr);
    }

    // The shared proportions, now resolved rather than hand-computed:
    // contentWidth = 900 - 2*16 = 868, 868 * 0.46 = 399.28 capped at 390, and
    // the encoder region takes what is left after one 14 gap, capped at 462.
    RequireNear(FindNodeById(tree, "miniapp.visualizers")->bounds.width, 390.0f, 0.01f,
                "visualizer stack width");
    RequireNear(FindNodeById(tree, "miniapp.encoders")->bounds.width, 462.0f, 0.01f,
                "encoder region width");
    RequireNear(FindNodeById(tree, "miniapp.title")->bounds.height, 30.0f, 0.01f, "title height");
    REQUIRE_TRUE(SurfaceBoundsOf(tree, "miniapp.visualizers").x <
                 SurfaceBoundsOf(tree, "miniapp.encoders").x);
    REQUIRE_TRUE(IsDescendantOf(tree, synth_miniapp::MiniAppNodeIds::kVcoScope, "miniapp.slot.upper"));
    REQUIRE_TRUE(IsDescendantOf(tree, synth_miniapp::MiniAppNodeIds::kLfoScope, "miniapp.slot.lower"));

    REQUIRE_TRUE(!SourceContains("projects/synth/apps/miniapp/MiniAppUiModel.hpp", "ScopeStackArea"));
    REQUIRE_TRUE(!SourceContains("projects/synth/apps/miniapp/MiniAppUiModel.hpp", "BoundsForIndex"));
    REQUIRE_TRUE(!SourceContains("projects/synth/apps/miniapp/MiniAppUiModel.hpp", "EncoderArea"));
}

TEST_CASE(miniapp_every_control_resolves_inside_the_widget_bay) {
    const synth::ui::NodeTree tree = BuildMiniAppTreeAt(900.0f, 560.0f);
    const std::vector<std::string> controls = {
        synth_miniapp::MiniAppNodeIds::kBankVco,       synth_miniapp::MiniAppNodeIds::kBankLfo,
        synth_miniapp::MiniAppNodeIds::kGestureToggle, synth_miniapp::MiniAppNodeIds::kReset,
        synth_miniapp::MiniAppNodeIds::kRandom,        synth_miniapp::MiniAppNodeIds::kRandomMod,
        synth_miniapp::MiniAppNodeIds::SceneButton(0), synth_miniapp::MiniAppNodeIds::SceneButton(1),
        synth_miniapp::MiniAppNodeIds::SceneButton(2), synth_miniapp::MiniAppNodeIds::kStart,
        synth_miniapp::MiniAppNodeIds::kStop,          synth_miniapp::MiniAppNodeIds::kGestureValue,
        synth_miniapp::MiniAppNodeIds::kSceneBlend,
    };
    for (const std::string& id : controls) {
        REQUIRE_TRUE(IsDescendantOf(tree, id, "miniapp.bay"));
    }
    REQUIRE_TRUE(NoTwoNodesOverlap(tree, controls));
    // Mini App now has a populated widget bay it did not have before, and its
    // visualizer stack no longer runs the full content height.
    REQUIRE_TRUE(FindNodeById(tree, "miniapp.bay")->bounds.height > 0.0f);
    REQUIRE_TRUE(SurfaceBoundsOf(tree, "miniapp.visualizers").y +
                     FindNodeById(tree, "miniapp.visualizers")->bounds.height <=
                 SurfaceBoundsOf(tree, "miniapp.bay").y);

    std::vector<std::string> encoders;
    for (std::size_t ix = 0; ix < synth_miniapp::EncoderGridLayout::kEncoderCount; ++ix) {
        encoders.push_back(synth_miniapp::MiniAppNodeIds::Encoder(ix));
        REQUIRE_TRUE(IsDescendantOf(tree, encoders.back(), "miniapp.encoders"));
    }
    REQUIRE_TRUE(NoTwoNodesOverlap(tree, encoders));
}

TEST_CASE(miniapp_regions_redistribute_at_a_different_root_extent) {
    const synth::ui::NodeTree narrow = BuildMiniAppTreeAt(700.0f, 560.0f);
    const synth::ui::NodeTree wide = BuildMiniAppTreeAt(1400.0f, 560.0f);
    // 700 - 2*16 = 668 content; 668 * 0.46 = 307.28 is below the 390 cap.
    RequireNear(FindNodeById(narrow, "miniapp.visualizers")->bounds.width, 307.28f, 0.01f,
                "narrow stack width");
    RequireNear(FindNodeById(wide, "miniapp.visualizers")->bounds.width, 390.0f, 0.01f,
                "wide stack width");
    REQUIRE_TRUE(FindNodeById(wide, "miniapp.encoders")->bounds.width >
                 FindNodeById(narrow, "miniapp.encoders")->bounds.width);
    const synth::ui::Node* wideScope = FindNodeById(wide, synth_miniapp::MiniAppNodeIds::kVcoScope);
    const synth::ui::Node* narrowScope = FindNodeById(narrow, synth_miniapp::MiniAppNodeIds::kVcoScope);
    REQUIRE_TRUE(wideScope->bounds.width > narrowScope->bounds.width);
}

int main() {
    int failed = 0;
    for (const auto& test : Registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": " << ex.what() << "\n";
        }
    }
    return failed == 0 ? 0 : 1;
}
