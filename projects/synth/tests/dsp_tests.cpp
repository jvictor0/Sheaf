#include "synth/DspAdsr.hpp"
#include "synth/DspBuffers.hpp"
#include "synth/DspConstant.hpp"
#include "synth/DspDegrade.hpp"
#include "synth/DspFilters.hpp"
#include "synth/DspMath.hpp"
#include "synth/DspMetering.hpp"
#include "synth/DspNoise.hpp"
#include "synth/DspNumbers.hpp"
#include "synth/DspOla.hpp"
#include "synth/DspOscillators.hpp"
#include "synth/DspPhasor2Tick.hpp"
#include "synth/DspRandomLfo.hpp"
#include "synth/DspResynthesis.hpp"
#include "synth/DspScope.hpp"
#include "synth/DspSpectral.hpp"
#include "synth/DspTransferFunction.hpp"
#include "synth/DspWavetable.hpp"
#include "synth/ParameterModulation.hpp"
#include "synth/StandardModulators.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth DSP tests must not see JUCE headers"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace allocation_probe {

std::atomic<bool> enabled{false};
std::atomic<std::size_t> count{0};

} // namespace allocation_probe

void* operator new(std::size_t size) {
    if (allocation_probe::enabled.load(std::memory_order_relaxed)) {
        allocation_probe::count.fetch_add(1, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

template<typename Processor>
concept HasPublicScopeColorStorage = requires(Processor processor) {
    processor.m_scopeColor;
};

template<typename Processor>
concept HasProcessMethod = requires(Processor& processor) { processor.Process(); };

template<typename Snapshot>
concept HasRecordedScopeStorage = requires(Snapshot snapshot) {
    snapshot.scope;
};

template<typename Snapshot>
concept HasRecordedScopeBufferStorage = requires(Snapshot snapshot) {
    snapshot.scopeBuffer;
};

template<typename Snapshot>
concept HasRecordedHistoryStorage = requires(Snapshot snapshot) {
    snapshot.history;
};

template<typename Snapshot>
concept HasRecordedSampleStorage = requires(Snapshot snapshot) {
    snapshot.samples;
};

static_assert(!HasPublicScopeColorStorage<synth::DefaultWavetableVco>);
static_assert(!HasPublicScopeColorStorage<synth::BasicLFOProcessor>);
static_assert(!std::is_copy_constructible_v<synth::NoiseModulatorProcessor>);
static_assert(!std::is_copy_assignable_v<synth::NoiseModulatorProcessor>);
static_assert(!std::is_move_constructible_v<synth::NoiseModulatorProcessor>);
static_assert(!std::is_move_assignable_v<synth::NoiseModulatorProcessor>);
static_assert(noexcept(std::declval<synth::NoiseModulatorProcessor&>().Process()));
static_assert(!std::is_copy_constructible_v<synth::ConstantModulatorProcessor>);
static_assert(!std::is_copy_assignable_v<synth::ConstantModulatorProcessor>);
static_assert(!std::is_move_constructible_v<synth::ConstantModulatorProcessor>);
static_assert(!std::is_move_assignable_v<synth::ConstantModulatorProcessor>);
static_assert(!HasProcessMethod<synth::ConstantModulatorProcessor>);
static_assert(!std::is_copy_constructible_v<synth::StandardModulators<1>>);
static_assert(!std::is_copy_assignable_v<synth::StandardModulators<1>>);
static_assert(!std::is_move_constructible_v<synth::StandardModulators<1>>);
static_assert(!std::is_move_assignable_v<synth::StandardModulators<1>>);
static_assert(!std::is_copy_constructible_v<synth::StandardModulators<2>>);
static_assert(!std::is_copy_assignable_v<synth::StandardModulators<2>>);
static_assert(!std::is_move_constructible_v<synth::StandardModulators<2>>);
static_assert(!std::is_move_assignable_v<synth::StandardModulators<2>>);
static_assert(!std::is_copy_constructible_v<synth::StandardModulators<4>>);
static_assert(!std::is_copy_assignable_v<synth::StandardModulators<4>>);
static_assert(!std::is_move_constructible_v<synth::StandardModulators<4>>);
static_assert(!std::is_move_assignable_v<synth::StandardModulators<4>>);
static_assert(noexcept(std::declval<synth::Phasor2Tick&>().Prime({0.0, 24})));
static_assert(noexcept(std::declval<synth::Phasor2Tick&>().Process({0.0, 24})));
static_assert(noexcept(std::declval<const synth::Phasor2Tick&>().Tick()));
static_assert(std::is_trivially_copyable_v<synth::Phasor2Tick>);

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

void RequireNear(double actual, double expected, double tolerance, const char* expr) {
    if (std::fabs(actual - expected) > tolerance) {
        std::ostringstream oss;
        oss << expr << " expected " << expected << " got " << actual;
        throw std::runtime_error(oss.str());
    }
}

void RequireComplexNear(
    std::complex<float> actual,
    std::complex<float> expected,
    float tolerance,
    const char* expr) {
    RequireNear(actual.real(), expected.real(), tolerance, expr);
    RequireNear(actual.imag(), expected.imag(), tolerance, expr);
}

void RequireFinite(float actual, const char* expr) {
    if (!std::isfinite(actual)) {
        std::ostringstream oss;
        oss << expr << " expected finite value got " << actual;
        throw std::runtime_error(oss.str());
    }
}

#define REQUIRE_NEAR(actual, expected, tolerance) RequireNear((actual), (expected), (tolerance), #actual)

struct ScriptedRandomLfoDrawSource {
    enum class EventKind {
        Normal,
        Uniform,
    };

    struct NormalCall {
        double mean = 0.0;
        double sigma = 0.0;
    };

    std::vector<double> normalResults;
    std::vector<float> uniformResults;
    std::vector<NormalCall> normalCalls;
    std::vector<EventKind> events;
    std::size_t nextNormalResult = 0;
    std::size_t nextUniformResult = 0;

    double Normal(double mean, double sigma) {
        events.push_back(EventKind::Normal);
        normalCalls.push_back({mean, sigma});
        if (nextNormalResult >= normalResults.size()) {
            throw std::runtime_error("scripted normal draw exhausted");
        }
        return normalResults[nextNormalResult++];
    }

    float Uniform01() {
        events.push_back(EventKind::Uniform);
        if (nextUniformResult >= uniformResults.size()) {
            throw std::runtime_error("scripted uniform draw exhausted");
        }
        return uniformResults[nextUniformResult++];
    }
};

struct FixedRandomLfoDrawSource {
    double Normal(double mean, double) {
        return mean;
    }

    float Uniform01() {
        return 0.5f;
    }
};

template<std::size_t VoiceCount>
synth::ParameterGroup& MakeStandardModulatorGroup(
    synth::ParameterManager& manager,
    std::size_t groupVoiceCount = VoiceCount,
    std::size_t modulatorCount = 15) {
    return manager.CreateGroup({
        .numVoices = groupVoiceCount,
        .numModulators = modulatorCount,
        .numScenes = 1,
        .maxParameters = 1,
    });
}

void RequireDisconnected(const synth::ParameterGroup& group) {
    for (const auto& metadata : group.GetModulators().Metadata()) {
        REQUIRE_TRUE(!metadata.connected);
        REQUIRE_TRUE(metadata.visualizer == nullptr);
        REQUIRE_TRUE(metadata.name.empty());
        REQUIRE_TRUE(metadata.shortName.empty());
    }
}

template<typename Exception, typename Callable>
void RequireThrows(Callable&& callable) {
    bool threw = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

template<std::size_t VoiceCount>
void RequireStandardModulatorOwnedShape() {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<VoiceCount>(manager);
    synth::StandardModulators<VoiceCount> bundle(group);
    static_assert(std::is_same_v<
                  std::remove_reference_t<decltype(bundle.RandomProcessor(0))>,
                  synth::GangedRandomLfoProcessor<VoiceCount>>);

    REQUIRE_TRUE(&bundle.TargetGroup() == &group);
    REQUIRE_TRUE(bundle.NoiseProcessor().VoiceCount() == VoiceCount);
    REQUIRE_TRUE(bundle.ConstantProcessor().VoiceCount() == VoiceCount);
    std::array<const float*, 4> outputAddresses{};
    std::array<const synth::ui::Visualizer*, 6> visualizerAddresses{};
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomOutputRow(random).size() == VoiceCount);
        REQUIRE_TRUE(bundle.RandomPointerRow(random).size() == VoiceCount);
        outputAddresses[random] = bundle.RandomOutputRow(random).data();
        for (std::size_t voice = 0; voice < VoiceCount; ++voice) {
            REQUIRE_TRUE(bundle.RandomPointerRow(random)[voice] ==
                         &bundle.RandomOutputRow(random)[voice]);
            (void)bundle.RandomProcessor(random).Output(voice);
        }
        visualizerAddresses[random] = &bundle.RandomVisualizer(random);
    }
    visualizerAddresses[4] = &bundle.ConstantVisualizer();
    visualizerAddresses[5] = &bundle.NoiseVisualizer();
    for (std::size_t left = 0; left < visualizerAddresses.size(); ++left) {
        for (std::size_t right = left + 1; right < visualizerAddresses.size(); ++right) {
            REQUIRE_TRUE(visualizerAddresses[left] != visualizerAddresses[right]);
        }
    }

    bundle.Register();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomOutputRow(random).data() == outputAddresses[random]);
        REQUIRE_TRUE(&bundle.RandomVisualizer(random) == visualizerAddresses[random]);
    }
    REQUIRE_TRUE(&bundle.ConstantVisualizer() == visualizerAddresses[4]);
    REQUIRE_TRUE(&bundle.NoiseVisualizer() == visualizerAddresses[5]);
}

} // namespace

TEST_CASE(phasor2tick_priming_and_same_cell_processing_are_silent) {
    synth::Phasor2Tick detector;

    REQUIRE_TRUE(detector.Prime({1.25, 4}));
    REQUIRE_TRUE(detector.IsPrimed());
    REQUIRE_TRUE(!detector.Tick());
    REQUIRE_TRUE(!detector.Process({1.25, 4}));
    REQUIRE_TRUE(!detector.Process({1.499999, 4}));
    REQUIRE_TRUE(!detector.Tick());
}

TEST_CASE(phasor2tick_emits_exactly_when_the_floored_cell_changes) {
    synth::Phasor2Tick detector;
    REQUIRE_TRUE(detector.Prime({0.999, 24}));

    REQUIRE_TRUE(detector.Process({1.0, 24}));
    REQUIRE_TRUE(detector.Tick());
    REQUIRE_TRUE(!detector.Process({1.01, 24}));
    REQUIRE_TRUE(!detector.Tick());
}

TEST_CASE(phasor2tick_detects_backward_time_and_multi_cell_jumps_once_per_call) {
    synth::Phasor2Tick detector;
    REQUIRE_TRUE(detector.Prime({2.0, 8}));

    REQUIRE_TRUE(detector.Process({1.99, 8}));
    REQUIRE_TRUE(!detector.Process({1.98, 8}));
    REQUIRE_TRUE(detector.Process({4.0, 8}));
    REQUIRE_TRUE(!detector.Process({4.01, 8}));
}

TEST_CASE(phasor2tick_rejects_invalid_inputs_without_corrupting_its_cell) {
    synth::Phasor2Tick detector;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();

    REQUIRE_TRUE(!detector.Prime({nan, 24}));
    REQUIRE_TRUE(!detector.Prime({0.0, 0}));
    REQUIRE_TRUE(!detector.Prime({0.0, -1}));
    REQUIRE_TRUE(!detector.IsPrimed());

    REQUIRE_TRUE(detector.Prime({0.25, 24}));
    REQUIRE_TRUE(!detector.Process({infinity, 24}));
    REQUIRE_TRUE(!detector.Process({0.25, 0}));
    REQUIRE_TRUE(!detector.Process({std::numeric_limits<double>::max(), 24}));
    REQUIRE_TRUE(!detector.Tick());
    REQUIRE_TRUE(!detector.Process({0.26, 24}));
    REQUIRE_TRUE(detector.PreviousCell() == 6.0);
}

TEST_CASE(phasor2tick_first_valid_process_silently_primes) {
    synth::Phasor2Tick detector;

    REQUIRE_TRUE(!detector.Process({-0.25, 4}));
    REQUIRE_TRUE(detector.IsPrimed());
    REQUIRE_TRUE(detector.PreviousCell() == -1.0);
}

TEST_CASE(phasor2tick_processing_performs_no_dynamic_allocation) {
    synth::Phasor2Tick detector;
    REQUIRE_TRUE(detector.Prime({0.0, 960}));

    allocation_probe::count.store(0, std::memory_order_relaxed);
    allocation_probe::enabled.store(true, std::memory_order_release);
    for (int index = 1; index <= 10000; ++index) {
        (void)detector.Process({static_cast<double>(index) / 48000.0, 960});
    }
    allocation_probe::enabled.store(false, std::memory_order_release);

    REQUIRE_TRUE(allocation_probe::count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE(standard_modulators_defaults_match_min16_contract) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<4>(manager);
    synth::StandardModulators<4> bundle(group);
    const auto& config = std::as_const(bundle).Config();

    REQUIRE_TRUE((config.randomIndexes == std::array<std::size_t, 4>{0, 1, 2, 3}));
    REQUIRE_TRUE(config.constantIndex == 11);
    REQUIRE_TRUE(config.noiseIndex == 14);

    const std::array<std::string, 4> expectedNames{
        "Random 500 ms", "Random 2 s", "Random 6 s", "Random 16 s"};
    const std::array<std::string, 4> expectedShortNames{
        "Rnd .5", "Rnd 2", "Rnd 6", "Rnd 16"};
    const std::array<synth::Color, 4> expectedSourceColors{
        synth::Color::Cyan, synth::Color::Blue, synth::Color::Indigo, synth::Color::Orange};
    const std::array<synth::Color, 4> expectedVoiceColors{
        synth::Color::Cyan, synth::Color::Orange, synth::Color::Green, synth::Color::Yellow};
    const std::array<double, 4> waitingMeans{0.5, 2.0, 6.0, 16.0};
    const std::array<float, 4> targetSigmas{0.1f, 0.3f, 0.2f, 0.1f};

    REQUIRE_TRUE(config.randomVoiceColors.size() == expectedVoiceColors.size());
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(config.randomMetadata[random].name == expectedNames[random]);
        REQUIRE_TRUE(config.randomMetadata[random].shortName == expectedShortNames[random]);
        REQUIRE_TRUE(config.randomMetadata[random].sourceColor == expectedSourceColors[random]);
        REQUIRE_TRUE(config.randomMetadata[random].visualizer == nullptr);
        REQUIRE_TRUE(!config.randomMetadata[random].connected);
        REQUIRE_TRUE(config.randomVoiceColors[random] == expectedVoiceColors[random]);

        const double waitingMean = waitingMeans[random];
        const auto& input = config.randomInputs[random];
        REQUIRE_NEAR(input.waiting.muSeconds, waitingMean, 1.0e-12);
        REQUIRE_NEAR(input.waiting.sigmaSeconds, 0.3 * waitingMean, 1.0e-12);
        REQUIRE_NEAR(input.waiting.internalSigmaHz, 0.2 / waitingMean, 1.0e-12);
        REQUIRE_NEAR(input.moving.muSeconds, waitingMean / 2.0, 1.0e-12);
        REQUIRE_NEAR(input.moving.sigmaSeconds, 0.15 * waitingMean, 1.0e-12);
        REQUIRE_NEAR(input.moving.internalSigmaHz, 0.4 / waitingMean, 1.0e-12);
        REQUIRE_NEAR(input.targetInternalSigma, targetSigmas[random], 1.0e-7);
    }

    REQUIRE_TRUE(config.constantMetadata.name == "Constant");
    REQUIRE_TRUE(config.constantMetadata.shortName == "Const");
    REQUIRE_TRUE(config.constantMetadata.sourceColor == synth::Color::Yellow);
    REQUIRE_TRUE(config.constantMetadata.visualizer == nullptr);
    REQUIRE_TRUE(!config.constantMetadata.connected);
    REQUIRE_TRUE(config.noiseMetadata.name == "Noise");
    REQUIRE_TRUE(config.noiseMetadata.shortName == "Noise");
    REQUIRE_TRUE(config.noiseMetadata.sourceColor == synth::Color::White);
    REQUIRE_TRUE(config.noiseMetadata.visualizer == nullptr);
    REQUIRE_TRUE(!config.noiseMetadata.connected);

    bundle.Register();
    for (std::size_t random = 0; random < 4; ++random) {
        for (std::size_t voice = 0; voice < 4; ++voice) {
            REQUIRE_TRUE(bundle.RandomProcessor(random).VoiceColor(voice) == expectedVoiceColors[voice]);
        }
    }

    synth::ParameterManager monoManager;
    auto& monoGroup = MakeStandardModulatorGroup<1>(monoManager);
    synth::StandardModulators<1> mono(monoGroup);
    REQUIRE_TRUE(std::as_const(mono).Config().randomVoiceColors ==
                 std::vector<synth::Color>{synth::Color::Cyan});
    mono.Register();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(mono.RandomProcessor(random).VoiceColor(0) == synth::Color::Cyan);
    }

    synth::ParameterManager stereoManager;
    auto& stereoGroup = MakeStandardModulatorGroup<2>(stereoManager);
    synth::StandardModulators<2> stereo(stereoGroup);
    REQUIRE_TRUE(std::as_const(stereo).Config().randomVoiceColors ==
                 (std::vector<synth::Color>{synth::Color::Cyan, synth::Color::Orange}));
    stereo.Register();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(stereo.RandomProcessor(random).VoiceColor(0) == synth::Color::Cyan);
        REQUIRE_TRUE(stereo.RandomProcessor(random).VoiceColor(1) == synth::Color::Orange);
    }
}

TEST_CASE(standard_modulators_owns_address_stable_source_and_visualizer_storage) {
    RequireStandardModulatorOwnedShape<1>();
    RequireStandardModulatorOwnedShape<4>();

    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);

    REQUIRE_TRUE(&bundle.TargetGroup() == &group);
    REQUIRE_TRUE(bundle.NoiseProcessor().VoiceCount() == 2);
    REQUIRE_TRUE(bundle.ConstantProcessor().VoiceCount() == 2);

    std::array<const float*, 4> outputAddresses{};
    std::array<const synth::ui::Visualizer*, 6> visualizerAddresses{};
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomOutputRow(random).size() == 2);
        REQUIRE_TRUE(bundle.RandomPointerRow(random).size() == 2);
        outputAddresses[random] = bundle.RandomOutputRow(random).data();
        for (std::size_t voice = 0; voice < 2; ++voice) {
            REQUIRE_TRUE(bundle.RandomPointerRow(random)[voice] ==
                         &bundle.RandomOutputRow(random)[voice]);
            (void)bundle.RandomProcessor(random).Output(voice);
        }
        visualizerAddresses[random] = &bundle.RandomVisualizer(random);
    }
    visualizerAddresses[4] = &bundle.ConstantVisualizer();
    visualizerAddresses[5] = &bundle.NoiseVisualizer();
    for (std::size_t left = 0; left < visualizerAddresses.size(); ++left) {
        for (std::size_t right = left + 1; right < visualizerAddresses.size(); ++right) {
            REQUIRE_TRUE(visualizerAddresses[left] != visualizerAddresses[right]);
        }
    }

    bundle.Register();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomOutputRow(random).data() == outputAddresses[random]);
        REQUIRE_TRUE(&bundle.RandomVisualizer(random) == visualizerAddresses[random]);
        const auto& metadata = group.GetModulators().Metadata(random);
        REQUIRE_TRUE(metadata.visualizer == visualizerAddresses[random]);
        REQUIRE_TRUE(metadata.connected);
        for (std::size_t voice = 0; voice < 2; ++voice) {
            bundle.RandomOutputRow(random)[voice] =
                static_cast<float>(10 * random + voice + 1) / 100.0f;
        }
    }
    REQUIRE_TRUE(group.GetModulators().Metadata(11).visualizer == visualizerAddresses[4]);
    REQUIRE_TRUE(group.GetModulators().Metadata(14).visualizer == visualizerAddresses[5]);
    bundle.NoiseProcessor().Process();
    group.UpdateModValues();
    for (std::size_t random = 0; random < 4; ++random) {
        for (std::size_t voice = 0; voice < 2; ++voice) {
            REQUIRE_TRUE(group.GetModulators().Value(voice, random) ==
                         bundle.RandomOutputRow(random)[voice]);
        }
    }
    for (std::size_t voice = 0; voice < 2; ++voice) {
        REQUIRE_TRUE(group.GetModulators().Value(voice, 11) ==
                     bundle.ConstantProcessor().Output(voice));
        REQUIRE_TRUE(group.GetModulators().Value(voice, 14) ==
                     bundle.NoiseProcessor().Output(voice));
    }
}

TEST_CASE(standard_modulators_pre_registration_overrides_are_registered) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);
    auto& config = bundle.Config();
    config.randomIndexes = {4, 5, 6, 7};
    config.constantIndex = 12;
    config.noiseIndex = 13;
    const std::array<synth::Color, 4> sourceColors{
        synth::Color::Red, synth::Color::Green, synth::Color::Yellow, synth::Color::White};
    for (std::size_t random = 0; random < 4; ++random) {
        config.randomMetadata[random].name = "Custom Random " + std::to_string(random);
        config.randomMetadata[random].shortName = "CR" + std::to_string(random);
        config.randomMetadata[random].sourceColor = sourceColors[random];
    }
    config.constantMetadata = {
        .name = "Custom Constant",
        .shortName = "CC",
        .sourceColor = synth::Color::Indigo,
    };
    config.noiseMetadata = {
        .name = "Custom Noise",
        .shortName = "CN",
        .sourceColor = synth::Color::Orange,
    };
    config.randomInputs[2].waiting.muSeconds = 9.0;
    config.randomInputs[2].waiting.sigmaSeconds = 0.9;
    config.randomInputs[2].waiting.internalSigmaHz = 1.0 / 90.0;
    config.randomInputs[2].moving.muSeconds = 4.5;
    config.randomInputs[2].moving.sigmaSeconds = 0.45;
    config.randomInputs[2].moving.internalSigmaHz = 1.0 / 45.0;
    config.randomInputs[2].targetInternalSigma = 0.25f;
    config.randomVoiceColors = {synth::Color::Blue, synth::Color::Red};
    config.randomMetadata[0].visualizer = &bundle.NoiseVisualizer();
    config.randomMetadata[0].connected = true;
    config.constantMetadata.visualizer = &bundle.NoiseVisualizer();
    config.constantMetadata.connected = true;
    config.noiseMetadata.visualizer = &bundle.ConstantVisualizer();
    config.noiseMetadata.connected = true;

    bundle.Register();

    REQUIRE_TRUE(bundle.IsRegistered());
    const auto& frozen = std::as_const(bundle).Config();
    REQUIRE_NEAR(std::as_const(bundle).RandomInput(2).waiting.muSeconds, 9.0, 1.0e-12);
    REQUIRE_NEAR(std::as_const(bundle).RandomInput(2).moving.muSeconds, 4.5, 1.0e-12);
    REQUIRE_NEAR(std::as_const(bundle).RandomInput(2).targetInternalSigma, 0.25, 1.0e-7);
    for (std::size_t random = 0; random < 4; ++random) {
        const auto& metadata = group.GetModulators().Metadata(frozen.randomIndexes[random]);
        REQUIRE_TRUE(metadata.name == frozen.randomMetadata[random].name);
        REQUIRE_TRUE(metadata.shortName == frozen.randomMetadata[random].shortName);
        REQUIRE_TRUE(metadata.sourceColor == frozen.randomMetadata[random].sourceColor);
        REQUIRE_TRUE(metadata.visualizer == &bundle.RandomVisualizer(random));
        REQUIRE_TRUE(metadata.connected);
        REQUIRE_TRUE(bundle.RandomProcessor(random).VoiceColor(0) == synth::Color::Blue);
        REQUIRE_TRUE(bundle.RandomProcessor(random).VoiceColor(1) == synth::Color::Red);
    }
    const auto& constantMetadata = group.GetModulators().Metadata(frozen.constantIndex);
    REQUIRE_TRUE(constantMetadata.name == "Custom Constant");
    REQUIRE_TRUE(constantMetadata.shortName == "CC");
    REQUIRE_TRUE(constantMetadata.sourceColor == synth::Color::Indigo);
    REQUIRE_TRUE(constantMetadata.visualizer == &bundle.ConstantVisualizer());
    REQUIRE_TRUE(constantMetadata.connected);
    const auto& noiseMetadata = group.GetModulators().Metadata(frozen.noiseIndex);
    REQUIRE_TRUE(noiseMetadata.name == "Custom Noise");
    REQUIRE_TRUE(noiseMetadata.shortName == "CN");
    REQUIRE_TRUE(noiseMetadata.sourceColor == synth::Color::Orange);
    REQUIRE_TRUE(noiseMetadata.visualizer == &bundle.NoiseVisualizer());
    REQUIRE_TRUE(noiseMetadata.connected);
    REQUIRE_TRUE(frozen.randomMetadata[0].visualizer == &bundle.NoiseVisualizer());
    REQUIRE_TRUE(frozen.constantMetadata.visualizer == &bundle.NoiseVisualizer());
    REQUIRE_TRUE(frozen.noiseMetadata.visualizer == &bundle.ConstantVisualizer());
    for (const std::size_t disconnected : std::array<std::size_t, 6>{0, 1, 2, 3, 11, 14}) {
        REQUIRE_TRUE(!group.GetModulators().Metadata(disconnected).connected);
    }
}

TEST_CASE(standard_modulators_configuration_freezes_after_registration) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);
    bundle.Register();
    const auto* randomVisualizer = group.GetModulators().Metadata(0).visualizer;

    RequireThrows<std::logic_error>([&] { (void)bundle.Config(); });

    REQUIRE_TRUE(std::as_const(bundle).Config().randomIndexes[0] == 0);
    REQUIRE_TRUE(group.GetModulators().Metadata(0).visualizer == randomVisualizer);
    REQUIRE_TRUE(group.GetModulators().Metadata(0).connected);
}

TEST_CASE(standard_modulators_rejects_each_out_of_range_active_index_atomically) {
    for (std::size_t activeSource = 0; activeSource < 6; ++activeSource) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager);
        synth::StandardModulators<2> bundle(group);
        if (activeSource < 4) {
            bundle.Config().randomIndexes[activeSource] = 15;
        } else if (activeSource == 4) {
            bundle.Config().constantIndex = 15;
        } else {
            bundle.Config().noiseIndex = 15;
        }

        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        REQUIRE_TRUE(!bundle.IsRegistered());
        RequireDisconnected(group);
    }
}

TEST_CASE(standard_modulators_rejects_each_duplicate_active_index_atomically) {
    for (std::size_t collidingSource = 1; collidingSource < 6; ++collidingSource) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager);
        synth::StandardModulators<2> bundle(group);
        if (collidingSource < 4) {
            bundle.Config().randomIndexes[collidingSource] = bundle.Config().randomIndexes[0];
        } else if (collidingSource == 4) {
            bundle.Config().constantIndex = bundle.Config().randomIndexes[0];
        } else {
            bundle.Config().noiseIndex = bundle.Config().randomIndexes[0];
        }

        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        REQUIRE_TRUE(!bundle.IsRegistered());
        RequireDisconnected(group);
    }
}

TEST_CASE(standard_modulators_rejects_invalid_random_timing_atomically) {
    for (std::size_t random = 0; random < 4; ++random) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager);
        synth::StandardModulators<2> bundle(group);
        bundle.Config().randomInputs[random].waiting.sigmaSeconds = -0.01;
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }

    for (std::size_t invalidField = 0; invalidField < 7; ++invalidField) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager);
        synth::StandardModulators<2> bundle(group);
        auto& input = bundle.Config().randomInputs[0];
        switch (invalidField) {
        case 0: input.waiting.muSeconds = std::numeric_limits<double>::quiet_NaN(); break;
        case 1: input.waiting.sigmaSeconds = std::numeric_limits<double>::infinity(); break;
        case 2: input.waiting.internalSigmaHz = -0.1; break;
        case 3: input.moving.muSeconds = std::numeric_limits<double>::quiet_NaN(); break;
        case 4: input.moving.sigmaSeconds = -0.1; break;
        case 5: input.moving.internalSigmaHz = std::numeric_limits<double>::infinity(); break;
        case 6: input.targetInternalSigma = -0.1f; break;
        }
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
}

TEST_CASE(standard_modulators_rejects_empty_active_metadata_atomically) {
    for (std::size_t activeSource = 0; activeSource < 6; ++activeSource) {
        for (bool clearShortName : {false, true}) {
            synth::ParameterManager manager;
            auto& group = MakeStandardModulatorGroup<2>(manager);
            synth::StandardModulators<2> bundle(group);
            auto clearField = [clearShortName](synth::ModulatorMetadata& metadata) {
                (clearShortName ? metadata.shortName : metadata.name).clear();
            };
            if (activeSource < 4) {
                clearField(bundle.Config().randomMetadata[activeSource]);
            } else if (activeSource == 4) {
                clearField(bundle.Config().constantMetadata);
            } else {
                clearField(bundle.Config().noiseMetadata);
            }
            RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
            RequireDisconnected(group);
        }
    }
}

TEST_CASE(standard_modulators_rejects_wrong_voice_palette_size_atomically) {
    {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<1>(manager);
        synth::StandardModulators<1> bundle(group);
        bundle.Config().randomVoiceColors.clear();
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
    for (std::size_t size : {1u, 3u}) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager);
        synth::StandardModulators<2> bundle(group);
        bundle.Config().randomVoiceColors.resize(size, synth::Color::Red);
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
    for (std::size_t size : {3u, 5u}) {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<4>(manager);
        synth::StandardModulators<4> bundle(group);
        bundle.Config().randomVoiceColors.resize(size, synth::Color::Red);
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
}

TEST_CASE(standard_modulators_rejects_mismatched_group_shape_atomically) {
    {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager, 1, 15);
        synth::StandardModulators<2> bundle(group);
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
    {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager, 2, 14);
        synth::StandardModulators<2> bundle(group);
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
    {
        synth::ParameterManager manager;
        auto& group = MakeStandardModulatorGroup<2>(manager, 2, 16);
        synth::StandardModulators<2> bundle(group);
        RequireThrows<std::invalid_argument>([&] { bundle.Register(); });
        RequireDisconnected(group);
    }
}

TEST_CASE(standard_modulators_mono_omits_constant_and_ignores_constant_collision) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<1>(manager);
    synth::StandardModulators<1> bundle(group);
    bundle.Config().constantIndex = bundle.Config().randomIndexes[0];
    bundle.Config().constantMetadata.name.clear();
    bundle.Config().constantMetadata.shortName.clear();

    bundle.Register();

    for (std::size_t modulator = 0; modulator < 15; ++modulator) {
        const bool shouldBeConnected = modulator < 4 || modulator == 14;
        REQUIRE_TRUE(group.GetModulators().Metadata(modulator).connected == shouldBeConnected);
        if (!shouldBeConnected) {
            REQUIRE_TRUE(group.GetModulators().Metadata(modulator).visualizer == nullptr);
        }
    }

    synth::ParameterManager outOfRangeManager;
    auto& outOfRangeGroup = MakeStandardModulatorGroup<1>(outOfRangeManager);
    synth::StandardModulators<1> outOfRangeBundle(outOfRangeGroup);
    outOfRangeBundle.Config().constantIndex = 99;
    outOfRangeBundle.Register();
    REQUIRE_TRUE(outOfRangeBundle.IsRegistered());
    REQUIRE_TRUE(!outOfRangeGroup.GetModulators().Metadata(11).connected);
}

TEST_CASE(standard_modulators_rejects_double_registration_without_mutation) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);
    bundle.Register();
    const auto metadataBefore = group.GetModulators().Metadata(0);

    RequireThrows<std::logic_error>([&] { bundle.Register(); });

    REQUIRE_TRUE(bundle.IsRegistered());
    const auto& metadataAfter = group.GetModulators().Metadata(0);
    REQUIRE_TRUE(metadataAfter.name == metadataBefore.name);
    REQUIRE_TRUE(metadataAfter.shortName == metadataBefore.shortName);
    REQUIRE_TRUE(metadataAfter.sourceColor == metadataBefore.sourceColor);
    REQUIRE_TRUE(metadataAfter.visualizer == metadataBefore.visualizer);
    REQUIRE_TRUE(metadataAfter.connected == metadataBefore.connected);
}

TEST_CASE(standard_modulators_lifecycle_requires_registration_and_finite_preparation) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);

    REQUIRE_TRUE(!bundle.IsPrepared());
    RequireThrows<std::logic_error>([&] { bundle.Prepare(48000.0); });
    REQUIRE_TRUE(!bundle.IsPrepared());
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).SampleRate() == 0.0);
    }

    bundle.Register();
    RequireThrows<std::logic_error>([&] { bundle.Process(); });
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).RoundElapsedSamples() == 0.0);
    }
    for (std::size_t voice = 0; voice < 2; ++voice) {
        REQUIRE_TRUE(bundle.NoiseProcessor().Output(voice) == 0.0f);
    }

    for (const double invalidRate : std::array<double, 4>{
             0.0,
             -1.0,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         }) {
        RequireThrows<std::invalid_argument>([&] { bundle.Prepare(invalidRate); });
        REQUIRE_TRUE(!bundle.IsPrepared());
    }

    bundle.Prepare(48000.0);
    REQUIRE_TRUE(bundle.IsPrepared());
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).SampleRate() == 48000.0);
    }

    bundle.Prepare(96000.0);
    REQUIRE_TRUE(bundle.IsPrepared());
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).SampleRate() == 96000.0);
    }
    RequireThrows<std::invalid_argument>([&] { bundle.Prepare(-96000.0); });
    REQUIRE_TRUE(bundle.IsPrepared());
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).SampleRate() == 96000.0);
    }
}

TEST_CASE(standard_modulators_process_advances_dynamic_sources_once_and_copies_voice_order) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<4>(manager);
    synth::StandardModulators<4> bundle(group);
    for (auto& input : bundle.Config().randomInputs) {
        input.waiting = {.muSeconds = 0.01, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0};
        input.moving = {.muSeconds = 0.01, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0};
        input.targetInternalSigma = 0.25f;
    }
    bundle.Register();
    bundle.Prepare(100.0);

    std::array<std::array<const float*, 4>, 4> randomPointers{};
    std::array<float, 4> constantValues{};
    std::array<float*, 4> constantPointers{};
    for (std::size_t random = 0; random < 4; ++random) {
        bundle.RandomProcessor(random).Process(std::as_const(bundle).RandomInput(random));
        REQUIRE_TRUE(bundle.RandomProcessor(random).RoundElapsedSamples() == 0.0);
        for (std::size_t voice = 0; voice < 4; ++voice) {
            REQUIRE_TRUE(bundle.RandomProcessor(random).Voices()[voice].GetState() ==
                         synth::GangedRandomLfoVoice::State::Waiting);
            bundle.RandomOutputRow(random)[voice] = -1.0f;
            randomPointers[random][voice] = bundle.RandomPointerRow(random)[voice];
        }
    }
    for (std::size_t voice = 0; voice < 4; ++voice) {
        constantValues[voice] = bundle.ConstantProcessor().Output(voice);
        constantPointers[voice] = bundle.ConstantProcessor().SourcePointers()[voice];
        REQUIRE_TRUE(bundle.NoiseProcessor().Output(voice) == 0.0f);
    }

    bundle.Process();

    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).RoundElapsedSamples() == 1.0);
        for (std::size_t voice = 0; voice < 4; ++voice) {
            REQUIRE_TRUE(bundle.RandomProcessor(random).Voices()[voice].GetState() ==
                         synth::GangedRandomLfoVoice::State::Moving);
            REQUIRE_TRUE(bundle.RandomOutputRow(random)[voice] ==
                         bundle.RandomProcessor(random).Output(voice));
            REQUIRE_TRUE(bundle.RandomPointerRow(random)[voice] == randomPointers[random][voice]);
            REQUIRE_TRUE(bundle.RandomPointerRow(random)[voice] ==
                         &bundle.RandomOutputRow(random)[voice]);
        }
    }
    for (std::size_t voice = 0; voice < 4; ++voice) {
        REQUIRE_TRUE(bundle.NoiseProcessor().Output(voice) > 0.0f);
        REQUIRE_TRUE(bundle.NoiseProcessor().Output(voice) < 1.0f);
        REQUIRE_TRUE(bundle.ConstantProcessor().Output(voice) == constantValues[voice]);
        REQUIRE_TRUE(bundle.ConstantProcessor().SourcePointers()[voice] == constantPointers[voice]);
    }
}

TEST_CASE(standard_modulators_group_updates_and_ui_publication_remain_explicit) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<4>(manager);
    synth::StandardModulators<4> bundle(group);
    bundle.Register();
    bundle.Prepare(48000.0);

    std::array<const float*, 4> outputAddresses{};
    std::array<std::array<float*, 4>, 4> pointerRows{};
    std::array<const synth::ui::Visualizer*, 4> visualizerAddresses{};
    std::array<synth::GangedRandomLfoSnapshot<4>, 4> publishedBefore{};
    for (std::size_t random = 0; random < 4; ++random) {
        outputAddresses[random] = bundle.RandomOutputRow(random).data();
        pointerRows[random] = bundle.RandomPointerRow(random);
        visualizerAddresses[random] = &bundle.RandomVisualizer(random);
        REQUIRE_TRUE(group.GetModulators().Metadata(random).visualizer == visualizerAddresses[random]);
    }
    const float* noiseOutputs = bundle.NoiseProcessor().Outputs().data();
    float* const* noisePointers = bundle.NoiseProcessor().SourcePointers().data();
    const float* constantOutputs = bundle.ConstantProcessor().Outputs().data();
    float* const* constantPointers = bundle.ConstantProcessor().SourcePointers().data();
    const auto* noiseVisualizer = &bundle.NoiseVisualizer();
    const auto* constantVisualizer = &bundle.ConstantVisualizer();

    bundle.PublishUiState();
    for (std::size_t random = 0; random < 4; ++random) {
        REQUIRE_TRUE(bundle.RandomProcessor(random).ReadSnapshot(publishedBefore[random]));
        REQUIRE_TRUE(publishedBefore[random].sampleRate == 48000.0);
        REQUIRE_TRUE(publishedBefore[random].voices[0].state ==
                     synth::GangedRandomLfoVoice::State::Done);
    }

    bundle.Process();
    bundle.Process();
    for (std::size_t voice = 0; voice < 4; ++voice) {
        for (const std::size_t modulator : std::array<std::size_t, 6>{0, 1, 2, 3, 11, 14}) {
            REQUIRE_TRUE(group.GetModulators().Value(voice, modulator) == 0.0f);
        }
    }
    for (std::size_t random = 0; random < 4; ++random) {
        synth::GangedRandomLfoSnapshot<4> stillPublished{};
        REQUIRE_TRUE(bundle.RandomProcessor(random).ReadSnapshot(stillPublished));
        REQUIRE_TRUE(stillPublished.roundElapsedSamples ==
                     publishedBefore[random].roundElapsedSamples);
        REQUIRE_TRUE(stillPublished.voices[0].state == publishedBefore[random].voices[0].state);
        REQUIRE_TRUE(stillPublished.voices[0].currentStateProgress ==
                     publishedBefore[random].voices[0].currentStateProgress);
    }

    group.UpdateModValues();
    for (std::size_t random = 0; random < 4; ++random) {
        for (std::size_t voice = 0; voice < 4; ++voice) {
            REQUIRE_TRUE(group.GetModulators().Value(voice, random) ==
                         bundle.RandomOutputRow(random)[voice]);
        }
    }
    for (std::size_t voice = 0; voice < 4; ++voice) {
        REQUIRE_TRUE(group.GetModulators().Value(voice, 11) ==
                     bundle.ConstantProcessor().Output(voice));
        REQUIRE_TRUE(group.GetModulators().Value(voice, 14) ==
                     bundle.NoiseProcessor().Output(voice));
    }

    bundle.PublishUiState();
    for (std::size_t random = 0; random < 4; ++random) {
        synth::GangedRandomLfoSnapshot<4> publishedAfter{};
        REQUIRE_TRUE(bundle.RandomProcessor(random).ReadSnapshot(publishedAfter));
        REQUIRE_TRUE(publishedAfter.roundElapsedSamples == 1.0);
        REQUIRE_TRUE(publishedAfter.voices[0].state ==
                     synth::GangedRandomLfoVoice::State::Waiting);
        REQUIRE_TRUE(bundle.RandomOutputRow(random).data() == outputAddresses[random]);
        REQUIRE_TRUE(bundle.RandomPointerRow(random) == pointerRows[random]);
        REQUIRE_TRUE(&bundle.RandomVisualizer(random) == visualizerAddresses[random]);
        REQUIRE_TRUE(group.GetModulators().Metadata(random).visualizer == visualizerAddresses[random]);
    }
    REQUIRE_TRUE(bundle.NoiseProcessor().Outputs().data() == noiseOutputs);
    REQUIRE_TRUE(bundle.NoiseProcessor().SourcePointers().data() == noisePointers);
    REQUIRE_TRUE(bundle.ConstantProcessor().Outputs().data() == constantOutputs);
    REQUIRE_TRUE(bundle.ConstantProcessor().SourcePointers().data() == constantPointers);
    REQUIRE_TRUE(&bundle.NoiseVisualizer() == noiseVisualizer);
    REQUIRE_TRUE(&bundle.ConstantVisualizer() == constantVisualizer);
    REQUIRE_TRUE(group.GetModulators().Metadata(14).visualizer == noiseVisualizer);
    REQUIRE_TRUE(group.GetModulators().Metadata(11).visualizer == constantVisualizer);

    synth::ParameterManager monoManager;
    auto& monoGroup = MakeStandardModulatorGroup<1>(monoManager);
    synth::StandardModulators<1> mono(monoGroup);
    mono.Register();
    mono.Prepare(48000.0);
    mono.Process();
    monoGroup.UpdateModValues();
    REQUIRE_TRUE(!monoGroup.GetModulators().Metadata(11).connected);
    REQUIRE_TRUE(monoGroup.GetModulators().Metadata(11).visualizer == nullptr);
    REQUIRE_TRUE(monoGroup.GetModulators().Value(0, 11) == 0.0f);
}

TEST_CASE(standard_modulators_random_inspection_is_bounds_checked) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);
    const auto& constBundle = std::as_const(bundle);

    RequireThrows<std::out_of_range>([&] { (void)bundle.RandomProcessor(4); });
    RequireThrows<std::out_of_range>([&] { (void)constBundle.RandomProcessor(4); });
    RequireThrows<std::out_of_range>([&] { (void)bundle.RandomInput(4); });
    RequireThrows<std::out_of_range>([&] { (void)constBundle.RandomInput(4); });
    RequireThrows<std::out_of_range>([&] { (void)bundle.RandomOutputRow(4); });
    RequireThrows<std::out_of_range>([&] { (void)constBundle.RandomOutputRow(4); });
    RequireThrows<std::out_of_range>([&] { (void)bundle.RandomPointerRow(4); });
    RequireThrows<std::out_of_range>([&] { (void)constBundle.RandomPointerRow(4); });
    RequireThrows<std::out_of_range>([&] { (void)bundle.RandomVisualizer(4); });
    RequireThrows<std::out_of_range>([&] { (void)constBundle.RandomVisualizer(4); });
}

TEST_CASE(standard_modulators_metadata_color_overrides_reach_owned_visualizers) {
    synth::ParameterManager manager;
    auto& group = MakeStandardModulatorGroup<2>(manager);
    synth::StandardModulators<2> bundle(group);
    bundle.Config().constantMetadata.sourceColor = synth::Color::Indigo;
    bundle.Config().noiseMetadata.sourceColor = synth::Color::Orange;
    bundle.Register();

    bundle.ConstantVisualizer().SetBounds({0.0f, 0.0f, 20.0f, 20.0f});
    bundle.NoiseVisualizer().SetBounds({0.0f, 0.0f, 20.0f, 20.0f});
    const auto constantCommands = bundle.ConstantVisualizer().Draw();
    const auto noiseCommands = bundle.NoiseVisualizer().Draw();
    REQUIRE_TRUE(constantCommands.size() == 2);
    for (const auto& command : constantCommands) {
        REQUIRE_TRUE(command.color == synth::Color::Indigo);
    }
    REQUIRE_TRUE(noiseCommands.size() == 1);
    REQUIRE_TRUE(noiseCommands[0].color == synth::Color::Orange);
}

TEST_CASE(smartgrid_dsp_public_headers_are_dependency_clean) {
    #ifdef JUCE_MAJOR_VERSION
    throw std::runtime_error("DSP headers must not include JUCE");
    #endif
    REQUIRE_TRUE(std::is_default_constructible_v<synth::BitCrusher>);
    REQUIRE_TRUE(std::is_default_constructible_v<synth::Meter>);
    REQUIRE_TRUE(std::is_default_constructible_v<synth::Ola<12>>);
}

TEST_CASE(constant_modulator_validates_runtime_voice_count_and_bounds) {
    bool rejectedZero = false;
    try {
        synth::ConstantModulatorProcessor invalid(0);
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejectedZero = true;
    }
    REQUIRE_TRUE(rejectedZero);

    synth::ConstantModulatorProcessor mono(1);
    REQUIRE_TRUE(mono.VoiceCount() == 1);
    REQUIRE_TRUE(mono.Outputs().size() == 1);
    REQUIRE_TRUE(mono.SourcePointers().size() == 1);
    REQUIRE_TRUE(mono.Output(0) == 0.0f);
    bool rejectedPastEnd = false;
    try {
        (void)mono.Output(1);
    } catch (const std::out_of_range&) {
        rejectedPastEnd = true;
    }
    REQUIRE_TRUE(rejectedPastEnd);
}

TEST_CASE(constant_modulator_uses_exact_greedy_even_and_odd_assignments) {
    const std::vector<std::vector<float>> expected{
        {},
        {0.0f},
        {0.0f, 1.0f},
        {0.0f, 0.5f, 1.0f},
        {0.0f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f},
        {0.0f, 0.5f, 0.75f, 0.25f, 1.0f},
        {0.0f, 3.0f / 5.0f, 1.0f / 5.0f, 4.0f / 5.0f, 2.0f / 5.0f, 1.0f},
        {0.0f, 0.5f, 2.0f / 3.0f, 1.0f / 6.0f,
         5.0f / 6.0f, 1.0f / 3.0f, 1.0f},
    };
    for (std::size_t voices = 1; voices < expected.size(); ++voices) {
        synth::ConstantModulatorProcessor processor(voices);
        for (std::size_t voice = 0; voice < voices; ++voice) {
            REQUIRE_NEAR(processor.Output(voice), expected[voices][voice], 1.0e-6f);
        }
    }
}

TEST_CASE(constant_modulator_covers_ranks_and_maximizes_cyclic_distance) {
    for (std::size_t voices = 2; voices <= 16; ++voices) {
        synth::ConstantModulatorProcessor processor(voices);
        std::vector<std::size_t> ranks;
        ranks.reserve(voices);
        for (const float output : processor.Outputs()) {
            ranks.push_back(static_cast<std::size_t>(
                std::lround(output * static_cast<float>(voices - 1))));
        }
        auto sorted = ranks;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t rank = 0; rank < voices; ++rank) {
            REQUIRE_TRUE(sorted[rank] == rank);
        }
        std::size_t distance = 0;
        for (std::size_t voice = 0; voice < voices; ++voice) {
            const std::size_t next = (voice + 1) % voices;
            distance += ranks[voice] > ranks[next]
                ? ranks[voice] - ranks[next]
                : ranks[next] - ranks[voice];
        }
        REQUIRE_TRUE(distance == (voices * voices) / 2);
    }
}

TEST_CASE(constant_modulator_keeps_values_and_registered_addresses_stable) {
    synth::ConstantModulatorProcessor processor(4);
    const auto pointers = processor.SourcePointers();
    const std::array<float*, 4> initialPointers{pointers[0], pointers[1], pointers[2], pointers[3]};
    const std::array<float, 4> initialValues{
        processor.Output(0), processor.Output(1), processor.Output(2), processor.Output(3)};
    for (std::size_t voice = 0; voice < processor.VoiceCount(); ++voice) {
        REQUIRE_TRUE(processor.SourcePointers()[voice] == initialPointers[voice]);
        REQUIRE_TRUE(*initialPointers[voice] == initialValues[voice]);
        REQUIRE_TRUE(*initialPointers[voice] == processor.Output(voice));
    }
}

TEST_CASE(constant_modulator_registers_directly_as_pointer_backed_group_source) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 4, .numModulators = 1, .numScenes = 1, .maxParameters = 1,
    });
    synth::ConstantModulatorProcessor processor(4);
    group.SetModulationSource(0, processor.SourcePointers(), {
        .name = "Constant", .shortName = "Const", .connected = true,
    });
    for (int update = 0; update < 2; ++update) {
        group.UpdateModValues();
        for (std::size_t voice = 0; voice < processor.VoiceCount(); ++voice) {
            REQUIRE_TRUE(group.GetModulators().Value(voice, 0) == processor.Output(voice));
        }
    }
}

TEST_CASE(noise_modulator_requires_positive_runtime_voice_count) {
    bool threw = false;
    try {
        synth::NoiseModulatorProcessor processor(0, 1);
        (void)processor;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    synth::NoiseModulatorProcessor processor(3, 1);
    REQUIRE_TRUE(processor.VoiceCount() == 3);
    REQUIRE_TRUE(processor.Outputs().size() == 3);
    REQUIRE_TRUE(processor.SourcePointers().size() == 3);
}

TEST_CASE(noise_modulator_output_inspection_is_bounds_checked) {
    synth::NoiseModulatorProcessor processor(2, 1);
    bool threw = false;
    try {
        (void)processor.Output(processor.VoiceCount());
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(noise_modulator_explicit_seed_is_repeatable_and_strictly_open) {
    synth::NoiseModulatorProcessor left(4, 0x12345678ULL);
    synth::NoiseModulatorProcessor right(4, 0x12345678ULL);
    for (std::size_t sample = 0; sample < 4096; ++sample) {
        left.Process();
        right.Process();
        for (std::size_t voice = 0; voice < left.VoiceCount(); ++voice) {
            REQUIRE_TRUE(left.Output(voice) == right.Output(voice));
            REQUIRE_TRUE(left.Output(voice) > 0.0f);
            REQUIRE_TRUE(left.Output(voice) < 1.0f);
        }
    }
}

TEST_CASE(noise_modulator_advances_one_shared_word_per_voice) {
    synth::NoiseModulatorProcessor stereo(2, 0x9abcdef0ULL);
    synth::NoiseModulatorProcessor mono(1, 0x9abcdef0ULL);
    stereo.Process();
    mono.Process();
    REQUIRE_TRUE(stereo.Output(0) == mono.Output(0));
    mono.Process();
    REQUIRE_TRUE(stereo.Output(1) == mono.Output(0));
}

TEST_CASE(noise_modulator_fixed_seed_has_sane_unipolar_distribution) {
    synth::NoiseModulatorProcessor processor(1, 0xdecafbadULL);
    std::array<std::size_t, 8> bins{};
    double sum = 0.0;
    constexpr std::size_t kSamples = 32768;
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
        processor.Process();
        const float value = processor.Output(0);
        sum += value;
        const std::size_t bin = std::min<std::size_t>(7, static_cast<std::size_t>(value * 8.0f));
        ++bins[bin];
    }
    REQUIRE_NEAR(sum / static_cast<double>(kSamples), 0.5, 0.015);
    for (const std::size_t count : bins) {
        REQUIRE_TRUE(count > 3500);
        REQUIRE_TRUE(count < 4700);
    }
}

TEST_CASE(noise_modulator_keeps_registered_output_addresses_stable) {
    synth::NoiseModulatorProcessor processor(2, 7);
    const auto initialPointers = processor.SourcePointers();
    float* const voice0 = initialPointers[0];
    float* const voice1 = initialPointers[1];
    for (std::size_t sample = 0; sample < 4096; ++sample) {
        processor.Process();
        REQUIRE_TRUE(processor.SourcePointers()[0] == voice0);
        REQUIRE_TRUE(processor.SourcePointers()[1] == voice1);
        REQUIRE_TRUE(*voice0 == processor.Output(0));
        REQUIRE_TRUE(*voice1 == processor.Output(1));
    }
}

TEST_CASE(noise_modulator_registers_directly_as_pointer_backed_group_source) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    synth::NoiseModulatorProcessor processor(2, 42);
    group.SetModulationSource(0, processor.SourcePointers(), {
        .name = "Noise",
        .shortName = "Noise",
        .connected = true,
    });
    processor.Process();
    group.UpdateModValues();
    REQUIRE_TRUE(group.GetModulators().Value(0, 0) == processor.Output(0));
    REQUIRE_TRUE(group.GetModulators().Value(1, 0) == processor.Output(1));
}

TEST_CASE(bit_crusher_passes_zero_amount_and_quantizes_at_full_amount) {
    synth::BitCrusher crusher;

    REQUIRE_NEAR(crusher.Process({.value = 0.375f, .amount = 0.0f}), 0.375f, 0.0001f);
    REQUIRE_NEAR(crusher.Process({.value = 0.25f, .amount = 1.0f}), 1.0f, 0.0001f);
    REQUIRE_NEAR(crusher.Process({.value = -0.25f, .amount = 1.0f}), -1.0f, 0.0001f);
}

TEST_CASE(sample_rate_reducer_holds_until_phase_wraps) {
    synth::SampleRateReducer reducer;

    REQUIRE_NEAR(reducer.Process({.value = 1.0f, .freq = 0.5f}), 0.0f, 0.0001f);
    REQUIRE_NEAR(reducer.Process({.value = 0.25f, .freq = 0.5f}), 0.25f, 0.0001f);
    REQUIRE_NEAR(reducer.Process({.value = 0.75f, .freq = 0.5f}), 0.25f, 0.0001f);
    REQUIRE_NEAR(reducer.Process({.value = -0.5f, .freq = 1.0f}), -0.5f, 0.0001f);
}

TEST_CASE(meter_tracks_rms_peak_snapshots_and_gain_reduction) {
    synth::Meter meter;

    meter.Process(0.5f);
    meter.Process(-1.0f);
    const synth::MeterSnapshot snapshot = meter.Snapshot();
    REQUIRE_TRUE(snapshot.rms > 0.0f);
    REQUIRE_NEAR(snapshot.peak, 1.0f, 0.0001f);

    const float output = meter.ProcessAndSaturate(2.0f);
    const float expectedOutput = std::atan(2.0f * std::numbers::pi_v<float> * 0.5f) / (std::numbers::pi_v<float> * 0.5f);
    const synth::MeterSnapshot saturatedSnapshot = meter.Snapshot();
    REQUIRE_NEAR(output, expectedOutput, 0.0001f);
    REQUIRE_NEAR(saturatedSnapshot.reduction, std::abs(expectedOutput) / 2.0f, 0.0001f);
}

TEST_CASE(meter_snapshot_rms_is_linear_amplitude_and_db_helpers_use_linear_inputs) {
    synth::Meter meter;

    meter.Process(0.5f);
    const float expectedMeanSquare = 0.25f * synth::Meter::kSmoothingAlphaUp;
    const synth::MeterSnapshot snapshot = meter.Snapshot();

    REQUIRE_NEAR(snapshot.rms, std::sqrt(expectedMeanSquare), 0.0001f);
    REQUIRE_NEAR(synth::Meter::RmsDbFS(0.5f), -6.0206f, 0.001f);
    REQUIRE_NEAR(synth::Meter::PeakDbFS(0.5f), -6.0206f, 0.001f);
}

TEST_CASE(nary_meter_processes_channels_and_publishes_snapshots) {
    synth::NaryMeter<2> meter;
    synth::StereoFloat input{{0.25f, -0.75f}};

    meter.Process(input);
    const synth::NaryMeterSnapshot<2> snapshot = meter.Snapshot();

    REQUIRE_TRUE(snapshot.meters[0].rms > 0.0f);
    REQUIRE_TRUE(snapshot.meters[1].rms > snapshot.meters[0].rms);
    REQUIRE_NEAR(snapshot.meters[0].peak, 0.25f, 0.0001f);
    REQUIRE_NEAR(snapshot.meters[1].peak, 0.75f, 0.0001f);
}

TEST_CASE(math_supports_multiple_precisions_and_periodic_trig) {
    REQUIRE_TRUE(synth::DspMath<8>::kTableSize == 256);
    REQUIRE_TRUE(synth::DspMath<12>::kTableSize == 4096);
    REQUIRE_NEAR(synth::DspMath<10>::Sin2Pi(0.125f), synth::DspMath<10>::Sin2Pi(1.125f), 0.0001f);
    REQUIRE_NEAR(synth::DspMath<10>::Cos2Pi(0.25f), 0.0f, 0.002f);
    REQUIRE_NEAR(synth::DspMath<10>::TanPi(0.25f), 1.0f, 0.004f);
    const auto polar = synth::DspMath<10>::Polar2Pi(2.0f, 0.0f);
    REQUIRE_NEAR(polar.real(), 2.0f, 0.0001f);
    REQUIRE_NEAR(std::abs(synth::DspMath<10>::RootOfUnityByIndex(0)), 1.0f, 0.0001f);
    REQUIRE_NEAR(synth::DspMath<10>::HannKernel(0.0f).real(), 0.5f, 0.001f);
}

TEST_CASE(shaped_interpolate_endpoints_and_landmarks) {
    REQUIRE_NEAR(synth::ShapedInterpolate(-0.25f, 0.75f, 0.4f, 0.0), -0.25f, 0.000001f);
    REQUIRE_NEAR(synth::ShapedInterpolate(-0.25f, 0.75f, 0.4f, 1.0), 0.75f, 0.000001f);

    constexpr double t = 0.25;
    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, 0.0f, t), 0.25f, 0.000001f);
    const float smoothT = 0.5f
        - 0.5f * synth::DefaultDspMath::Cos2Pi(0.5f * static_cast<float>(t));
    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, 1.0f, t), smoothT, 0.000001f);
    REQUIRE_NEAR(
        synth::ShapedInterpolate(0.0f, 1.0f, 0.25f, t),
        0.25f * smoothT + 0.75f * static_cast<float>(t),
        0.000001f);

    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, -2.0f, 0.25), 0.25f, 0.000001f);
    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, 2.0f, 0.25), smoothT, 0.000001f);
    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, 0.5f, -0.25), 0.0f, 0.000001f);
    REQUIRE_NEAR(synth::ShapedInterpolate(0.0f, 1.0f, 0.5f, 1.25), 1.0f, 0.000001f);
}

TEST_CASE(shaped_interpolate_preserves_double_progress) {
    using ShapedInterpolateFunction = float (*)(float, float, float, double);
    static_assert(std::is_same_v<decltype(&synth::ShapedInterpolate), ShapedInterpolateFunction>);

    const double progress = 0.123456789012345;
    const double originalProgress = progress;
    REQUIRE_TRUE(static_cast<double>(static_cast<float>(progress)) != progress);

    const float narrowedProgress = static_cast<float>(std::clamp(progress, 0.0, 1.0));
    const float smoothT = 0.5f - 0.5f * synth::DefaultDspMath::Cos2Pi(0.5f * narrowedProgress);
    const float shapedT = 0.75f * smoothT + 0.25f * narrowedProgress;
    const float expected = -0.5f * (1.0f - shapedT) + 0.5f * shapedT;
    REQUIRE_NEAR(synth::ShapedInterpolate(-0.5f, 0.5f, 0.75f, progress), expected, 0.000001f);
    REQUIRE_TRUE(progress == originalProgress);
}

TEST_CASE(correlated_increments_use_reciprocal_center_and_hz_sigma) {
    ScriptedRandomLfoDrawSource draws{
        .normalResults = {-2.0, 0.4, -0.6},
    };
    const synth::RandomTimingConfig config{
        .muSeconds = 2.0,
        .sigmaSeconds = 0.5,
        .internalSigmaHz = 0.125,
    };

    const auto increments = synth::SampleCorrelatedIncrements<2>(10.0, config, draws);

    REQUIRE_NEAR(increments[0], 0.04, 1.0e-12);
    REQUIRE_NEAR(increments[1], 0.06, 1.0e-12);
    REQUIRE_TRUE(draws.normalCalls.size() == 3);
    REQUIRE_NEAR(draws.normalCalls[0].mean, 2.0, 0.0);
    REQUIRE_NEAR(draws.normalCalls[0].sigma, 0.5, 0.0);
    REQUIRE_NEAR(draws.normalCalls[1].mean, 0.5, 1.0e-12);
    REQUIRE_NEAR(draws.normalCalls[1].sigma, 0.125, 0.0);
    REQUIRE_NEAR(draws.normalCalls[2].mean, 0.5, 1.0e-12);
    REQUIRE_NEAR(draws.normalCalls[2].sigma, 0.125, 0.0);
}

TEST_CASE(correlated_increments_floor_near_zero_rate) {
    constexpr double sampleRate = 8.0;
    ScriptedRandomLfoDrawSource draws{
        .normalResults = {0.0, 0.0, 1.0e-30},
    };
    const synth::RandomTimingConfig config{
        .muSeconds = 2.0,
        .sigmaSeconds = 0.5,
        .internalSigmaHz = 0.125,
    };

    const auto increments = synth::SampleCorrelatedIncrements<2>(sampleRate, config, draws);
    const double epsilonIncrement = 1.0 / (sampleRate * 3600.0);

    REQUIRE_NEAR(draws.normalCalls[1].mean, sampleRate, 0.0);
    REQUIRE_NEAR(draws.normalCalls[2].mean, sampleRate, 0.0);
    REQUIRE_NEAR(increments[0], epsilonIncrement, 0.0);
    REQUIRE_NEAR(increments[1], epsilonIncrement, 0.0);
    REQUIRE_TRUE(std::ceil(1.0 / epsilonIncrement) == std::ceil(sampleRate * 3600.0));
}

TEST_CASE(correlated_increments_reject_invalid_config) {
    const synth::RandomTimingConfig valid{
        .muSeconds = 2.0,
        .sigmaSeconds = 0.5,
        .internalSigmaHz = 0.125,
    };
    auto rejects = [](double sampleRate, const synth::RandomTimingConfig& config) {
        ScriptedRandomLfoDrawSource draws{.normalResults = {2.0, 0.5}};
        try {
            (void)synth::SampleCorrelatedIncrements<1>(sampleRate, config, draws);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    REQUIRE_TRUE(rejects(0.0, valid));
    REQUIRE_TRUE(rejects(-48000.0, valid));
    REQUIRE_TRUE(rejects(std::numeric_limits<double>::infinity(), valid));
    REQUIRE_TRUE(rejects(std::numeric_limits<double>::quiet_NaN(), valid));

    auto invalid = valid;
    invalid.muSeconds = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_TRUE(rejects(48000.0, invalid));
    invalid.muSeconds = std::numeric_limits<double>::infinity();
    REQUIRE_TRUE(rejects(48000.0, invalid));
    invalid = valid;
    invalid.sigmaSeconds = -0.01;
    REQUIRE_TRUE(rejects(48000.0, invalid));
    invalid = valid;
    invalid.sigmaSeconds = std::numeric_limits<double>::infinity();
    REQUIRE_TRUE(rejects(48000.0, invalid));
    invalid = valid;
    invalid.internalSigmaHz = -0.01;
    REQUIRE_TRUE(rejects(48000.0, invalid));
    invalid = valid;
    invalid.internalSigmaHz = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_TRUE(rejects(48000.0, invalid));
}

TEST_CASE(adsr_processor_runs_linear_stages_and_exact_endpoints) {
    synth::AdsrProcessor adsr;
    synth::AdsrProcessor::Input input{
        .attackIncrement = 0.5,
        .decayIncrement = 0.5,
        .sustain = 0.25f,
        .releaseIncrement = 0.5,
        .gate = false,
    };

    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Idle);
    REQUIRE_NEAR(adsr.Process(input), 0.0f, 0.0f);

    input.gate = true;
    REQUIRE_NEAR(adsr.Process(input), 0.5f, 0.000001f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Attack);
    REQUIRE_NEAR(adsr.Process(input), 1.0f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Decay);
    REQUIRE_NEAR(adsr.Process(input), 0.625f, 0.000001f);
    REQUIRE_NEAR(adsr.Process(input), 0.25f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Sustain);

    input.gate = false;
    REQUIRE_NEAR(adsr.Process(input), 0.125f, 0.000001f);
    REQUIRE_NEAR(adsr.Process(input), 0.0f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Idle);
}

TEST_CASE(adsr_processor_interrupts_and_retriggers_from_current_value) {
    synth::AdsrProcessor adsr;
    synth::AdsrProcessor::Input input{
        .attackIncrement = 0.25,
        .decayIncrement = 0.25,
        .sustain = 0.2f,
        .releaseIncrement = 0.25,
        .gate = true,
    };

    REQUIRE_NEAR(adsr.Process(input), 0.25f, 0.000001f);
    REQUIRE_NEAR(adsr.Process(input), 0.5f, 0.000001f);

    input.gate = false;
    REQUIRE_NEAR(adsr.Process(input), 0.375f, 0.000001f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Release);

    input.gate = true;
    REQUIRE_NEAR(adsr.Process(input), 0.53125f, 0.000001f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Attack);
}

TEST_CASE(adsr_processor_holds_zero_increments_and_tracks_live_sustain) {
    synth::AdsrProcessor adsr;
    synth::AdsrProcessor::Input input{
        .attackIncrement = 0.0,
        .decayIncrement = 0.5,
        .sustain = 0.5f,
        .releaseIncrement = 1.0,
        .gate = true,
    };

    REQUIRE_NEAR(adsr.Process(input), 0.0f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Attack);
    REQUIRE_NEAR(adsr.Process(input), 0.0f, 0.0f);

    input.attackIncrement = 1.0;
    REQUIRE_NEAR(adsr.Process(input), 1.0f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Decay);
    REQUIRE_NEAR(adsr.Process(input), 0.75f, 0.000001f);

    input.sustain = 0.25f;
    REQUIRE_NEAR(adsr.Process(input), 0.25f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Sustain);
    input.sustain = 0.6f;
    REQUIRE_NEAR(adsr.Process(input), 0.6f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Sustain);

    input.gate = false;
    REQUIRE_NEAR(adsr.Process(input), 0.0f, 0.0f);
    REQUIRE_TRUE(adsr.GetState() == synth::AdsrProcessor::State::Idle);
    REQUIRE_TRUE(adsr.Output() >= 0.0f && adsr.Output() <= 1.0f);
}

TEST_CASE(ganged_random_lfo_voice_runs_wait_move_and_done_states) {
    synth::GangedRandomLfoVoice voice;
    const synth::VoiceInput input{
        .waitingIncrement = 0.4,
        .movingIncrement = 0.25,
        .shape = 0.0f,
    };

    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Done);
    REQUIRE_NEAR(voice.Output(), 0.0f, 0.0f);

    voice.Reset(0.75f);
    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 0.0, 0.0);
    REQUIRE_NEAR(voice.Source(), 0.0f, 0.0f);
    REQUIRE_NEAR(voice.Target(), 0.75f, 0.0f);
    REQUIRE_NEAR(voice.Output(), 0.0f, 0.0f);

    REQUIRE_NEAR(voice.Process(input), 0.0f, 0.0f);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 0.4, 1.0e-15);
    REQUIRE_NEAR(voice.Process(input), 0.0f, 0.0f);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 0.8, 1.0e-15);
    REQUIRE_NEAR(voice.Process(input), 0.0f, 0.0f);
    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Moving);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 0.0, 0.0);

    REQUIRE_NEAR(voice.Process(input), 0.1875f, 0.000001f);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 0.25, 0.0);

    const synth::VoiceInput overshoot{
        .waitingIncrement = 0.4,
        .movingIncrement = 0.8,
        .shape = 1.0f,
    };
    REQUIRE_NEAR(voice.Process(overshoot), 0.75f, 0.0f);
    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Done);
    REQUIRE_TRUE(voice.CurrentStateProgress() > 1.0);
    REQUIRE_NEAR(voice.Process(overshoot), 0.75f, 0.0f);

    voice.Reset(0.5f);
    const synth::VoiceInput exactBoundary{
        .waitingIncrement = 1.0,
        .movingIncrement = 1.0,
        .shape = 0.5f,
    };
    REQUIRE_NEAR(voice.Process(exactBoundary), 0.75f, 0.0f);
    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Moving);
    REQUIRE_NEAR(voice.Process(exactBoundary), 0.5f, 0.0f);
    REQUIRE_TRUE(voice.GetState() == synth::GangedRandomLfoVoice::State::Done);
    REQUIRE_NEAR(voice.CurrentStateProgress(), 1.0, 0.0);

    voice.Reset(0.25f);
    REQUIRE_NEAR(voice.Source(), 0.5f, 0.0f);
    REQUIRE_NEAR(voice.Target(), 0.25f, 0.0f);
    REQUIRE_NEAR(voice.Output(), 0.5f, 0.0f);
}

TEST_CASE(ganged_random_lfo_samples_round_in_canonical_logical_order) {
    ScriptedRandomLfoDrawSource draws{
        .normalResults = {2.0, 0.4, 0.6, 4.0, 0.2, 0.3, -0.2, 1.2},
        .uniformResults = {0.5f, 0.1f, 0.9f},
    };
    synth::GangedRandomLfoProcessor<2, ScriptedRandomLfoDrawSource> gang{std::move(draws)};
    gang.Prepare(10.0);
    const synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 2.0, .sigmaSeconds = 0.5, .internalSigmaHz = 0.125},
        .moving = {.muSeconds = 4.0, .sigmaSeconds = 1.0, .internalSigmaHz = 0.25},
        .targetInternalSigma = 0.25f,
    };

    gang.Process(input);

    REQUIRE_NEAR(gang.Output(0), 0.0f, 0.0f);
    REQUIRE_NEAR(gang.Output(1), 0.0f, 0.0f);
    REQUIRE_NEAR(gang.RoundElapsedSamples(), 0.0, 0.0);
    REQUIRE_TRUE(gang.Voices()[0].GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    REQUIRE_TRUE(gang.Voices()[1].GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    REQUIRE_NEAR(gang.VoiceInputs()[0].waitingIncrement, 0.04, 1.0e-12);
    REQUIRE_NEAR(gang.VoiceInputs()[1].waitingIncrement, 0.06, 1.0e-12);
    REQUIRE_NEAR(gang.VoiceInputs()[0].movingIncrement, 0.02, 1.0e-12);
    REQUIRE_NEAR(gang.VoiceInputs()[1].movingIncrement, 0.03, 1.0e-12);
    REQUIRE_NEAR(gang.VoiceInputs()[0].shape, 0.1f, 0.0f);
    REQUIRE_NEAR(gang.VoiceInputs()[1].shape, 0.9f, 0.0f);
    REQUIRE_NEAR(gang.Voices()[0].Target(), 0.0f, 0.0f);
    REQUIRE_NEAR(gang.Voices()[1].Target(), 1.0f, 0.0f);

    const auto& observed = gang.RandomSource();
    const std::vector<ScriptedRandomLfoDrawSource::EventKind> expectedEvents{
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Uniform,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Normal,
        ScriptedRandomLfoDrawSource::EventKind::Uniform,
        ScriptedRandomLfoDrawSource::EventKind::Uniform,
    };
    REQUIRE_TRUE(observed.events == expectedEvents);
    REQUIRE_TRUE(observed.normalCalls.size() == 8);
    REQUIRE_NEAR(observed.normalCalls[0].mean, 2.0, 0.0);
    REQUIRE_NEAR(observed.normalCalls[3].mean, 4.0, 0.0);
    REQUIRE_NEAR(observed.normalCalls[6].mean, 0.5, 0.0);
    REQUIRE_NEAR(observed.normalCalls[7].mean, 0.5, 0.0);
    REQUIRE_NEAR(observed.normalCalls[6].sigma, 0.25, 0.0);
}

TEST_CASE(ganged_random_lfo_slowest_voice_gates_round_turnover) {
    ScriptedRandomLfoDrawSource draws{
        .normalResults = {
            1.0, 1.0, 0.5, 1.0, 1.0, 0.5, 0.25, 0.75,
            1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.4, 0.6,
        },
        .uniformResults = {0.5f, 0.0f, 1.0f, 0.5f, 0.25f, 0.75f},
    };
    synth::GangedRandomLfoProcessor<2, ScriptedRandomLfoDrawSource> gang{std::move(draws)};
    gang.Prepare(1.0);
    const synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 1.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .moving = {.muSeconds = 1.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .targetInternalSigma = 0.2f,
    };

    gang.Process(input);
    REQUIRE_TRUE(gang.RandomSource().nextNormalResult == 8);
    gang.Process(input);
    REQUIRE_TRUE(gang.Voices()[0].GetState() == synth::GangedRandomLfoVoice::State::Moving);
    REQUIRE_TRUE(gang.Voices()[1].GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    gang.Process(input);
    REQUIRE_TRUE(gang.Voices()[0].GetState() == synth::GangedRandomLfoVoice::State::Done);
    REQUIRE_TRUE(gang.Voices()[1].GetState() == synth::GangedRandomLfoVoice::State::Moving);
    REQUIRE_TRUE(gang.RandomSource().nextNormalResult == 8);
    gang.Process(input);
    REQUIRE_TRUE(gang.Voices()[0].GetState() == synth::GangedRandomLfoVoice::State::Done);
    REQUIRE_TRUE(gang.RandomSource().nextNormalResult == 8);
    gang.Process(input);
    REQUIRE_TRUE(gang.Voices()[0].GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    REQUIRE_TRUE(gang.Voices()[1].GetState() == synth::GangedRandomLfoVoice::State::Waiting);
    REQUIRE_TRUE(gang.RandomSource().nextNormalResult == 16);
    REQUIRE_NEAR(gang.Output(0), 0.25f, 0.0f);
    REQUIRE_NEAR(gang.Output(1), 0.75f, 0.0f);
    REQUIRE_NEAR(gang.RoundElapsedSamples(), 0.0, 0.0);
}

TEST_CASE(ganged_random_lfo_floors_heavy_tail_increments) {
    ScriptedRandomLfoDrawSource draws{
        .normalResults = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.5, 0.5},
        .uniformResults = {0.5f, 0.5f, 0.5f},
    };
    synth::GangedRandomLfoProcessor<2, ScriptedRandomLfoDrawSource> gang{std::move(draws)};
    constexpr double sampleRate = 8.0;
    gang.Prepare(sampleRate);
    const synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 0.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .moving = {.muSeconds = 0.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .targetInternalSigma = 0.0f,
    };

    gang.Process(input);

    const double epsilon = 1.0 / (sampleRate * 3600.0);
    for (const auto& voiceInput : gang.VoiceInputs()) {
        REQUIRE_NEAR(voiceInput.waitingIncrement, epsilon, 0.0);
        REQUIRE_NEAR(voiceInput.movingIncrement, epsilon, 0.0);
        REQUIRE_TRUE(std::ceil(1.0 / voiceInput.waitingIncrement) == std::ceil(sampleRate * 3600.0));
    }
}

TEST_CASE(ganged_random_lfo_fixed_seed_is_reproducible) {
    synth::GangedRandomLfoProcessor<2> first{0x12345678u};
    synth::GangedRandomLfoProcessor<2> second{0x12345678u};
    first.Prepare(32.0);
    second.Prepare(32.0);
    const synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 0.1, .sigmaSeconds = 0.02, .internalSigmaHz = 0.5},
        .moving = {.muSeconds = 0.1, .sigmaSeconds = 0.02, .internalSigmaHz = 0.5},
        .targetInternalSigma = 0.1f,
    };

    for (int sample = 0; sample < 512; ++sample) {
        first.Process(input);
        second.Process(input);
        for (std::size_t voice = 0; voice < 2; ++voice) {
            REQUIRE_NEAR(first.Output(voice), second.Output(voice), 0.0f);
            REQUIRE_NEAR(
                first.VoiceInputs()[voice].waitingIncrement,
                second.VoiceInputs()[voice].waitingIncrement,
                0.0);
            REQUIRE_NEAR(
                first.VoiceInputs()[voice].movingIncrement,
                second.VoiceInputs()[voice].movingIncrement,
                0.0);
        }
    }
}

TEST_CASE(default_random_lfo_draw_source_supports_zero_sigma) {
    synth::DefaultRandomDrawSource draws{123u};
    REQUIRE_NEAR(draws.Normal(0.75, 0.0), 0.75, 0.0);
}

TEST_CASE(ganged_random_lfo_validates_setup_and_uses_fixed_storage) {
    using Processor = synth::GangedRandomLfoProcessor<2, FixedRandomLfoDrawSource>;
    static_assert(std::is_same_v<
        decltype(std::declval<const Processor&>().Voices()),
        const std::array<synth::GangedRandomLfoVoice, 2>&>);
    static_assert(std::is_same_v<
        decltype(std::declval<const Processor&>().VoiceInputs()),
        const std::array<synth::VoiceInput, 2>&>);

    Processor gang;
    bool rejectedVoiceIndex = false;
    try {
        (void)gang.Output(2);
    } catch (const std::out_of_range&) {
        rejectedVoiceIndex = true;
    }
    REQUIRE_TRUE(rejectedVoiceIndex);

    bool rejectedSampleRate = false;
    try {
        gang.Prepare(0.0);
    } catch (const std::invalid_argument&) {
        rejectedSampleRate = true;
    }
    REQUIRE_TRUE(rejectedSampleRate);

    gang.Prepare(1.0);
    synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 1.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .moving = {.muSeconds = 1.0, .sigmaSeconds = 0.0, .internalSigmaHz = 0.0},
        .targetInternalSigma = 0.0f,
    };
    for (int sample = 0; sample < 10000; ++sample) {
        gang.Process(input);
    }
    REQUIRE_TRUE(gang.RoundElapsedSamples() >= 0.0);

    input.targetInternalSigma = -0.1f;
    bool rejectedTargetSigma = false;
    try {
        gang.Process(input);
    } catch (const std::invalid_argument&) {
        rejectedTargetSigma = true;
    }
    REQUIRE_TRUE(rejectedTargetSigma);
}

TEST_CASE(ganged_random_lfo_snapshot_publishes_every_live_field_and_assigned_color) {
    using Processor = synth::GangedRandomLfoProcessor<2, ScriptedRandomLfoDrawSource>;
    using Snapshot = synth::GangedRandomLfoSnapshot<2>;
    static_assert(std::is_trivially_copyable_v<synth::GangedRandomLfoVoiceSnapshot>);
    static_assert(std::is_trivially_copyable_v<Snapshot>);
    static_assert(!HasRecordedScopeStorage<Snapshot>);
    static_assert(!HasRecordedScopeBufferStorage<Snapshot>);
    static_assert(!HasRecordedHistoryStorage<Snapshot>);
    static_assert(!HasRecordedSampleStorage<Snapshot>);

    ScriptedRandomLfoDrawSource draws{
        .normalResults = {2.0, 0.4, 0.6, 4.0, 0.2, 0.3, 0.3, 0.7},
        .uniformResults = {0.5f, 0.2f, 0.8f},
    };
    Processor gang{std::move(draws)};
    gang.Prepare(10.0);
    REQUIRE_TRUE(gang.VoiceColor(0) == synth::Color::Grey);
    REQUIRE_TRUE(gang.VoiceColor(1) == synth::Color::Grey);
    REQUIRE_TRUE(gang.VoiceColor(0) != synth::Color::Cyan);
    REQUIRE_TRUE(gang.VoiceColor(1) != synth::Color::Orange);

    const auto color0 = synth::Color::Rgba(7, 31, 83, 191);
    const auto color1 = synth::Color::Rgba(211, 113, 19, 239);
    gang.SetVoiceColor(0, color0);
    gang.SetVoiceColor(1, color1);
    const synth::GangedRandomLfoInput input{
        .waiting = {.muSeconds = 2.0, .sigmaSeconds = 0.5, .internalSigmaHz = 0.125},
        .moving = {.muSeconds = 4.0, .sigmaSeconds = 1.0, .internalSigmaHz = 0.25},
        .targetInternalSigma = 0.25f,
    };
    gang.Process(input);
    gang.Process(input);
    REQUIRE_TRUE(gang.UiState().revision.load(std::memory_order_relaxed) == 0u);
    gang.PublishUiState();
    const auto publishedRevision = gang.UiState().revision.load(std::memory_order_acquire);
    REQUIRE_TRUE(publishedRevision == 2u);
    REQUIRE_TRUE((publishedRevision & 1u) == 0u);

    Snapshot snapshot{};
    REQUIRE_TRUE(gang.ReadSnapshot(snapshot));
    REQUIRE_NEAR(snapshot.sampleRate, gang.SampleRate(), 0.0);
    REQUIRE_NEAR(snapshot.roundElapsedSamples, gang.RoundElapsedSamples(), 0.0);
    for (std::size_t voice = 0; voice < 2; ++voice) {
        const auto& actualVoice = gang.Voices()[voice];
        const auto& actualInput = gang.VoiceInputs()[voice];
        const auto& published = snapshot.voices[voice];
        REQUIRE_TRUE(published.state == actualVoice.GetState());
        REQUIRE_NEAR(published.currentStateProgress, actualVoice.CurrentStateProgress(), 0.0);
        REQUIRE_NEAR(published.source, actualVoice.Source(), 0.0f);
        REQUIRE_NEAR(published.target, actualVoice.Target(), 0.0f);
        REQUIRE_NEAR(published.output, actualVoice.Output(), 0.0f);
        REQUIRE_NEAR(published.shape, actualInput.shape, 0.0f);
        REQUIRE_NEAR(published.waitingIncrement, actualInput.waitingIncrement, 0.0);
        REQUIRE_NEAR(published.movingIncrement, actualInput.movingIncrement, 0.0);
        REQUIRE_TRUE(published.color == (voice == 0 ? color0 : color1));
    }
}

TEST_CASE(ganged_random_lfo_snapshot_reader_rejects_odd_revision) {
    synth::GangedRandomLfoUiState<1> state;
    state.sampleRate.store(48000.0, std::memory_order_relaxed);
    state.revision.store(1, std::memory_order_release);

    synth::GangedRandomLfoSnapshot<1> snapshot{.sampleRate = -1.0};
    REQUIRE_TRUE(!state.ReadSnapshot(snapshot));
    REQUIRE_NEAR(snapshot.sampleRate, -1.0, 0.0);
}

TEST_CASE(ganged_random_lfo_snapshot_reader_retries_a_revision_change) {
    synth::GangedRandomLfoUiState<1> state;
    state.sampleRate.store(32000.0, std::memory_order_relaxed);
    state.voices[0].output.store(0.25f, std::memory_order_relaxed);
    state.revision.store(2, std::memory_order_release);

    unsigned callbacks = 0;
    synth::GangedRandomLfoSnapshot<1> snapshot{};
    const bool read = synth::detail::ReadGangedRandomLfoSnapshot(
        state,
        snapshot,
        4,
        [&](unsigned attempt) {
            ++callbacks;
            if (attempt == 0) {
                state.revision.store(3, std::memory_order_release);
                state.sampleRate.store(96000.0, std::memory_order_relaxed);
                state.voices[0].output.store(0.75f, std::memory_order_relaxed);
                state.revision.store(4, std::memory_order_release);
            }
        });

    REQUIRE_TRUE(read);
    REQUIRE_TRUE(callbacks == 2);
    REQUIRE_NEAR(snapshot.sampleRate, 96000.0, 0.0);
    REQUIRE_NEAR(snapshot.voices[0].output, 0.75f, 0.0f);
}

TEST_CASE(ganged_random_lfo_snapshot_reader_exhausts_bounded_retries) {
    synth::GangedRandomLfoUiState<1> state;
    state.sampleRate.store(44100.0, std::memory_order_relaxed);
    state.revision.store(2, std::memory_order_release);

    constexpr unsigned retries = 3;
    unsigned callbacks = 0;
    synth::GangedRandomLfoSnapshot<1> snapshot{.sampleRate = -1.0};
    const bool read = synth::detail::ReadGangedRandomLfoSnapshot(
        state,
        snapshot,
        retries,
        [&](unsigned) {
            ++callbacks;
            const auto revision = state.revision.load(std::memory_order_relaxed);
            state.revision.store(revision + 2, std::memory_order_release);
        });

    REQUIRE_TRUE(!read);
    REQUIRE_TRUE(callbacks == retries);
    REQUIRE_NEAR(snapshot.sampleRate, -1.0, 0.0);
    REQUIRE_TRUE(!state.ReadSnapshot(snapshot, 0));
}

TEST_CASE(nary_numbers_are_elementwise_and_have_aliases) {
    synth::StereoFloat a{{1.25f, -0.25f}};
    synth::StereoFloat b{{0.75f, 0.5f}};
    const auto sum = a + b;
    REQUIRE_NEAR(sum[0], 2.0f, 0.0001f);
    REQUIRE_NEAR(sum[1], 0.25f, 0.0001f);
    const auto scaled = sum * 2.0f;
    REQUIRE_NEAR(scaled.Average(), 2.25f, 0.0001f);
    const auto wrapped = a.ModOne();
    REQUIRE_NEAR(wrapped[0], 0.25f, 0.0001f);
    REQUIRE_NEAR(wrapped[1], 0.75f, 0.0001f);
    REQUIRE_TRUE(synth::QuadDouble::Count() == 4);
}

TEST_CASE(classic_svf_blend_selects_low_band_and_high_outputs) {
    auto process = [](float blend) {
        synth::ClassicStateVariableFilter filter;
        filter.Process({.value = 0.75f, .cutoff = 0.05f, .resonance = 0.9f, .blend = blend});
        return filter;
    };

    const auto low = process(-1.0f);
    REQUIRE_NEAR(low.m_output, low.m_low, 0.0001f);

    const auto band = process(0.0f);
    REQUIRE_NEAR(band.m_output, band.m_band, 0.0001f);

    const auto high = process(1.0f);
    REQUIRE_NEAR(high.m_output, high.m_high, 0.0001f);

    const auto lowBlend = process(-0.6f);
    REQUIRE_NEAR(lowBlend.m_output, lowBlend.m_low * 0.6f + lowBlend.m_band * 0.8f, 0.0001f);

    const auto highBlend = process(0.8f);
    REQUIRE_NEAR(highBlend.m_output, highBlend.m_high * 0.8f + highBlend.m_band * 0.6f, 0.0001f);
}

TEST_CASE(classic_svf_low_pass_converges_and_high_resonance_stays_finite) {
    synth::ClassicStateVariableFilter lowPass;
    for (int i = 0; i < 2048; ++i) {
        lowPass.Process({.value = 1.0f, .cutoff = 1000.0f / 48000.0f, .resonance = 0.707f, .blend = -1.0f});
    }
    REQUIRE_NEAR(lowPass.m_output, 1.0f, 0.01f);

    for (const float cutoff : {20.0f / 48000.0f, 20000.0f / 48000.0f}) {
        synth::ClassicStateVariableFilter filter;
        for (int i = 0; i < 256; ++i) {
            filter.Process({.value = 0.25f, .cutoff = cutoff, .resonance = 5.5f, .blend = 0.35f});
            REQUIRE_TRUE(std::isfinite(filter.m_low));
            REQUIRE_TRUE(std::isfinite(filter.m_band));
            REQUIRE_TRUE(std::isfinite(filter.m_high));
            REQUIRE_TRUE(std::isfinite(filter.m_output));
        }
    }
}

TEST_CASE(classic_svf_ui_state_publishes_finite_blended_transfer_function) {
    synth::ClassicStateVariableFilter filter;
    filter.Process({.value = 0.5f, .cutoff = 440.0f / 48000.0f, .resonance = 1.25f, .blend = -0.25f});

    synth::ClassicStateVariableFilter::UIState ui;
    filter.PopulateUIState(ui);
    REQUIRE_NEAR(ui.cutoff.load(), filter.m_cutoff, 0.0001f);
    REQUIRE_NEAR(ui.resonance.load(), filter.m_resonance, 0.0001f);
    REQUIRE_NEAR(ui.blend.load(), filter.m_blend, 0.0001f);

    for (const float frequency : {0.0f, 0.01f, 0.125f, 0.45f}) {
        const float response = ui.FrequencyResponse(frequency);
        const auto transfer = ui.TransferFunctionValue(frequency);
        REQUIRE_TRUE(std::isfinite(response));
        REQUIRE_TRUE(std::isfinite(transfer.real()));
        REQUIRE_TRUE(std::isfinite(transfer.imag()));
    }
}

TEST_CASE(biquad_section_reset_clears_delayed_state) {
    synth::BiquadSection section;
    section.SetLowPassCoefficients(0.08f, 0.70710678f);

    section.Process({.value = 1.0f});
    const float ringingOutput = section.Process({.value = 0.0f});
    REQUIRE_TRUE(std::abs(ringingOutput) > 0.0001f);

    section.Reset();
    REQUIRE_NEAR(section.m_x1, 0.0f, 0.0001f);
    REQUIRE_NEAR(section.m_x2, 0.0f, 0.0001f);
    REQUIRE_NEAR(section.m_y1, 0.0f, 0.0001f);
    REQUIRE_NEAR(section.m_y2, 0.0f, 0.0001f);
    REQUIRE_NEAR(section.Process({.value = 0.0f}), 0.0f, 0.0001f);
}

TEST_CASE(butterworth_filter_attenuates_above_cutoff_more_than_below_cutoff) {
    auto outputRms = [](float frequency) {
        synth::ButterworthFilter filter;
        filter.SetCutoff(0.08f);

        double energy = 0.0;
        int measured = 0;
        for (int i = 0; i < 2048; ++i) {
            const float sample = synth::DefaultDspMath::Sin2Pi(frequency * static_cast<float>(i));
            const float output = filter.Process({.value = sample, .cutoff = 0.08f});
            if (i >= 512) {
                energy += static_cast<double>(output) * static_cast<double>(output);
                ++measured;
            }
        }
        return std::sqrt(energy / static_cast<double>(measured));
    };

    const double lowFrequencyRms = outputRms(0.02f);
    const double highFrequencyRms = outputRms(0.30f);
    REQUIRE_TRUE(lowFrequencyRms > highFrequencyRms * 20.0);
}

TEST_CASE(linkwitz_riley_crossover_transfer_function_recombines_to_unity) {
    for (const float frequency : {0.01f, 0.05f, 0.10f, 0.20f, 0.40f}) {
        const auto transfer = synth::LinkwitzRileyCrossover::TransferFunction(0.10f, frequency);
        REQUIRE_TRUE(std::isfinite(transfer.lowPass.real()));
        REQUIRE_TRUE(std::isfinite(transfer.lowPass.imag()));
        REQUIRE_TRUE(std::isfinite(transfer.highPass.real()));
        REQUIRE_TRUE(std::isfinite(transfer.highPass.imag()));
        REQUIRE_NEAR(std::abs(transfer.lowPass + transfer.highPass), 1.0f, 0.015f);
    }
}

TEST_CASE(one_pole_filters_and_tanh_follow_dsp_contract) {
    synth::OnePoleLowPass lp;
    synth::OnePoleLowPass::Input lpInput{.value = 1.0f, .cutoff = 0.05f};
    float previous = 0.0f;
    for (int i = 0; i < 32; ++i) {
        const float next = lp.Process(lpInput);
        REQUIRE_TRUE(next >= previous);
        previous = next;
    }
    REQUIRE_TRUE(lp.m_output > 0.9f);

    const float alpha = synth::OnePoleLowPass::AlphaFromNatFreq(1000.0f / 48000.0f);
    synth::OnePoleLowPass cutoffPath;
    synth::OnePoleLowPass alphaPath;
    REQUIRE_NEAR(cutoffPath.Process({.value = 0.75f, .cutoff = 1000.0f / 48000.0f}),
                 alphaPath.ProcessWithAlpha(0.75f, alpha), 0.000001f);
    alphaPath.Reset(0.4f);
    REQUIRE_NEAR(alphaPath.m_output, 0.4f, 0.000001f);

    synth::OnePoleLowPass firstSharedAlphaPath;
    synth::OnePoleLowPass secondSharedAlphaPath;
    REQUIRE_NEAR(firstSharedAlphaPath.ProcessWithAlpha(0.25f, alpha), alpha * 0.25f, 0.000001f);
    REQUIRE_NEAR(secondSharedAlphaPath.ProcessWithAlpha(0.75f, alpha), alpha * 0.75f, 0.000001f);

    synth::OnePoleHighPass hp;
    synth::OnePoleHighPass::Input hpInput{.value = 1.0f, .cutoff = 0.05f};
    for (int i = 0; i < 128; ++i) {
        hp.Process(hpInput);
    }
    REQUIRE_NEAR(hp.m_output, 0.0f, 0.002f);

    synth::OnePoleLowPass::UIState ui;
    lp.PopulateUIState(ui);
    REQUIRE_TRUE(ui.FrequencyResponse(0.0f) > ui.FrequencyResponse(0.45f));

    synth::TanhSaturator<> tanh;
    REQUIRE_NEAR(synth::TanhSaturator<>::RawApprox(0.5f), 0.5f * (27.0f + 0.25f) / (27.0f + 2.25f), 0.0001f);
    REQUIRE_NEAR(tanh.Process({.value = 100.0f, .gain = 1.0f}), 1.0f, 0.0001f);
    REQUIRE_NEAR(tanh.Process({.value = -100.0f, .gain = 1.0f}), -1.0f, 0.0001f);
}

TEST_CASE(scope_reserves_flat_channels_and_publishes_stable_readers) {
    synth::ScopeWriter writer(4, 16);
    auto first = writer.ReserveChans(2);
    auto second = writer.ReserveChans(2);
    REQUIRE_TRUE(first.BaseChan() == 0);
    REQUIRE_TRUE(second.BaseChan() == 2);

    first.Write(1, 0.25f);
    writer.AdvanceIndex();
    first.Write(1, 0.5f);
    writer.Publish();
    writer.AdvanceIndex();
    first.Write(1, 1.0f);

    REQUIRE_NEAR(writer.ReadSample(first.FlatChan(1), 0), 0.25f, 0.0001f);
    REQUIRE_TRUE(writer.PublishedIndex() == 1);

    second.RecordStart(0);
    second.Write(0, -0.5f);
    writer.AdvanceIndex();
    second.Write(0, 0.5f);
    second.RecordEnd(0);
    writer.Publish();
    synth::ScopeReader reader(&writer, second.FlatChan(), 8);
    REQUIRE_TRUE(!reader.Empty());
    REQUIRE_NEAR(reader.Get(0), -0.5f, 0.0001f);
    REQUIRE_TRUE(reader.TransferXSample() <= reader.NumXSamples());
}

TEST_CASE(scope_reader_uses_floating_point_sample_coordinates) {
    synth::ScopeWriter writer(1, 32);
    auto holder = writer.ReserveChans(1);

    holder.RecordStart();
    holder.Write(0.0f);
    writer.AdvanceIndex();
    holder.Write(10.0f);
    writer.AdvanceIndex();
    holder.Write(20.0f);
    writer.AdvanceIndex();
    holder.RecordEnd();
    writer.Publish();

    synth::ScopeReader reader(&writer, holder.FlatChan(), 3);
    REQUIRE_TRUE(!reader.Empty());
    REQUIRE_NEAR(reader.Get(0.5), 5.0f, 0.0001f);
    REQUIRE_NEAR(reader.Get(1.5), 15.0f, 0.0001f);
}

TEST_CASE(scope_reader_exposes_floating_point_sampling_api) {
    static_assert(std::is_same_v<decltype(&synth::ScopeReader::Get), float (synth::ScopeReader::*)(double) const>);
    static_assert(std::is_same_v<decltype(std::declval<const synth::ScopeReader&>().TransferXSample()), double>);
}

TEST_CASE(scope_reader_stitches_previous_cycle_after_latest_partial_cycle) {
    synth::ScopeWriter writer(1, 64);
    auto holder = writer.ReserveChans(1);

    holder.RecordStart();
    for (std::size_t ix = 0; ix < 10; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }

    holder.RecordStart();
    for (std::size_t ix = 10; ix < 15; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }
    writer.Publish();

    synth::ScopeReader reader(&writer, holder.FlatChan(), 10);
    REQUIRE_TRUE(!reader.Empty());
    REQUIRE_NEAR(reader.TransferXSample(), 4.0f, 0.0001f);
    REQUIRE_NEAR(reader.Get(0), 10.0f, 0.0001f);
    REQUIRE_NEAR(reader.Get(3.5), 13.5f, 0.0001f);
    REQUIRE_NEAR(reader.Get(4.5), 4.5f, 0.0001f);
    REQUIRE_NEAR(reader.Get(9), 9.0f, 0.0001f);
}

TEST_CASE(scope_reader_ignores_unpublished_cycle_start_markers) {
    synth::ScopeWriter writer(1, 64);
    auto holder = writer.ReserveChans(1);

    holder.RecordStart();
    for (std::size_t ix = 0; ix < 10; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }
    holder.RecordStart();
    for (std::size_t ix = 10; ix < 15; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }
    writer.Publish();

    const synth::ScopeReader published(&writer, holder.FlatChan(), 10);
    REQUIRE_TRUE(!published.Empty());
    REQUIRE_NEAR(published.Get(0), 10.0f, 0.0001f);

    holder.RecordStart();
    for (std::size_t ix = 15; ix < 18; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }

    const synth::ScopeReader whileWriting(&writer, holder.FlatChan(), 10);
    REQUIRE_TRUE(!whileWriting.Empty());
    REQUIRE_NEAR(whileWriting.Get(0), published.Get(0), 0.0001f);

    writer.Publish();
    const synth::ScopeReader afterPublish(&writer, holder.FlatChan(), 10);
    REQUIRE_TRUE(!afterPublish.Empty());
    REQUIRE_NEAR(afterPublish.Get(0), 15.0f, 0.0001f);
}

TEST_CASE(scope_reader_aligns_fractional_start_markers) {
    synth::ScopeWriter writer(1, 64);
    auto holder = writer.ReserveChans(1);

    holder.RecordStart(0, 0.25);
    for (std::size_t ix = 0; ix < 10; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }

    holder.RecordStart(0, 0.25);
    for (std::size_t ix = 10; ix < 15; ++ix) {
        holder.Write(static_cast<float>(ix));
        writer.AdvanceIndex();
    }
    writer.Publish();

    synth::ScopeReader reader(&writer, holder.FlatChan(), 10);
    REQUIRE_TRUE(!reader.Empty());
    REQUIRE_NEAR(reader.Get(0), 10.25f, 0.0001f);
    REQUIRE_NEAR(reader.Get(4.0), 4.25f, 0.0001f);
}

TEST_CASE(wavetables_wrap_adapt_morph_and_provide_defaults) {
    synth::BasicWavetable<8> table;
    table.Write(0, 1.0f);
    table.Write(1, 0.0f);
    REQUIRE_NEAR(table.Evaluate(0.0f), table.Evaluate(1.0f), 0.0001f);

    auto sine = synth::MakeAdaptiveWavetable(synth::BasicWavetable<8>::Sine());
    REQUIRE_NEAR(sine.Evaluate(0.0f, 0.01f, 0.5f), 0.0f, 0.02f);

    synth::MorphingWavetable<8> morph;
    morph.Add(synth::MakeAdaptiveWavetable(synth::BasicWavetable<8>::Sine()));
    morph.Add(synth::MakeAdaptiveWavetable(synth::BasicWavetable<8>::Square()));
    REQUIRE_TRUE(morph.Size() == 2);
    const float left = morph.Evaluate(0.25f, 0.01f, 0.5f, 0.0f);
    const float right = morph.Evaluate(0.25f, 0.01f, 0.5f, 1.0f);
    const float middle = morph.Evaluate(0.25f, 0.01f, 0.5f, 0.5f);
    REQUIRE_NEAR(middle, (left + right) * 0.5f, 0.02f);

    const auto defaultMorph = synth::MakeDefaultMorphingWavetable<8>();
    REQUIRE_TRUE(defaultMorph.Size() == 4);
}

TEST_CASE(discrete_fourier_transform_normalizes_aligned_cosine_bins) {
    synth::BasicWavetable<10> table;
    for (std::size_t i = 0; i < synth::DspMath<10>::kTableSize; ++i) {
        const float phase = 8.0f * static_cast<float>(i) / static_cast<float>(synth::DspMath<10>::kTableSize);
        table.Write(i, synth::DspMath<10>::Cos2Pi(phase));
    }

    synth::DiscreteFourierTransform<10> dft;
    dft.Transform(table);

    REQUIRE_TRUE(dft.kTableSize == synth::DspMath<10>::kTableSize);
    REQUIRE_TRUE(dft.kMaxComponents == synth::DspMath<10>::kTableSize / 2);
    RequireComplexNear(dft.m_components[8], {0.5f, 0.0f}, 0.002f, "dft.m_components[8]");
    RequireNear(std::abs(dft.m_components[8]), 0.5f, 0.002f, "std::abs(dft.m_components[8])");
}

TEST_CASE(discrete_fourier_transform_inverse_reconstructs_aligned_cosine) {
    synth::BasicWavetable<10> source;
    for (std::size_t i = 0; i < synth::DspMath<10>::kTableSize; ++i) {
        const float phase = 8.0f * static_cast<float>(i) / static_cast<float>(synth::DspMath<10>::kTableSize);
        source.Write(i, synth::DspMath<10>::Cos2Pi(phase));
    }

    synth::DiscreteFourierTransform<10> dft;
    dft.Init();
    dft.Transform(source);

    synth::BasicWavetable<10> reconstructed;
    dft.InverseTransform(reconstructed, 8);

    for (const std::size_t index : {std::size_t{0}, std::size_t{17}, std::size_t{128}, std::size_t{511}}) {
        RequireNear(reconstructed.m_table[index], source.m_table[index], 0.002f, "reconstructed cosine sample");
        RequireFinite(reconstructed.m_table[index], "reconstructed cosine sample");
    }
}

TEST_CASE(discrete_fourier_transform_writes_windowed_partial_at_exact_bin) {
    synth::DiscreteFourierTransform<10> dft;
    dft.Init();
    dft.WriteWindowedPartial(0.0f, 1.0f, 8.0f / static_cast<float>(synth::DspMath<10>::kTableSize));

    RequireComplexNear(dft.m_components[8], {0.5f, 0.0f}, 0.002f, "dft.m_components[8]");

    synth::BasicWavetable<10> table;
    dft.InverseTransform(table, dft.kMaxComponents);
    RequireFinite(table.m_table[0], "table.m_table[0]");
}

TEST_CASE(ola_overlap_add_outputs_finite_frame_and_clears_samples_after_read) {
    synth::DiscreteFourierTransform<8> dft;
    dft.Init();
    dft.m_components[4] = {0.5f, 0.0f};

    synth::Ola<8> ola;
    ola.Write(dft);

    REQUIRE_TRUE(synth::Ola<8>::kHopDenom == 4);
    REQUIRE_TRUE(synth::Ola<8>::kTableSize == synth::DspMath<8>::kTableSize);
    REQUIRE_TRUE(synth::Ola<8>::kHopSize == synth::Ola<8>::kTableSize / 4);
    REQUIRE_NEAR(ola.Process(), 1.0f, 0.002f);
    REQUIRE_NEAR(ola.Process(), synth::DspMath<8>::Cos2Pi(4.0f / static_cast<float>(synth::Ola<8>::kTableSize)), 0.002f);

    for (std::size_t i = 2; i < synth::Ola<8>::kTableSize; ++i) {
        RequireFinite(ola.Process(), "ola.Process()");
    }
    REQUIRE_NEAR(ola.Process(), 0.0f, 0.0001f);
}

TEST_CASE(nary_ola_preserves_channel_independence) {
    synth::NaryDftFrame<8, 4> frame;
    frame.Init();
    frame.AddComponent(4, {0.5f, 0.0f}, synth::QuadFloat{{1.0f, 0.0f, 0.5f, -1.0f}});

    synth::NaryOla<8, 4> ola;
    ola.Write(frame);
    const auto output = ola.Process();

    REQUIRE_NEAR(output[0], 1.0f, 0.002f);
    REQUIRE_NEAR(output[1], 0.0f, 0.002f);
    REQUIRE_NEAR(output[2], 0.5f, 0.002f);
    REQUIRE_NEAR(output[3], -1.0f, 0.002f);
}

TEST_CASE(ola_resynthesizer_primes_finite_analysis_state) {
    synth::BasicWavetable<8> previousFrame;
    previousFrame.Write(0, 1.0f);
    previousFrame.Write(1, 0.5f);

    synth::OlaResynthesizer<8> resynth;
    resynth.PrimeAnalysis(previousFrame);

    REQUIRE_TRUE(synth::OlaResynthesizer<8>::kHopDenom == 4);
    REQUIRE_TRUE(synth::OlaResynthesizer<8>::kHopSize == synth::Ola<8>::kHopSize);
    for (std::size_t bin = 1; bin < synth::OlaResynthesizer<8>::kMaxComponents; ++bin) {
        RequireFinite(resynth.m_prevAnalysisMagnitudes[bin], "resynth.m_prevAnalysisMagnitudes[bin]");
        RequireFinite(resynth.m_prevAnalysisPhases[bin], "resynth.m_prevAnalysisPhases[bin]");
    }
}

TEST_CASE(ola_resynthesizer_process_hop_writes_ola_frame) {
    auto makeFrame = [](std::size_t bin) {
        synth::DiscreteFourierTransform<8> dft;
        dft.Init();
        dft.m_components[bin] = {0.5f, 0.0f};
        synth::BasicWavetable<8> frame;
        dft.InverseTransform(frame, dft.kMaxComponents);
        return frame;
    };

    synth::OlaResynthesizer<8> resynth;
    synth::OlaResynthesizer<8>::Input input;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;
    resynth.PrimeAnalysis(makeFrame(8));
    resynth.ProcessHop(makeFrame(8), input);

    REQUIRE_TRUE(std::abs(resynth.Process()) > 0.1f);
    for (std::size_t i = 1; i < synth::OlaResynthesizer<8>::kTableSize; ++i) {
        RequireFinite(resynth.Process(), "resynth.Process()");
    }
}

TEST_CASE(ola_resynthesizer_uses_pitch_ratio_directly) {
    auto makeFrame = [] {
        synth::DiscreteFourierTransform<8> dft;
        dft.Init();
        dft.m_components[8] = {0.5f, 0.0f};
        synth::BasicWavetable<8> frame;
        dft.InverseTransform(frame, dft.kMaxComponents);
        return frame;
    };

    synth::OlaResynthesizer<8> resynth;
    synth::OlaResynthesizer<8>::Input input;
    input.m_pitchRatio = 2.0f;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;
    resynth.PrimeAnalysis(makeFrame());
    resynth.ProcessHop(makeFrame(), input);

    REQUIRE_TRUE(std::abs(resynth.m_lastSynthesisDft.m_components[16]) > 0.4f);
    REQUIRE_TRUE(std::abs(resynth.m_lastSynthesisDft.m_components[8]) < 0.05f);
}

TEST_CASE(ola_resynthesizer_unison_outputs_remain_finite_with_gain) {
    synth::BasicWavetable<8> frame = synth::BasicWavetable<8>::Sine();

    synth::OlaResynthesizer<8> resynth;
    synth::OlaResynthesizer<8>::Input input;
    input.m_unisonGain = 0.75f;
    input.m_unisonDetune = 1.01f;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;
    resynth.PrimeAnalysis(frame);
    resynth.ProcessHop(frame, input);

    for (std::size_t i = 0; i < synth::OlaResynthesizer<8>::kHopSize * 2; ++i) {
        RequireFinite(resynth.Process(), "resynth.Process()");
    }
}

TEST_CASE(ola_resynthesizer_slews_magnitude_motion) {
    auto makeFrame = [](float magnitude) {
        synth::DiscreteFourierTransform<8> dft;
        dft.Init();
        dft.m_components[8] = {magnitude, 0.0f};
        synth::BasicWavetable<8> frame;
        dft.InverseTransform(frame, dft.kMaxComponents);
        return frame;
    };

    synth::OlaResynthesizer<8> resynth;
    synth::OlaResynthesizer<8>::Input input;
    input.m_slewUpAlpha = 0.25f;
    input.m_slewDownAlpha = 0.5f;

    resynth.PrimeAnalysis(makeFrame(0.0f));
    resynth.ProcessHop(makeFrame(0.5f), input);
    REQUIRE_NEAR(resynth.m_synthesisMagnitudes[8], 0.125f, 0.01f);

    resynth.ProcessHop(makeFrame(0.0f), input);
    REQUIRE_NEAR(resynth.m_synthesisMagnitudes[8], 0.0625f, 0.01f);
}

TEST_CASE(ola_resynthesizer_spectral_distortion_outputs_finite_frame) {
    synth::DiscreteFourierTransform<8> dft;
    dft.Init();
    dft.m_components[8] = {0.5f, 0.0f};
    dft.m_components[16] = {0.25f, 0.0f};
    synth::BasicWavetable<8> frame;
    dft.InverseTransform(frame, dft.kMaxComponents);

    synth::OlaResynthesizer<8> resynth;
    synth::OlaResynthesizer<8>::Input input;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;
    input.m_useSpectralDistortion = true;
    input.m_spectralThreshold = 0.01f;
    input.m_spectralQuiet = 0.5f;
    input.m_spectralLoud = 0.5f;
    input.m_spectralShiftAmount = 0.5f;
    input.m_spectralShiftPitchRatio = 1.5f;
    resynth.PrimeAnalysis(frame);
    resynth.ProcessHop(frame, input);

    for (std::size_t i = 0; i < synth::OlaResynthesizer<8>::kTableSize; ++i) {
        RequireFinite(resynth.Process(), "resynth.Process()");
    }
}

TEST_CASE(ola_resynthesizer_public_api_has_no_grain_dependencies) {
    using Resynth = synth::OlaResynthesizer<8>;
    static_assert(std::is_same_v<decltype(&Resynth::PrimeAnalysis), void (Resynth::*)(const synth::BasicWavetable<8>&)>);
    static_assert(std::is_same_v<decltype(&Resynth::ProcessHop), void (Resynth::*)(const synth::BasicWavetable<8>&, const Resynth::Input&)>);
    static_assert(std::is_same_v<decltype(&Resynth::Process), float (Resynth::*)()>);
    REQUIRE_TRUE(std::is_default_constructible_v<Resynth::Input>);
}

TEST_CASE(spectral_model_extracts_local_maxima_as_tracked_atoms) {
    synth::DiscreteFourierTransform<8> dft;
    dft.Init();
    dft.m_components[7] = {0.08f, 0.0f};
    dft.m_components[8] = {0.5f, 0.0f};
    dft.m_components[9] = {0.10f, 0.0f};
    synth::BasicWavetable<8> table;
    dft.InverseTransform(table, dft.kMaxComponents);

    synth::SpectralModel<8> model;
    synth::SpectralModel<8>::Input input;
    input.m_gainThreshold = 0.05f;
    input.m_numAtoms = 8;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;
    input.m_omegaPortamentoAlpha = 1.0f;

    model.ExtractAtoms(table, input);

    REQUIRE_TRUE(!model.m_atoms.empty());
    REQUIRE_NEAR(model.m_atoms.front().m_analysisOmega, 8.0f / static_cast<float>(synth::SpectralModel<8>::kTableSize), 0.001f);
    REQUIRE_TRUE(model.m_atoms.front().m_analysisMagnitude > 0.45f);
    REQUIRE_NEAR(model.m_atoms.front().m_synthesisMagnitude, model.m_atoms.front().m_analysisMagnitude, 0.0001f);
}

TEST_CASE(spectral_model_tracks_atoms_with_slew_and_portamento_alphas) {
    synth::SpectralModel<8> model;
    synth::SpectralModel<8>::Input input;
    input.m_gainThreshold = 0.01f;
    input.m_numAtoms = 4;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 0.25f;
    input.m_omegaPortamentoAlpha = 0.5f;
    input.m_omegaDensity = 4.0f / static_cast<float>(synth::SpectralModel<8>::kTableSize);

    auto makeFrame = [](std::size_t bin, float magnitude) {
        synth::DiscreteFourierTransform<8> dft;
        dft.Init();
        dft.m_components[bin] = {magnitude, 0.0f};
        synth::BasicWavetable<8> table;
        dft.InverseTransform(table, dft.kMaxComponents);
        return table;
    };

    model.ExtractAtoms(makeFrame(8, 0.5f), input);
    REQUIRE_TRUE(!model.m_atoms.empty());
    const float firstOmega = model.m_atoms.front().m_synthesisOmega;
    model.ExtractAtoms(makeFrame(9, 0.25f), input);

    REQUIRE_TRUE(model.m_atoms.size() == 1);
    REQUIRE_NEAR(model.m_atoms.front().m_synthesisMagnitude, 0.4375f, 0.02f);
    REQUIRE_TRUE(model.m_atoms.front().m_synthesisOmega > firstOmega);
    REQUIRE_TRUE(model.m_atoms.front().m_synthesisOmega < 9.0f / static_cast<float>(synth::SpectralModel<8>::kTableSize));
}

TEST_CASE(spectral_model_adds_synthetic_harmonics_when_enabled) {
    synth::DiscreteFourierTransform<8> dft;
    dft.Init();
    dft.m_components[6] = {0.5f, 0.0f};
    synth::BasicWavetable<8> table;
    dft.InverseTransform(table, dft.kMaxComponents);

    synth::SpectralModel<8> model;
    synth::SpectralModel<8>::Input input;
    input.m_gainThreshold = 0.01f;
    input.m_numAtoms = 8;
    input.m_slewUpAlpha = 1.0f;
    input.m_useSyntheticHarmonics = true;
    input.m_syntheticHarmonics[0] = 0.5f;

    model.ExtractAtoms(table, input);

    bool sawSynthetic = false;
    for (const auto& atom : model.m_atoms) {
        if (atom.m_isSynthetic) {
            sawSynthetic = true;
            REQUIRE_NEAR(atom.m_analysisOmega, 12.0f / static_cast<float>(synth::SpectralModel<8>::kTableSize), 0.001f);
            REQUIRE_TRUE(atom.m_analysisMagnitude > 0.20f);
        }
    }
    REQUIRE_TRUE(sawSynthetic);
}

TEST_CASE(spectral_model_extracts_residual_after_canceling_detected_atoms) {
    synth::DiscreteFourierTransform<8> dft;
    dft.Init();
    dft.m_components[8] = {0.5f, 0.0f};
    dft.m_components[20] = {0.125f, 0.0f};
    synth::BasicWavetable<8> table;
    dft.InverseTransform(table, dft.kMaxComponents);

    synth::SpectralModel<8> model;
    synth::SpectralModel<8>::Input input;
    input.m_gainThreshold = 0.25f;
    input.m_numAtoms = 4;
    input.m_slewUpAlpha = 1.0f;
    input.m_slewDownAlpha = 1.0f;

    model.ExtractAtomsAndResidual(table, input);

    REQUIRE_TRUE(!model.m_atoms.empty());
    REQUIRE_TRUE(model.m_residualModel.GetEnvelope(8) < 0.05f);
    REQUIRE_TRUE(model.m_residualModel.GetEnvelope(20) > 0.10f);
    RequireFinite(model.m_residualModel.GetEnvelope(20), "residual envelope");
}

TEST_CASE(incrementer_accumulates_total_phase_and_reports_top) {
    synth::Incrementer incrementer;
    incrementer.Process({.freq = 0.75});
    REQUIRE_NEAR(static_cast<float>(incrementer.m_phase), 0.75f, 0.0001f);
    REQUIRE_TRUE(!incrementer.m_top);
    incrementer.Process({.freq = 0.5});
    REQUIRE_NEAR(static_cast<float>(incrementer.m_phase), 1.25f, 0.0001f);
    REQUIRE_NEAR(static_cast<float>(incrementer.m_wrappedPhase), 0.25f, 0.0001f);
    REQUIRE_TRUE(incrementer.m_top);
}

TEST_CASE(incrementer_reports_fractional_top_offset) {
    synth::Incrementer incrementer;
    incrementer.m_phase = 0.75;

    incrementer.Process({.freq = 0.5});

    REQUIRE_TRUE(incrementer.m_top);
    REQUIRE_NEAR(static_cast<float>(incrementer.m_topOffset), 0.5f, 0.0001f);
}

TEST_CASE(lfo_shape_processes_triangle_shape_phase_distortion_wrap_and_exponent) {
    REQUIRE_NEAR(synth::LFOShape::Tri(0.0f), 0.0f, 0.0001f);
    REQUIRE_NEAR(synth::LFOShape::Tri(0.5f), 1.0f, 0.0001f);
    REQUIRE_NEAR(synth::LFOShape::Tri(1.0f), 0.0f, 0.0001f);

    const float curved = synth::LFOShape::Shape(0.0f, 0.5f);
    REQUIRE_NEAR(curved, std::sin(3.14159265358979323846f * 0.25f), 0.0001f);
    REQUIRE_NEAR(synth::LFOShape::Shape(0.5f, 0.25f), 0.25f, 0.0001f);
    REQUIRE_NEAR(synth::LFOShape::Shape(0.999f, 0.25f), 0.0f, 0.0001f);
    REQUIRE_NEAR(synth::LFOShape::Shape(0.999f, 0.75f), 1.0f, 0.0001f);

    REQUIRE_NEAR(synth::LFOShape::PD(0.5f, 0.25f), 0.25f, 0.0001f);
    const float earlyPeak = synth::LFOShape::Process({
        .inPhase = 0.25f,
        .shape = 0.5f,
        .phaseOffset = 0.0f,
        .skew = 0.25f,
        .exponent = 1.0f,
    });
    REQUIRE_NEAR(earlyPeak, 1.0f, 0.0001f);

    const float wrappedNegative = synth::LFOShape::Process({
        .inPhase = -0.25f,
        .shape = 0.5f,
        .phaseOffset = 0.0f,
        .skew = 0.5f,
        .exponent = 1.0f,
    });
    REQUIRE_NEAR(wrappedNegative, 0.5f, 0.0001f);

    const float squared = synth::LFOShape::Process({
        .inPhase = 0.125f,
        .shape = 0.5f,
        .phaseOffset = 0.0f,
        .skew = 0.5f,
        .exponent = 2.0f,
    });
    REQUIRE_NEAR(squared, 0.0625f, 0.0001f);
}

TEST_CASE(basic_lfo_processor_advances_writes_scope_markers_and_publishes_ui_state) {
    synth::ScopeWriter writer(1, 32);
    auto holder = writer.ReserveChans(1);
    synth::BasicLFOProcessor lfo;
    lfo.SetScopeWriterHolder(&holder);
    lfo.SetScopeColor(synth::Color::Yellow);

    synth::BasicLFOProcessor::Input input{
        .frequency = 0.25,
        .shape = {
            .inPhase = 0.0f,
            .shape = 0.5f,
            .phaseOffset = 0.0f,
            .skew = 0.5f,
            .exponent = 1.0f,
        },
    };

    REQUIRE_NEAR(lfo.Process(input), 0.5f, 0.0001f);
    REQUIRE_NEAR(writer.ReadSample(holder.FlatChan(), 0), 0.5f, 0.0001f);
    writer.AdvanceIndex();
    REQUIRE_NEAR(lfo.Process(input), 1.0f, 0.0001f);
    writer.AdvanceIndex();
    REQUIRE_NEAR(lfo.Process(input), 0.5f, 0.0001f);
    writer.AdvanceIndex();
    REQUIRE_NEAR(lfo.Process(input), 0.0f, 0.0001f);

    double latestStart = -1.0;
    REQUIRE_TRUE(!writer.LatestStart(holder.FlatChan(), latestStart));
    writer.Publish();
    REQUIRE_TRUE(writer.LatestStart(holder.FlatChan(), latestStart));
    REQUIRE_NEAR(static_cast<float>(latestStart), 3.0f, 0.0001f);

    synth::BasicLFOProcessor::UIState ui;
    lfo.PopulateUIState(ui);
    REQUIRE_TRUE(ui.connected.load());
    REQUIRE_TRUE(ui.scope.load() == &writer);
    REQUIRE_TRUE(ui.scopeChannel.load() == holder.FlatChan());
    REQUIRE_TRUE(ui.scopeColor.Load() == synth::Color::Yellow);
}

TEST_CASE(wavetable_vco_records_top_marker_at_true_cycle_boundary) {
    synth::ScopeWriter writer(2, 32);
    auto holder = writer.ReserveChans(1);
    synth::WavetableVco<8> vco;
    vco.SetScopeWriterHolder(&holder);

    vco.m_incrementer.m_phase = 0.7;
    vco.Process({.freq = 0.2, .phaseOffset = 0.0f, .wavetablePosition = 0.0f, .maxFreq = 0.5f});
    writer.AdvanceIndex();
    vco.Process({.freq = 0.2, .phaseOffset = 0.0f, .wavetablePosition = 0.0f, .maxFreq = 0.5f});
    writer.AdvanceIndex();
    vco.Process({.freq = 0.2, .phaseOffset = 0.0f, .wavetablePosition = 0.0f, .maxFreq = 0.5f});
    writer.AdvanceIndex();
    writer.Publish();

    synth::ScopeReader reader(&writer, holder.FlatChan(), 4);
    REQUIRE_TRUE(!reader.Empty());
    REQUIRE_NEAR(reader.Get(0), 0.0f, 0.05f);
}

TEST_CASE(wavetable_vco_uses_position_scope_and_ui_state) {
    synth::ScopeWriter writer(2, 32);
    auto holder = writer.ReserveChans(1);
    synth::WavetableVco<8> vco;
    vco.SetScopeWriterHolder(&holder);
    vco.SetScopeColor(synth::Color::Orange);

    synth::WavetableVco<8>::Input input{.freq = 0.25, .phaseOffset = 0.0f, .wavetablePosition = 0.0f, .maxFreq = 0.5f};
    const float first = vco.Process(input);
    writer.AdvanceIndex();
    input.wavetablePosition = 1.0f;
    const float second = vco.Process(input);
    REQUIRE_TRUE(std::abs(first - second) > 0.01f);
    REQUIRE_NEAR(writer.ReadSample(holder.FlatChan(), 0), first, 0.0001f);

    synth::WavetableVco<8>::UIState ui;
    vco.PopulateUIState(ui);
    REQUIRE_TRUE(ui.connected.load());
    REQUIRE_TRUE(ui.scope.load() == &writer);
    REQUIRE_TRUE(ui.scopeChannel.load() == holder.FlatChan());
    REQUIRE_TRUE(ui.scopeColor.Load() == synth::Color::Orange);
}

TEST_CASE(fir_decimator_factor_four_cadence_survives_block_splits) {
    constexpr std::array<double, 1> coefficients{1.0};
    synth::FirDecimator<4, 1, coefficients.size()> decimator(coefficients);

    const std::array<std::size_t, 4> blockSizes{1, 3, 2, 4};
    std::vector<float> emitted;
    std::size_t inputIndex = 0;
    for (const std::size_t blockSize : blockSizes) {
        for (std::size_t i = 0; i < blockSize; ++i) {
            const std::array<float, 1> input{static_cast<float>(inputIndex + 1)};
            std::array<float, 1> output{0.0f};
            if (decimator.ProcessFrame(input, output)) {
                emitted.push_back(output[0]);
            }
            ++inputIndex;
        }
    }

    REQUIRE_TRUE(emitted.size() == 2);
    REQUIRE_NEAR(emitted[0], 4.0f, 0.0001f);
    REQUIRE_NEAR(emitted[1], 8.0f, 0.0001f);
}

TEST_CASE(fir_decimator_stereo_history_is_independent_and_reset_deterministic) {
    constexpr std::array<double, 3> previousSampleCoefficients{0.0, 1.0, 0.0};
    synth::FirDecimator<2, 2, previousSampleCoefficients.size()> decimator(previousSampleCoefficients);

    auto runSequence = [&decimator] {
        std::vector<std::array<float, 2>> emitted;
        for (std::size_t i = 0; i < 6; ++i) {
            const std::array<float, 2> input{
                10.0f + static_cast<float>(i),
                -100.0f - static_cast<float>(i),
            };
            std::array<float, 2> output{0.0f, 0.0f};
            if (decimator.ProcessFrame(input, output)) {
                emitted.push_back(output);
            }
        }
        return emitted;
    };

    const auto firstRun = runSequence();
    REQUIRE_TRUE(firstRun.size() == 3);
    REQUIRE_NEAR(firstRun[0][0], 10.0f, 0.0001f);
    REQUIRE_NEAR(firstRun[0][1], -100.0f, 0.0001f);
    REQUIRE_NEAR(firstRun[1][0], 12.0f, 0.0001f);
    REQUIRE_NEAR(firstRun[1][1], -102.0f, 0.0001f);
    REQUIRE_NEAR(firstRun[2][0], 14.0f, 0.0001f);
    REQUIRE_NEAR(firstRun[2][1], -104.0f, 0.0001f);

    decimator.Reset();
    const auto secondRun = runSequence();
    REQUIRE_TRUE(secondRun == firstRun);
}

TEST_CASE(fir_decimator_exposes_compile_time_shape_contract) {
    using Decimator = synth::FirDecimator<4, 2, 3>;

    static_assert(Decimator::kFactor == 4);
    static_assert(Decimator::kChannels == 2);
    static_assert(Decimator::kTaps == 3);
}

TEST_CASE(fir_decimator_four_to_one_coefficients_are_symmetric_with_expected_group_delay) {
    const auto coefficients = synth::FourToOneDecimatorCoefficients();
    REQUIRE_TRUE(coefficients.size() == 287);

    double dcGain = 0.0;
    for (std::size_t i = 0; i < coefficients.size(); ++i) {
        REQUIRE_NEAR(coefficients[i], coefficients[coefficients.size() - 1 - i], 1.0e-14);
        dcGain += coefficients[i];
    }

    REQUIRE_NEAR(dcGain, 1.0, 1.0e-12);
    REQUIRE_TRUE((coefficients.size() - 1) / 2 == 143);
}

TEST_CASE(fir_decimator_four_to_one_frequency_response_meets_spec) {
    const auto coefficients = synth::FourToOneDecimatorCoefficients();
    REQUIRE_TRUE(coefficients.size() == 287);

    auto responseMagnitude = [coefficients](double normalizedFrequency) {
        std::complex<double> response{0.0, 0.0};
        for (std::size_t i = 0; i < coefficients.size(); ++i) {
            const double phase = -2.0 * std::numbers::pi * normalizedFrequency * static_cast<double>(i);
            response += coefficients[i] * std::complex<double>{std::cos(phase), std::sin(phase)};
        }
        return std::abs(response);
    };

    const double dcMagnitude = responseMagnitude(0.0);
    double minPassbandDb = 0.0;
    double maxPassbandDb = 0.0;
    constexpr std::size_t kPassbandSamples = 256;
    for (std::size_t i = 0; i <= kPassbandSamples; ++i) {
        const double normalizedFrequency = (5.0 / 48.0) * static_cast<double>(i) / kPassbandSamples;
        const double db = 20.0 * std::log10(responseMagnitude(normalizedFrequency) / dcMagnitude);
        minPassbandDb = std::min(minPassbandDb, db);
        maxPassbandDb = std::max(maxPassbandDb, db);
    }

    double maxStopbandDb = -1000.0;
    constexpr std::size_t kStopbandSamples = 512;
    for (std::size_t i = 0; i <= kStopbandSamples; ++i) {
        const double normalizedFrequency = (1.0 / 8.0)
            + (0.5 - (1.0 / 8.0)) * static_cast<double>(i) / kStopbandSamples;
        const double magnitude = std::max(responseMagnitude(normalizedFrequency), 1.0e-300);
        maxStopbandDb = std::max(maxStopbandDb, 20.0 * std::log10(magnitude / dcMagnitude));
    }

    REQUIRE_TRUE(maxPassbandDb - minPassbandDb <= 0.1);
    REQUIRE_TRUE(maxStopbandDb <= -90.0);
}

TEST_CASE(fir_decimator_four_to_one_runtime_path_meets_response_bounds) {
    using Decimator = synth::FirDecimator<4, 1, synth::kFourToOneDecimatorTaps>;

    auto runtimeGain = [](double normalizedFrequency) {
        constexpr std::size_t kWarmupInternalFrames = 48 * 64;
        constexpr std::size_t kMeasuredInternalFrames = 48 * 256;
        Decimator decimator{synth::FourToOneDecimatorCoefficients()};

        double inputEnergy = 0.0;
        double outputEnergy = 0.0;
        std::size_t inputCount = 0;
        std::size_t outputCount = 0;

        for (std::size_t n = 0; n < kWarmupInternalFrames + kMeasuredInternalFrames; ++n) {
            const float sample = static_cast<float>(
                std::sin(2.0 * std::numbers::pi * normalizedFrequency * static_cast<double>(n)));
            const std::array<float, 1> input{sample};
            std::array<float, 1> output{0.0f};
            const bool emitted = decimator.ProcessFrame(input, output);

            if (n >= kWarmupInternalFrames) {
                inputEnergy += static_cast<double>(sample) * static_cast<double>(sample);
                ++inputCount;
                if (emitted) {
                    outputEnergy += static_cast<double>(output[0]) * static_cast<double>(output[0]);
                    ++outputCount;
                }
            }
        }

        REQUIRE_TRUE(inputCount == kMeasuredInternalFrames);
        REQUIRE_TRUE(outputCount == kMeasuredInternalFrames / Decimator::kFactor);
        const double inputRms = std::sqrt(inputEnergy / static_cast<double>(inputCount));
        const double outputRms = std::sqrt(outputEnergy / static_cast<double>(outputCount));
        return outputRms / inputRms;
    };

    const double passbandEdgeGain = runtimeGain(5.0 / 48.0);
    const double passbandEdgeDb = 20.0 * std::log10(passbandEdgeGain);
    REQUIRE_TRUE(passbandEdgeDb >= -0.1);
    REQUIRE_TRUE(passbandEdgeDb <= 0.1);

    const double stopbandEdgeGain = runtimeGain(1.0 / 8.0);
    const double stopbandEdgeDb = 20.0 * std::log10(std::max(stopbandEdgeGain, 1.0e-300));
    REQUIRE_TRUE(stopbandEdgeDb <= -90.0);
}

TEST_CASE(oversampled_output_stage_calls_generator_with_exact_internal_indices) {
    constexpr std::array<double, 1> coefficients{1.0};
    using Decimator = synth::FirDecimator<4, 2, coefficients.size()>;
    synth::OversampledOutputStage<4, 2, Decimator> outputStage{Decimator{coefficients}};

    std::vector<std::uint64_t> visitedIndices;
    const auto output = outputStage.ProcessHostFrame(7, [&visitedIndices](std::uint64_t internalIndex) {
        visitedIndices.push_back(internalIndex);
        return std::array<float, 2>{
            static_cast<float>(internalIndex),
            -static_cast<float>(internalIndex),
        };
    });

    REQUIRE_TRUE((visitedIndices == std::vector<std::uint64_t>{28, 29, 30, 31}));
    REQUIRE_NEAR(output[0], 31.0f, 0.0001f);
    REQUIRE_NEAR(output[1], -31.0f, 0.0001f);
}

TEST_CASE(oversampled_output_stage_preserves_decimator_state_across_host_blocks) {
    constexpr std::array<double, 5> fourSamplesAgoCoefficients{0.0, 0.0, 0.0, 0.0, 1.0};
    using Decimator = synth::FirDecimator<4, 1, fourSamplesAgoCoefficients.size()>;
    synth::OversampledOutputStage<4, 1, Decimator> outputStage{Decimator{fourSamplesAgoCoefficients}};

    auto generator = [](std::uint64_t internalIndex) {
        return std::array<float, 1>{static_cast<float>(internalIndex + 1)};
    };

    const auto first = outputStage.ProcessHostFrame(0, generator);
    const auto second = outputStage.ProcessHostFrame(1, generator);

    REQUIRE_NEAR(first[0], 0.0f, 0.0001f);
    REQUIRE_NEAR(second[0], 4.0f, 0.0001f);
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
