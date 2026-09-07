#include "synth/DspFilters.hpp"
#include "synth/ButtonGrid.hpp"
#include "synth/EncoderDraw.hpp"
#include "synth/MidiController.hpp"
#include "synth/Json.hpp"
#include "synth/ParameterModulation.hpp"
#include "synth/PatchPersistence.hpp"
#include "synth/PortableUI.hpp"
#include "synth/ThreadId.hpp"

#ifdef JUCE_MAJOR_VERSION
#error "synth core tests must not see JUCE headers"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
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

struct TestVisualizer final : synth::ui::Visualizer {
    std::vector<synth::ui::DrawCommand> DrawVisible() const override { return {}; }
};

struct RecordingGridCell final : synth::Cell {
    RecordingGridCell(std::vector<std::string>* events, int gridIx)
        : events(events), gridIx(gridIx) {}

    void OnPress(std::uint8_t velocity) override {
        events->push_back("press:" + std::to_string(gridIx) + ":" + std::to_string(velocity));
    }
    void OnRelease() override {
        events->push_back("release:" + std::to_string(gridIx));
    }
    void OnPressureChange(std::uint8_t pressure) override {
        events->push_back("pressure:" + std::to_string(gridIx) + ":" + std::to_string(pressure));
    }
    synth::Color GetColor() const override { return synth::Color::Off; }
    bool GetOnOff() const override { return false; }

    std::vector<std::string>* events;
    int gridIx;
};

std::string JsonToString(synth::JSON json) {
    char* dumped = json.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);
    std::string text(dumped);
    std::free(dumped);
    return text;
}

bool JsonSemanticallyEqual(synth::JSON left, synth::JSON right) {
    if (left.IsNull() || right.IsNull()) {
        return left.IsNull() && right.IsNull();
    }
    const synth::JsonType leftType = left.m_node->m_type;
    const synth::JsonType rightType = right.m_node->m_type;
    const bool leftNumber = leftType == synth::JsonType::Integer || leftType == synth::JsonType::Real;
    const bool rightNumber = rightType == synth::JsonType::Integer || rightType == synth::JsonType::Real;
    if (leftNumber || rightNumber) {
        return leftNumber && rightNumber && left.NumberValue() == right.NumberValue();
    }
    if (leftType != rightType) {
        return false;
    }

    switch (leftType) {
    case synth::JsonType::Null:
        return true;
    case synth::JsonType::Object: {
        if (left.Size() != right.Size()) {
            return false;
        }
        const synth::JsonMember* members =
            static_cast<const synth::JsonMember*>(left.m_node->m_container.m_entries);
        for (std::size_t ix = 0; ix < left.Size(); ++ix) {
            if (members[ix].m_key == nullptr ||
                !JsonSemanticallyEqual(synth::JSON(members[ix].m_value), right.Get(members[ix].m_key))) {
                return false;
            }
        }
        return true;
    }
    case synth::JsonType::Array:
        if (left.Size() != right.Size()) {
            return false;
        }
        for (std::size_t ix = 0; ix < left.Size(); ++ix) {
            if (!JsonSemanticallyEqual(left.GetAt(ix), right.GetAt(ix))) {
                return false;
            }
        }
        return true;
    case synth::JsonType::String:
        return std::string(left.StringValue()) == std::string(right.StringValue());
    case synth::JsonType::Boolean:
        return left.BooleanValue() == right.BooleanValue();
    case synth::JsonType::Integer:
    case synth::JsonType::Real:
        return false;
    }
    return false;
}

void RequireRouteBijection(const synth::Parameter& parameter, std::size_t sourceCount);

void MarkAllModulatorsConnectedForUi(synth::ParameterGroup& group) {
    for (synth::ModulatorMetadata& metadata : group.GetModulators().Metadata()) {
        metadata.connected = true;
    }
}

// Wraps a single WrldBldr-kind MidiControllerProfileConfig (as produced by
// WrldBldrDefaultProfileConfig, whose system-message associations always
// carry both a control address and a wrldBldrPosition -- see
// SlotValidForKind's WrldBldr branch) plus a pair of endpoint identifiers
// into a one-controller MidiInstrumentConfig, for patch-persistence tests
// that used to build a bare MidiControllerProfileConfig + MidiEndpointState
// pair directly. Named "controller" to match MidiControllerSlot's default
// name so assertions reading loaded.controllers[0] read naturally.
synth::MidiInstrumentConfig MakeInstrumentFromProfile(const synth::MidiControllerProfileConfig& profile,
                                                       std::string_view inputIdentifier = "",
                                                       std::string_view outputIdentifier = "") {
    synth::MidiInstrumentConfig instrument;
    synth::MidiControllerSlot slot;
    slot.name = "controller";
    slot.kind = synth::MidiProfileKind::WrldBldr;
    slot.config = profile;
    slot.input.identifier = std::string(inputIdentifier);
    slot.output.identifier = std::string(outputIdentifier);
    const bool added = instrument.AddController(std::move(slot));
    (void)added;
    return instrument;
}

} // namespace

TEST_CASE(absolute_edit_locations_form_the_independently_computed_convex_system) {
    float leftBase = 0.2f;
    float rightBase = 0.8f;
    float leftGestureA = 0.1f;
    float rightGestureA = 0.9f;
    float leftGestureB = 0.3f;
    float rightGestureB = 0.7f;
    constexpr double blend = 0.25;
    constexpr double gestureAWeight = 0.5;
    constexpr double gestureBGroupWeight = 0.5;
    constexpr bool gestureBActiveLeft = false;
    constexpr bool gestureBActiveRight = true;
    constexpr double gestureBWeight =
        gestureBGroupWeight * ((gestureBActiveLeft ? 1.0 - blend : 0.0) +
                               (gestureBActiveRight ? blend : 0.0));
    const std::array gestures = {
        synth::detail::AbsoluteGestureContribution{&leftGestureA, &rightGestureA, gestureAWeight},
        synth::detail::AbsoluteGestureContribution{&leftGestureB, &rightGestureB, gestureBWeight},
    };

    const auto locations =
        synth::detail::BuildAbsoluteEditLocations(leftBase, rightBase, blend, gestures);

    const double weightSum = gestureAWeight + gestureBWeight;
    const double expectedBase =
        (gestureAWeight * (1.0 - gestureAWeight) + gestureBWeight * (1.0 - gestureBWeight)) /
        weightSum;
    const double expectedGestureA = gestureAWeight * gestureAWeight / weightSum;
    const double expectedGestureB = gestureBWeight * gestureBWeight / weightSum;
    const std::array expected = {
        std::pair{&leftBase, expectedBase * (1.0 - blend)},
        std::pair{&rightBase, expectedBase * blend},
        std::pair{&leftGestureA, expectedGestureA * (1.0 - blend)},
        std::pair{&rightGestureA, expectedGestureA * blend},
        std::pair{&leftGestureB, expectedGestureB * (1.0 - blend)},
        std::pair{&rightGestureB, expectedGestureB * blend},
    };
    REQUIRE_TRUE(locations.size() == expected.size());
    double coefficientSum = 0.0;
    for (const auto& [storage, coefficient] : expected) {
        const auto found = std::find_if(locations.begin(), locations.end(),
                                        [&](const auto& location) { return location.storage == storage; });
        REQUIRE_TRUE(found != locations.end());
        REQUIRE_TRUE(std::fabs(found->coefficient - coefficient) <= 1e-12);
        coefficientSum += found->coefficient;
    }
    REQUIRE_TRUE(std::fabs(coefficientSum - 1.0) <= 1e-12);
}

TEST_CASE(absolute_runtime_workspace_has_fixed_capacity_and_noexcept_helpers) {
    using synth::detail::AbsoluteEditWorkspace;
    static_assert(synth::detail::kAbsoluteMaxEditLocations ==
                  2 + 2 * std::numeric_limits<synth::GestureMask>::digits);
    static_assert(std::is_trivially_destructible_v<AbsoluteEditWorkspace>);
    static_assert(noexcept(std::declval<synth::Parameter&>().HandleSetAbsolute(
        std::declval<const synth::SceneState&>(), 0.5f)));

    float left = 0.2f;
    float right = 0.8f;
    AbsoluteEditWorkspace workspace;
    const std::span<const synth::detail::AbsoluteGestureContribution> noGestures;
    static_assert(noexcept(synth::detail::TryBuildAbsoluteEditLocations(
        left, right, 0.5, noGestures, workspace)));
    static_assert(noexcept(synth::detail::ProjectAbsoluteTarget(workspace, 0.0, 1.0, 0.6)));

    REQUIRE_TRUE(workspace.locations.size() == synth::detail::kAbsoluteMaxEditLocations);
    REQUIRE_TRUE(synth::detail::TryBuildAbsoluteEditLocations(
        left, right, 0.5, noGestures, workspace));
    REQUIRE_TRUE(workspace.locationCount == 2);
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(workspace, 0.0, 1.0, 0.6));

    std::array<synth::detail::AbsoluteGestureContribution,
               synth::detail::kAbsoluteMaxGestureContributions + 1>
        tooManyGestures{};
    REQUIRE_TRUE(!synth::detail::TryBuildAbsoluteEditLocations(
        left, right, 0.5, tooManyGestures, workspace));
    REQUIRE_TRUE(workspace.locationCount == 0);
}

TEST_CASE(absolute_edit_locations_cover_endpoints_no_gestures_and_aliased_storage) {
    float left = 0.2f;
    float right = 0.8f;
    const std::span<const synth::detail::AbsoluteGestureContribution> noGestures;

    const auto leftEndpoint = synth::detail::BuildAbsoluteEditLocations(left, right, 0.0, noGestures);
    REQUIRE_TRUE(leftEndpoint.size() == 1);
    REQUIRE_TRUE(leftEndpoint[0].storage == &left);
    REQUIRE_TRUE(std::fabs(leftEndpoint[0].coefficient - 1.0) <= 1e-12);

    const auto rightEndpoint = synth::detail::BuildAbsoluteEditLocations(left, right, 1.0, noGestures);
    REQUIRE_TRUE(rightEndpoint.size() == 1);
    REQUIRE_TRUE(rightEndpoint[0].storage == &right);
    REQUIRE_TRUE(std::fabs(rightEndpoint[0].coefficient - 1.0) <= 1e-12);

    const auto aliased = synth::detail::BuildAbsoluteEditLocations(left, left, 0.37, noGestures);
    REQUIRE_TRUE(aliased.size() == 1);
    REQUIRE_TRUE(aliased[0].storage == &left);
    REQUIRE_TRUE(std::fabs(aliased[0].coefficient - 1.0) <= 1e-12);
}

TEST_CASE(absolute_projection_is_exact_minimum_change_and_redistributes_saturation) {
    float first = 0.2f;
    float second = 0.8f;
    std::array locations = {
        synth::detail::AbsoluteEditLocation{&first, 0.75},
        synth::detail::AbsoluteEditLocation{&second, 0.25},
    };

    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(locations, 0.0, 1.0, 0.475));
    REQUIRE_NEAR(first, 0.35f, 0.000001f);
    REQUIRE_NEAR(second, 0.85f, 0.000001f);
    REQUIRE_TRUE(std::fabs(0.75 * first + 0.25 * second - 0.475) <= 1e-5);

    first = 0.9f;
    second = 0.2f;
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(locations, 0.0, 1.0, 0.85));
    REQUIRE_NEAR(first, 1.0f, 0.000001f);
    REQUIRE_NEAR(second, 0.4f, 0.000001f);

    first = 0.1f;
    second = 0.8f;
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(locations, 0.0, 1.0, 0.15));
    REQUIRE_NEAR(first, 0.0f, 0.000001f);
    REQUIRE_NEAR(second, 0.6f, 0.000001f);
}

TEST_CASE(absolute_projection_handles_noop_endpoints_bipolar_ranges_and_rejects_invalid_contracts) {
    float value = -0.25f;
    std::array one = {synth::detail::AbsoluteEditLocation{&value, 1.0}};
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(one, -1.0, 1.0, -0.25));
    REQUIRE_NEAR(value, -0.25f, 0.0f);
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(one, -1.0, 1.0, 1.0));
    REQUIRE_NEAR(value, 1.0f, 0.0f);
    REQUIRE_TRUE(synth::detail::ProjectAbsoluteTarget(one, -1.0, 1.0, -1.0));
    REQUIRE_NEAR(value, -1.0f, 0.0f);

    float aliased = 0.4f;
    std::array duplicate = {
        synth::detail::AbsoluteEditLocation{&aliased, 0.5},
        synth::detail::AbsoluteEditLocation{&aliased, 0.5},
    };
    REQUIRE_TRUE(!synth::detail::ProjectAbsoluteTarget(duplicate, 0.0, 1.0, 0.75));
    REQUIRE_NEAR(aliased, 0.4f, 0.0f);

    float invalidSumValue = 0.4f;
    std::array invalidSum = {synth::detail::AbsoluteEditLocation{&invalidSumValue, 0.75}};
    REQUIRE_TRUE(!synth::detail::ProjectAbsoluteTarget(invalidSum, 0.0, 1.0, 0.5));
    REQUIRE_NEAR(invalidSumValue, 0.4f, 0.0f);

    float finiteValue = 0.4f;
    std::array nonFinite = {synth::detail::AbsoluteEditLocation{&finiteValue, 1.0}};
    REQUIRE_TRUE(!synth::detail::ProjectAbsoluteTarget(
        nonFinite, 0.0, 1.0, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE_NEAR(finiteValue, 0.4f, 0.0f);
}

TEST_CASE(handle_set_absolute_reaches_endpoint_mid_blend_and_aliased_scene_targets) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 3,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Absolute", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.2f;
    parameter.SceneCenter(1) = 0.8f;
    parameter.SceneCenter(2) = 0.33f;

    const synth::SceneState endpoint{.leftScene = 0, .rightScene = 1, .blend = 0.0f};
    parameter.HandleSetAbsolute(endpoint, 0.75f);
    parameter.Compute(endpoint);  // alpha=1 exposes the raw center before ordinary production slew.
    REQUIRE_NEAR(parameter.TargetCenter(), 0.75f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.8f, 0.0f);
    REQUIRE_NEAR(parameter.SceneCenter(2), 0.33f, 0.0f);

    parameter.SceneCenter(0) = 0.2f;
    parameter.SceneCenter(1) = 0.8f;
    const synth::SceneState middle{.leftScene = 0, .rightScene = 1, .blend = 0.25f};
    parameter.HandleSetAbsolute(middle, 0.9f);
    parameter.Compute(middle);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.9f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(0), 13.0f / 15.0f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 1.0f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(2), 0.33f, 0.0f);

    const synth::SceneState aliased{.leftScene = 1, .rightScene = 1, .blend = 0.5f};
    parameter.HandleSetAbsolute(aliased, 0.4f);
    parameter.Compute(aliased);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.4f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.4f, 0.00001f);
}

TEST_CASE(handle_set_absolute_arms_then_rebuilds_the_proof_counterexample) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Counterexample", .defaultValue = 0.0f});
    parameter.SceneCenter(0) = 0.0f;
    parameter.GestureValue(0, 0) = 1.0f;
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 0.5f);
    manager.DeselectGesture(0);
    parameter.GestureValue(0, 1) = 0.9f;
    manager.SetGestureValue(1, 0.5f);
    manager.SelectGesture(1);
    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};

    parameter.HandleSetAbsolute(scene, 0.75f);
    parameter.Compute(scene);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_TRUE(!manager.GestureSelected(0));
    REQUIRE_TRUE(parameter.GestureActive(0, 1));
    REQUIRE_NEAR(parameter.TargetCenter(), 0.75f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.8f, 0.00001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 1.0f, 0.00001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 1), 0.4f, 0.00001f);
}

TEST_CASE(handle_set_absolute_arms_both_touched_endpoints_and_preserves_unrelated_storage) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 3,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Arming", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.1f;
    parameter.SceneCenter(1) = 0.9f;
    parameter.SceneCenter(2) = 0.3f;
    parameter.GestureValue(0, 0) = 0.8f;
    parameter.GestureValue(1, 0) = 0.2f;
    parameter.GestureValue(2, 0) = 0.7f;
    parameter.GestureValue(2, 1) = 0.6f;
    manager.SetGestureValue(0, 0.75f);
    manager.SelectGesture(0);
    const synth::SceneState scene{.leftScene = 0, .rightScene = 1, .blend = 0.5f};

    parameter.HandleSetAbsolute(scene, 0.65f);
    parameter.Compute(scene);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_TRUE(parameter.GestureActive(1, 0));
    REQUIRE_NEAR(parameter.TargetCenter(), 0.65f, 0.00001f);
    REQUIRE_NEAR(parameter.SceneCenter(2), 0.3f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(2, 0), 0.7f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(2, 1), 0.6f, 0.0f);
}

TEST_CASE(handle_set_absolute_clamps_input_keeps_normalized_bipolar_storage_and_rejects_nonfinite) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(
        group, {.name = "Bipolar", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};

    parameter.HandleSetAbsolute(scene, -3.0f);
    parameter.Compute(scene);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.0f, 0.00001f);
    parameter.HandleSetAbsolute(scene, 3.0f);
    parameter.Compute(scene);
    REQUIRE_NEAR(parameter.TargetCenter(), 1.0f, 0.00001f);
    parameter.HandleSetAbsolute(scene, 0.25f);
    parameter.Compute(scene);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.00001f);

    parameter.SceneCenter(0) = 0.4f;
    parameter.GestureValue(0, 0) = 0.9f;
    manager.SetGestureValue(0, 1.0f);
    manager.SelectGesture(0);
    parameter.HandleSetAbsolute(scene, std::numeric_limits<float>::quiet_NaN());
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.9f, 0.0f);
    REQUIRE_TRUE(!parameter.GestureActive(0, 0));
}

TEST_CASE(handle_set_absolute_rejects_malformed_latent_storage_without_throwing_or_arming) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Malformed", .defaultValue = 0.4f});
    parameter.SceneCenter(0) = 0.4f;
    parameter.GestureValue(0, 0) = 1.25f;  // Simulate malformed persisted latent storage.
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 0.5f);
    parameter.GestureValue(0, 1) = 0.8f;
    manager.SetGestureValue(1, 0.5f);
    manager.SelectGesture(1);
    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};

    parameter.HandleSetAbsolute(scene, 0.6f);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 1.25f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(0, 1), 0.8f, 0.0f);
    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_TRUE(!parameter.GestureActive(0, 1));
}

TEST_CASE(handle_set_absolute_rejects_nonfinite_control_state_and_invalid_scene_without_mutation) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Invalid", .defaultValue = 0.4f});
    parameter.SceneCenter(0) = 0.4f;
    parameter.GestureValue(0, 0) = 0.8f;
    manager.SetGestureValue(0, std::numeric_limits<float>::quiet_NaN());
    manager.SelectGesture(0);

    const synth::SceneState validScene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    parameter.HandleSetAbsolute(validScene, 0.6f);
    const synth::SceneState invalidScene{.leftScene = 0, .rightScene = 4, .blend = 0.5f};
    parameter.HandleSetAbsolute(invalidScene, 0.7f);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.8f, 0.0f);
    REQUIRE_TRUE(!parameter.GestureActive(0, 0));
}

namespace {

constexpr std::size_t kAbsolutePropertySceneCount = 3;
constexpr std::size_t kAbsolutePropertyGestureCount = 4;
constexpr std::size_t kAbsolutePropertyStorageCount =
    kAbsolutePropertySceneCount * (1 + kAbsolutePropertyGestureCount);

struct AbsolutePropertyCase {
    synth::SceneState scene;
    std::array<float, kAbsolutePropertySceneCount> centers{};
    std::array<std::array<float, kAbsolutePropertyGestureCount>, kAbsolutePropertySceneCount>
        gestureValues{};
    std::array<std::array<bool, kAbsolutePropertyGestureCount>, kAbsolutePropertySceneCount>
        active{};
    std::array<float, kAbsolutePropertyGestureCount> weights{};
    std::array<bool, kAbsolutePropertyGestureCount> selected{};
    float target = 0.5f;
    synth::RangeKind range = synth::RangeKind::Unipolar;
    bool requireArming = false;
    bool requireSaturation = false;
};

struct AbsolutePropertyOracle {
    std::array<float, kAbsolutePropertyStorageCount> postArmingValues{};
    std::array<double, kAbsolutePropertyStorageCount> coefficients{};
    std::array<bool, kAbsolutePropertyStorageCount> armed{};
    std::array<std::array<bool, kAbsolutePropertyGestureCount>, kAbsolutePropertySceneCount>
        active{};
};

struct AbsolutePropertyResult {
    std::array<float, kAbsolutePropertyStorageCount> values{};
    std::array<std::array<bool, kAbsolutePropertyGestureCount>, kAbsolutePropertySceneCount>
        active{};
    float rawCenter = 0.0f;
};

constexpr std::size_t AbsoluteBaseStorage(std::size_t sceneIx) {
    return sceneIx;
}

constexpr std::size_t AbsoluteGestureStorage(std::size_t sceneIx, std::size_t gestureIx) {
    return kAbsolutePropertySceneCount + sceneIx * kAbsolutePropertyGestureCount + gestureIx;
}

void AccumulateAbsoluteOracleCoefficient(AbsolutePropertyOracle& oracle, std::size_t storageIx,
                                         double coefficient) {
    if (coefficient > 0.0) {
        oracle.coefficients[storageIx] += coefficient;
    }
}

AbsolutePropertyOracle BuildIndependentAbsoluteOracle(const AbsolutePropertyCase& testCase) {
    AbsolutePropertyOracle oracle;
    oracle.active = testCase.active;
    for (std::size_t sceneIx = 0; sceneIx < kAbsolutePropertySceneCount; ++sceneIx) {
        oracle.postArmingValues[AbsoluteBaseStorage(sceneIx)] = testCase.centers[sceneIx];
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            oracle.postArmingValues[AbsoluteGestureStorage(sceneIx, gestureIx)] =
                testCase.gestureValues[sceneIx][gestureIx];
        }
    }

    const double blend = std::clamp(static_cast<double>(testCase.scene.blend), 0.0, 1.0);
    auto arm = [&](std::size_t sceneIx, std::size_t gestureIx) {
        if (oracle.active[sceneIx][gestureIx]) {
            return;
        }
        oracle.active[sceneIx][gestureIx] = true;
        const std::size_t storageIx = AbsoluteGestureStorage(sceneIx, gestureIx);
        oracle.postArmingValues[storageIx] = testCase.centers[sceneIx];
        oracle.armed[storageIx] = true;
    };
    for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
        if (!testCase.selected[gestureIx]) {
            continue;
        }
        if (blend <= 0.0) {
            arm(testCase.scene.leftScene, gestureIx);
        } else if (blend >= 1.0) {
            arm(testCase.scene.rightScene, gestureIx);
        } else {
            arm(testCase.scene.leftScene, gestureIx);
            if (testCase.scene.rightScene != testCase.scene.leftScene) {
                arm(testCase.scene.rightScene, gestureIx);
            }
        }
    }

    std::array<double, kAbsolutePropertyGestureCount> effectiveWeights{};
    double weightSum = 0.0;
    double baseNumerator = 0.0;
    for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
        const double weight = static_cast<double>(testCase.weights[gestureIx]);
        const double leftShare = oracle.active[testCase.scene.leftScene][gestureIx]
                                     ? 1.0 - blend
                                     : 0.0;
        const double rightShare = oracle.active[testCase.scene.rightScene][gestureIx]
                                      ? blend
                                      : 0.0;
        const double effectiveWeight = weight * (leftShare + rightShare);
        effectiveWeights[gestureIx] = effectiveWeight;
        weightSum += effectiveWeight;
        baseNumerator += effectiveWeight * (1.0 - effectiveWeight);
    }

    const double inverseBlend = 1.0 - blend;
    const double baseCoefficient = weightSum == 0.0 ? 1.0 : baseNumerator / weightSum;
    AccumulateAbsoluteOracleCoefficient(
        oracle, AbsoluteBaseStorage(testCase.scene.leftScene), baseCoefficient * inverseBlend);
    AccumulateAbsoluteOracleCoefficient(
        oracle, AbsoluteBaseStorage(testCase.scene.rightScene), baseCoefficient * blend);
    if (weightSum > 0.0) {
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            const double weight = effectiveWeights[gestureIx];
            if (weight == 0.0) {
                continue;
            }
            const double componentCoefficient = weight * weight / weightSum;
            AccumulateAbsoluteOracleCoefficient(
                oracle, AbsoluteGestureStorage(testCase.scene.leftScene, gestureIx),
                componentCoefficient * inverseBlend);
            AccumulateAbsoluteOracleCoefficient(
                oracle, AbsoluteGestureStorage(testCase.scene.rightScene, gestureIx),
                componentCoefficient * blend);
        }
    }

    double coefficientSum = 0.0;
    for (double coefficient : oracle.coefficients) {
        REQUIRE_TRUE(coefficient >= 0.0);
        coefficientSum += coefficient;
    }
    REQUIRE_TRUE(std::fabs(coefficientSum - 1.0) <= 1e-10);
    return oracle;
}

std::array<float, kAbsolutePropertyStorageCount> SolveIndependentAbsoluteProjection(
    const AbsolutePropertyOracle& oracle, double target) {
    double lowerLambda = 0.0;
    double upperLambda = 0.0;
    bool haveContributor = false;
    for (std::size_t storageIx = 0; storageIx < oracle.coefficients.size(); ++storageIx) {
        const double coefficient = oracle.coefficients[storageIx];
        if (coefficient == 0.0) {
            continue;
        }
        const double value = oracle.postArmingValues[storageIx];
        const double lower = -value / coefficient;
        const double upper = (1.0 - value) / coefficient;
        if (!haveContributor) {
            lowerLambda = lower;
            upperLambda = upper;
            haveContributor = true;
        } else {
            lowerLambda = std::min(lowerLambda, lower);
            upperLambda = std::max(upperLambda, upper);
        }
    }
    REQUIRE_TRUE(haveContributor);

    auto effectiveAt = [&](double lambda) {
        double effective = 0.0;
        for (std::size_t storageIx = 0; storageIx < oracle.coefficients.size(); ++storageIx) {
            const double coefficient = oracle.coefficients[storageIx];
            if (coefficient == 0.0) {
                continue;
            }
            const double candidate = static_cast<double>(oracle.postArmingValues[storageIx]) +
                                     lambda * coefficient;
            effective += coefficient * std::clamp(candidate, 0.0, 1.0);
        }
        return effective;
    };
    for (int iteration = 0; iteration < 160; ++iteration) {
        const double middle = (lowerLambda + upperLambda) * 0.5;
        if (effectiveAt(middle) < target) {
            lowerLambda = middle;
        } else {
            upperLambda = middle;
        }
    }
    const double lambda = (lowerLambda + upperLambda) * 0.5;

    auto expected = oracle.postArmingValues;
    for (std::size_t storageIx = 0; storageIx < oracle.coefficients.size(); ++storageIx) {
        if (oracle.coefficients[storageIx] > 0.0) {
            expected[storageIx] = static_cast<float>(std::clamp(
                static_cast<double>(oracle.postArmingValues[storageIx]) +
                    lambda * oracle.coefficients[storageIx],
                0.0, 1.0));
        }
    }
    return expected;
}

AbsolutePropertyResult RunAbsolutePropertyCase(const AbsolutePropertyCase& testCase) {
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.SetGestureCount(kAbsolutePropertyGestureCount));
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = kAbsolutePropertySceneCount,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(
        group, {.name = "Randomized absolute", .defaultValue = 0.5f, .range = testCase.range});
    for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
        manager.SetGestureValue(gestureIx, testCase.weights[gestureIx]);
        if (testCase.selected[gestureIx]) {
            manager.SelectGesture(gestureIx);
        } else {
            manager.DeselectGesture(gestureIx);
        }
    }
    for (std::size_t sceneIx = 0; sceneIx < kAbsolutePropertySceneCount; ++sceneIx) {
        parameter.SceneCenter(sceneIx) = testCase.centers[sceneIx];
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            parameter.GestureValue(sceneIx, gestureIx) = testCase.gestureValues[sceneIx][gestureIx];
            parameter.SetGestureActive(sceneIx, gestureIx, testCase.active[sceneIx][gestureIx]);
        }
    }

    parameter.HandleSetAbsolute(testCase.scene, testCase.target);
    parameter.Compute(testCase.scene);  // alpha=1 publishes the exact pre-slew raw center.

    AbsolutePropertyResult result;
    result.rawCenter = parameter.TargetCenter();
    for (std::size_t sceneIx = 0; sceneIx < kAbsolutePropertySceneCount; ++sceneIx) {
        result.values[AbsoluteBaseStorage(sceneIx)] = parameter.SceneCenter(sceneIx);
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            result.values[AbsoluteGestureStorage(sceneIx, gestureIx)] =
                parameter.GestureValue(sceneIx, gestureIx);
            result.active[sceneIx][gestureIx] = parameter.GestureActive(sceneIx, gestureIx);
        }
    }
    return result;
}

void VerifyAbsolutePropertyCase(const AbsolutePropertyCase& testCase) {
    const AbsolutePropertyOracle oracle = BuildIndependentAbsoluteOracle(testCase);
    const auto expectedProjection = SolveIndependentAbsoluteProjection(oracle, testCase.target);
    const AbsolutePropertyResult first = RunAbsolutePropertyCase(testCase);
    const AbsolutePropertyResult second = RunAbsolutePropertyCase(testCase);

    REQUIRE_TRUE(std::bit_cast<std::uint32_t>(first.rawCenter) ==
                 std::bit_cast<std::uint32_t>(second.rawCenter));
    bool armedAny = false;
    bool saturatedAny = false;
    double independentRawCenter = 0.0;
    for (std::size_t storageIx = 0; storageIx < oracle.coefficients.size(); ++storageIx) {
        REQUIRE_TRUE(first.values[storageIx] >= 0.0f && first.values[storageIx] <= 1.0f);
        REQUIRE_TRUE(std::bit_cast<std::uint32_t>(first.values[storageIx]) ==
                     std::bit_cast<std::uint32_t>(second.values[storageIx]));
        if (oracle.coefficients[storageIx] > 0.0) {
            REQUIRE_NEAR(first.values[storageIx], expectedProjection[storageIx], 0.00002f);
            independentRawCenter +=
                oracle.coefficients[storageIx] * static_cast<double>(first.values[storageIx]);
            saturatedAny = saturatedAny || first.values[storageIx] == 0.0f ||
                           first.values[storageIx] == 1.0f;
        } else if (!oracle.armed[storageIx]) {
            REQUIRE_TRUE(std::bit_cast<std::uint32_t>(first.values[storageIx]) ==
                         std::bit_cast<std::uint32_t>(oracle.postArmingValues[storageIx]));
        }
        armedAny = armedAny || oracle.armed[storageIx];
    }
    for (std::size_t sceneIx = 0; sceneIx < kAbsolutePropertySceneCount; ++sceneIx) {
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            REQUIRE_TRUE(first.active[sceneIx][gestureIx] == oracle.active[sceneIx][gestureIx]);
            REQUIRE_TRUE(second.active[sceneIx][gestureIx] == oracle.active[sceneIx][gestureIx]);
        }
    }
    REQUIRE_TRUE(std::fabs(independentRawCenter - static_cast<double>(testCase.target)) <= 1e-5);
    REQUIRE_NEAR(first.rawCenter, testCase.target, 0.00001f);
    if (testCase.requireArming) {
        REQUIRE_TRUE(armedAny);
    }
    if (testCase.requireSaturation) {
        REQUIRE_TRUE(testCase.target > 0.0f && testCase.target < 1.0f);
        REQUIRE_TRUE(saturatedAny);
    }
}

}  // namespace

TEST_CASE(handle_set_absolute_seeded_property_matches_independent_post_arming_model) {
    std::vector<AbsolutePropertyCase> cases;

    AbsolutePropertyCase leftEndpoint;
    leftEndpoint.scene = {.leftScene = 0, .rightScene = 1, .blend = 0.0f};
    leftEndpoint.centers = {0.2f, 0.8f, 0.3f};
    leftEndpoint.target = 0.0f;
    cases.push_back(leftEndpoint);

    AbsolutePropertyCase rightEndpoint = leftEndpoint;
    rightEndpoint.scene.blend = 1.0f;
    rightEndpoint.target = 1.0f;
    rightEndpoint.range = synth::RangeKind::Bipolar;
    cases.push_back(rightEndpoint);

    AbsolutePropertyCase aliased = leftEndpoint;
    aliased.scene = {.leftScene = 1, .rightScene = 1, .blend = 0.37f};
    aliased.target = 0.43f;
    cases.push_back(aliased);

    AbsolutePropertyCase zeroAndOneWeights = leftEndpoint;
    zeroAndOneWeights.scene.blend = 0.4f;
    zeroAndOneWeights.weights = {0.0f, 1.0f, 0.5f, 0.0f};
    zeroAndOneWeights.active[0] = {true, true, false, true};
    zeroAndOneWeights.active[1] = {true, true, true, false};
    zeroAndOneWeights.gestureValues[0] = {0.9f, 0.1f, 0.8f, 0.7f};
    zeroAndOneWeights.gestureValues[1] = {0.1f, 0.9f, 0.2f, 0.3f};
    zeroAndOneWeights.target = 0.6f;
    cases.push_back(zeroAndOneWeights);

    AbsolutePropertyCase saturation = leftEndpoint;
    saturation.scene.blend = 0.1f;
    saturation.centers = {0.9f, 0.1f, 0.4f};
    saturation.target = 0.95f;
    saturation.requireSaturation = true;
    cases.push_back(saturation);

    AbsolutePropertyCase postArming = leftEndpoint;
    postArming.scene = {.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    postArming.centers = {0.0f, 0.3f, 0.7f};
    postArming.weights = {0.5f, 0.5f, 0.0f, 1.0f};
    postArming.active[0][0] = true;
    postArming.gestureValues[0][0] = 1.0f;
    postArming.gestureValues[0][1] = 0.9f;
    postArming.selected[1] = true;
    postArming.target = 0.75f;
    postArming.requireArming = true;
    cases.push_back(postArming);

    std::mt19937 rng(0xAB501U);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::bernoulli_distribution coin(0.5);
    std::uniform_int_distribution<std::size_t> sceneIndex(0, kAbsolutePropertySceneCount - 1);
    for (std::size_t caseIx = 0; caseIx < 192; ++caseIx) {
        AbsolutePropertyCase randomized;
        randomized.scene.leftScene = sceneIndex(rng);
        randomized.scene.rightScene = caseIx % 11 == 0 ? randomized.scene.leftScene : sceneIndex(rng);
        randomized.scene.blend = unit(rng);
        randomized.target = unit(rng);
        randomized.range = coin(rng) ? synth::RangeKind::Unipolar : synth::RangeKind::Bipolar;
        for (float& center : randomized.centers) {
            center = unit(rng);
        }
        for (std::size_t gestureIx = 0; gestureIx < kAbsolutePropertyGestureCount; ++gestureIx) {
            randomized.weights[gestureIx] = unit(rng);
            randomized.selected[gestureIx] = coin(rng);
            for (std::size_t sceneIx = 0; sceneIx < kAbsolutePropertySceneCount; ++sceneIx) {
                randomized.gestureValues[sceneIx][gestureIx] = unit(rng);
                randomized.active[sceneIx][gestureIx] = coin(rng);
            }
        }
        cases.push_back(randomized);
    }

    for (const AbsolutePropertyCase& testCase : cases) {
        VerifyAbsolutePropertyCase(testCase);
    }
}

TEST_CASE(smoke_clamps_ranges) {
    REQUIRE_NEAR(synth::ClampToRange(2.0f, synth::RangeKind::Unipolar), 1.0f, 0.0001f);
    REQUIRE_NEAR(synth::ClampToRange(-2.0f, synth::RangeKind::Bipolar), 0.0f, 0.0001f);
}

TEST_CASE(json_arena_build_parse_dump_and_grow_retry) {
    synth::JsonArena arena(1024);
    synth::JSON root = arena.Object();
    root.SetNew("name", arena.String("Patch A"));
    root.SetNew("version", arena.Integer(1));
    synth::JSON values = arena.Array();
    values.AppendNew(arena.Real(0.25));
    values.AppendNew(arena.Boolean(true));
    root.SetNew("values", values);
    REQUIRE_TRUE(!arena.Failed());

    char* dumped = root.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);

    synth::JsonArena parsedArena(16);
    synth::JSON parsed = parsedArena.Loads(dumped);
    bool grew = false;
    while (parsed.IsNull() && parsedArena.Failed()) {
        grew = true;
        parsedArena.GrowAndReset();
        parsed = parsedArena.Loads(dumped);
    }
    free(dumped);

    REQUIRE_TRUE(grew);
    REQUIRE_TRUE(!parsed.IsNull());
    REQUIRE_TRUE(std::string(parsed.Get("name").StringValue()) == "Patch A");
    REQUIRE_TRUE(parsed.Get("version").IntegerValue() == 1);
    REQUIRE_NEAR(static_cast<float>(parsed.Get("values").GetAt(0).NumberValue()), 0.25f, 0.000001f);
    REQUIRE_TRUE(parsed.Get("values").GetAt(1).BooleanValue());

    synth::JSON missing = parsed.Get("missing");
    REQUIRE_TRUE(missing.IsNull());
    REQUIRE_TRUE(missing.StringValue() == nullptr);
    REQUIRE_TRUE(missing.IntegerValue() == 0);
    REQUIRE_TRUE(missing.NumberValue() == 0.0);
    REQUIRE_TRUE(!missing.BooleanValue());
    REQUIRE_TRUE(missing.Size() == 0);

    synth::JsonArena nullArena(64);
    synth::JSON parsedNull = nullArena.Loads("null");
    REQUIRE_TRUE(parsedNull.IsNull());
    REQUIRE_TRUE(!nullArena.Failed());

    synth::JsonArena integerArena(128);
    synth::JSON largeInteger = integerArena.Loads("922337203685477580");
    REQUIRE_TRUE(!largeInteger.IsNull());
    REQUIRE_TRUE(largeInteger.IntegerValue() == 922337203685477580LL);

    synth::JsonArena malformedUnicodeArena(256);
    synth::JSON malformedUnicode = malformedUnicodeArena.Loads("\"\\uD800\"");
    REQUIRE_TRUE(malformedUnicode.IsNull());
    REQUIRE_TRUE(!malformedUnicodeArena.Failed());
}

TEST_CASE(group_config_validation) {
    const float expectedDefaultAlpha = synth::OnePoleLowPass::AlphaFromNatFreq(1000.0f / 48000.0f);
    const synth::ParameterGroupConfig defaultAlpha{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
    };
    REQUIRE_NEAR(defaultAlpha.processLiteAlpha, expectedDefaultAlpha, 0.000001f);
    const float expectedDefaultTargetCenterAlpha = 0.0994231307f;
    REQUIRE_NEAR(defaultAlpha.targetCenterAlpha, expectedDefaultTargetCenterAlpha, 0.000001f);
    REQUIRE_TRUE(defaultAlpha.targetComputeIntervalSamples == 16);

    synth::ParameterGroupConfig valid{
        .numVoices = 4,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 8,
        .processLiteAlpha = 0.5f,
        .targetCenterAlpha = 0.5f,
    };
    REQUIRE_TRUE(valid.IsValid());
    valid.targetComputeIntervalSamples = 32;
    REQUIRE_TRUE(valid.IsValid());
    const synth::ParameterGroupConfig zeroVoices{.numVoices = 0, .numScenes = 1, .maxParameters = 1};
    const synth::ParameterGroupConfig zeroScenes{.numVoices = 1, .numScenes = 0, .maxParameters = 1};
    const synth::ParameterGroupConfig zeroMaxParameters{.numVoices = 1, .numScenes = 1, .maxParameters = 0};
    const synth::ParameterGroupConfig zeroTargetInterval{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetComputeIntervalSamples = 0,
    };
    const synth::ParameterGroupConfig lowAlpha{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = -0.01f,
        .targetCenterAlpha = 1.0f,
    };
    const synth::ParameterGroupConfig highAlpha{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.01f,
        .targetCenterAlpha = 1.0f,
    };
    const synth::ParameterGroupConfig lowTargetCenterAlpha{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = -0.01f,
    };
    const synth::ParameterGroupConfig highTargetCenterAlpha{
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.01f,
    };
    REQUIRE_TRUE(!zeroVoices.IsValid());
    REQUIRE_TRUE(!zeroScenes.IsValid());
    REQUIRE_TRUE(!zeroMaxParameters.IsValid());
    REQUIRE_TRUE(!zeroTargetInterval.IsValid());
    REQUIRE_TRUE(!lowAlpha.IsValid());
    REQUIRE_TRUE(!highAlpha.IsValid());
    REQUIRE_TRUE(!lowTargetCenterAlpha.IsValid());
    REQUIRE_TRUE(!highTargetCenterAlpha.IsValid());
}

TEST_CASE(parameter_timing_alpha_conversion_preserves_wall_clock_response) {
    const float a192 = synth::ConvertOnePoleAlpha(synth::kDefaultProcessLiteAlpha, 48000.0, 192000.0);
    float y = 0.0f;
    for (int i = 0; i < 4; ++i) {
        y += a192 * (1.0f - y);
    }
    REQUIRE_TRUE(std::fabs(y - synth::kDefaultProcessLiteAlpha) < 1e-5f);
}

TEST_CASE(parameter_timing_interval_conversion_preserves_cadence) {
    REQUIRE_TRUE(synth::ConvertSampleInterval(synth::kDefaultTargetComputeIntervalSamples, 48000.0, 192000.0) == 64);
    REQUIRE_TRUE(synth::ConvertSampleInterval(16, 48000.0, 44100.0) == 15);
    REQUIRE_TRUE(synth::ConvertSampleInterval(1, 48000.0, 8000.0) == 1);
}

TEST_CASE(parameter_timing_rejects_invalid_inputs_without_mutation) {
    bool threw = false;
    try {
        (void)synth::ConvertOnePoleAlpha(1.01f, 48000.0, 192000.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)synth::ConvertOnePoleAlpha(0.5f, 0.0, 192000.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)synth::ConvertSampleInterval(0, 48000.0, 192000.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetComputeIntervalSamples = 16,
        .uiDisplayCenterAlpha = 0.5f,
        .uiDisplaySpreadAlpha = 0.75f,
    });
    const synth::ParameterGroupConfig before = group.Config();

    threw = false;
    try {
        group.ConfigureProcessingTiming({
            .processLiteAlpha = 1.5f,
            .targetComputeIntervalSamples = 8,
            .uiDisplayCenterAlpha = 0.25f,
            .uiDisplaySpreadAlpha = 0.25f,
        });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    REQUIRE_NEAR(group.Config().processLiteAlpha, before.processLiteAlpha, 0.0001f);
    REQUIRE_TRUE(group.Config().targetComputeIntervalSamples == before.targetComputeIntervalSamples);
    REQUIRE_NEAR(group.Config().uiDisplayCenterAlpha, before.uiDisplayCenterAlpha, 0.0001f);
    REQUIRE_NEAR(group.Config().uiDisplaySpreadAlpha, before.uiDisplaySpreadAlpha, 0.0001f);
}

TEST_CASE(parameter_group_timing_reconfiguration_preserves_topology_values_and_pointers) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 4,
        .processLiteAlpha = 0.25f,
        .targetComputeIntervalSamples = 16,
        .uiDisplayCenterAlpha = 0.5f,
        .uiDisplaySpreadAlpha = 0.75f,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    carrier.SceneCenter(1) = 0.75f;
    depth->SceneCenter(0) = 0.5f;
    manager.ComputeAllTargets();
    carrier.ProcessLite();

    synth::Parameter* const carrierPointer = &carrier;
    synth::Parameter* const depthPointer = depth;
    float* const currentDepthPointer = carrier.CurrentDepthSlots(0).data();
    const float currentCenter = carrier.CurrentCenter();
    const float currentDepth = carrier.CurrentDepthForSource(0, 0);
    const float sceneValue = carrier.SceneCenter(1);

    group.ConfigureProcessingTiming({
        .processLiteAlpha = 0.125f,
        .targetComputeIntervalSamples = 64,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    });

    REQUIRE_TRUE(&group.ParameterByLocalIndex(0) == carrierPointer);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depthPointer);
    REQUIRE_TRUE(carrier.CurrentDepthSlots(0).data() == currentDepthPointer);
    REQUIRE_TRUE(group.Config().numVoices == 2);
    REQUIRE_TRUE(group.Config().numModulators == 1);
    REQUIRE_TRUE(group.Config().numScenes == 2);
    REQUIRE_TRUE(group.Config().maxParameters == 4);
    REQUIRE_NEAR(carrier.CurrentCenter(), currentCenter, 0.0001f);
    REQUIRE_NEAR(carrier.CurrentDepthForSource(0, 0), currentDepth, 0.0001f);
    REQUIRE_NEAR(carrier.SceneCenter(1), sceneValue, 0.0001f);
    REQUIRE_NEAR(group.Config().processLiteAlpha, 0.125f, 0.0001f);
    REQUIRE_TRUE(group.Config().targetComputeIntervalSamples == 64);
    REQUIRE_NEAR(group.Config().uiDisplayCenterAlpha, 0.25f, 0.0001f);
    REQUIRE_NEAR(group.Config().uiDisplaySpreadAlpha, 0.5f, 0.0001f);
}

TEST_CASE(parameter_group_timing_reconfiguration_is_non_compounding) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });

    const synth::ParameterProcessingTiming timing{
        .processLiteAlpha = synth::ConvertOnePoleAlpha(synth::kDefaultProcessLiteAlpha, 48000.0, 192000.0),
        .targetComputeIntervalSamples =
            synth::ConvertSampleInterval(synth::kDefaultTargetComputeIntervalSamples, 48000.0, 192000.0),
        .uiDisplayCenterAlpha = synth::ConvertOnePoleAlpha(synth::kDefaultUiDisplayCenterAlpha, 48000.0, 192000.0),
        .uiDisplaySpreadAlpha = synth::ConvertOnePoleAlpha(synth::kDefaultUiDisplaySpreadAlpha, 48000.0, 192000.0),
    };

    group.ConfigureProcessingTiming(timing);
    group.ConfigureProcessingTiming(timing);

    REQUIRE_NEAR(group.Config().processLiteAlpha, timing.processLiteAlpha, 0.000001f);
    REQUIRE_TRUE(group.Config().targetComputeIntervalSamples == timing.targetComputeIntervalSamples);
    REQUIRE_NEAR(group.Config().uiDisplayCenterAlpha, timing.uiDisplayCenterAlpha, 0.000001f);
    REQUIRE_NEAR(group.Config().uiDisplaySpreadAlpha, timing.uiDisplaySpreadAlpha, 0.000001f);
}

TEST_CASE(color_hsv_and_atomic_storage) {
    const synth::Color color{.r = 64, .g = 128, .b = 255};
    const synth::HsvColor hsv = synth::ToHsv(color);
    const synth::Color roundTrip =
        synth::Color::FromHsvTurns(hsv.hueTurns, hsv.saturation, hsv.value);
    REQUIRE_TRUE(std::abs(static_cast<int>(roundTrip.r) - static_cast<int>(color.r)) <= 1);
    REQUIRE_TRUE(std::abs(static_cast<int>(roundTrip.g) - static_cast<int>(color.g)) <= 1);
    REQUIRE_TRUE(std::abs(static_cast<int>(roundTrip.b) - static_cast<int>(color.b)) <= 1);
    REQUIRE_TRUE(color.AdjustBrightness(0.5f).r == 32);

    const synth::Color degreeGreen = synth::Color::FromHsvDegrees(120.0f, 1.0f, 1.0f);
    const synth::Color turnGreen = synth::Color::FromHsvTurns(1.0f / 3.0f, 1.0f, 1.0f);
    REQUIRE_TRUE(degreeGreen == synth::Color::Rgb(0, 255, 0));
    REQUIRE_TRUE(turnGreen == synth::Color::Rgb(0, 255, 0));
    bool rejectedDegreeAsTurns = false;
    try {
        (void)synth::Color::FromHsvTurns(120.0f, 1.0f, 1.0f);
    } catch (const std::invalid_argument&) {
        rejectedDegreeAsTurns = true;
    }
    REQUIRE_TRUE(rejectedDegreeAsTurns);

    REQUIRE_TRUE(synth::ScaleAlpha(synth::Color::Rgba(10, 20, 30, 200), 0.5f) ==
                 synth::Color::Rgba(10, 20, 30, 100));
    REQUIRE_TRUE(synth::Darken(synth::Color::Rgba(100, 128, 200, 180), 0.5f) ==
                 synth::Color::Rgba(50, 64, 100, 180));
    REQUIRE_TRUE(synth::Brighten(synth::Color::Rgba(100, 128, 200, 180), 0.45f) ==
                 synth::Color::Rgba(148, 167, 217, 180));

    synth::AtomicColor atomicColor;
    atomicColor.Store(synth::Color::Orange);
    REQUIRE_TRUE(atomicColor.Load() == synth::Color::Orange);
    REQUIRE_TRUE(atomicColor.IsLockFree());
}

TEST_CASE(color_rejects_non_finite_conversion_inputs) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const auto rejects = [](auto operation) {
        try {
            operation();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    REQUIRE_TRUE(rejects([&] { (void)synth::Color::FromHsvTurns(0.25f, nan, 0.5f); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::Color::FromHsvTurns(0.25f, 0.5f, infinity); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::Color::FromHsvDegrees(90.0f, infinity, 0.5f); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::Color::FromHsvDegrees(90.0f, 0.5f, nan); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::ScaleAlpha(synth::Color::White, nan); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::Darken(synth::Color::White, infinity); }));
    REQUIRE_TRUE(rejects([&] { (void)synth::Brighten(synth::Color::White, nan); }));
}

TEST_CASE(twister_color_helper_matches_smart_grid_hue_shape) {
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Off) == 0);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Blue) == 8);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Red) == 85);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Green) == 35);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Yellow) == 67);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::White) == 1);
    REQUIRE_TRUE(synth::ColorToTwister(synth::Color::Grey) == 1);
}

TEST_CASE(manager_gesture_count_is_fixed_before_groups) {
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.GestureCount() == 0);
    REQUIRE_TRUE(manager.SetGestureCount(2));
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    REQUIRE_TRUE(group.GestureCount() == 2);
    REQUIRE_TRUE(!manager.SetGestureCount(3));
}

TEST_CASE(manager_gesture_count_supports_zero_through_64_and_rejects_65_without_mutation) {
    for (const std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{32},
                                    std::size_t{33}, std::size_t{64}}) {
        synth::ParameterManager manager;
        REQUIRE_TRUE(manager.SetGestureCount(count));
        REQUIRE_TRUE(manager.GestureCount() == count);
        if (count != 0) {
            manager.SelectGesture(count - 1);
            REQUIRE_TRUE(manager.GestureSelected(count - 1));
        }
    }

    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.SetGestureCount(64));
    manager.SelectGesture(63);
    REQUIRE_TRUE(manager.GestureSelected(63));
    REQUIRE_TRUE(manager.SelectedGestureMask() == (synth::GestureMask{1} << 63));
    REQUIRE_TRUE(!manager.SetGestureCount(65));
    REQUIRE_TRUE(manager.GestureCount() == 64);
    REQUIRE_TRUE(manager.GestureSelected(63));

    bool threw = false;
    try {
        (void)synth::Gestures(65);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(gesture_masks_visit_only_active_bits_through_index_63) {
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.SetGestureCount(64));
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 2,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture sparse", .defaultValue = 0.25f});
    synth::ParameterProcessingObserver work{};
    group.SetProcessingObserverForTests(&work);

    parameter.Compute(manager.Scene());
    REQUIRE_TRUE(work.activeGestureVisits == 0);

    parameter.SetGestureActive(0, 63, true);
    parameter.GestureValue(0, 63) = 0.75f;
    manager.SetGestureValue(63, 1.0f);
    parameter.Compute(manager.Scene());
    REQUIRE_TRUE(work.activeGestureVisits == 1);
    REQUIRE_TRUE((parameter.GesturesAffectingMask() & (std::uint64_t{1} << 63)) != 0);
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.GetRaw(0), 0.75f, 0.000001f);
}

TEST_CASE(validated_scene_endpoint_setter_preserves_state_on_reject) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    (void)manager.CreateGroup({.numVoices = 1, .numScenes = 2, .maxParameters = 1});
    (void)manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});

    REQUIRE_TRUE(manager.SetSceneEndpoints(0, 0));
    manager.SetSceneBlend(0.25f);
    REQUIRE_TRUE(!manager.SetSceneEndpoints(1, 0));
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 0);
    REQUIRE_NEAR(manager.Scene().blend, 0.25f, 0.0001f);
}

TEST_CASE(parameter_appearance_resolves_empty_single_and_exact_indicator_palettes) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 4,
        .numScenes = 1,
        .maxParameters = 3,
    });
    auto& empty = manager.CreateParameter(group, {
        .name = "Empty",
        .baseColor = synth::Color::Red,
    });
    auto& single = manager.CreateParameter(group, {
        .name = "Single",
        .baseColor = synth::Color::Green,
        .indicatorColors = {synth::Color::Yellow},
    });
    auto& exact = manager.CreateParameter(group, {
        .name = "Exact",
        .baseColor = synth::Color::Blue,
        .indicatorColors = {synth::Color::Cyan, synth::Color::Orange, synth::Color::Green, synth::Color::Indigo},
    });

    REQUIRE_TRUE(empty.BaseColor() == synth::Color::Red);
    REQUIRE_TRUE(single.BaseColor() == synth::Color::Green);
    REQUIRE_TRUE(exact.BaseColor() == synth::Color::Blue);
    for (std::size_t voiceIx = 0; voiceIx < 4; ++voiceIx) {
        REQUIRE_TRUE(empty.IndicatorColor(voiceIx) == synth::Color::Red);
        REQUIRE_TRUE(single.IndicatorColor(voiceIx) == synth::Color::Yellow);
    }
    REQUIRE_TRUE(exact.IndicatorColor(0) == synth::Color::Cyan);
    REQUIRE_TRUE(exact.IndicatorColor(1) == synth::Color::Orange);
    REQUIRE_TRUE(exact.IndicatorColor(2) == synth::Color::Green);
    REQUIRE_TRUE(exact.IndicatorColor(3) == synth::Color::Indigo);
}

TEST_CASE(invalid_parameter_indicator_cardinality_rejects_registration_atomically) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 4, .numScenes = 1, .maxParameters = 2});

    bool threw = false;
    try {
        (void)manager.CreateParameter(group, {
            .name = "Invalid",
            .baseColor = synth::Color::Red,
            .indicatorColors = {synth::Color::Cyan, synth::Color::Orange},
        });
    } catch (const std::invalid_argument&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(manager.ParameterCount() == 0);
    REQUIRE_TRUE(group.ParameterCount() == 0);
    REQUIRE_TRUE(manager.FindParameterByName("Invalid") == nullptr);
    auto& valid = manager.CreateParameter(group, {.name = "Valid"});
    REQUIRE_TRUE(valid.Id() == 0);
}

TEST_CASE(parameter_ui_snapshot_owns_parameter_source_and_gesture_colors) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    manager.GestureMetadataAt(0).gestureColor = synth::Color::Orange;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    auto& first = manager.CreateParameter(group, {
        .name = "First",
        .baseColor = synth::Color::Red,
        .indicatorColors = {synth::Color::Green, synth::Color::Blue},
    });
    auto& second = manager.CreateParameter(group, {
        .name = "Second",
        .baseColor = synth::Color::Yellow,
        .indicatorColors = {synth::Color::Indigo, synth::Color::Orange},
    });

    synth::Parameter::UIState firstState(2, 1, 1);
    synth::Parameter::UIState secondState(2, 1, 1);
    first.PopulateUIState(firstState);
    second.PopulateUIState(secondState);
    REQUIRE_TRUE(firstState.indicatorColors[0].Load() == synth::Color::Green);
    REQUIRE_TRUE(firstState.indicatorColors[1].Load() == synth::Color::Blue);
    REQUIRE_TRUE(secondState.indicatorColors[0].Load() == synth::Color::Indigo);
    REQUIRE_TRUE(secondState.indicatorColors[1].Load() == synth::Color::Orange);
    REQUIRE_TRUE(firstState.modulatorColorCount.load() == 1);
    REQUIRE_TRUE(firstState.modulatorSourceColors[0].Load() == synth::Color::Cyan);
    REQUIRE_TRUE(firstState.gestureColorCount.load() == 1);
    REQUIRE_TRUE(firstState.gestureColors[0].Load() == synth::Color::Orange);

    synth::Parameter* depth = first.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    REQUIRE_TRUE(depth->BaseColor() == synth::Color::Cyan);
    REQUIRE_TRUE(depth->IndicatorColor(0) == synth::Color::Green);
    REQUIRE_TRUE(depth->IndicatorColor(1) == synth::Color::Blue);
}

TEST_CASE(modulation_depth_publishes_source_visualizer_topology) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 4,
    });
    TestVisualizer visualizer;
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    group.GetModulators().Metadata(0).visualizer = &visualizer;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier"});

    synth::Parameter* depth0 = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth0 != nullptr);
    synth::Parameter::UIState depthState(1, 2, 0);
    depth0->PopulateUIState(depthState);
    REQUIRE_TRUE(depthState.visualizer.load(std::memory_order_relaxed) == &visualizer);

    synth::Parameter* depth1 = carrier.EnsureModulationDepth(1);
    REQUIRE_TRUE(depth1 != nullptr);
    synth::Parameter::UIState nullDepthState(1, 2, 0);
    depth1->PopulateUIState(nullDepthState);
    REQUIRE_TRUE(nullDepthState.visualizer.load(std::memory_order_relaxed) == nullptr);

    depthState.SetDisconnected();
    REQUIRE_TRUE(depthState.visualizer.load(std::memory_order_relaxed) == nullptr);
}

TEST_CASE(visualizer_topology_is_not_serialized) {
    synth::JsonArena arena(4096);
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
    });
    TestVisualizer visualizer;
    group.GetModulators().Metadata(0).visualizer = &visualizer;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier"});
    REQUIRE_TRUE(carrier.EnsureModulationDepth(0) != nullptr);

    const std::string json = JsonToString(carrier.ToValueJSON(arena));
    REQUIRE_TRUE(json.find("visualizer") == std::string::npos);
    REQUIRE_TRUE(json.find("Visualizer") == std::string::npos);
}

TEST_CASE(manager_cells_use_max_source_capacity_and_clear_reused_higher_entries) {
    synth::ParameterManager manager;
    auto& smallGroup = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& largeGroup = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 1,
    });
    smallGroup.GetModulators().Metadata(0).sourceColor = synth::Color::Red;
    largeGroup.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    largeGroup.GetModulators().Metadata(1).sourceColor = synth::Color::Orange;
    largeGroup.GetModulators().Metadata(2).sourceColor = synth::Color::Green;
    auto& smallParameter = manager.CreateParameter(smallGroup, {.name = "Small"});
    auto& largeParameter = manager.CreateParameter(largeGroup, {.name = "Large"});
    auto& smallBank = manager.CreateBank();
    auto& largeBank = manager.CreateBank();
    smallBank.AddMapping(10, smallParameter);
    largeBank.AddMapping(10, largeParameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&largeBank);
    auto ui = manager.CreateUIState();

    manager.PopulateUIState(*ui);
    synth::Parameter::UIState& cell = ui->slots[0].cells[0];
    REQUIRE_TRUE(cell.modulatorColorCapacity == 3);
    REQUIRE_TRUE(cell.modulatorColorCount.load() == 3);
    REQUIRE_TRUE(cell.modulatorSourceColors[0].Load() == synth::Color::Cyan);
    REQUIRE_TRUE(cell.modulatorSourceColors[1].Load() == synth::Color::Orange);
    REQUIRE_TRUE(cell.modulatorSourceColors[2].Load() == synth::Color::Green);

    slot.SelectBank(&smallBank);
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(cell.modulatorColorCapacity == 3);
    REQUIRE_TRUE(cell.modulatorColorCount.load() == 1);
    REQUIRE_TRUE(cell.modulatorSourceColors[0].Load() == synth::Color::Red);
    REQUIRE_TRUE(cell.modulatorSourceColors[1].Load() == synth::Color::Off);
    REQUIRE_TRUE(cell.modulatorSourceColors[2].Load() == synth::Color::Off);
}

TEST_CASE(manager_assigns_unique_ids) {
    synth::ParameterManager manager;
    manager.SetGestureCount(4);
    auto& groupA = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& groupB = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });

    REQUIRE_TRUE(groupA.CanAllocate());
    REQUIRE_TRUE(groupB.CanAllocate());
    auto& first = manager.CreateParameter(groupA, {.name = "A1", .defaultValue = 0.1f});
    auto& second = manager.CreateParameter(groupB, {.name = "B1", .defaultValue = 0.2f});
    auto& third = manager.CreateParameter(groupA, {.name = "A2", .defaultValue = 0.3f});
    REQUIRE_TRUE(first.Id() != second.Id());
    REQUIRE_TRUE(second.Id() != third.Id());
    REQUIRE_TRUE(first.Id() == 0);
    REQUIRE_TRUE(second.Id() == 1);
    REQUIRE_TRUE(third.Id() == 2);
}

TEST_CASE(manager_register_parameter_returns_zero_based_list_index) {
    synth::ParameterManager manager;
    auto& groupA = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& groupB = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});

    const synth::ParameterId firstId = manager.RegisterParameter(groupA, {.name = "A", .defaultValue = 0.1f});
    const synth::ParameterId secondId = manager.RegisterParameter(groupB, {.name = "B", .defaultValue = 0.2f});
    const synth::ParameterId thirdId = manager.RegisterParameter(groupA, {.name = "C", .defaultValue = 0.3f});

    REQUIRE_TRUE(firstId == 0);
    REQUIRE_TRUE(secondId == 1);
    REQUIRE_TRUE(thirdId == 2);
    REQUIRE_TRUE(manager.ParameterCount() == 3);
    REQUIRE_TRUE(&manager.ParameterById(firstId) == &groupA.ParameterByLocalIndex(0));
    REQUIRE_TRUE(&manager.ParameterById(secondId) == &groupB.ParameterByLocalIndex(0));
    REQUIRE_TRUE(&manager.ParameterById(thirdId) == &groupA.ParameterByLocalIndex(1));
}

TEST_CASE(register_parameter_rejects_duplicate_effective_names) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    (void)manager.RegisterParameter(group, {.name = "Cutoff", .defaultValue = 0.1f});

    bool threw = false;
    try {
        (void)manager.RegisterParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    } catch (const std::logic_error&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(manager.ParameterCount() == 1);
    REQUIRE_TRUE(group.ParameterCount() == 1);
}

TEST_CASE(parameter_lookup_rejects_invalid_id) {
    synth::ParameterManager manager;

    bool threw = false;
    try {
        (void)manager.ParameterById(0);
    } catch (const std::out_of_range&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
}

TEST_CASE(create_parameter_uses_zero_based_list_index_ids) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});

    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.1f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.2f});

    REQUIRE_TRUE(first.Id() == 0);
    REQUIRE_TRUE(second.Id() == 1);
    REQUIRE_TRUE(manager.ParameterCount() == 2);
    REQUIRE_TRUE(&manager.ParameterById(first.Id()) == &first);
    REQUIRE_TRUE(&manager.ParameterById(second.Id()) == &second);
}

TEST_CASE(modulators_use_voice_major_dot_product) {
    synth::Modulators modulators(3, 3);
    modulators.Value(2, 0) = 0.25f;
    modulators.Value(2, 1) = -0.5f;
    modulators.Value(2, 2) = 1.0f;

    const float depths[] = {0.4f, 0.2f, -0.1f};
    REQUIRE_NEAR(modulators.Apply(2, std::span<const float>(depths)), -0.1f, 0.0001f);
}

TEST_CASE(modulator_metadata_is_not_per_voice) {
    synth::Modulators modulators(3, 2);
    modulators.Metadata(1).name = "LFO";
    modulators.Metadata(1).shortName = "LF";
    modulators.Metadata(1).sourceColor = {.r = 12, .g = 34, .b = 56};
    modulators.Metadata(1).connected = true;

    modulators.Value(0, 1) = 0.25f;
    modulators.Value(2, 1) = -0.75f;

    REQUIRE_TRUE(modulators.Metadata(1).name == "LFO");
    REQUIRE_TRUE(modulators.Metadata(1).shortName == "LF");
    REQUIRE_TRUE(modulators.Metadata(1).sourceColor.g == 34);
    REQUIRE_TRUE(modulators.Metadata(1).connected);
    REQUIRE_NEAR(modulators.Value(0, 1), 0.25f, 0.0001f);
    REQUIRE_NEAR(modulators.Value(2, 1), -0.75f, 0.0001f);
}

TEST_CASE(group_modulation_source_registration_stores_metadata) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    float voice0 = 0.25f;
    float voice1 = 0.75f;
    std::array<float*, 2> sources{&voice0, &voice1};

    group.SetModulationSource(0, sources, {
                                             .name = "VCO",
                                             .shortName = "VCO",
                                             .sourceColor = synth::Color::Cyan,
                                             .connected = true,
                                         });

    const synth::ModulatorMetadata& metadata = group.GetModulators().Metadata(0);
    REQUIRE_TRUE(metadata.name == "VCO");
    REQUIRE_TRUE(metadata.shortName == "VCO");
    REQUIRE_TRUE(metadata.sourceColor == synth::Color::Cyan);
    REQUIRE_TRUE(metadata.connected);
}

TEST_CASE(group_update_mod_values_refreshes_registered_source_each_sample) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    float voice0 = 0.25f;
    float voice1 = 0.75f;
    std::array<float*, 2> sources{&voice0, &voice1};
    group.SetModulationSource(0, sources, {.name = "VCO", .connected = true});

    group.UpdateModValues();
    REQUIRE_NEAR(group.GetModulators().Value(0, 0), 0.25f, 0.0001f);
    REQUIRE_NEAR(group.GetModulators().Value(1, 0), 0.75f, 0.0001f);

    voice0 = 0.4f;
    voice1 = 0.6f;
    group.UpdateModValues();
    REQUIRE_NEAR(group.GetModulators().Value(0, 0), 0.4f, 0.0001f);
    REQUIRE_NEAR(group.GetModulators().Value(1, 0), 0.6f, 0.0001f);
}

TEST_CASE(group_modulation_source_registration_rejects_invalid_inputs_without_mutation) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    float voice0 = 0.2f;
    float voice1 = 0.8f;
    std::array<float*, 2> validSources{&voice0, &voice1};
    group.SetModulationSource(0, validSources, {
                                                .name = "Original",
                                                .shortName = "Orig",
                                                .sourceColor = synth::Color::Green,
                                                .connected = true,
                                            });
    group.UpdateModValues();

    bool threw = false;
    try {
        std::array<float*, 1> wrongCount{&voice0};
        group.SetModulationSource(0, wrongCount, {.name = "Wrong", .connected = true});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        std::array<float*, 2> nullConnected{&voice0, nullptr};
        group.SetModulationSource(0, nullConnected, {.name = "Null", .connected = true});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        group.SetModulationSource(1, validSources, {.name = "Invalid", .connected = true});
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    const synth::ModulatorMetadata& metadata = group.GetModulators().Metadata(0);
    REQUIRE_TRUE(metadata.name == "Original");
    REQUIRE_TRUE(metadata.shortName == "Orig");
    REQUIRE_TRUE(metadata.sourceColor == synth::Color::Green);
    REQUIRE_TRUE(metadata.connected);

    voice0 = 0.3f;
    voice1 = 0.7f;
    group.UpdateModValues();
    REQUIRE_NEAR(group.GetModulators().Value(0, 0), 0.3f, 0.0001f);
    REQUIRE_NEAR(group.GetModulators().Value(1, 0), 0.7f, 0.0001f);
}

TEST_CASE(manager_update_mod_values_delegates_to_one_group_or_all_groups) {
    synth::ParameterManager manager;
    auto& groupA = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& groupB = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    float sourceA = 0.1f;
    float sourceB = 0.9f;
    std::array<float*, 1> sourcesA{&sourceA};
    std::array<float*, 1> sourcesB{&sourceB};
    groupA.SetModulationSource(0, sourcesA, {.name = "A", .connected = true});
    groupB.SetModulationSource(0, sourcesB, {.name = "B", .connected = true});

    manager.UpdateModValues(groupA);
    REQUIRE_NEAR(groupA.GetModulators().Value(0, 0), 0.1f, 0.0001f);
    REQUIRE_NEAR(groupB.GetModulators().Value(0, 0), 0.0f, 0.0001f);

    sourceA = 0.2f;
    sourceB = 0.8f;
    manager.UpdateModValues();
    REQUIRE_NEAR(groupA.GetModulators().Value(0, 0), 0.2f, 0.0001f);
    REQUIRE_NEAR(groupB.GetModulators().Value(0, 0), 0.8f, 0.0001f);
}

TEST_CASE(update_mod_values_copies_source_values_unchanged) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    float voice0 = -0.25f;
    float voice1 = 1.25f;
    std::array<float*, 2> sources{&voice0, &voice1};
    group.SetModulationSource(0, sources, {.name = "Raw", .connected = true});

    group.UpdateModValues();
    REQUIRE_NEAR(group.GetModulators().Value(0, 0), -0.25f, 0.0001f);
    REQUIRE_NEAR(group.GetModulators().Value(1, 0), 1.25f, 0.0001f);
}

TEST_CASE(gestures_store_values_and_selection) {
    synth::Gestures gestures(2);
    gestures.Metadata(1).name = "Pressure";
    gestures.Metadata(1).gestureColor = {.r = 200, .g = 20, .b = 30};

    gestures.Value(1) = 0.75f;
    gestures.Select(1, true);

    REQUIRE_NEAR(gestures.Value(1), 0.75f, 0.0001f);
    REQUIRE_TRUE(gestures.Selected(1));
    REQUIRE_TRUE(gestures.Metadata(1).name == "Pressure");

    gestures.ClearSelection();
    REQUIRE_TRUE(!gestures.Selected(1));
    REQUIRE_NEAR(gestures.Value(1), 0.75f, 0.0001f);
}

TEST_CASE(parameter_default_state) {
    synth::ParameterManager manager;
    manager.SetGestureCount(4);
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 2,
        .numScenes = 2,
        .maxParameters = 1,
    });

    auto& parameter = manager.CreateParameter(group, {
        .name = "Cutoff",
        .shortName = "Cut",
        .defaultValue = 0.3f,
        .range = synth::RangeKind::Unipolar,
    });

    REQUIRE_TRUE(parameter.Id() == 0);
    REQUIRE_TRUE(parameter.Name() == "Cutoff");
    REQUIRE_TRUE(&parameter.Group() == &group);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.3f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.3f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.3f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.3f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenterScale(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenterScale(1), 1.0f, 0.0001f);
    REQUIRE_TRUE(parameter.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(!parameter.GestureActive(0, 0));
    REQUIRE_NEAR(parameter.GestureValue(1, 0), 0.3f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentDepthForSource(0, 0), 0.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(1, 1), 0.0f, 0.0001f);
}

TEST_CASE(bipolar_parameter_core_stores_normalized_center) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {
        .name = "Pan",
        .defaultValue = 0.5f,
        .range = synth::RangeKind::Bipolar,
    });

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.GetRaw(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 0.5f, 0.0001f);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.GetRaw(0), 0.0f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.GetRaw(0), 1.0f, 0.0001f);
}

TEST_CASE(allocator_exhaustion_does_not_register_partial_parameter) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });

    auto& first = manager.CreateParameter(group, {.name = "A", .defaultValue = 0.1f});
    REQUIRE_TRUE(first.Id() == 0);
    REQUIRE_TRUE(group.ParameterCount() == 1);
    REQUIRE_TRUE(!group.CanAllocate());

    bool threw = false;
    try {
        (void)manager.CreateParameter(group, {.name = "B", .defaultValue = 0.2f});
    } catch (const std::length_error&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(group.ParameterCount() == 1);
    REQUIRE_TRUE(first.Id() == 0);
    REQUIRE_TRUE(manager.ParameterCount() == 1);
}

TEST_CASE(stable_parameter_pointers_survive_later_allocations) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });

    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.1f});
    synth::Parameter* firstPtr = &first;
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.2f});
    auto& third = manager.CreateParameter(group, {.name = "Third", .defaultValue = 0.3f});

    REQUIRE_TRUE(&first == firstPtr);
    REQUIRE_TRUE(firstPtr->Id() == 0);
    REQUIRE_TRUE(second.Id() == 1);
    REQUIRE_TRUE(third.Id() == 2);
    REQUIRE_TRUE(group.ParameterCount() == 3);
}

TEST_CASE(scene_and_gesture_interpolation) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
    });

    auto& sceneOnly = manager.CreateParameter(group, {.name = "Scene", .defaultValue = 0.0f});
    sceneOnly.SceneCenter(0) = 0.2f;
    sceneOnly.SceneCenter(1) = 0.8f;
    sceneOnly.Compute({.leftScene = 0, .rightScene = 1, .blend = 0.25f});
    REQUIRE_NEAR(sceneOnly.TargetCenter(), 0.35f, 0.0001f);

    auto& gesture = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.4f});
    gesture.SceneCenter(0) = 0.4f;
    gesture.SceneCenter(1) = 0.4f;
    gesture.GestureValue(0, 0) = 0.9f;
    gesture.GestureValue(1, 0) = 0.9f;
    gesture.SetGestureActive(0, 0, true);
    gesture.SetGestureActive(1, 0, true);
    manager.SetGestureValue(0, 0.5f);
    gesture.Compute({.leftScene = 0, .rightScene = 1, .blend = 0.25f});
    REQUIRE_NEAR(gesture.TargetCenter(), 0.65f, 0.0001f);
}

TEST_CASE(top_level_compute_slews_target_center_with_group_alpha) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 0.25f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Smoothed", .defaultValue = 0.0f});

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.0001f);
}

TEST_CASE(target_center_alpha_one_preserves_direct_top_level_assignment) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Direct", .defaultValue = 0.0f});

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(parameter.TargetCenter(), 1.0f, 0.0001f);
}

TEST_CASE(multiple_gestures_use_effective_weighted_average) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "GestureMix", .defaultValue = 0.0f});
    parameter.SceneCenter(0) = 0.0f;
    parameter.GestureValue(0, 0) = 1.0f;
    parameter.GestureValue(0, 1) = 1.0f;
    parameter.SetGestureActive(0, 0, true);
    parameter.SetGestureActive(0, 1, true);
    manager.SetGestureValue(0, 0.2f);
    manager.SetGestureValue(1, 0.8f);

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(parameter.TargetCenter(), 0.68f, 0.0001f);
}

TEST_CASE(modulation_normalization_under_one) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.75f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(parameter.TargetCenterScale(0), 0.75f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 0), 0.25f, 0.0001f);
}

TEST_CASE(modulation_normalization_over_one_preserves_sign) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& positive = manager.CreateParameter(group, {
        .name = "Positive",
        .defaultValue = 1.0f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& negative = manager.CreateParameter(group, {
        .name = "Negative",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &positive));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &negative));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(parameter.TargetCenterScale(0), 0.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 1), -0.5f, 0.0001f);
}

TEST_CASE(negative_modulation_depths_add_normalization_offset) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& positive = manager.CreateParameter(group, {
        .name = "Positive",
        .defaultValue = 0.75f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& negative = manager.CreateParameter(group, {
        .name = "Negative",
        .defaultValue = 0.25f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &positive));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &negative));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();

    REQUIRE_NEAR(parameter.TargetCenterScale(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetNormalizationOffset(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 1), -0.25f, 0.0001f);

    group.GetModulators().Value(0, 0) = 0.0f;
    group.GetModulators().Value(0, 1) = 0.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 0.5f, 0.0001f);

    group.GetModulators().Value(0, 0) = 1.0f;
    group.GetModulators().Value(0, 1) = 0.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 0.75f, 0.0001f);

    group.GetModulators().Value(0, 0) = 0.0f;
    group.GetModulators().Value(0, 1) = 1.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 0.25f, 0.0001f);
}

TEST_CASE(overfull_negative_modulation_offset_uses_normalized_depths) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& positive = manager.CreateParameter(group, {
        .name = "Positive",
        .defaultValue = 1.0f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& negative = manager.CreateParameter(group, {
        .name = "Negative",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &positive));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &negative));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();

    REQUIRE_NEAR(parameter.TargetCenterScale(0), 0.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetNormalizationOffset(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 1), -0.5f, 0.0001f);

    group.GetModulators().Value(0, 0) = 0.0f;
    group.GetModulators().Value(0, 1) = 0.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 0.5f, 0.0001f);

    group.GetModulators().Value(0, 0) = 1.0f;
    group.GetModulators().Value(0, 1) = 0.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 1.0f, 0.0001f);

    group.GetModulators().Value(0, 0) = 0.0f;
    group.GetModulators().Value(0, 1) = 1.0f;
    REQUIRE_NEAR(parameter.GetRaw(0), 0.0f, 0.0001f);
}

TEST_CASE(recursive_modulation_depth_targets_use_bipolar_zero_based_exponential_curve) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);

    struct Case {
        float knob;
        float expectedDepth;
    };
    const std::array<Case, 5> cases{{
        {.knob = 0.0f, .expectedDepth = -1.0f},
        {.knob = 0.25f, .expectedDepth = -0.25f},
        {.knob = 0.5f, .expectedDepth = 0.0f},
        {.knob = 0.75f, .expectedDepth = 0.25f},
        {.knob = 1.0f, .expectedDepth = 1.0f},
    }};

    for (const Case& testCase : cases) {
        depth->SceneCenter(0) = testCase.knob;
        carrier.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
        REQUIRE_NEAR(depth->GetRaw(0), testCase.knob, 0.0001f);
        REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), testCase.expectedDepth, 0.0001f);
    }
}

TEST_CASE(recursive_modulation_depth_compute_ignores_target_center_smoothing_for_parent_reads) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 0.25f,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);

    depth->SceneCenter(0) = 1.0f;
    carrier.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(depth->TargetCenter(), 1.0f, 0.0001f);
    REQUIRE_NEAR(depth->CurrentCenter(), 1.0f, 0.0001f);
    REQUIRE_NEAR(depth->GetRaw(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), 1.0f, 0.0001f);
}

TEST_CASE(recursive_modulation_depth_three_quarter_turn_sets_quarter_raw_depth_before_normalization) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 0.75f;

    carrier.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), 0.25f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetCenterScale(0), 0.75f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetNormalizationOffset(0), 0.0f, 0.0001f);
}

TEST_CASE(curved_modulation_depth_targets_still_use_signed_normalization) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    synth::Parameter* positive = carrier.EnsureModulationDepth(0);
    synth::Parameter* negative = carrier.EnsureModulationDepth(1);
    REQUIRE_TRUE(positive != nullptr);
    REQUIRE_TRUE(negative != nullptr);
    positive->SceneCenter(0) = 1.0f;
    negative->SceneCenter(0) = 0.0f;

    carrier.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    carrier.ProcessLite();

    REQUIRE_NEAR(carrier.TargetCenterScale(0), 0.0f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetNormalizationOffset(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), 0.5f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 1), -0.5f, 0.0001f);
}

TEST_CASE(curved_modulation_depth_targets_keep_modulator_dot_product_linear) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.0f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 0.75f;

    carrier.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    carrier.ProcessLite();

    group.GetModulators().Value(0, 0) = 0.8f;
    REQUIRE_NEAR(carrier.CurrentDepthForSource(0, 0), 0.25f, 0.0001f);
    REQUIRE_NEAR(carrier.GetRaw(0), 0.2f, 0.0001f);
}

TEST_CASE(parameter_get_raw_includes_normalization_offset) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 8,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    float mod0 = 0.0f;
    float mod1 = 0.0f;
    std::array<float*, 1> source0{&mod0};
    std::array<float*, 1> source1{&mod1};
    group.SetModulationSource(0, source0, {.connected = true});
    group.SetModulationSource(1, source1, {.connected = true});
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Cutoff", .shortName = "Cut", .defaultValue = 0.5f});
    parameter.EnsureModulationDepth(0)->SceneCenter(0) = 0.75f;
    parameter.EnsureModulationDepth(1)->SceneCenter(0) = 0.25f;
    manager.ComputeAllParameters();
    group.UpdateModValues();
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.GetRaw(0), 0.5f, 0.0001f);
}

TEST_CASE(ui_state_min_max_reports_underfull_modulation_reachable_range) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& positive = manager.CreateParameter(group, {
        .name = "Positive",
        .defaultValue = 0.25f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& negative = manager.CreateParameter(group, {
        .name = "Negative",
        .defaultValue = 0.25f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &positive));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &negative));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();

    synth::Parameter::UIState ui(1);
    parameter.PopulateUIState(ui);
    REQUIRE_NEAR(ui.minValues[0].load(), 0.25f, 0.0001f);
    REQUIRE_NEAR(ui.maxValues[0].load(), 0.75f, 0.0001f);
}

TEST_CASE(ui_state_min_max_reports_full_range_when_modulation_is_overfull) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {
        .name = "Carrier",
        .defaultValue = 0.5f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& positive = manager.CreateParameter(group, {
        .name = "Positive",
        .defaultValue = 1.0f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& negative = manager.CreateParameter(group, {
        .name = "Negative",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &positive));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &negative));

    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();

    synth::Parameter::UIState ui(1);
    parameter.PopulateUIState(ui);
    REQUIRE_NEAR(ui.minValues[0].load(), -1.0f, 0.0001f);
    REQUIRE_NEAR(ui.maxValues[0].load(), 1.0f, 0.0001f);
}

TEST_CASE(nested_depth_route_reads_get_and_bypasses_slew) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
        .processLiteAlpha = 0.1f,
        .targetCenterAlpha = 1.0f,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.0f});
    depth.SceneCenter(0) = 0.8f;
    depth.SceneCenter(1) = 0.8f;
    REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depth));

    carrier.Compute({.leftScene = 0, .rightScene = 1, .blend = 0.0f});

    REQUIRE_TRUE(depth.RecursionDepth() == 1);
    REQUIRE_NEAR(depth.CurrentCenter(), 0.8f, 0.0001f);
    REQUIRE_NEAR(depth.GetRaw(0), 0.8f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), 0.3421493f, 0.0001f);
    REQUIRE_NEAR(carrier.TargetCenterScale(0), 0.6578507f, 0.0001f);
}

TEST_CASE(process_lite_slews_center_scale_offset_and_depths) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 1.0f,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.0f});
    auto& depth = manager.CreateParameter(group, {
        .name = "Depth",
        .defaultValue = 0.25f,
        .range = synth::RangeKind::Bipolar,
    });
    parameter.SceneCenter(0) = 1.0f;
    parameter.SceneCenter(1) = 1.0f;
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));

    parameter.Compute({.leftScene = 0, .rightScene = 1, .blend = 0.0f});
    parameter.ProcessLite();

    REQUIRE_NEAR(parameter.CurrentCenter(), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenterScale(0), 0.9375f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentNormalizationOffset(0), 0.0625f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentDepthForSource(0, 0), -0.0625f, 0.0001f);

    synth::Parameter::UIState ui(1);
    parameter.PopulateUIState(ui);
    REQUIRE_NEAR(ui.minValues[0].load(), 0.1875f, 0.0001f);
    REQUIRE_NEAR(ui.maxValues[0].load(), 0.25f, 0.0001f);
}

TEST_CASE(process_lite_samples_cached_knob_after_slew) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 1.0f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Level", .shortName = "Lvl", .defaultValue = 0.0f});
    manager.ComputeAllParameters();
    parameter.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.GetRaw(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 0.25f, 0.0001f);
}

TEST_CASE(process_lite_phases_replace_cached_knob_before_ui_smoothing) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Level", .defaultValue = 0.0f});
    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());

    parameter.ProcessLitePhase1();
    const float rawCached = parameter.CachedKnobValue(0);
    const float centerBeforePhase2 = parameter.UIDisplayCenter(0);
    parameter.ReplaceCachedKnobValue(0, 0.25f);
    REQUIRE_NEAR(parameter.GetRaw(0), rawCached, 0.000001f);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), centerBeforePhase2, 0.000001f);
    parameter.ProcessLitePhase2();
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 0.25f, 0.000001f);

    parameter.ReplaceCachedKnobValue(0, -1.0f);
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 0.0f, 0.000001f);
    parameter.ReplaceCachedKnobValue(0, 2.0f);
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 1.0f, 0.000001f);
}

TEST_CASE(process_lite_wrapper_matches_explicit_phases) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 1.0f,
    });
    auto& wrapped = manager.CreateParameter(group, {.name = "Wrapped", .defaultValue = 0.0f});
    auto& phased = manager.CreateParameter(group, {.name = "Phased", .defaultValue = 0.0f});
    wrapped.SceneCenter(0) = 1.0f;
    phased.SceneCenter(0) = 1.0f;
    wrapped.Compute(manager.Scene());
    phased.Compute(manager.Scene());

    wrapped.ProcessLite();
    phased.ProcessLitePhase1();
    phased.ProcessLitePhase2();

    REQUIRE_NEAR(phased.GetRaw(0), wrapped.GetRaw(0), 0.000001f);
    REQUIRE_NEAR(phased.CachedKnobValue(0), wrapped.CachedKnobValue(0), 0.000001f);
    REQUIRE_NEAR(phased.UIDisplayCenter(0), wrapped.UIDisplayCenter(0), 0.000001f);
    REQUIRE_NEAR(phased.UIDisplaySpread(0), wrapped.UIDisplaySpread(0), 0.000001f);
}

TEST_CASE(parameter_process_sample_phases_recompute_only_in_phase_one) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .targetComputeIntervalSamples = 16,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Probe", .defaultValue = 0.0f});

    parameter.SceneCenter(0) = 0.25f;
    parameter.ProcessSamplePhase1(0);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.000001f);

    parameter.SceneCenter(0) = 0.75f;
    parameter.ProcessSamplePhase2();
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.000001f);
    parameter.ProcessSamplePhase1(15);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.000001f);
    parameter.ProcessSamplePhase1(16);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.75f, 0.000001f);
}

TEST_CASE(parameter_process_sample_recomputes_on_configured_interval) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .targetComputeIntervalSamples = 16,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Probe", .defaultValue = 0.0f});

    parameter.SceneCenter(0) = 0.25f;
    parameter.ProcessSample(0);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.25f, 0.0001f);

    parameter.SceneCenter(0) = 0.75f;
    parameter.ProcessSample(15);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.0001f);

    parameter.ProcessSample(16);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.75f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.75f, 0.0001f);
}

TEST_CASE(parameter_process_sample_slews_target_center_before_process_lite) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 0.5f,
        .targetCenterAlpha = 0.25f,
        .targetComputeIntervalSamples = 16,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Probe", .defaultValue = 0.0f});

    parameter.SceneCenter(0) = 1.0f;
    parameter.ProcessSample(16);

    REQUIRE_NEAR(parameter.TargetCenter(), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.125f, 0.0001f);
}

TEST_CASE(parameter_group_process_sample_covers_top_level_and_modulation_depth_targets) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .targetComputeIntervalSamples = 16,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.0f});
    auto& sibling = manager.CreateParameter(group, {.name = "Sibling", .defaultValue = 0.1f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);

    carrier.SceneCenter(0) = 0.2f;
    sibling.SceneCenter(0) = 0.4f;
    depth->SceneCenter(0) = 0.75f;

    group.ProcessSample(0);

    REQUIRE_NEAR(carrier.TargetCenter(), 0.2f, 0.0001f);
    REQUIRE_NEAR(sibling.TargetCenter(), 0.4f, 0.0001f);
    REQUIRE_NEAR(depth->TargetCenter(), 0.75f, 0.0001f);
    REQUIRE_NEAR(carrier.CurrentDepthForSource(0, 0), 0.25f, 0.0001f);
}

TEST_CASE(group_process_sample_visits_only_registered_roots) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 8,
        .targetComputeIntervalSamples = 16,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier"});
    (void)manager.CreateParameter(group, {.name = "Tone"});
    auto& depth = carrier.EnsureModulationDepth(0, {.name = "Carrier M1", .defaultValue = 0.5f});
    (void)depth.EnsureModulationDepth(1, {.name = "Carrier M1 M2", .defaultValue = 0.5f});
    synth::ParameterProcessingObserver work{};
    group.SetProcessingObserverForTests(&work);

    group.ProcessSample(1);
    REQUIRE_TRUE(work.topLevelProcessLiteCalls == 2);
    REQUIRE_TRUE(work.localRecursiveComputeCalls == 0);

    group.ProcessSample(16);
    REQUIRE_TRUE(work.topLevelProcessLiteCalls == 4);
    REQUIRE_TRUE(work.localRecursiveComputeCalls == 2);
}

TEST_CASE(group_process_sample_phases_visit_only_registered_roots) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 8,
        .targetComputeIntervalSamples = 16,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier"});
    (void)manager.CreateParameter(group, {.name = "Tone"});
    auto& depth = carrier.EnsureModulationDepth(0, {.name = "Carrier M1", .defaultValue = 0.5f});
    (void)depth.EnsureModulationDepth(1, {.name = "Carrier M1 M2", .defaultValue = 0.5f});
    synth::ParameterProcessingObserver work{};
    group.SetProcessingObserverForTests(&work);

    group.ProcessSamplePhase1(16);
    REQUIRE_TRUE(work.topLevelProcessLiteCalls == 2);
    REQUIRE_TRUE(work.localRecursiveComputeCalls == 2);

    group.ProcessSamplePhase2();
    REQUIRE_TRUE(work.topLevelProcessLiteCalls == 2);
}

TEST_CASE(recursive_local_compute_seeds_display_without_audio_rate_processing) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .targetComputeIntervalSamples = 16,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier"});
    auto& depth = carrier.EnsureModulationDepth(0, {.name = "Carrier M1", .defaultValue = 0.5f});
    depth.SceneCenter(0) = 0.75f;
    synth::ParameterProcessingObserver work{};
    group.SetProcessingObserverForTests(&work);

    group.ProcessSample(16);

    REQUIRE_TRUE(work.topLevelProcessLiteCalls == 1);
    REQUIRE_TRUE(work.localRecursiveComputeCalls == 1);
    REQUIRE_NEAR(depth.UIDisplayCenter(0), depth.GetRaw(0), 0.000001f);
    REQUIRE_NEAR(depth.UIDisplaySpread(0), 0.0f, 0.000001f);
}

TEST_CASE(mapping_helpers_use_cached_process_lite_knob_value) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    float mod = 0.0f;
    std::array<float*, 1> source{&mod};
    group.SetModulationSource(0, source, {.connected = true});
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Shape", .shortName = "Shp", .defaultValue = 0.0f});
    const synth::ParameterId id = parameter.Id();
    parameter.EnsureModulationDepth(0)->SceneCenter(0) = 1.0f;
    synth::Parameter& bipolar = manager.CreateParameter(group, {
        .name = "Amount",
        .shortName = "Amt",
        .defaultValue = 0.5f,
        .range = synth::RangeKind::Bipolar,
    });
    const synth::ParameterId bipolarId = bipolar.Id();
    bipolar.EnsureModulationDepth(0)->SceneCenter(0) = 1.0f;

    manager.ComputeAllParameters();
    group.UpdateModValues();
    parameter.ProcessLite();
    bipolar.ProcessLite();
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, id), 10.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetExponential(10.0f, 40.0f, 0, id), 10.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, id), 0.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, bipolarId), -2.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, bipolarId), -4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 1.0f, 4.0f, 0, bipolarId), 0.25f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, bipolarId), -1.0f, 0.0001f);

    mod = 1.0f;
    group.UpdateModValues();
    REQUIRE_NEAR(parameter.GetRaw(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(bipolar.GetRaw(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, id), 10.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetExponential(10.0f, 40.0f, 0, id), 10.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, id), 0.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, bipolarId), -2.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, bipolarId), -4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 1.0f, 4.0f, 0, bipolarId), 0.25f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, bipolarId), -1.0f, 0.0001f);

    parameter.ProcessLite();
    bipolar.ProcessLite();
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, id), 20.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetExponential(10.0f, 40.0f, 0, id), 40.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, id), 1.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, bipolarId), 2.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, bipolarId), 4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 1.0f, 4.0f, 0, bipolarId), 4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, bipolarId), 1.0f, 0.0001f);
}

TEST_CASE(process_lite_updates_ui_display_center_and_spread) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Level", .shortName = "Lvl", .defaultValue = 0.0f});
    manager.ComputeAllParameters();
    parameter.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    parameter.ProcessLite();
    REQUIRE_NEAR(parameter.CachedKnobValue(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplaySpread(0), std::sqrt(0.28125f), 0.0001f);
}

TEST_CASE(ui_display_center_and_spread_follow_cached_knob_order) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Cutoff", .shortName = "Cut", .defaultValue = 0.0f});

    manager.ComputeAllParameters();
    parameter.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    parameter.ProcessLite();

    synth::Parameter::UIState ui(1);
    parameter.PopulateUIState(ui);
    REQUIRE_NEAR(ui.values[0].load(std::memory_order_relaxed), 0.25f, 0.0001f);
    REQUIRE_NEAR(ui.spreadValues[0].load(std::memory_order_relaxed), std::sqrt(0.28125f), 0.0001f);
}

TEST_CASE(bipolar_ui_state_publishes_signed_center_range_and_spread) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& parameter = manager.CreateParameter(group, {
        .name = "Pan",
        .shortName = "Pan",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
    });

    manager.ComputeAllParameters();
    parameter.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    parameter.ProcessLite();

    synth::Parameter::UIState ui(1);
    parameter.PopulateUIState(ui);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplaySpread(0), std::sqrt(0.28125f), 0.0001f);
    REQUIRE_NEAR(ui.values[0].load(std::memory_order_relaxed), -0.5f, 0.0001f);
    REQUIRE_NEAR(ui.spreadValues[0].load(std::memory_order_relaxed), 2.0f * std::sqrt(0.28125f), 0.0001f);
    REQUIRE_NEAR(ui.minValues[0].load(std::memory_order_relaxed), 1.0f, 0.0001f);
    REQUIRE_NEAR(ui.maxValues[0].load(std::memory_order_relaxed), 1.0f, 0.0001f);
}

TEST_CASE(cached_knob_and_ui_display_state_seed_on_construction_and_revert) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& parameter =
        manager.CreateParameter(group, {.name = "Level", .shortName = "Lvl", .defaultValue = 0.25f});

    REQUIRE_NEAR(parameter.CachedKnobValue(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplaySpread(0), 0.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.75f;
    manager.ComputeAllParameters();
    REQUIRE_NEAR(parameter.CachedKnobValue(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplaySpread(0), 0.0f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    parameter.ProcessLite();
    REQUIRE_TRUE(parameter.UIDisplaySpread(0) > 0.0f);

    parameter.RevertToDefault(manager.Scene());
    REQUIRE_NEAR(parameter.CachedKnobValue(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplayCenter(0), parameter.GetRaw(0), 0.0001f);
    REQUIRE_NEAR(parameter.UIDisplaySpread(0), 0.0f, 0.0001f);
}

TEST_CASE(nested_modulation_depth_ui_state_uses_true_value_without_motion) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    float modValue = 0.75f;
    std::array<float*, 1> source{&modValue};
    group.SetModulationSource(0, source, {.name = "VCO", .shortName = "VCO", .connected = true});

    synth::Parameter& target =
        manager.CreateParameter(group, {.name = "Phase", .shortName = "Phas", .defaultValue = 0.0f});
    synth::Parameter* depth = target.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);

    manager.ComputeAllParameters();
    REQUIRE_NEAR(depth->UIDisplayCenter(0), depth->GetRaw(0), 0.0001f);

    depth->SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();

    REQUIRE_NEAR(depth->GetRaw(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(depth->UIDisplayCenter(0), depth->GetRaw(0), 0.0001f);
    REQUIRE_NEAR(depth->UIDisplaySpread(0), 0.0f, 0.0001f);
}

TEST_CASE(switch_metadata_and_buckets_use_unslewed_display_target) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 1.0f,
    });

    auto& stepped = manager.CreateParameter(group, {
        .name = "Mode",
        .defaultValue = 0.0f,
        .switchValues = 4,
    });
    stepped.SceneCenter(1) = 1.0f;
    stepped.Compute({.leftScene = 0, .rightScene = 1, .blend = 1.0f});

    REQUIRE_TRUE(stepped.SwitchValues() == 4);
    REQUIRE_TRUE(stepped.IsSwitch());
    REQUIRE_NEAR(stepped.GetRaw(0), 0.0f, 0.0001f);
    REQUIRE_TRUE(stepped.GetSwitchVal(0) == 3);

    auto& bipolar = manager.CreateParameter(group, {
        .name = "Bipolar",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
        .switchValues = 4,
    });
    bipolar.SceneCenter(1) = 0.5f;
    bipolar.Compute({.leftScene = 0, .rightScene = 1, .blend = 1.0f});
    REQUIRE_TRUE(bipolar.GetSwitchVal(0) == 2);

    synth::Parameter::UIState ui(1);
    stepped.PopulateUIState(ui);
    REQUIRE_TRUE(ui.switchValues.load() == 4);
    REQUIRE_TRUE(ui.switchValue[0].load() == 3);
    REQUIRE_NEAR(ui.spreadValues[0].load(std::memory_order_relaxed), 0.0f, 0.0001f);

    ui.SetDisconnected();
    REQUIRE_TRUE(ui.switchValues.load() == 0);
    REQUIRE_TRUE(ui.switchValue[0].load() == 0);
    REQUIRE_NEAR(ui.spreadValues[0].load(std::memory_order_relaxed), 0.0f, 0.0001f);
    REQUIRE_TRUE(ui.modulatorsAffectingMask.load() == 0);
    REQUIRE_TRUE(ui.gesturesAffectingMask.load() == 0);
}

TEST_CASE(switch_value_uses_target_despite_process_lite_slew) {
    synth::ParameterManager manager;
    synth::ParameterGroupConfig config{
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 0.25f,
        .targetCenterAlpha = 1.0f,
        .uiDisplayCenterAlpha = 0.25f,
        .uiDisplaySpreadAlpha = 0.5f,
    };
    synth::ParameterGroup& group = manager.CreateGroup(config);
    synth::Parameter& stepped = manager.CreateParameter(group, {
        .name = "Mode",
        .shortName = "Mode",
        .defaultValue = 0.0f,
        .switchValues = 4,
    });

    manager.ComputeAllParameters();
    stepped.SceneCenter(0) = 1.0f;
    manager.ComputeAllTargets();
    stepped.ProcessLite();

    REQUIRE_NEAR(stepped.GetRaw(0), 0.25f, 0.0001f);
    REQUIRE_TRUE(stepped.GetSwitchVal(0) == 3);

    synth::Parameter::UIState ui(1);
    stepped.PopulateUIState(ui);
    REQUIRE_NEAR(ui.spreadValues[0].load(std::memory_order_relaxed), 0.0f, 0.0001f);
}

TEST_CASE(parameter_ui_state_clears_semantic_colors_when_disconnected) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {
        .name = "Level",
        .defaultValue = 0.5f,
    });
    synth::Parameter::UIState state(1, 1, 1);

    parameter.PopulateUIState(state);
    REQUIRE_TRUE(state.connected.load(std::memory_order_relaxed));
    REQUIRE_TRUE(state.baseColor.Load() == synth::Color::Grey);

    state.SetDisconnected();
    REQUIRE_TRUE(!state.connected.load(std::memory_order_relaxed));
    REQUIRE_TRUE(state.baseColor.Load() == synth::Color::Off);
    REQUIRE_TRUE(state.indicatorColors[0].Load() == synth::Color::Off);
    REQUIRE_TRUE(state.modulatorColorCount.load() == 0);
    REQUIRE_TRUE(state.gestureColorCount.load() == 0);
    REQUIRE_TRUE(state.modulatorSourceColors[0].Load() == synth::Color::Off);
    REQUIRE_TRUE(state.gestureColors[0].Load() == synth::Color::Off);
}

TEST_CASE(ui_state_reports_affecting_masks_through_gesture_index_63) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 33,
        .numScenes = 2,
        .maxParameters = 4,
    });

    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth31 = manager.CreateParameter(group, {.name = "Depth31", .defaultValue = 0.25f});
    auto& depth32 = manager.CreateParameter(group, {.name = "Depth32", .defaultValue = 0.25f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(31, &depth31));
    REQUIRE_TRUE(parameter.AssignModulationDepth(32, &depth32));
    depth31.SceneCenter(0) = 0.75f;
    parameter.SetGestureActive(0, 0, true);
    parameter.SetGestureActive(0, 31, true);
    parameter.SetGestureActive(0, 32, true);
    parameter.SetGestureActive(0, 63, true);
    parameter.SetGestureActive(1, 1, true);
    parameter.SetGestureActive(1, 31, true);
    parameter.SetGestureActive(1, 32, true);

    synth::Parameter::UIState ui(1);
    REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));

    manager.SetSceneBlend(0.0f);
    parameter.PopulateUIState(ui);
    REQUIRE_TRUE(ui.modulatorsAffectingMask.load() == (1u << 31));
    REQUIRE_TRUE(ui.gesturesAffectingMask.load() ==
                 ((std::uint64_t{1} << 0) | (std::uint64_t{1} << 31) |
                  (std::uint64_t{1} << 32) | (std::uint64_t{1} << 63)));

    manager.SetSceneBlend(1.0f);
    parameter.PopulateUIState(ui);
    REQUIRE_TRUE(ui.gesturesAffectingMask.load() ==
                 ((std::uint64_t{1} << 1) | (std::uint64_t{1} << 31) |
                  (std::uint64_t{1} << 32)));

    manager.SetSceneBlend(0.5f);
    parameter.PopulateUIState(ui);
    REQUIRE_TRUE(ui.gesturesAffectingMask.load() ==
                 ((std::uint64_t{1} << 0) | (std::uint64_t{1} << 1) |
                  (std::uint64_t{1} << 31) | (std::uint64_t{1} << 32) |
                  (std::uint64_t{1} << 63)));
}

TEST_CASE(ui_state_ignores_inactive_depth_gesture_values_for_modulator_mask) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });

    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    synth::Parameter* depth = carrier.EnsureModulationDepth(0);
    depth->SceneCenter(0) = 0.75f;
    depth->GestureValue(0, 0) = 0.75f;
    depth->SetGestureActive(0, 0, true);

    carrier.RevertToDefault(manager.Scene());

    REQUIRE_NEAR(depth->SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(depth->GestureValue(0, 0), 0.75f, 0.0001f);
    REQUIRE_TRUE(!depth->GestureActive(0, 0));

    synth::Parameter::UIState ui(1);
    carrier.PopulateUIState(ui);
    REQUIRE_TRUE(ui.modulatorsAffectingMask.load() == 0);
}

TEST_CASE(cycle_rejection_direct_and_indirect) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });

    auto& a = manager.CreateParameter(group, {.name = "A", .defaultValue = 0.1f});
    auto& b = manager.CreateParameter(group, {.name = "B", .defaultValue = 0.2f});
    auto& c = manager.CreateParameter(group, {.name = "C", .defaultValue = 0.3f});

    REQUIRE_TRUE(!a.AssignModulationDepth(0, &a));
    REQUIRE_TRUE(a.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(a.AssignModulationDepth(0, &b));
    REQUIRE_TRUE(b.AssignModulationDepth(0, &c));
    REQUIRE_TRUE(!c.AssignModulationDepth(0, &a));
    REQUIRE_TRUE(c.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(a.ModulationDepthParameter(0) == &b);
}

TEST_CASE(modulation_depth_assignment_rejects_cross_group_routes) {
    synth::ParameterManager manager;
    auto& carrierGroup = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 1,
    });
    auto& smallerDepthGroup = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });

    auto& carrier = manager.CreateParameter(carrierGroup, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth = manager.CreateParameter(smallerDepthGroup, {.name = "Other Group Depth", .defaultValue = 0.25f});

    REQUIRE_TRUE(!carrier.AssignModulationDepth(0, &depth));
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
}

TEST_CASE(get_clamps_and_rejects_out_of_range_voice) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Clamp", .defaultValue = 1.0f});
    auto* depth = parameter.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 1.0f;
    group.GetModulators().Value(0, 0) = 1.0f;
    manager.ComputeAllParameters();

    REQUIRE_NEAR(parameter.GetRaw(0), 1.0f, 0.0001f);

    bool threw = false;
    try {
        (void)parameter.GetRaw(1);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(manager_linear_mapping_reaches_endpoints) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(group, {.name = "Level", .defaultValue = 0.0f});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, paramId), 10.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, paramId), 15.0f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, paramId), 20.0f, 0.0001f);
}

TEST_CASE(manager_exponential_mapping_reaches_endpoints_and_midpoint) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(group, {.name = "Frequency", .defaultValue = 0.0f});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetExponential(32.0f, 3000.0f, 0, paramId), 32.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetExponential(32.0f, 3000.0f, 0, paramId), std::sqrt(32.0f * 3000.0f), 0.001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetExponential(32.0f, 3000.0f, 0, paramId), 3000.0f, 0.001f);
}

TEST_CASE(manager_zero_based_exponential_mapping_honors_midpoint) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(group, {.name = "Depth", .defaultValue = 0.0f});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, paramId), 0.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, paramId), 0.1f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetZeroBasedExponential(1.0f, 0.1f, 0, paramId), 1.0f, 0.0001f);
}

TEST_CASE(zero_based_exponential_base_helper_matches_midpoint_curve) {
    const float maxValue = 2.0f;
    const float midpointValue = 0.25f;
    const float base = synth::ZeroBasedExponentialBaseFromMidpoint(midpointValue, maxValue);

    REQUIRE_NEAR(synth::ZeroBasedExponentialMap(0.0f, base, maxValue), 0.0f, 0.0001f);
    REQUIRE_NEAR(synth::ZeroBasedExponentialMap(0.5f, base, maxValue), midpointValue, 0.0001f);
    REQUIRE_NEAR(synth::ZeroBasedExponentialMap(1.0f, base, maxValue), maxValue, 0.0001f);
    REQUIRE_NEAR(synth::ZeroBasedExponentialBaseFromMidpoint(1.0f, 2.0f), 1.0f, 0.0001f);
    REQUIRE_NEAR(synth::ZeroBasedExponentialMap(0.25f, 1.0f, maxValue), 0.5f, 0.0001f);
}

TEST_CASE(manager_bipolar_mapping_helpers_return_signed_values) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(
        group, {.name = "Amount", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, paramId), -2.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId), -4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), -1.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.25f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId), -0.25f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, paramId), 0.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId), 0.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), 0.0f, 0.0001f);
    bool threw = false;
    try {
        (void)manager.GetBipolarZeroBasedExponential(1.0f, 2.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    parameter.SceneCenter(0) = 0.75f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId), 0.25f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, paramId), 2.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId), 4.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), 1.0f, 0.0001f);
}

TEST_CASE(manager_bipolar_zero_based_exponential_is_continuous_at_center) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(
        group, {.name = "Amount", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.5005f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    const float smallPositive = manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    const float center = manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId);

    parameter.SceneCenter(0) = 0.4995f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    const float smallNegative = manager.GetBipolarZeroBasedExponential(4.0f, 0.25f, 0, paramId);

    REQUIRE_NEAR(center, 0.0f, 0.0001f);
    REQUIRE_TRUE(smallPositive > 0.0f);
    REQUIRE_TRUE(smallPositive < 0.01f);
    REQUIRE_NEAR(smallNegative, -smallPositive, 0.0001f);
}

TEST_CASE(manager_bipolar_mapping_helpers_reject_unipolar_parameters) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId =
        manager.RegisterParameter(group, {.name = "Amount", .defaultValue = 0.5f});
    auto& parameter = manager.ParameterById(paramId);
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();

    bool threw = false;
    try {
        (void)manager.GetBipolarLinear(1.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)manager.GetBipolarExponential(0.2f, 1.0f, 5.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(manager_centered_bipolar_exponential_maps_left_center_and_right_from_bipolar_knob) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(
        group, {.name = "Exponent", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarExponential(0.2f, 1.0f, 5.0f, 0, paramId), 0.2f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarExponential(0.2f, 1.0f, 5.0f, 0, paramId), 1.0f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarExponential(0.2f, 1.0f, 5.0f, 0, paramId), 5.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.25f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 2.0f, 32.0f, 0, paramId),
                 2.0f * std::sqrt(0.25f / 2.0f), 0.0001f);

    parameter.SceneCenter(0) = 0.75f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 2.0f, 32.0f, 0, paramId),
                 2.0f * std::sqrt(32.0f / 2.0f), 0.0001f);
}

TEST_CASE(manager_centered_bipolar_exponential_rejects_invalid_values) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(
        group, {.name = "Exponent", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});

    bool threw = false;
    try {
        (void)manager.GetBipolarExponential(0.0f, 1.0f, 5.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)manager.GetBipolarExponential(0.2f, 0.0f, 5.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)manager.GetBipolarExponential(0.2f, 1.0f, -5.0f, 0, paramId);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(manager_bipolar_zero_based_exponential_uses_signed_bipolar_knob) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId paramId = manager.RegisterParameter(
        group, {.name = "Amount", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& parameter = manager.ParameterById(paramId);

    parameter.SceneCenter(0) = 0.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), -1.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.25f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), -0.1f, 0.0001f);

    parameter.SceneCenter(0) = 0.5f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), 0.0f, 0.0001f);

    parameter.SceneCenter(0) = 0.75f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), 0.1f, 0.0001f);

    parameter.SceneCenter(0) = 1.0f;
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    REQUIRE_NEAR(manager.GetBipolarZeroBasedExponential(1.0f, 0.1f, 0, paramId), 1.0f, 0.0001f);
}

TEST_CASE(manager_mapping_helpers_map_cached_voice_value) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    const synth::ParameterId carrierId = manager.RegisterParameter(
        group, {.name = "Carrier", .defaultValue = 0.6f, .range = synth::RangeKind::Bipolar});
    const synth::ParameterId depthId = manager.RegisterParameter(group, {.name = "Depth", .defaultValue = 0.75f});
    auto& carrier = manager.ParameterById(carrierId);
    auto& depth = manager.ParameterById(depthId);
    REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depth));

    group.GetModulators().Value(0, 0) = 1.0f;
    carrier.Compute(manager.Scene());
    carrier.ProcessLite();

    REQUIRE_NEAR(carrier.GetRaw(0), 0.7f, 0.0001f);
    REQUIRE_NEAR(manager.GetLinear(10.0f, 20.0f, 0, carrierId), 17.0f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarLinear(2.0f, 0, carrierId), 0.8f, 0.0001f);
    REQUIRE_NEAR(manager.GetBipolarExponential(0.25f, 1.0f, 4.0f, 0, carrierId), std::pow(4.0f, 0.4f),
                 0.0001f);
}

TEST_CASE(handle_inc_dec_endpoint_scene) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Edit", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.4f;
    parameter.SceneCenter(1) = 0.8f;

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 1, .blend = 0.0f}, 0.7f);

    REQUIRE_NEAR(parameter.SceneCenter(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.8f, 0.0001f);
}

TEST_CASE(handle_inc_dec_mid_blend_matches_smart_grid_attenuation) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Edit", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.5f;
    parameter.SceneCenter(1) = 0.5f;

    const synth::SceneState scene{.leftScene = 0, .rightScene = 1, .blend = 0.5f};
    parameter.HandleIncDec(scene, 0.2f);
    parameter.Compute(scene);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.6f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.6f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.6f, 0.0001f);
}

TEST_CASE(handle_inc_dec_saturation_solve_matches_smart_grid) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Edit", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.9f;
    parameter.SceneCenter(1) = 0.2f;

    const synth::SceneState scene{.leftScene = 0, .rightScene = 1, .blend = 0.25f};
    parameter.HandleIncDec(scene, 0.2f);
    parameter.Compute(scene);

    REQUIRE_NEAR(parameter.SceneCenter(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.7f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.925f, 0.0001f);
}

TEST_CASE(selected_gesture_activation_snapshots_parent_value) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.1f});
    parameter.SceneCenter(0) = 0.25f;
    parameter.SceneCenter(1) = 0.75f;
    parameter.GestureValue(0, 63) = 0.9f;
    parameter.GestureValue(1, 63) = 0.9f;
    manager.SelectGesture(63);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 1, .blend = 0.0f}, 0.0f);

    REQUIRE_TRUE(parameter.GestureActive(0, 63));
    REQUIRE_TRUE(!parameter.GestureActive(1, 63));
    REQUIRE_NEAR(parameter.GestureValue(0, 63), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(1, 63), 0.9f, 0.0001f);
}

TEST_CASE(selected_inactive_gesture_first_turn_arms_without_applying_delta) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.25f});
    parameter.SceneCenter(0) = 0.25f;
    parameter.GestureValue(0, 0) = 1.0f;
    manager.SetGestureValue(0, 1.0f);
    manager.SelectGesture(0);

    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    parameter.HandleIncDec(scene, 0.2f);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.25f, 0.0001f);
}

TEST_CASE(selected_zero_weight_gesture_first_turn_arms_without_applying_delta) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.25f});
    parameter.SceneCenter(0) = 0.25f;
    parameter.GestureValue(0, 0) = 0.9f;
    manager.SetGestureValue(0, 0.0f);
    manager.SelectGesture(0);

    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    parameter.HandleIncDec(scene, 0.2f);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.25f, 0.0001f);
}

TEST_CASE(active_high_gesture_distributes_after_deselection) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.25f});
    parameter.SceneCenter(0) = 0.25f;
    parameter.GestureValue(0, 63) = 0.9f;
    parameter.SetGestureActive(0, 63, true);
    manager.SetGestureValue(63, 1.0f);
    manager.DeselectGesture(63);

    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    parameter.HandleIncDec(scene, 0.2f);

    REQUIRE_TRUE(parameter.GestureActive(0, 63));
    REQUIRE_TRUE(!manager.GestureSelected(63));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 63), 1.0f, 0.0001f);
}

TEST_CASE(selected_gesture_weight_one_edits_gesture_without_moving_base) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.5f;
    parameter.GestureValue(0, 0) = 0.5f;
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 1.0f);

    const synth::SceneState scene{.leftScene = 0, .rightScene = 0, .blend = 0.0f};
    parameter.HandleIncDec(scene, 0.2f);
    parameter.Compute(scene);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.7f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.7f, 0.0001f);
}

TEST_CASE(selected_gesture_weight_biases_gesture_edit_over_base_edit) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.5f;
    parameter.GestureValue(0, 0) = 0.5f;
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 0.75f);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 0, .blend = 0.0f}, 0.2f);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.55f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.65f, 0.0001f);
}

TEST_CASE(selected_gesture_mid_blend_arms_both_scenes_without_applying_delta) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.2f;
    parameter.SceneCenter(1) = 0.8f;
    parameter.GestureValue(0, 0) = 1.0f;
    parameter.GestureValue(1, 0) = 0.0f;
    manager.SelectGesture(0);
    manager.SetGestureValue(0, 0.5f);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 1, .blend = 0.5f}, 0.4f);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_TRUE(parameter.GestureActive(1, 0));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.2f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.8f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.2f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(1, 0), 0.8f, 0.0001f);
}

TEST_CASE(active_gesture_distribution_ignores_current_selection) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.5f});
    parameter.GestureValue(0, 0) = 0.5f;
    parameter.GestureValue(0, 1) = 0.5f;
    parameter.SetGestureActive(0, 0, true);
    parameter.SetGestureActive(0, 1, true);
    manager.SetGestureValue(0, 0.8f);
    manager.SetGestureValue(1, 0.4f);
    manager.DeselectGesture(0);
    manager.DeselectGesture(1);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 0, .blend = 0.0f}, 0.3f);

    REQUIRE_TRUE(!manager.GestureSelected(0));
    REQUIRE_TRUE(!manager.GestureSelected(1));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.6f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.66f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 1), 0.54f, 0.0001f);
}

TEST_CASE(selected_active_and_inactive_gestures_arm_before_distribution) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.4f});
    parameter.SceneCenter(0) = 0.4f;
    parameter.GestureValue(0, 0) = 0.6f;
    parameter.GestureValue(0, 1) = 0.1f;
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 0.75f);
    manager.SetGestureValue(1, 0.5f);
    manager.SelectGesture(0);
    manager.SelectGesture(1);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 0, .blend = 0.0f}, 0.2f);

    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_TRUE(parameter.GestureActive(0, 1));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.6f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 1), 0.4f, 0.0001f);
}

TEST_CASE(active_gesture_mid_blend_distributes_across_both_scenes) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.2f;
    parameter.SceneCenter(1) = 0.8f;
    parameter.GestureValue(0, 0) = 0.2f;
    parameter.GestureValue(1, 0) = 0.8f;
    parameter.SetGestureActive(0, 0, true);
    parameter.SetGestureActive(1, 0, true);
    manager.SetGestureValue(0, 0.5f);
    manager.DeselectGesture(0);

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 1, .blend = 0.5f}, 0.2f);

    REQUIRE_TRUE(!manager.GestureSelected(0));
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.85f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(1, 0), 0.85f, 0.0001f);
}

TEST_CASE(handle_inc_dec_negative_saturates_lower_bound) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Edit", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.1f;

    parameter.HandleIncDec({.leftScene = 0, .rightScene = 0, .blend = 0.0f}, -0.4f);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.0f, 0.0001f);
}

TEST_CASE(revert_to_default_clears_modulation_and_gestures) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
        .processLiteAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.9f;
    parameter.SceneCenter(1) = 0.8f;
    parameter.GestureValue(0, 0) = 1.0f;
    parameter.GestureValue(1, 0) = 0.0f;
    parameter.SetGestureActive(0, 0, true);
    parameter.SetGestureActive(1, 0, true);
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));
    parameter.Compute({.leftScene = 0, .rightScene = 1, .blend = 0.5f});
    parameter.ProcessLite();
    REQUIRE_TRUE(parameter.ModulationDepthParameter(0) == &depth);

    const synth::SceneState scene{.leftScene = 0, .rightScene = 1, .blend = 0.5f};
    parameter.RevertToDefault(scene);
    parameter.Compute(scene);
    parameter.ProcessLite();

    REQUIRE_TRUE(parameter.ModulationDepthParameter(0) == &depth);
    REQUIRE_NEAR(depth.SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_NEAR(depth.SceneCenter(1), 0.5f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(1), 0.4f, 0.0001f);
    REQUIRE_TRUE(!parameter.GestureActive(0, 0));
    REQUIRE_TRUE(!parameter.GestureActive(1, 0));
    REQUIRE_NEAR(parameter.CurrentDepthForSource(0, 0), 0.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetDepthForSource(1, 0), 0.0f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.4f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.4f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenterScale(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenterScale(1), 1.0f, 0.0001f);
    REQUIRE_NEAR(parameter.GetRaw(0), 0.4f, 0.0001f);
    REQUIRE_NEAR(parameter.GetRaw(1), 0.4f, 0.0001f);
}

TEST_CASE(revert_to_default_rejects_invalid_scene_without_mutation) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.5f});
    parameter.SceneCenter(0) = 0.9f;
    parameter.SetGestureActive(0, 0, true);
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));
    depth.SceneCenter(0) = 0.75f;
    manager.ComputeAllParameters();
    const std::size_t routeSlot = parameter.RoutePositionForSource(0);
    parameter.TargetDepthSlots(0)[routeSlot] = 0.75f;
    parameter.CurrentDepthSlots(0)[routeSlot] = 0.5f;

    bool threw = false;
    try {
        parameter.RevertToDefault({.leftScene = 3, .rightScene = 0, .blend = 0.0f});
    } catch (const std::out_of_range&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(parameter.ModulationDepthParameter(0) == &depth);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.9f, 0.0001f);
    REQUIRE_TRUE(parameter.GestureActive(0, 0));
    REQUIRE_NEAR(parameter.TargetDepthForSource(0, 0), 0.75f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentDepthForSource(0, 0), 0.5f, 0.0001f);
}

TEST_CASE(page_routing_changes_without_mutating_parameter_state) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.4f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.25f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));
    parameter.SceneCenter(0) = 0.7f;
    parameter.GestureValue(0, 0) = 0.9f;
    parameter.SetGestureActive(0, 0, true);
    manager.SetGestureValue(0, 0.5f);
    group.GetModulators().Value(0, 0) = 0.25f;

    auto& pageA = manager.CreatePage("A");
    auto& pageB = manager.CreatePage("B");
    manager.AssignParameterToPage(pageA.ordinal, parameter);
    manager.AssignParameterToPage(pageB.ordinal, depth);
    REQUIRE_TRUE(manager.ActivePage() == &pageA);

    const float sceneCenter = parameter.SceneCenter(0);
    const float gestureValue = parameter.GestureValue(0, 0);
    const bool gestureActive = parameter.GestureActive(0, 0);
    const float modulatorValue = group.GetModulators().Value(0, 0);
    synth::Parameter* route = parameter.ModulationDepthParameter(0);

    REQUIRE_TRUE(manager.SelectActivePage(pageB.ordinal));

    REQUIRE_TRUE(manager.ActivePage() == &pageB);
    REQUIRE_NEAR(parameter.SceneCenter(0), sceneCenter, 0.0001f);
    REQUIRE_NEAR(parameter.GestureValue(0, 0), gestureValue, 0.0001f);
    REQUIRE_TRUE(parameter.GestureActive(0, 0) == gestureActive);
    REQUIRE_NEAR(group.GetModulators().Value(0, 0), modulatorValue, 0.0001f);
    REQUIRE_TRUE(parameter.ModulationDepthParameter(0) == route);
}

TEST_CASE(mixed_group_bank_routes_each_parameter_to_its_group) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& groupA = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& groupB = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& a = manager.CreateParameter(groupA, {.name = "A", .defaultValue = 0.1f});
    auto& b = manager.CreateParameter(groupB, {.name = "B", .defaultValue = 0.2f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, a);
    bank.AddMapping(11, b);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    manager.HandleTick(10, 0.2f);
    manager.HandleTick(11, 0.3f);

    REQUIRE_NEAR(a.SceneCenter(0), 0.3f, 0.0001f);
    REQUIRE_NEAR(b.SceneCenter(0), 0.5f, 0.0001f);
}

TEST_CASE(bank_module_registration_uses_associated_slot_layout_and_offset) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& tune = manager.CreateParameter(group, {.name = "Tune", .defaultValue = 0.5f});
    auto& shape = manager.CreateParameter(group, {.name = "Shape", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.AddPhysicalEncoder(13);
    slot.SelectBank(&bank);

    std::array<synth::Parameter*, 2> parameters{&tune, &shape};
    bank.RegisterParameters(parameters, 1);

    REQUIRE_TRUE(bank.AssociatedSlot() == &slot);
    REQUIRE_TRUE(bank.SlotCapacity() == 4);
    REQUIRE_TRUE(bank.VisibleParameter(10) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(11) == &tune);
    REQUIRE_TRUE(bank.VisibleParameter(12) == &shape);
    REQUIRE_TRUE(bank.VisibleParameter(13) == nullptr);
}

TEST_CASE(multiple_banks_share_one_slot_layout_for_module_registration) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& firstParam = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.1f});
    auto& secondParam = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.2f});
    auto& firstBank = manager.CreateBank();
    auto& secondBank = manager.CreateBank();
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(20);
    slot.AddPhysicalEncoder(21);

    slot.SelectBank(&firstBank);
    std::array<synth::Parameter*, 1> firstParams{&firstParam};
    firstBank.RegisterParameters(firstParams, 0);

    slot.SelectBank(&secondBank);
    std::array<synth::Parameter*, 1> secondParams{&secondParam};
    secondBank.RegisterParameters(secondParams, 1);

    REQUIRE_TRUE(firstBank.AssociatedSlot() == &slot);
    REQUIRE_TRUE(secondBank.AssociatedSlot() == &slot);
    REQUIRE_TRUE(firstBank.SlotCapacity() == 2);
    REQUIRE_TRUE(secondBank.SlotCapacity() == 2);
    REQUIRE_TRUE(slot.SelectedBank() == &secondBank);
    REQUIRE_TRUE(firstBank.VisibleParameter(20) == &firstParam);
    REQUIRE_TRUE(secondBank.VisibleParameter(21) == &secondParam);
}

TEST_CASE(bank_module_registration_rejects_missing_slot_and_capacity_overrun_without_mutation) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 3});
    auto& existing = manager.CreateParameter(group, {.name = "Existing", .defaultValue = 0.1f});
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.2f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.3f});
    auto& bank = manager.CreateBank();
    std::array<synth::Parameter*, 2> parameters{&first, &second};

    bool threw = false;
    try {
        (void)bank.SlotCapacity();
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        bank.RegisterParameters(parameters, 0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);
    bank.AddMapping(10, existing);

    threw = false;
    try {
        bank.RegisterParameters(parameters, 1);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(bank.VisibleParameter(10) == &existing);
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);
}

TEST_CASE(bank_module_registration_rejects_duplicate_names_nulls_and_duplicate_slots) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& parameter = manager.CreateParameter(group, {.name = "Repeated", .defaultValue = 0.1f});
    auto& other = manager.CreateParameter(group, {.name = "Other", .defaultValue = 0.2f});
    auto& bank = manager.CreateBank();
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    bool threw = false;
    try {
        std::array<synth::Parameter*, 2> duplicateNames{&parameter, &parameter};
        bank.RegisterParameters(duplicateNames, 0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(bank.VisibleParameter(10) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);

    threw = false;
    try {
        std::array<synth::Parameter*, 2> nullParameter{&parameter, nullptr};
        bank.RegisterParameters(nullParameter, 0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(bank.VisibleParameter(10) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);

    threw = false;
    try {
        slot.AddPhysicalEncoder(10);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    std::array<synth::Parameter*, 2> valid{&parameter, &other};
    bank.RegisterParameters(valid, 0);
    REQUIRE_TRUE(bank.VisibleParameter(10) == &parameter);
    REQUIRE_TRUE(bank.VisibleParameter(11) == &other);
}

TEST_CASE(bank_cannot_be_associated_with_two_slots) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Only", .defaultValue = 0.5f});
    auto& bank = manager.CreateBank();
    auto& firstSlot = manager.CreateBankSlot();
    auto& secondSlot = manager.CreateBankSlot();
    firstSlot.AddPhysicalEncoder(1);
    secondSlot.AddPhysicalEncoder(2);
    firstSlot.SelectBank(&bank);
    std::array<synth::Parameter*, 1> parameters{&parameter};
    bank.RegisterParameters(parameters, 0);

    bool threw = false;
    try {
        secondSlot.SelectBank(&bank);
    } catch (const std::logic_error&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(bank.AssociatedSlot() == &firstSlot);
    REQUIRE_TRUE(firstSlot.SelectedBank() == &bank);
    REQUIRE_TRUE(secondSlot.SelectedBank() == nullptr);
}

TEST_CASE(press_opens_modulation_view_and_target_cell_closes_it) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depthA = manager.CreateParameter(group, {.name = "DepthA", .defaultValue = 0.1f});
    auto& depthB = manager.CreateParameter(group, {.name = "DepthB", .defaultValue = 0.2f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depthA));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &depthB));
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, parameter);
    bank.AddMapping(2, depthA);
    bank.AddMapping(3, depthB);

    bank.HandlePress(1);

    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.SelectedParameter() == &parameter);
    REQUIRE_TRUE(bank.TargetParameter() == &parameter);
    REQUIRE_TRUE(bank.VisibleMappingCount() == 3);
    REQUIRE_TRUE(bank.VisibleParameter(1) == &depthA);
    REQUIRE_TRUE(bank.VisibleParameter(2) == &depthB);
    REQUIRE_TRUE(bank.VisibleParameter(3) == &parameter);

    bank.HandlePress(3);

    REQUIRE_TRUE(!bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleParameter(1) == &parameter);
}

TEST_CASE(modulation_view_return_cell_uses_final_compact_fallback_position) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& depthA = manager.CreateParameter(group, {.name = "DepthA", .defaultValue = 0.1f});
    auto& depthB = manager.CreateParameter(group, {.name = "DepthB", .defaultValue = 0.2f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depthA));
    REQUIRE_TRUE(parameter.AssignModulationDepth(1, &depthB));
    parameter.SceneCenter(0) = 0.8f;
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    bank.AddMapping(11, depthA);
    bank.AddMapping(12, depthB);

    bank.HandlePress(10);

    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleMappingCount() == 3);
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depthA);
    REQUIRE_TRUE(bank.VisibleParameter(11) == &depthB);
    REQUIRE_TRUE(bank.VisibleParameter(12) == &parameter);
    REQUIRE_TRUE(bank.TargetParameter() == &parameter);

    bank.HandleTick(12, {.leftScene = 0, .rightScene = 0, .blend = 0.0f}, 0.1f);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.9f, 0.0001f);

    manager.SetResetHeld(true);
    bank.HandlePress(12);
    manager.SetResetHeld(false);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.4f, 0.0001f);

    bank.HandlePress(12);

    REQUIRE_TRUE(!bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleParameter(10) == &parameter);
}

TEST_CASE(modulation_view_places_return_at_final_slot_position) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.0f});
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    bank.AddMapping(11, depth);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.SelectBank(&bank);

    slot.HandlePress(10);
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);

    REQUIRE_TRUE(ui->slots[0].showingModulationView.load());
    REQUIRE_TRUE(ui->slots[0].cells[0].connected.load());
    REQUIRE_TRUE(!ui->slots[0].cells[1].connected.load());
    REQUIRE_TRUE(ui->slots[0].cells[2].connected.load());
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depth);
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(12) == &parameter);
    REQUIRE_TRUE(ui->slots[0].cells[1].switchValues.load() == 0);
    REQUIRE_TRUE(ui->slots[0].cells[1].modulatorsAffectingMask.load() == 0);
    REQUIRE_TRUE(ui->slots[0].cells[1].gesturesAffectingMask.load() == 0);
}

TEST_CASE(modulation_view_rejects_more_modulators_than_slot_depth_positions) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 4,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.SelectBank(&bank);

    bool threw = false;
    try {
        slot.HandlePress(10);
    } catch (const std::logic_error&) {
        threw = true;
    }

    REQUIRE_TRUE(threw);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(modulation_view_open_is_noop_when_capacity_cannot_fill_all_modulators) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& filler = manager.CreateParameter(group, {.name = "Filler", .defaultValue = 0.25f});
    group.GetModulators().Metadata(0).name = "Filter Env";
    group.GetModulators().Metadata(0).shortName = "Env";
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, carrier);
    bank.AddMapping(2, filler);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.AddPhysicalEncoder(2);
    slot.AddPhysicalEncoder(3);
    slot.SelectBank(&bank);

    slot.HandlePress(1);

    REQUIRE_TRUE(!bank.ShowingModulation());
    REQUIRE_TRUE(group.ParameterCount() == 2);
    REQUIRE_TRUE(manager.ParameterCount() == 2);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(1) == &carrier);
    REQUIRE_TRUE(bank.VisibleParameter(2) == &filler);
    REQUIRE_TRUE(bank.VisibleParameter(3) == nullptr);
}

TEST_CASE(modulation_view_requests_storage_batch_and_succeeds_after_reinforcement) {
    synth::ParameterMessageOutBus outputBus(4);
    synth::ParameterManager manager;
    manager.SetParameterMessageOutBus(&outputBus);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 2,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& filler = manager.CreateParameter(group, {.name = "Filler", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, carrier);
    bank.AddMapping(2, filler);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.AddPhysicalEncoder(2);
    slot.AddPhysicalEncoder(3);
    slot.SelectBank(&bank);

    slot.HandlePress(1);

    REQUIRE_TRUE(!bank.ShowingModulation());
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    synth::ParameterMessageOut request;
    REQUIRE_TRUE(outputBus.Pop(request));
    REQUIRE_TRUE(request.type == synth::ParameterMessageOut::Type::ParameterStorageBatchNeeded);
    REQUIRE_TRUE(request.group == &group);
    REQUIRE_TRUE(request.minimumAdditionalParameters >= 2);
    REQUIRE_TRUE(request.requestedParameters >= group.Config().numModulators * 2);

    group.AddParameterStorageBatch(synth::MakeParameterStorageBatch(group.Config(), group.GestureCount(),
                                                                    request.requestedParameters));
    slot.HandlePress(1);

    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) != nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) != nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(1) == carrier.ModulationDepthParameter(0));
    REQUIRE_TRUE(bank.VisibleParameter(2) == carrier.ModulationDepthParameter(1));
    REQUIRE_TRUE(bank.VisibleParameter(3) == &carrier);
}

TEST_CASE(modulation_view_materializes_all_missing_depth_parameters_when_capacity_allows) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 1,
        .maxParameters = 4,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& filler = manager.CreateParameter(group, {.name = "Filler", .defaultValue = 0.25f});
    group.GetModulators().Metadata(0).name = "Filter Env";
    group.GetModulators().Metadata(0).shortName = "Env";
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, carrier);
    bank.AddMapping(2, filler);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.AddPhysicalEncoder(2);
    slot.AddPhysicalEncoder(3);
    slot.SelectBank(&bank);

    slot.HandlePress(1);

    synth::Parameter* depth = carrier.ModulationDepthParameter(0);
    synth::Parameter* secondDepth = carrier.ModulationDepthParameter(1);
    REQUIRE_TRUE(depth != nullptr);
    REQUIRE_TRUE(secondDepth != nullptr);
    REQUIRE_TRUE(group.ParameterCount() == 4);
    REQUIRE_TRUE(manager.ParameterCount() == 2);
    REQUIRE_TRUE(depth->Name() == "Carrier Filter Env");
    REQUIRE_TRUE(depth->ShortName() == "Env");
    REQUIRE_TRUE(depth->BaseColor() == synth::Color::Cyan);
    REQUIRE_TRUE(depth->Range() == synth::RangeKind::Bipolar);
    REQUIRE_NEAR(depth->SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_TRUE(bank.VisibleParameter(1) == depth);
    REQUIRE_TRUE(bank.VisibleParameter(2) == secondDepth);
    REQUIRE_TRUE(bank.VisibleParameter(3) == &carrier);
    REQUIRE_TRUE(bank.TargetParameter() == &carrier);

    bank.HandleTick(1, {.leftScene = 0, .rightScene = 0, .blend = 0.0f}, 0.2f);

    REQUIRE_NEAR(depth->SceneCenter(0), 0.7f, 0.0001f);
}

TEST_CASE(modulation_view_leaves_disconnected_sources_empty_and_hides_explicit_depths) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 4,
    });
    group.GetModulators().Metadata(0).connected = true;
    group.GetModulators().Metadata(2).connected = true;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& hiddenDepth = manager.CreateParameter(group, {
        .name = "Hidden Depth",
        .defaultValue = 0.2f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(carrier.AssignModulationDepth(1, &hiddenDepth));
    hiddenDepth.SceneCenter(0) = 0.8f;

    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.AddPhysicalEncoder(13);
    slot.SelectBank(&bank);

    std::size_t valueCalls = 0;
    std::size_t coinCalls = 0;
    std::size_t indexCalls = 0;
    manager.SetRandomSource(
        [&valueCalls]() {
            ++valueCalls;
            return 0.1f;
        },
        [&coinCalls]() {
            ++coinCalls;
            return 0.1f;
        },
        [&indexCalls](std::size_t) {
            ++indexCalls;
            return std::size_t{0};
        });

    slot.HandlePress(10);
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);

    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) != nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == &hiddenDepth);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(2) != nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(10) == carrier.ModulationDepthParameter(0));
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(12) == carrier.ModulationDepthParameter(2));
    REQUIRE_TRUE(bank.VisibleParameter(13) == &carrier);
    REQUIRE_TRUE(!ui->slots[0].cells[1].connected.load());

    const float hiddenValue = hiddenDepth.SceneCenter(0);
    synth::Parameter* const selectedTarget = bank.TargetParameter();
    const std::size_t parameterCount = group.ParameterCount();
    manager.HandleTick(11, 0.2f);
    manager.HandlePress(11);
    manager.SetResetHeld(true);
    manager.HandlePress(11);
    manager.SetResetHeld(false);
    manager.SetRandomHeld(true);
    manager.HandlePress(11);
    manager.SetRandomHeld(false);
    manager.SetRandomModHeld(true);
    manager.HandlePress(11);
    manager.SetRandomModHeld(false);

    REQUIRE_NEAR(hiddenDepth.SceneCenter(0), hiddenValue, 0.0001f);
    REQUIRE_TRUE(bank.TargetParameter() == selectedTarget);
    REQUIRE_TRUE(group.ParameterCount() == parameterCount);
    REQUIRE_TRUE(valueCalls == 0);
    REQUIRE_TRUE(coinCalls == 0);
    REQUIRE_TRUE(indexCalls == 0);
}

TEST_CASE(modulation_view_capacity_counts_only_connected_missing_depths) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 2,
    });
    group.GetModulators().Metadata(2).connected = true;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.AddPhysicalEncoder(13);
    slot.SelectBank(&bank);

    slot.HandlePress(10);

    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(2) != nullptr);
    REQUIRE_TRUE(group.ParameterCount() == 2);
    REQUIRE_TRUE(bank.VisibleParameter(10) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(11) == nullptr);
    REQUIRE_TRUE(bank.VisibleParameter(12) == carrier.ModulationDepthParameter(2));
    REQUIRE_TRUE(bank.VisibleParameter(13) == &carrier);
}

TEST_CASE(modulation_view_keeps_owned_depth_parameter_after_reset) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    group.GetModulators().Metadata(0).name = "VCO Direct";
    group.GetModulators().Metadata(0).shortName = "VCO";
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;
    auto& tune = manager.CreateParameter(group, {.name = "Tune", .defaultValue = 0.5f});
    auto& depth = tune.EnsureModulationDepth(0, {
        .name = "Tune VCO Direct",
        .shortName = "VCO",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
        .baseColor = synth::Color::Cyan,
    });

    auto& bank = manager.CreateBank();
    bank.AddMapping(10, tune);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    slot.HandlePress(10);
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depth);
    REQUIRE_TRUE(bank.VisibleParameter(11) == &tune);

    manager.SetResetHeld(true);
    slot.HandlePress(11);
    manager.SetResetHeld(false);
    REQUIRE_TRUE(tune.ModulationDepthParameter(0) == &depth);
    slot.HandlePress(11);
    REQUIRE_TRUE(!bank.ShowingModulation());

    slot.HandlePress(10);

    REQUIRE_TRUE(tune.ModulationDepthParameter(0) == &depth);
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depth);
    REQUIRE_TRUE(bank.VisibleParameter(11) == &tune);
    REQUIRE_TRUE(manager.ParameterCount() == 1);
    REQUIRE_TRUE(group.ParameterCount() == 2);
}

TEST_CASE(modulation_view_lazy_depth_names_include_target_parameter_for_duplicate_modulators) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& first = manager.CreateParameter(group, {.name = "Carrier A", .defaultValue = 0.25f});
    auto& second = manager.CreateParameter(group, {.name = "Carrier B", .defaultValue = 0.75f});
    group.GetModulators().Metadata(0).name = "Filter Env";
    group.GetModulators().Metadata(0).shortName = "Env";
    group.GetModulators().Metadata(0).sourceColor = synth::Color::Cyan;

    auto& bank = manager.CreateBank();
    bank.AddMapping(1, first);
    bank.AddMapping(2, second);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.AddPhysicalEncoder(2);
    slot.SelectBank(&bank);

    slot.HandlePress(1);
    slot.HandlePress(2);
    slot.HandlePress(2);

    synth::Parameter* firstDepth = first.ModulationDepthParameter(0);
    synth::Parameter* secondDepth = second.ModulationDepthParameter(0);
    REQUIRE_TRUE(firstDepth == nullptr);
    REQUIRE_TRUE(secondDepth != nullptr);
    REQUIRE_TRUE(secondDepth->Name() == "Carrier B Filter Env");
    REQUIRE_TRUE(secondDepth->ShortName() == "Env");
    const std::size_t highWater = group.ParameterCount();

    bank.Deselect();
    slot.HandlePress(1);
    firstDepth = first.ModulationDepthParameter(0);
    REQUIRE_TRUE(firstDepth != nullptr);
    REQUIRE_TRUE(firstDepth->Name() == "Carrier A Filter Env");
    REQUIRE_TRUE(firstDepth->ShortName() == "Env");
    REQUIRE_TRUE(group.ParameterCount() == highWater);
    REQUIRE_TRUE(manager.ParameterCount() == 2);
}

TEST_CASE(pressing_modulation_cell_opens_nested_modulation_view) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.2f});
    auto& nested = manager.CreateParameter(group, {.name = "Nested", .defaultValue = 0.1f});
    REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depth));
    REQUIRE_TRUE(depth.AssignModulationDepth(0, &nested));
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, carrier);
    bank.AddMapping(2, depth);

    bank.HandlePress(1);
    bank.HandlePress(1);

    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.SelectedParameter() == &depth);
    REQUIRE_TRUE(bank.TargetParameter() == &depth);
    REQUIRE_TRUE(bank.VisibleParameter(1) == &nested);
    REQUIRE_TRUE(bank.VisibleParameter(2) == &depth);
}

TEST_CASE(slot_bank_switch_deselects_prior_modulation_view) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.1f});
    auto& other = manager.CreateParameter(group, {.name = "Other", .defaultValue = 0.2f});
    REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depth));
    auto& first = manager.CreateBank();
    first.AddMapping(1, carrier);
    first.AddMapping(2, depth);
    auto& second = manager.CreateBank();
    second.AddMapping(1, other);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.AddPhysicalEncoder(2);
    slot.SelectBank(&first);
    slot.HandlePress(1);
    REQUIRE_TRUE(first.ShowingModulation());

    slot.SelectBank(&second);

    REQUIRE_TRUE(!first.ShowingModulation());
    REQUIRE_TRUE(slot.SelectedBank() == &second);
}

TEST_CASE(routed_tick_dispatches_to_selected_bank) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& selected = manager.CreateParameter(group, {.name = "Selected", .defaultValue = 0.25f});
    auto& unselected = manager.CreateParameter(group, {.name = "Unselected", .defaultValue = 0.5f});
    auto& first = manager.CreateBank();
    first.AddMapping(1, selected);
    auto& second = manager.CreateBank();
    second.AddMapping(1, unselected);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.SelectBank(&first);

    manager.HandleTick(1, 0.15f);

    REQUIRE_NEAR(selected.SceneCenter(0), 0.4f, 0.0001f);
    REQUIRE_NEAR(unselected.SceneCenter(0), 0.5f, 0.0001f);
}

TEST_CASE(reset_modifier_press_resets_visible_parameter_to_default) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Reset", .defaultValue = 0.35f});
    parameter.SceneCenter(0) = 0.9f;
    auto& bank = manager.CreateBank();
    bank.AddMapping(4, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(4);
    slot.SelectBank(&bank);

    manager.SetResetHeld(true);
    manager.HandlePress(4);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.35f, 0.0001f);
    REQUIRE_NEAR(parameter.CurrentCenter(), 0.35f, 0.0001f);
}

TEST_CASE(random_modifier_press_randomizes_visible_value_without_touching_mod_depths) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    auto& depth = manager.CreateParameter(group, {
        .name = "Depth",
        .defaultValue = 0.5f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depth));
    carrier.SceneCenter(0) = 0.25f;
    depth.SceneCenter(0) = 0.5f;
    group.GetModulators().Value(0, 0) = 0.0f;
    manager.ComputeAllParameters();

    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    manager.SetRandomSource([]() { return 0.75f; }, []() { return 1.0f; },
                            [](std::size_t) { return std::size_t{0}; });
    manager.SetRandomHeld(true);

    manager.HandlePress(10);

    REQUIRE_NEAR(carrier.SceneCenter(0), 0.75f, 0.0001f);
    REQUIRE_NEAR(carrier.GetRaw(0), 0.75f, 0.0001f);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == &depth);
    REQUIRE_NEAR(depth.SceneCenter(0), 0.5f, 0.0001f);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(random_mod_modifier_press_uses_geometric_slot_loop_with_replacement_and_stops_on_materialization_failure) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 3,
        .targetCenterAlpha = 1.0f,
    });
    MarkAllModulatorsConnectedForUi(group);
    group.GetModulators().Metadata(0).name = "LFO";
    group.GetModulators().Metadata(1).name = "Env";
    group.GetModulators().Metadata(2).name = "Vel";
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);

    std::vector<float> values{0.75f, 0.25f, 1.0f};
    std::vector<float> coins{0.1f, 0.2f, 0.3f, 0.4f, 0.7f};
    std::vector<std::size_t> indices{2, 2, 0, 1};
    std::size_t valueIx = 0;
    std::size_t coinIx = 0;
    std::size_t indexIx = 0;
    manager.SetRandomSource(
        [&values, &valueIx]() { return values.at(valueIx++); },
        [&coins, &coinIx]() { return coins.at(coinIx++); },
        [&indices, &indexIx](std::size_t) { return indices.at(indexIx++); });
    manager.SetRandomModHeld(true);

    manager.HandlePress(10);

    synth::Parameter* depth0 = carrier.ModulationDepthParameter(0);
    synth::Parameter* depth1 = carrier.ModulationDepthParameter(1);
    synth::Parameter* depth2 = carrier.ModulationDepthParameter(2);
    REQUIRE_TRUE(depth0 != nullptr);
    REQUIRE_TRUE(depth1 == nullptr);
    REQUIRE_TRUE(depth2 != nullptr);
    REQUIRE_NEAR(depth0->SceneCenter(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(depth2->SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_TRUE(valueIx == 3);
    REQUIRE_TRUE(indexIx == 4);
    REQUIRE_TRUE(coinIx == 4);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(random_mod_maps_connected_ordinals_and_skips_disconnected_sources) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 5,
        .numScenes = 1,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
    });
    group.GetModulators().Metadata(0).connected = true;
    group.GetModulators().Metadata(4).connected = true;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);

    std::array<float, 2> coins{0.1f, 0.7f};
    std::size_t coinIx = 0;
    std::size_t indexCalls = 0;
    std::size_t valueCalls = 0;
    manager.SetRandomSource(
        [&valueCalls]() {
            ++valueCalls;
            return 0.8f;
        },
        [&coins, &coinIx]() { return coins.at(coinIx++); },
        [&indexCalls](std::size_t exclusiveMax) {
            ++indexCalls;
            REQUIRE_TRUE(exclusiveMax == 2);
            return std::size_t{1};
        });
    manager.SetRandomModHeld(true);

    manager.HandlePress(10);

    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(2) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(3) == nullptr);
    synth::Parameter* depth = carrier.ModulationDepthParameter(4);
    REQUIRE_TRUE(depth != nullptr);
    REQUIRE_NEAR(depth->SceneCenter(0), 0.8f, 0.0001f);
    REQUIRE_TRUE(coinIx == 2);
    REQUIRE_TRUE(indexCalls == 1);
    REQUIRE_TRUE(valueCalls == 1);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(random_mod_with_no_connected_sources_is_a_noop) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 1,
        .maxParameters = 4,
    });
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);

    std::size_t valueCalls = 0;
    std::size_t coinCalls = 0;
    std::size_t indexCalls = 0;
    manager.SetRandomSource(
        [&valueCalls]() {
            ++valueCalls;
            return 0.8f;
        },
        [&coinCalls]() {
            ++coinCalls;
            return 0.7f;
        },
        [&indexCalls](std::size_t) {
            ++indexCalls;
            return std::size_t{0};
        });
    manager.SetRandomModHeld(true);

    manager.HandlePress(10);

    REQUIRE_TRUE(valueCalls == 0);
    REQUIRE_TRUE(coinCalls == 0);
    REQUIRE_TRUE(indexCalls == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(2) == nullptr);
    REQUIRE_TRUE(group.ParameterCount() == 1);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(modified_bank_selection_applies_modifier_to_target_bank_without_switching_selected_bank) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 7,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& selectedParam = manager.CreateParameter(group, {.name = "Selected", .defaultValue = 0.1f});
    auto& resetTarget = manager.CreateParameter(group, {.name = "Reset Target", .defaultValue = 0.25f});
    auto& randomTarget = manager.CreateParameter(group, {.name = "Random Target", .defaultValue = 0.35f});
    auto& randomModTarget = manager.CreateParameter(group, {.name = "Random Mod Target", .defaultValue = 0.45f});

    auto& selectedBank = manager.CreateBank();
    selectedBank.AddMapping(10, selectedParam);
    auto& targetBank = manager.CreateBank();
    targetBank.AddMapping(10, resetTarget);
    targetBank.AddMapping(11, randomTarget);
    targetBank.AddMapping(12, randomModTarget);

    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.SelectBank(&selectedBank);

    resetTarget.SceneCenter(0) = 0.9f;
    randomTarget.SceneCenter(0) = 0.1f;
    randomModTarget.SceneCenter(0) = 0.2f;

    manager.SetResetHeld(true);
    REQUIRE_TRUE(manager.SelectBankForSlot(0, 1));
    manager.SetResetHeld(false);
    REQUIRE_TRUE(slot.SelectedBank() == &selectedBank);
    REQUIRE_NEAR(resetTarget.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_NEAR(randomTarget.SceneCenter(0), 0.35f, 0.0001f);
    REQUIRE_NEAR(randomModTarget.SceneCenter(0), 0.45f, 0.0001f);

    std::vector<float> values{0.6f, 0.7f, 0.8f, 1.0f, 0.75f, 0.5f};
    std::vector<float> coins{0.1f, 0.7f, 0.1f, 0.7f, 0.1f, 0.7f};
    std::size_t valueIx = 0;
    std::size_t coinIx = 0;
    manager.SetRandomSource(
        [&values, &valueIx]() { return values.at(valueIx++); },
        [&coins, &coinIx]() { return coins.at(coinIx++); },
        [](std::size_t) { return std::size_t{0}; });

    manager.SetRandomHeld(true);
    REQUIRE_TRUE(manager.SelectBankForSlot(0, 1));
    manager.SetRandomHeld(false);
    REQUIRE_TRUE(slot.SelectedBank() == &selectedBank);
    REQUIRE_NEAR(resetTarget.SceneCenter(0), 0.6f, 0.0001f);
    REQUIRE_NEAR(randomTarget.SceneCenter(0), 0.7f, 0.0001f);
    REQUIRE_NEAR(randomModTarget.SceneCenter(0), 0.8f, 0.0001f);

    manager.SetRandomModHeld(true);
    REQUIRE_TRUE(manager.SelectBankForSlot(0, 1));
    manager.SetRandomModHeld(false);
    synth::Parameter* resetDepth = resetTarget.ModulationDepthParameter(0);
    synth::Parameter* randomDepth = randomTarget.ModulationDepthParameter(0);
    synth::Parameter* randomModDepth = randomModTarget.ModulationDepthParameter(0);
    REQUIRE_TRUE(slot.SelectedBank() == &selectedBank);
    REQUIRE_TRUE(resetDepth != nullptr);
    REQUIRE_TRUE(randomDepth != nullptr);
    REQUIRE_TRUE(randomModDepth != nullptr);
    REQUIRE_NEAR(resetDepth->SceneCenter(0), 1.0f, 0.0001f);
    REQUIRE_NEAR(randomDepth->SceneCenter(0), 0.75f, 0.0001f);
    REQUIRE_NEAR(randomModDepth->SceneCenter(0), 0.5f, 0.0001f);
}

TEST_CASE(relative_bank_messages_navigate_wrap_and_preserve_invalid_state) {
    const synth::MessageIn next = synth::MessageIn::NextParamBank(17, 3);
    REQUIRE_TRUE(next.type == synth::MessageIn::Type::NextParamBank);
    REQUIRE_TRUE(next.timestamp == 17);
    REQUIRE_TRUE(next.slotIx == 3);
    REQUIRE_TRUE(next.bankIx == 0);
    const synth::MessageIn previous = synth::MessageIn::PrevParamBank(19, 4);
    REQUIRE_TRUE(previous.type == synth::MessageIn::Type::PrevParamBank);
    REQUIRE_TRUE(previous.timestamp == 19);
    REQUIRE_TRUE(previous.slotIx == 4);
    REQUIRE_TRUE(previous.bankIx == 0);

    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Relative", .defaultValue = 0.25f});
    auto& first = manager.CreateBank();
    first.AddMapping(10, parameter);
    auto& second = manager.CreateBank();
    second.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&first);
    synth::MessageInBus bus(&manager, 8);

    bus.Apply(synth::MessageIn::NextParamBank(0, 0));
    REQUIRE_TRUE(slot.SelectedBank() == &second);
    bus.Apply(synth::MessageIn::NextParamBank(0, 0));
    REQUIRE_TRUE(slot.SelectedBank() == &first);
    bus.Apply(synth::MessageIn::PrevParamBank(0, 0));
    REQUIRE_TRUE(slot.SelectedBank() == &second);
    bus.Apply(synth::MessageIn::PrevParamBank(0, 0));
    REQUIRE_TRUE(slot.SelectedBank() == &first);

    const float priorValue = parameter.SceneCenter(0);
    bus.Apply(synth::MessageIn::NextParamBank(0, 99));
    REQUIRE_TRUE(slot.SelectedBank() == &first);
    REQUIRE_NEAR(parameter.SceneCenter(0), priorValue, 0.0f);

    synth::ParameterManager oneBankManager;
    auto& oneGroup = oneBankManager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& oneParameter = oneBankManager.CreateParameter(oneGroup, {.name = "One", .defaultValue = 0.4f});
    auto& onlyBank = oneBankManager.CreateBank();
    onlyBank.AddMapping(10, oneParameter);
    auto& oneSlot = oneBankManager.CreateBankSlot();
    oneSlot.AddPhysicalEncoder(10);
    oneSlot.SelectBank(&onlyBank);
    synth::MessageInBus oneBus(&oneBankManager, 4);
    oneBus.Apply(synth::MessageIn::NextParamBank(0, 0));
    oneBus.Apply(synth::MessageIn::PrevParamBank(0, 0));
    REQUIRE_TRUE(oneSlot.SelectedBank() == &onlyBank);

    synth::ParameterManager zeroBanksManager;
    auto& zeroGroup = zeroBanksManager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& zeroParameter = zeroBanksManager.CreateParameter(zeroGroup, {.name = "Zero", .defaultValue = 0.6f});
    auto& zeroSlot = zeroBanksManager.CreateBankSlot();
    zeroSlot.AddPhysicalEncoder(10);
    synth::MessageInBus zeroBus(&zeroBanksManager, 4);
    zeroBus.Apply(synth::MessageIn::NextParamBank(0, 0));
    zeroBus.Apply(synth::MessageIn::PrevParamBank(0, 0));
    REQUIRE_TRUE(zeroSlot.SelectedBank() == nullptr);
    REQUIRE_NEAR(zeroParameter.SceneCenter(0), 0.6f, 0.0f);

    synth::ParameterManager noSelectionManager;
    auto& noSelectionGroup = noSelectionManager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& noSelectionParameter = noSelectionManager.CreateParameter(
        noSelectionGroup, {.name = "No Selection", .defaultValue = 0.7f});
    auto& ownedBank = noSelectionManager.CreateBank();
    ownedBank.AddMapping(10, noSelectionParameter);
    auto& noSelectionSlot = noSelectionManager.CreateBankSlot();
    noSelectionSlot.AddPhysicalEncoder(10);
    synth::MessageInBus noSelectionBus(&noSelectionManager, 4);
    noSelectionBus.Apply(synth::MessageIn::NextParamBank(0, 0));
    REQUIRE_TRUE(noSelectionSlot.SelectedBank() == nullptr);
    REQUIRE_NEAR(noSelectionParameter.SceneCenter(0), 0.7f, 0.0f);

    synth::Bank foreignBank;
    noSelectionSlot.SelectBank(&foreignBank);
    noSelectionBus.Apply(synth::MessageIn::PrevParamBank(0, 0));
    REQUIRE_TRUE(noSelectionSlot.SelectedBank() == &foreignBank);
    REQUIRE_NEAR(noSelectionParameter.SceneCenter(0), 0.7f, 0.0f);
}

TEST_CASE(relative_bank_messages_apply_effective_modifiers_without_selection) {
    for (const synth::BankDirection direction : {synth::BankDirection::Next, synth::BankDirection::Previous}) {
        for (const synth::Modifier modifier : {synth::Modifier::Reset, synth::Modifier::Random,
                                               synth::Modifier::RandomMod}) {
            synth::ParameterManager manager;
            auto& group = manager.CreateGroup({
                .numVoices = 1,
                .numModulators = 1,
                .numScenes = 1,
                .maxParameters = 3,
            });
            MarkAllModulatorsConnectedForUi(group);
            auto& parameter = manager.CreateParameter(group, {.name = "Modified", .defaultValue = 0.25f});
            auto& selected = manager.CreateBank();
            selected.AddMapping(10, parameter);
            auto& other = manager.CreateBank();
            other.AddMapping(10, parameter);
            auto& slot = manager.CreateBankSlot();
            slot.AddPhysicalEncoder(10);
            slot.SelectBank(&selected);
            std::size_t coinCalls = 0;
            manager.SetRandomSource([] { return 0.8f; }, [&coinCalls] { return coinCalls++ == 0 ? 0.0f : 1.0f; },
                                    [](std::size_t) { return std::size_t{0}; });
            parameter.SceneCenter(0) = 0.6f;
            synth::MessageInBus bus(&manager, 4);

            if (modifier == synth::Modifier::Reset) {
                manager.SetResetHeld(true);
            } else if (modifier == synth::Modifier::Random) {
                manager.SetRandomHeld(true);
            } else {
                manager.SetRandomModHeld(true);
            }
            bus.Apply(direction == synth::BankDirection::Next ? synth::MessageIn::NextParamBank(0, 0)
                                                               : synth::MessageIn::PrevParamBank(0, 0));
            REQUIRE_TRUE(slot.SelectedBank() == &selected);
            if (modifier == synth::Modifier::Reset) {
                REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
            } else if (modifier == synth::Modifier::Random) {
                REQUIRE_TRUE(parameter.SceneCenter(0) != 0.6f);
            } else {
                REQUIRE_TRUE(parameter.ModulationDepthParameter(0) != nullptr);
            }
        }
    }

    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 3});
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Precedence", .defaultValue = 0.25f});
    auto& selected = manager.CreateBank();
    selected.AddMapping(10, parameter);
    auto& other = manager.CreateBank();
    other.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&selected);
    std::size_t valueCalls = 0;
    std::size_t coinCalls = 0;
    std::size_t indexCalls = 0;
    manager.SetRandomSource(
        [&valueCalls] {
            ++valueCalls;
            return 0.8f;
        },
        [&coinCalls] { return coinCalls++ == 0 ? 0.0f : 1.0f; },
        [&indexCalls](std::size_t) {
            ++indexCalls;
            return std::size_t{0};
        });
    parameter.SceneCenter(0) = 0.6f;
    manager.SetResetHeld(true);
    manager.SetRandomHeld(true);
    manager.SetRandomModHeld(true);
    synth::MessageInBus bus(&manager, 4);
    bus.Apply(synth::MessageIn::NextParamBank(0, 0));
    REQUIRE_TRUE(slot.SelectedBank() == &selected);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.6f, 0.0001f);
    synth::Parameter* depth = parameter.ModulationDepthParameter(0);
    REQUIRE_TRUE(depth != nullptr);
    REQUIRE_NEAR(depth->SceneCenter(0), 0.8f, 0.0001f);
    REQUIRE_TRUE(valueCalls == 1);
    REQUIRE_TRUE(coinCalls == 2);
    REQUIRE_TRUE(indexCalls == 1);
}

TEST_CASE(reset_modifier_collects_each_affected_group_once_after_resetting_the_bank) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 6,
    });
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.1f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.2f});
    auto& third = manager.CreateParameter(group, {.name = "Third", .defaultValue = 0.3f});
    first.SceneCenter(0) = 0.9f;
    second.SceneCenter(0) = 0.8f;
    third.SceneCenter(0) = 0.7f;

    auto& bank = manager.CreateBank();
    bank.AddMapping(10, first);
    bank.AddMapping(11, second);
    bank.AddMapping(12, third);
    synth::ParameterProcessingObserver work{};
    group.SetProcessingObserverForTests(&work);

    bank.ApplyModifierToTopLevel(synth::Modifier::Reset, manager.Scene());

    REQUIRE_NEAR(first.SceneCenter(0), 0.1f, 0.000001f);
    REQUIRE_NEAR(second.SceneCenter(0), 0.2f, 0.000001f);
    REQUIRE_NEAR(third.SceneCenter(0), 0.3f, 0.000001f);
    REQUIRE_TRUE(work.neutralCollectionPasses == 1);
}

TEST_CASE(message_bus_param_inc_dec_ignores_ticks_while_any_modifier_is_active) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Stable", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    synth::MessageInBus bus(&manager, 8);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandom(0, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(0, 0, 0, 0.25f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandom(0, false)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandomMod(0, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(0, 0, 0, 0.25f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandomMod(0, false)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(0, 0, 0, 0.25f)));
    bus.Process(0);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.5f, 0.0001f);
}

TEST_CASE(param_set_absolute_message_constructs_and_round_trips_exact_payload) {
    const synth::MessageIn source = synth::MessageIn::ParamSetAbsolute(42, 3, 5, 0.625f);
    REQUIRE_TRUE(source.timestamp == 42);
    REQUIRE_TRUE(source.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(source.slotIx == 3);
    REQUIRE_TRUE(source.position == 5);
    REQUIRE_NEAR(source.value, 0.625f, 0.0f);

    synth::JsonArena arena(4096);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string_view(json.Get("type").StringValue()) == "paramSetAbsolute");
    REQUIRE_TRUE(json.Get("slotIx").IntegerValue() == 3);
    REQUIRE_TRUE(json.Get("position").IntegerValue() == 5);
    REQUIRE_NEAR(static_cast<float>(json.Get("value").NumberValue()), 0.625f, 0.0f);

    synth::MessageIn loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(loaded.slotIx == 3);
    REQUIRE_TRUE(loaded.position == 5);
    REQUIRE_NEAR(loaded.value, 0.625f, 0.0f);
}

TEST_CASE(param_set_absolute_survives_controller_system_association_round_trip) {
    synth::MidiControllerProfileConfig profile;
    synth::MidiControllerSystemMessageAssociation association;
    association.control = synth::MidiControlAddress{.channel = 2, .cc = 17};
    association.press = synth::MessageIn::ParamSetAbsolute(0, 4, 6, 0.375f);
    association.feedback = association.press;
    association.outputFeedback = false;
    profile.systemMessages.push_back(association);

    synth::JsonArena arena(16384);
    const synth::JSON json = synth::ToJSON(arena, profile);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string_view(json.Get("systemMessages").GetAt(0).Get("press").Get("type").StringValue()) ==
                 "paramSetAbsolute");

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.systemMessages.size() == 1);
    const synth::MessageIn& message = loaded.systemMessages[0].press;
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(message.slotIx == 4);
    REQUIRE_TRUE(message.position == 6);
    REQUIRE_NEAR(message.value, 0.375f, 0.0f);
}

TEST_CASE(param_set_absolute_runtime_epoch_survives_queue_but_is_not_persisted) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Epoch", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);

    synth::MessageInBus bus(&manager, 4);
    const synth::MessageIn source = synth::MessageIn::ParamSetAbsolute(42, 0, 0, 0.625f, 77);
    REQUIRE_TRUE(bus.Push(source));
    synth::MessageIn popped;
    REQUIRE_TRUE(bus.Pop(popped, 42));
    REQUIRE_TRUE(popped.absoluteEpoch == 77);
    bus.Apply(popped);
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 77);

    synth::JsonArena arena(4096);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(json.Get("absoluteEpoch").IsNull());
    synth::MessageIn loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.absoluteEpoch == 0);

    bus.Apply(loaded);
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.625f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 77);
}

TEST_CASE(param_set_absolute_acknowledges_apply_modifier_rejection_and_disconnected_routes_monotonically) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Acknowledged", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& connectedSlot = manager.CreateBankSlot();
    connectedSlot.AddPhysicalEncoder(10);
    connectedSlot.SelectBank(&bank);
    auto& disconnectedSlot = manager.CreateBankSlot();
    disconnectedSlot.AddPhysicalEncoder(20);

    synth::MessageInBus bus(&manager, 16);
    auto ui = manager.CreateUIState();
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.75f, 10));
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.75f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 10);

    manager.SetResetHeld(true);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.1f, 11));
    manager.SetResetHeld(false);
    manager.SetRandomHeld(true);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.2f, 12));
    manager.SetRandomHeld(false);
    manager.SetRandomModHeld(true);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.3f, 13));
    manager.SetRandomModHeld(false);
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.75f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 13);

    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.5f, 9));
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.625f, 0));
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 1, 0, 0.9f, 14));
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.625f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 13);
    REQUIRE_TRUE(!ui->slots[1].cells[0].connected.load());
    REQUIRE_NEAR(ui->slots[1].cells[0].rawKnobValue.load(), 0.0f, 0.0f);
    REQUIRE_TRUE(ui->slots[1].cells[0].processedAbsoluteEpoch.load() == 14);
}

TEST_CASE(processed_absolute_epoch_follows_slot_position_across_bank_and_modulation_view_changes) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .targetCenterAlpha = 1.0f,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.2f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.6f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.4f});
    REQUIRE_TRUE(second.AssignModulationDepth(0, &depth));
    auto& firstBank = manager.CreateBank();
    firstBank.AddMapping(10, first);
    auto& secondBank = manager.CreateBank();
    secondBank.AddMapping(10, second);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&firstBank);

    synth::MessageInBus bus(&manager, 8);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.3f, 21));
    slot.SelectBank(&secondBank);
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(ui->slots[0].cells[0].rawKnobValue.load(), 0.6f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 21);

    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.7f, 22));
    manager.HandlePress(0, 0);
    REQUIRE_TRUE(secondBank.ShowingModulation());
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->slots[0].cells[0].processedAbsoluteEpoch.load() == 22);
    REQUIRE_NEAR(ui->slots[0].cells[0].rawKnobValue.load(), 0.4f, 0.00001f);
}

TEST_CASE(ui_state_publishes_normalized_raw_center_before_modulation_smoothing_and_presentation) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 0.5f,
        .uiDisplayCenterAlpha = 1.0f,
    });
    auto& unipolar = manager.CreateParameter(group, {
        .name = "Raw",
        .shortName = "Raw",
        .defaultValue = 0.1f,
        .baseColor = synth::Color::Green,
        .indicatorColors = {synth::Color::Cyan, synth::Color::Orange},
        .switchValues = 5,
    });
    auto& bipolar = manager.CreateParameter(group, {
        .name = "Bipolar raw",
        .defaultValue = 0.25f,
        .range = synth::RangeKind::Bipolar,
    });
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 1.0f});
    REQUIRE_TRUE(unipolar.AssignModulationDepth(0, &depth));
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, unipolar);
    bank.AddMapping(11, bipolar);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);
    unipolar.SceneCenter(0) = 0.2f;
    unipolar.SceneCenter(1) = 0.8f;
    bipolar.SceneCenter(0) = 0.25f;
    bipolar.SceneCenter(1) = 0.75f;
    REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));

    auto ui = manager.CreateUIState();
    manager.SetSceneBlend(0.0f);
    manager.ComputeAllTargets();
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(ui->slots[0].cells[0].rawKnobValue.load(), 0.2f, 0.00001f);
    manager.SetSceneBlend(1.0f);
    manager.ComputeAllTargets();
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(ui->slots[0].cells[0].rawKnobValue.load(), 0.8f, 0.00001f);

    manager.SetSceneBlend(0.25f);
    group.GetModulators().Value(0, 0) = 0.0f;
    group.GetModulators().Value(1, 0) = 1.0f;
    manager.ComputeAllTargets();
    unipolar.ProcessLite();
    bipolar.ProcessLite();
    manager.PopulateUIState(*ui);
    const auto& rawCell = ui->slots[0].cells[0];
    REQUIRE_NEAR(rawCell.rawKnobValue.load(), 0.35f, 0.00001f);
    REQUIRE_TRUE(std::fabs(rawCell.values[0].load() - rawCell.rawKnobValue.load()) > 0.01f);
    REQUIRE_TRUE(rawCell.switchValues.load() == 5);
    REQUIRE_TRUE(rawCell.baseColor.Load() == synth::Color::Green);
    REQUIRE_TRUE(rawCell.indicatorColors[0].Load() == synth::Color::Cyan);
    REQUIRE_TRUE(rawCell.indicatorColors[1].Load() == synth::Color::Orange);
    REQUIRE_NEAR(rawCell.minValues[0].load(), 0.0f, 0.00001f);
    REQUIRE_NEAR(rawCell.maxValues[0].load(), 1.0f, 0.00001f);
    REQUIRE_NEAR(ui->slots[0].cells[1].rawKnobValue.load(), 0.375f, 0.00001f);
    REQUIRE_TRUE(ui->slots[0].cells[1].bipolar.load());
    REQUIRE_NEAR(ui->slots[0].cells[1].minValues[0].load(), -0.125f, 0.00001f);
    REQUIRE_NEAR(ui->slots[0].cells[1].maxValues[0].load(), -0.125f, 0.00001f);

    unipolar.GestureValue(0, 0) = 0.6f;
    unipolar.GestureValue(1, 0) = 0.6f;
    unipolar.SetGestureActive(0, 0, true);
    unipolar.SetGestureActive(1, 0, true);
    manager.SetGestureValue(0, 1.0f);
    manager.ComputeAllTargets();
    manager.PopulateUIState(*ui);
    REQUIRE_NEAR(rawCell.rawKnobValue.load(), 0.6f, 0.00001f);
}

TEST_CASE(raw_center_and_processed_epoch_require_one_stable_revision) {
    synth::Parameter::UIState cell(1);
    cell.revision.store(2, std::memory_order_relaxed);
    cell.rawKnobValue.store(0.25f, std::memory_order_relaxed);
    cell.processedAbsoluteEpoch.store(31, std::memory_order_relaxed);

    auto readStable = [](synth::Parameter::UIState& state, float& raw, std::uint64_t& epoch,
                         bool forceRevisionChange = false) {
        const std::uint32_t before = state.revision.load(std::memory_order_acquire);
        if ((before & 1u) != 0) {
            return false;
        }
        raw = state.rawKnobValue.load(std::memory_order_relaxed);
        epoch = state.processedAbsoluteEpoch.load(std::memory_order_relaxed);
        if (forceRevisionChange) {
            state.rawKnobValue.store(0.75f, std::memory_order_relaxed);
            state.processedAbsoluteEpoch.store(32, std::memory_order_relaxed);
            state.revision.store(before + 2, std::memory_order_release);
        }
        const std::uint32_t after = state.revision.load(std::memory_order_acquire);
        return before == after && (after & 1u) == 0;
    };

    float raw = 0.0f;
    std::uint64_t epoch = 0;
    REQUIRE_TRUE(readStable(cell, raw, epoch));
    REQUIRE_NEAR(raw, 0.25f, 0.0f);
    REQUIRE_TRUE(epoch == 31);
    REQUIRE_TRUE(!readStable(cell, raw, epoch, true));
    cell.revision.store(5, std::memory_order_release);
    REQUIRE_TRUE(!readStable(cell, raw, epoch));
}

TEST_CASE(param_set_absolute_routes_by_selected_bank_slot_position_and_physical_encoder) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 2,
        .maxParameters = 2,
        .targetCenterAlpha = 1.0f,
    });
    auto& hidden = manager.CreateParameter(group, {.name = "Hidden", .defaultValue = 0.1f});
    auto& visible = manager.CreateParameter(group, {.name = "Visible", .defaultValue = 0.2f});
    auto& hiddenBank = manager.CreateBank();
    hiddenBank.AddMapping(73, hidden);
    auto& visibleBank = manager.CreateBank();
    visibleBank.AddMapping(73, visible);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(73);
    slot.SelectBank(&visibleBank);
    REQUIRE_TRUE(manager.SetSceneEndpoints(1, 1));

    synth::MessageInBus bus(&manager, 4);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.75f));
    REQUIRE_NEAR(visible.SceneCenter(1), 0.75f, 0.00001f);
    REQUIRE_NEAR(visible.SceneCenter(0), 0.2f, 0.00001f);
    REQUIRE_NEAR(hidden.SceneCenter(1), 0.1f, 0.00001f);

    manager.HandleSetAbsolute(synth::PhysicalEncoderId{73}, 0.4f);
    REQUIRE_NEAR(visible.SceneCenter(1), 0.4f, 0.00001f);
    REQUIRE_TRUE(slot.SelectedBank() == &visibleBank);
}

TEST_CASE(param_set_absolute_edits_visible_modulation_depth_not_hidden_parent) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
        .targetCenterAlpha = 1.0f,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parent = manager.CreateParameter(group, {.name = "Parent", .defaultValue = 0.2f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.3f});
    REQUIRE_TRUE(parent.AssignModulationDepth(0, &depth));
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parent);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);
    manager.HandlePress(0, 0);
    REQUIRE_TRUE(bank.ShowingModulation());

    synth::MessageInBus bus(&manager, 4);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.8f));

    REQUIRE_NEAR(depth.SceneCenter(0), 0.8f, 0.00001f);
    REQUIRE_NEAR(parent.SceneCenter(0), 0.2f, 0.00001f);
}

TEST_CASE(param_set_absolute_is_blocked_by_every_effective_modifier) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Stable absolute", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    synth::MessageInBus bus(&manager, 4);
    const synth::MessageIn edit = synth::MessageIn::ParamSetAbsolute(0, 0, 0, 0.9f);

    manager.SetResetHeld(true);
    bus.Apply(edit);
    manager.SetResetHeld(false);
    manager.SetRandomHeld(true);
    bus.Apply(edit);
    manager.SetRandomHeld(false);
    manager.SetRandomModHeld(true);
    bus.Apply(edit);
    manager.SetRandomModHeld(false);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.00001f);
    bus.Apply(edit);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.9f, 0.00001f);
}

TEST_CASE(param_set_absolute_unmapped_boundaries_are_no_ops) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Unmapped absolute", .defaultValue = 0.25f});
    auto& mappedBank = manager.CreateBank();
    mappedBank.AddMapping(10, parameter);
    auto& emptyBank = manager.CreateBank();

    auto& mappedSlot = manager.CreateBankSlot();
    mappedSlot.AddPhysicalEncoder(10);
    mappedSlot.SelectBank(&mappedBank);
    auto& disconnectedSlot = manager.CreateBankSlot();
    disconnectedSlot.AddPhysicalEncoder(20);
    auto& emptySlot = manager.CreateBankSlot();
    emptySlot.AddPhysicalEncoder(30);
    emptySlot.SelectBank(&emptyBank);

    synth::MessageInBus bus(&manager, 8);
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 99, 0, 0.9f));
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 0, 99, 0.9f));
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 1, 0, 0.9f));
    bus.Apply(synth::MessageIn::ParamSetAbsolute(0, 2, 0, 0.9f));

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.00001f);
}

TEST_CASE(bank_addressed_absolute_write_reaches_a_bank_the_slot_has_not_selected) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& selectedParam = manager.CreateParameter(group, {.name = "Selected", .defaultValue = 0.2f});
    auto& targetParam = manager.CreateParameter(group, {.name = "Target", .defaultValue = 0.3f});
    auto& selectedBank = manager.CreateBank();
    selectedBank.AddMapping(10, selectedParam);
    auto& targetBank = manager.CreateBank();
    targetBank.AddMapping(10, targetParam);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&selectedBank);

    const float targetBefore = targetParam.SceneCenter(0);
    manager.HandleSetAbsoluteOnBank(1, 0, 0, 0.9f);

    REQUIRE_TRUE(targetParam.SceneCenter(0) != targetBefore);
    REQUIRE_NEAR(targetParam.SceneCenter(0), 0.9f, 0.0001f);
    REQUIRE_NEAR(selectedParam.SceneCenter(0), 0.2f, 0.0001f);
}

TEST_CASE(bank_addressed_absolute_write_leaves_the_slots_selected_bank_unchanged) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& selectedParam = manager.CreateParameter(group, {.name = "Selected", .defaultValue = 0.15f});
    auto& targetParam = manager.CreateParameter(group, {.name = "Target", .defaultValue = 0.25f});
    auto& selectedBank = manager.CreateBank();
    selectedBank.AddMapping(10, selectedParam);
    auto& targetBank = manager.CreateBank();
    targetBank.AddMapping(10, targetParam);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&selectedBank);

    manager.HandleSetAbsoluteOnBank(1, 0, 0, 0.6f);

    REQUIRE_TRUE(slot.SelectedBank() == &selectedBank);
    REQUIRE_NEAR(targetParam.SceneCenter(0), 0.6f, 0.0001f);
}

TEST_CASE(bank_addressed_absolute_write_to_a_different_bank_leaves_open_modulation_view_untouched) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 3,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& viewedParam = manager.CreateParameter(group, {.name = "Viewed", .defaultValue = 0.2f});
    auto& viewedDepth = manager.CreateParameter(group, {.name = "ViewedDepth", .defaultValue = 0.3f});
    REQUIRE_TRUE(viewedParam.AssignModulationDepth(0, &viewedDepth));
    auto& targetParam = manager.CreateParameter(group, {.name = "Target", .defaultValue = 0.4f});
    auto& viewedBank = manager.CreateBank();
    viewedBank.AddMapping(10, viewedParam);
    auto& targetBank = manager.CreateBank();
    targetBank.AddMapping(10, targetParam);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&viewedBank);
    manager.HandlePress(0, 0);
    REQUIRE_TRUE(viewedBank.ShowingModulation());
    REQUIRE_TRUE(viewedBank.SelectedParameter() == &viewedParam);

    manager.HandleSetAbsoluteOnBank(1, 0, 0, 0.85f);

    REQUIRE_NEAR(targetParam.SceneCenter(0), 0.85f, 0.0001f);
    REQUIRE_TRUE(viewedBank.ShowingModulation());
    REQUIRE_TRUE(viewedBank.SelectedParameter() == &viewedParam);
    REQUIRE_TRUE(viewedBank.VisibleParameter(10) == &viewedDepth);
}

TEST_CASE(bank_addressed_absolute_write_edits_top_level_parameter_not_visible_modulation_depth) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parent = manager.CreateParameter(group, {.name = "Parent", .defaultValue = 0.2f});
    auto& depth = manager.CreateParameter(group, {.name = "Depth", .defaultValue = 0.3f});
    REQUIRE_TRUE(parent.AssignModulationDepth(0, &depth));
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parent);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);
    manager.HandlePress(0, 0);
    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depth);

    const float parentBefore = parent.SceneCenter(0);
    const float depthBefore = depth.SceneCenter(0);

    manager.HandleSetAbsoluteOnBank(0, 0, 0, 0.8f);

    REQUIRE_TRUE(parent.SceneCenter(0) != parentBefore);
    REQUIRE_NEAR(parent.SceneCenter(0), 0.8f, 0.00001f);
    REQUIRE_NEAR(depth.SceneCenter(0), depthBefore, 0.00001f);
    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleParameter(10) == &depth);
}

TEST_CASE(bank_addressed_absolute_write_via_message_bus_matches_direct_call) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 2});
    auto& selectedParam = manager.CreateParameter(group, {.name = "Selected", .defaultValue = 0.1f});
    auto& targetParam = manager.CreateParameter(group, {.name = "Target", .defaultValue = 0.2f});
    auto& selectedBank = manager.CreateBank();
    selectedBank.AddMapping(10, selectedParam);
    auto& targetBank = manager.CreateBank();
    targetBank.AddMapping(10, targetParam);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&selectedBank);
    synth::MessageInBus bus(&manager, 4);

    manager.HandleSetAbsoluteOnBank(1, 0, 0, 0.55f);
    REQUIRE_NEAR(targetParam.SceneCenter(0), 0.55f, 0.0001f);

    bus.Apply(synth::MessageIn::ParamSetAbsoluteOnBank(0, 1, 0, 0, 0.83f));
    REQUIRE_NEAR(targetParam.SceneCenter(0), 0.83f, 0.0001f);
}

TEST_CASE(unmapped_encoder_ignored) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = manager.CreateParameter(group, {.name = "Stable", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(1, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(1);
    slot.SelectBank(&bank);

    manager.HandleTick(99, 0.5f);
    manager.HandlePress(99);
    manager.SetResetHeld(true);
    manager.HandlePress(99);
    manager.SetResetHeld(false);

    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(external_gesture_selection_and_value_api) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Gesture", .defaultValue = 0.2f});
    parameter.GestureValue(0, 0) = 1.0f;
    parameter.SetGestureActive(0, 0, true);

    manager.SelectGesture(0);
    manager.SetGestureValue(0, 0.75f);
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});

    REQUIRE_TRUE(manager.GestureSelected(0));
    REQUIRE_NEAR(manager.GestureValue(0), 0.75f, 0.0001f);
    REQUIRE_NEAR(parameter.TargetCenter(), 0.8f, 0.0001f);

    manager.DeselectGesture(0);
    REQUIRE_TRUE(!manager.GestureSelected(0));
}

TEST_CASE(parameter_and_slot_ui_state_reports_values_colors_and_target_cell_metadata) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 2,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {
        .name = "Pan",
        .shortName = "Pan",
        .defaultValue = 0.0f,
        .range = synth::RangeKind::Bipolar,
        .baseColor = synth::Color::Green,
        .indicatorColors = {synth::Color::Cyan, synth::Color::Orange},
        .switchValues = 5,
    });
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    bank.AddMapping(11, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    auto& depth = manager.CreateParameter(group, {
        .name = "Pan Depth",
        .defaultValue = 1.0f,
        .range = synth::RangeKind::Bipolar,
    });
    REQUIRE_TRUE(parameter.AssignModulationDepth(0, &depth));
    group.GetModulators().Value(0, 0) = 0.25f;
    group.GetModulators().Value(1, 0) = 0.75f;
    parameter.Compute({.leftScene = 0, .rightScene = 0, .blend = 0.0f});
    parameter.ProcessLite();
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);

    const synth::Parameter::UIState& cell = ui->slots[0].cells[0];
    REQUIRE_TRUE(cell.connected.load());
    REQUIRE_TRUE(cell.bipolar.load());
    REQUIRE_TRUE(cell.baseColor.Load() == synth::Color::Green);
    REQUIRE_TRUE(cell.indicatorColors[0].Load() == synth::Color::Cyan);
    REQUIRE_TRUE(cell.indicatorColors[1].Load() == synth::Color::Orange);
    REQUIRE_NEAR(cell.minValues[0].load(), -1.0f, 0.0001f);
    REQUIRE_NEAR(cell.maxValues[1].load(), 1.0f, 0.0001f);

    manager.HandlePress(0, 0);
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->slots[0].showingModulationView.load());
    const synth::Parameter::UIState& targetCell = ui->slots[0].cells[1];
    REQUIRE_TRUE(targetCell.connected.load());
    REQUIRE_TRUE(targetCell.switchValues.load() == 5);
    REQUIRE_TRUE(targetCell.switchValue[0].load() == 1);
    REQUIRE_TRUE(targetCell.switchValue[1].load() == 3);
    REQUIRE_TRUE(targetCell.modulatorsAffectingMask.load() == 1u);
    REQUIRE_TRUE(targetCell.gesturesAffectingMask.load() == 0u);
    REQUIRE_TRUE(targetCell.baseColor.Load() == synth::Color::Green);
    REQUIRE_TRUE(targetCell.shortName.load() == parameter.ShortName().c_str());
}

TEST_CASE(message_bus_routes_external_messages_and_timestamps) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 3,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& bankA = manager.CreateBank();
    bankA.AddMapping(10, parameter);
    auto& bankB = manager.CreateBank();
    bankB.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bankA);

    synth::MessageInBus bus(&manager, 4);
    auto ui = manager.CreateUIState();
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(10, 0, 0, 0.25f)));
    bus.Process(9);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    bus.Process(10);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.5f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(11, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(11, 0, 0)));
    bus.Process(11);
    REQUIRE_TRUE(manager.ResetHeld());
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->resetHeld.load());

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(12, false)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ToggleGestureSelect(12, 0)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(12, 0, 0.75f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(12, 0, 1)));
    REQUIRE_TRUE(!bus.Push(synth::MessageIn::Clock(12)));
    bus.Process(12);
    REQUIRE_TRUE(manager.GestureSelected(0));
    REQUIRE_NEAR(manager.GestureValue(0), 0.75f, 0.0001f);
    REQUIRE_TRUE(slot.SelectedBank() == &bankB);
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->gestures.selected[0].load());

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetSceneBlend(13, 0.25f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(13, 2)));
    bus.Process(13);
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 2);
    REQUIRE_NEAR(manager.Scene().blend, 0.25f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetSceneBlend(14, 0.75f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(14, 1)));
    bus.Process(14);
    REQUIRE_TRUE(manager.Scene().leftScene == 1);
    REQUIRE_TRUE(manager.Scene().rightScene == 2);
    REQUIRE_NEAR(manager.Scene().blend, 0.75f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetSceneBlend(15, 0.5f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(15, 0)));
    bus.Process(15);
    REQUIRE_TRUE(manager.Scene().leftScene == 1);
    REQUIRE_TRUE(manager.Scene().rightScene == 0);
    REQUIRE_NEAR(manager.Scene().blend, 0.5f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(16, 3)));
    bus.Process(16);
    REQUIRE_TRUE(manager.Scene().leftScene == 1);
    REQUIRE_TRUE(manager.Scene().rightScene == 0);
    manager.PopulateUIState(*ui);
    REQUIRE_TRUE(ui->leftScene.load() == 1);
    REQUIRE_TRUE(ui->rightScene.load() == 0);
}

TEST_CASE(message_bus_ignores_out_of_bounds_targets) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "A", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    manager.SetSceneEndpoints(0, 1);
    manager.SetSceneBlend(0.25f);
    manager.SetGestureValue(0, 0.5f);

    synth::MessageInBus bus(&manager, 16);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(0, 0, 99)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(0, 99, 1.0f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ToggleGestureSelect(0, 99)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(0, 99)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(0, 99, 0, 0.5f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(0, 0, 99, 0.5f)));
    bus.Process(0);

    REQUIRE_TRUE(slot.SelectedBank() == &bank);
    REQUIRE_TRUE(!manager.GestureSelected(0));
    REQUIRE_NEAR(manager.GestureValue(0), 0.5f, 0.0001f);
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 1);
    REQUIRE_NEAR(manager.Scene().blend, 0.25f, 0.0001f);
    REQUIRE_NEAR(parameter.SceneCenter(0), 0.25f, 0.0001f);
}

TEST_CASE(message_bus_routes_parameter_and_grid_families_without_namespace_aliasing) {
    synth::ParameterManager parameters;
    auto& group = parameters.CreateGroup({.numVoices = 1, .numScenes = 1, .maxParameters = 1});
    auto& parameter = parameters.CreateParameter(group, {.name = "Shared zero", .defaultValue = 0.25f});
    auto& bankA = parameters.CreateBank();
    bankA.AddMapping(10, parameter);
    auto& bankB = parameters.CreateBank();
    bankB.AddMapping(10, parameter);
    auto& parameterSlot = parameters.CreateBankSlot();
    parameterSlot.AddPhysicalEncoder(10);
    parameterSlot.SelectBank(&bankA);

    const auto range = synth::GridRange::Create(-2, 1, 6, 9);
    REQUIRE_TRUE(range.has_value());
    synth::GridManager grids;
    const std::size_t gridA = *grids.CreateGrid(*range);
    const std::size_t gridB = *grids.CreateGrid(*range);
    const std::size_t gridSlot = *grids.CreateSlot(*range);
    std::vector<std::string> events;
    REQUIRE_TRUE(grids.GridAt(gridA)->RegisterCell(-1, 7, std::make_unique<RecordingGridCell>(&events, 0)));
    REQUIRE_TRUE(grids.GridAt(gridB)->RegisterCell(-1, 7, std::make_unique<RecordingGridCell>(&events, 1)));
    REQUIRE_TRUE(grids.SelectGridForSlot(gridSlot, gridA));

    synth::MessageInBus bus(&parameters, 16);
    bus.SetGridManager(&grids);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectGrid(10, 0, 1)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPress(11, 0, -1, 7, 100)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(12, 0, 1)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPressureChange(12, 0, -1, 7, 64)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridRelease(13, 0, -1, 7)));

    bus.Process(9);
    REQUIRE_TRUE(grids.SlotAt(0)->SelectedGrid() == grids.GridAt(0));
    REQUIRE_TRUE(parameterSlot.SelectedBank() == &bankA);
    REQUIRE_TRUE(events.empty());

    bus.Process(10);
    REQUIRE_TRUE(grids.SlotAt(0)->SelectedGrid() == grids.GridAt(1));
    REQUIRE_TRUE(parameterSlot.SelectedBank() == &bankA);
    bus.Process(11);
    REQUIRE_TRUE((events == std::vector<std::string>{"press:1:100"}));
    bus.Process(12);
    REQUIRE_TRUE(parameterSlot.SelectedBank() == &bankB);
    REQUIRE_TRUE(grids.SlotAt(0)->SelectedGrid() == grids.GridAt(1));
    REQUIRE_TRUE((events == std::vector<std::string>{"press:1:100", "pressure:1:64"}));
    bus.Process(13);
    REQUIRE_TRUE((events == std::vector<std::string>{"press:1:100", "pressure:1:64", "release:1"}));

    const std::size_t eventCount = events.size();
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectGrid(14, 99, 0)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectGrid(14, 0, 99)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPress(14, 99, -1, 7, 127)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPress(14, 0, -100, 7, 127)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPressureChange(14, 0, -1, 100, 127)));
    bus.Process(14);
    REQUIRE_TRUE(grids.SlotAt(0)->SelectedGrid() == grids.GridAt(1));
    REQUIRE_TRUE(events.size() == eventCount);

    synth::MessageInBus parameterOnly(&parameters, 4);
    REQUIRE_TRUE(parameterOnly.Push(synth::MessageIn::GridPress(15, 0, -1, 7, 1)));
    parameterOnly.Process(15);
    REQUIRE_TRUE(events.size() == eventCount);

    synth::MessageInBus gridOnly(nullptr, 4);
    gridOnly.SetGridManager(&grids);
    REQUIRE_TRUE(gridOnly.Push(synth::MessageIn::SelectParamBank(15, 0, 0)));
    gridOnly.Process(15);
    REQUIRE_TRUE(parameterSlot.SelectedBank() == &bankB);
}

TEST_CASE(message_bus_set_reset_and_set_gesture_select_are_idempotent) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    (void)manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
    });
    synth::MessageInBus bus(&manager, 8);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(0, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(0, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(0, 1, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(0, 1, false)));
    bus.Process(0);

    REQUIRE_TRUE(manager.ResetHeld());
    REQUIRE_TRUE(!manager.GestureSelected(1));
}

TEST_CASE(message_bus_and_patch_round_trip_gesture_indices_32_and_63) {
    synth::ParameterManager source;
    REQUIRE_TRUE(source.SetGestureCount(64));
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& sourceParameter = source.CreateParameter(sourceGroup, {.name = "High gestures", .defaultValue = 0.25f});
    synth::MessageInBus bus(&source, 8);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(0, 32, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(0, 63, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(0, 32, 0.4f)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(0, 63, 0.8f)));
    bus.Process(0);
    REQUIRE_TRUE(source.GestureSelected(32));
    REQUIRE_TRUE(source.GestureSelected(63));

    sourceParameter.GestureValue(0, 32) = 0.45f;
    sourceParameter.GestureValue(0, 63) = 0.85f;
    sourceParameter.SetGestureActive(0, 32, true);
    sourceParameter.SetGestureActive(0, 63, true);

    synth::JsonArena arena(65536);
    const synth::JSON saved = source.ParameterValuesToJSON(arena);
    synth::ParameterManager target;
    REQUIRE_TRUE(target.SetGestureCount(64));
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .targetCenterAlpha = 1.0f,
    });
    auto& targetParameter = target.CreateParameter(targetGroup, {.name = "High gestures", .defaultValue = 0.25f});
    REQUIRE_TRUE(target.LoadParameterValuesFromJSON(saved));
    REQUIRE_NEAR(targetParameter.GestureValue(0, 32), 0.45f, 0.000001f);
    REQUIRE_NEAR(targetParameter.GestureValue(0, 63), 0.85f, 0.000001f);
    REQUIRE_TRUE(targetParameter.GestureActive(0, 32));
    REQUIRE_TRUE(targetParameter.GestureActive(0, 63));
}

TEST_CASE(manager_tracks_reset_random_and_random_mod_precedence) {
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::None);

    manager.SetResetHeld(true);
    REQUIRE_TRUE(manager.ResetHeld());
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::Reset);

    manager.SetRandomHeld(true);
    REQUIRE_TRUE(manager.RandomHeld());
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::Random);

    manager.SetRandomModHeld(true);
    REQUIRE_TRUE(manager.RandomModHeld());
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::RandomMod);

    manager.SetRandomModHeld(false);
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::Random);

    manager.SetRandomHeld(false);
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::Reset);

    manager.ToggleResetHeld();
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::None);
}

TEST_CASE(message_bus_sets_reset_random_and_random_mod_idempotently) {
    synth::ParameterManager manager;
    synth::MessageInBus bus(&manager, 16);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(1, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandom(2, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandomMod(3, true)));
    bus.Process(3);

    REQUIRE_TRUE(manager.ResetHeld());
    REQUIRE_TRUE(manager.RandomHeld());
    REQUIRE_TRUE(manager.RandomModHeld());
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::RandomMod);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandomMod(4, false)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandom(5, false)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(6, false)));
    bus.Process(6);

    REQUIRE_TRUE(!manager.ResetHeld());
    REQUIRE_TRUE(!manager.RandomHeld());
    REQUIRE_TRUE(!manager.RandomModHeld());
    REQUIRE_TRUE(manager.GetCurrentModifier() == synth::Modifier::None);
}

TEST_CASE(manager_random_source_hooks_are_deterministic_and_bounded) {
    synth::ParameterManager manager;
    std::vector<float> values{1.25f, -0.5f, 0.4f};
    std::vector<float> coins{0.8f, 0.2f};
    std::vector<std::size_t> indices{7, 12};
    std::size_t valueIx = 0;
    std::size_t coinIx = 0;
    std::size_t indexIx = 0;

    manager.SetRandomSource(
        [&values, &valueIx]() { return values.at(valueIx++); },
        [&coins, &coinIx]() { return coins.at(coinIx++); },
        [&indices, &indexIx](std::size_t) { return indices.at(indexIx++); });

    REQUIRE_NEAR(manager.NextRandomValue(), 1.0f, 0.0001f);
    REQUIRE_NEAR(manager.NextRandomValue(), 0.0f, 0.0001f);
    REQUIRE_NEAR(manager.NextRandomValue(), 0.4f, 0.0001f);
    REQUIRE_NEAR(manager.NextRandomCoin(), 0.8f, 0.0001f);
    REQUIRE_NEAR(manager.NextRandomCoin(), 0.2f, 0.0001f);
    REQUIRE_TRUE(manager.NextRandomIndex(5) == 2);
    REQUIRE_TRUE(manager.NextRandomIndex(5) == 2);
    REQUIRE_TRUE(manager.NextRandomIndex(0) == 0);
}

TEST_CASE(manager_ui_state_reports_bank_colors_selection_and_gesture_affecting) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 2,
        .maxParameters = 4,
    });
    auto& affected = manager.CreateParameter(group, {.name = "A", .defaultValue = 0.25f});
    auto& unaffected = manager.CreateParameter(group, {.name = "B", .defaultValue = 0.5f});
    auto& drillHidden = manager.CreateParameter(group, {.name = "C", .defaultValue = 0.75f});
    affected.SetGestureActive(0, 0, true);
    affected.SetGestureActive(0, 63, true);
    unaffected.SetGestureActive(0, 1, true);
    drillHidden.SetGestureActive(0, 2, true);

    auto& bankA = manager.CreateBank();
    bankA.SetBankColor(synth::Color::Green);
    bankA.AddMapping(10, affected);
    bankA.AddMapping(13, drillHidden);
    auto& bankB = manager.CreateBank();
    bankB.SetBankColor(synth::Color::Blue);
    bankB.AddMapping(11, unaffected);
    auto& bankC = manager.CreateBank();
    bankC.SetBankColor(synth::Color::Red);
    bankC.AddMapping(12, affected);

    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(13);
    slot.SelectBank(&bankA);
    manager.HandlePress(0, 0);
    REQUIRE_TRUE(bankA.ShowingModulation());

    synth::ParameterManager::UIState ui;
    ui.Configure(1, 2, 1, 0, 64, 4);
    manager.PopulateUIState(ui);

    REQUIRE_TRUE(ui.bankCapacity == 4);
    REQUIRE_TRUE(ui.banks[0].connected.load());
    REQUIRE_TRUE(ui.banks[0].selected.load());
    REQUIRE_TRUE(ui.banks[0].bankColor.Load() == synth::Color::Green);
    REQUIRE_TRUE(ui.banks[1].connected.load());
    REQUIRE_TRUE(!ui.banks[1].selected.load());
    REQUIRE_TRUE(ui.banks[1].bankColor.Load() == synth::Color::Blue);
    REQUIRE_TRUE(ui.banks[2].connected.load());
    REQUIRE_TRUE(!ui.banks[2].selected.load());
    REQUIRE_TRUE(ui.banks[2].bankColor.Load() == synth::Color::Red);
    REQUIRE_TRUE(!ui.banks[3].connected.load());
    REQUIRE_TRUE(!ui.banks[3].selected.load());
    REQUIRE_TRUE(ui.banks[3].bankColor.Load() == synth::Color::Off);
    REQUIRE_TRUE(ui.gestures.bankAffectingCount[0].load() == 2);
    REQUIRE_TRUE(ui.gestures.bankAffectingMask[0].load() == ((1u << 0u) | (1u << 2u)));
    REQUIRE_TRUE(ui.gestures.bankAffectingCount[1].load() == 1);
    REQUIRE_TRUE(ui.gestures.bankAffectingMask[1].load() == (1u << 1u));
    REQUIRE_TRUE(ui.gestures.bankAffectingCount[2].load() == 1);
    REQUIRE_TRUE(ui.gestures.bankAffectingMask[2].load() == (1u << 0u));
    REQUIRE_TRUE(ui.gestures.bankAffectingCount[3].load() == 0);
    REQUIRE_TRUE(ui.gestures.bankAffectingMask[3].load() == 0);
    REQUIRE_TRUE(ui.gestures.bankAffectingCount[63].load() == 2);
    REQUIRE_TRUE(ui.gestures.bankAffectingMask[63].load() == ((1u << 0u) | (1u << 2u)));
}

TEST_CASE(message_bus_routes_modulation_target_position_to_visible_parameter) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    bank.AddMapping(11, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    synth::MessageInBus bus(&manager, 8);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(1, 0, 0)));
    bus.Process(1);
    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(bank.VisibleParameter(11) == &carrier);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(2, 0, 1, 0.2f)));
    bus.Process(2);
    REQUIRE_NEAR(carrier.SceneCenter(0), 0.6f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(3, true)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(4, 0, 1)));
    REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(5, false)));
    bus.Process(5);
    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_NEAR(carrier.SceneCenter(0), 0.4f, 0.0001f);

    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(6, 0, 1)));
    bus.Process(6);
    REQUIRE_TRUE(!bank.ShowingModulation());
}

TEST_CASE(message_bus_bank_select_deselects_prior_modulation_view) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    MarkAllModulatorsConnectedForUi(group);
    auto& parameter = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& bankA = manager.CreateBank();
    bankA.AddMapping(10, parameter);
    bankA.AddMapping(11, parameter);
    auto& bankB = manager.CreateBank();
    bankB.AddMapping(10, parameter);
    bankB.AddMapping(11, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bankA);

    synth::MessageInBus bus(&manager, 8);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(1, 0, 0)));
    bus.Process(1);
    REQUIRE_TRUE(bankA.ShowingModulation());

    REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(2, 0, 1)));
    bus.Process(2);
    REQUIRE_TRUE(slot.SelectedBank() == &bankB);
    REQUIRE_TRUE(!bankA.ShowingModulation());
}

struct CountingMidiInProcessor : synth::MidiInProcessor {
    int count = 0;
    synth::BasicMidi last;

    void Process(const synth::BasicMidi& midi) override {
        ++count;
        last = midi;
    }
};

struct FakeMidiSink : synth::IMidiOutputSink {
    std::vector<synth::BasicMidi> sent;

    void Send(const synth::BasicMidi& midi) override {
        sent.push_back(midi);
    }
};

void PublishEncoderCellAt(synth::ParameterManager::UIState& ui, std::size_t slotIx,
                          std::size_t position, float displayValue, float rawValue,
                          std::uint64_t processedEpoch, bool connected = true,
                          synth::Color baseColor = synth::Color::Green,
                          synth::Color indicatorColor = synth::Color::Cyan) {
    auto& cell = ui.slots[slotIx].cells[position];
    const std::uint32_t revision = cell.revision.load(std::memory_order_relaxed);
    cell.revision.store(revision | 1u, std::memory_order_release);
    cell.connected.store(connected, std::memory_order_relaxed);
    cell.bipolar.store(false, std::memory_order_relaxed);
    cell.voiceCount.store(connected ? 1u : 0u, std::memory_order_relaxed);
    cell.values[0].store(displayValue, std::memory_order_relaxed);
    cell.rawKnobValue.store(rawValue, std::memory_order_relaxed);
    cell.processedAbsoluteEpoch.store(processedEpoch, std::memory_order_relaxed);
    cell.baseColor.Store(baseColor, std::memory_order_relaxed);
    cell.indicatorColors[0].Store(indicatorColor, std::memory_order_relaxed);
    cell.revision.store((revision | 1u) + 1u, std::memory_order_release);
}

void PublishEncoderCell(synth::ParameterManager::UIState& ui, float displayValue, float rawValue,
                        std::uint64_t processedEpoch, bool connected = true,
                        synth::Color baseColor = synth::Color::Green,
                        synth::Color indicatorColor = synth::Color::Cyan) {
    PublishEncoderCellAt(ui, 0, 0, displayValue, rawValue, processedEpoch,
                         connected, baseColor, indicatorColor);
}

std::unique_ptr<synth::MidiOutputProcessor> MakeEncoderOutput(
    synth::EncoderMidiOutProtocol protocol, synth::EncoderMode feedbackMode,
    synth::MidiSender* sender, synth::ParameterManager::UIState* ui,
    synth::AbsoluteFeedbackCoordinator* coordinator = nullptr,
    std::size_t controllerSlot = 0) {
    synth::EncoderMidiOutConfig config = protocol == synth::EncoderMidiOutProtocol::Twister
                                             ? synth::EncoderMidiOutConfig::TwisterDefault(0)
                                             : synth::EncoderMidiOutConfig::WrldBldrDefault(0);
    config.KeepFirstPositions(1);
    if (protocol == synth::EncoderMidiOutProtocol::Twister) {
        return std::make_unique<synth::TwisterMidiOutProcessor>(
            config, sender, ui, 0, feedbackMode, coordinator, controllerSlot);
    }
    return std::make_unique<synth::WrldBldrMidiOutProcessor>(
        config, sender, ui, 0, feedbackMode, coordinator, controllerSlot);
}

std::size_t CountPositionMessages(const FakeMidiSink& sink, std::size_t begin = 0) {
    return static_cast<std::size_t>(std::count_if(
        sink.sent.begin() + static_cast<std::ptrdiff_t>(begin), sink.sent.end(),
        [](const synth::BasicMidi& midi) { return midi.IsCC() && midi.Channel() == 0; }));
}

std::optional<std::uint8_t> LastPositionValue(const FakeMidiSink& sink, std::size_t begin = 0) {
    for (std::size_t ix = sink.sent.size(); ix > begin; --ix) {
        const synth::BasicMidi& midi = sink.sent[ix - 1];
        if (midi.IsCC() && midi.Channel() == 0) {
            return midi.GetValue();
        }
    }
    return std::nullopt;
}

TEST_CASE(midi_basic_helpers_expose_raw_bytes_and_sizes) {
    const synth::BasicMidi cc = synth::BasicMidi::CC(10, 2, 7, 99);
    REQUIRE_TRUE(cc.timestamp == 10);
    REQUIRE_TRUE(cc.Size() == 3);
    REQUIRE_TRUE(cc.Status() == synth::BasicMidi::kStatusCC);
    REQUIRE_TRUE(cc.Channel() == 2);
    REQUIRE_TRUE(cc.GetCC() == 7);
    REQUIRE_TRUE(cc.GetValue() == 99);
    REQUIRE_TRUE(cc.raw[0] == 0xB2);

    const synth::BasicMidi note = synth::BasicMidi::Note(11, 3, 60, 100);
    REQUIRE_TRUE(note.Status() == synth::BasicMidi::kStatusNote);
    REQUIRE_TRUE(note.Channel() == 3);
    REQUIRE_TRUE(note.GetNote() == 60);
    REQUIRE_TRUE(note.GetValue() == 100);

    const synth::BasicMidi noteOff = synth::BasicMidi::Note(12, 4, 61, 0);
    REQUIRE_TRUE(noteOff.Status() == synth::BasicMidi::kStatusNoteOff);
    REQUIRE_TRUE(noteOff.Channel() == 4);
    REQUIRE_TRUE(noteOff.GetNote() == 61);

    const synth::BasicMidi bend = synth::BasicMidi::PitchBend(13, 5, 0x1234);
    REQUIRE_TRUE(bend.Status() == synth::BasicMidi::kStatusPitchBend);
    REQUIRE_TRUE(bend.Channel() == 5);
    REQUIRE_TRUE(bend.GetPitchBend() == 0x1234);

    const synth::BasicMidi clock = synth::BasicMidi::Clock(44);
    REQUIRE_TRUE(clock.timestamp == 44);
    REQUIRE_TRUE(clock.Size() == 1);
    REQUIRE_TRUE(clock.raw[0] == synth::BasicMidi::kStatusClock);
    REQUIRE_TRUE(synth::BasicMidi::IsSupportedRealtimeStatus(clock.raw[0]));
    REQUIRE_TRUE(synth::BasicMidi::TransportStart(45).Size() == 1);
    REQUIRE_TRUE(synth::BasicMidi::TransportContinue(46).Size() == 1);
    REQUIRE_TRUE(synth::BasicMidi::TransportStop(47).Size() == 1);
}

TEST_CASE(midi_encoder_input_maps_scaled_turns_pushes_and_timestamps) {
    synth::ParameterManager manager;
    synth::MessageInBus bus(&manager, 16);
    synth::EncoderMidiInConfig config = synth::EncoderMidiInConfig::TwisterDefault(1);
    config.turnStep = 0.01f;
    config.KeepFirstPositions(6);
    synth::EncoderMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 0; });

    processor.Process(synth::BasicMidi::CC(9999, 0, 5, 63));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 0));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.timestamp == 0);
    REQUIRE_TRUE(message.slotIx == 1);
    REQUIRE_TRUE(message.position == 5);
    REQUIRE_NEAR(message.delta, -0.01f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(9999, 0, 5, 66));
    REQUIRE_TRUE(bus.Pop(message, 0));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.timestamp == 0);
    REQUIRE_TRUE(message.slotIx == 1);
    REQUIRE_TRUE(message.position == 5);
    REQUIRE_NEAR(message.delta, 0.02f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(9999, 1, 5, 127));
    REQUIRE_TRUE(bus.Pop(message, 0));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(message.timestamp == 0);
    REQUIRE_TRUE(message.slotIx == 1);
    REQUIRE_TRUE(message.position == 5);
}

TEST_CASE(midi_encoder_input_direction_only_zero_and_thru_behavior) {
    synth::MessageInBus bus(nullptr, 16);
    synth::EncoderMidiInConfig config;
    config.mode = synth::EncoderMode::DirectionOnly;
    config.turnStep = 0.01f;
    config.turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 0});
    config.pushes.push_back({.control = {.channel = 1, .cc = 1}, .slotIx = 0, .position = 0});
    synth::EncoderMidiInProcessor processor(config, &bus);
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::CC(1, 0, 1, 1));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 0));
    REQUIRE_NEAR(message.delta, -0.01f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(1, 0, 1, 64));
    REQUIRE_TRUE(!bus.Pop(message, 0));

    processor.Process(synth::BasicMidi::CC(1, 0, 1, 127));
    REQUIRE_TRUE(bus.Pop(message, 0));
    REQUIRE_NEAR(message.delta, 0.01f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(1, 1, 1, 0));
    REQUIRE_TRUE(!bus.Pop(message, 0));
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(1, 9, 9, 99));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.Channel() == 9);
    REQUIRE_TRUE(thru.last.GetCC() == 9);
}

TEST_CASE(midi_encoder_input_matches_typed_note_pushes_and_consumes_both_note_releases) {
    synth::MessageInBus bus(nullptr, 16);
    synth::EncoderMidiInConfig config;
    config.pushes.push_back({
        .control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note},
        .slotIx = 2,
        .position = 3,
    });
    config.pushes.push_back({
        .control = {.channel = 1, .cc = 61},
        .slotIx = 4,
        .position = 5,
    });
    synth::EncoderMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 77; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::Note(1, 1, 60, 100));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(message.slotIx == 2);
    REQUIRE_TRUE(message.position == 3);

    const synth::BasicMidi zeroVelocityNoteOn(
        1, std::vector<std::uint8_t>{static_cast<std::uint8_t>(synth::BasicMidi::kStatusNote | 1), 60, 0});
    processor.Process(zeroVelocityNoteOn);
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 0);

    const synth::BasicMidi nonzeroVelocityNoteOff(
        2,
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(synth::BasicMidi::kStatusNoteOff | 1), 60, 77});
    processor.Process(nonzeroVelocityNoteOff);
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(3, 1, 60, 127));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.IsCC());

    processor.Process(synth::BasicMidi::Note(4, 1, 61, 127));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 2);
    REQUIRE_TRUE(thru.last.Status() == synth::BasicMidi::kStatusNote);
}

TEST_CASE(absolute_feedback_coordinator_tracks_latest_expectations_per_controller_route) {
    static_assert(synth::AbsoluteFeedbackCoordinator::kMaxRoutes == 4096);

    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto firstController = coordinator.ReserveRoute({.controllerSlot = 1, .parameterSlot = 4, .position = 6});
    const auto secondController = coordinator.ReserveRoute({.controllerSlot = 2, .parameterSlot = 4, .position = 6});
    REQUIRE_TRUE(firstController.IsTracked());
    REQUIRE_TRUE(secondController.IsTracked());

    const auto first = coordinator.BeginInput(firstController, 17);
    const auto second = coordinator.BeginInput(secondController, 41);
    const auto replacement = coordinator.BeginInput(firstController, 93);
    REQUIRE_TRUE(first.IsPublished());
    REQUIRE_TRUE(first.Epoch() != 0);
    REQUIRE_TRUE(first.Epoch() < second.Epoch());
    REQUIRE_TRUE(second.Epoch() < replacement.Epoch());

    const auto firstState = coordinator.Snapshot(firstController);
    const auto secondState = coordinator.Snapshot(secondController);
    REQUIRE_TRUE(firstState.has_value());
    REQUIRE_TRUE(firstState->pending);
    REQUIRE_TRUE(firstState->epoch == replacement.Epoch());
    REQUIRE_TRUE(firstState->receivedValue == 93);
    REQUIRE_TRUE(secondState.has_value());
    REQUIRE_TRUE(secondState->pending);
    REQUIRE_TRUE(secondState->epoch == second.Epoch());
    REQUIRE_TRUE(secondState->receivedValue == 41);
}

TEST_CASE(absolute_feedback_coordinator_guard_resolves_only_the_expected_epoch) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 3, .parameterSlot = 2, .position = 7});
    const auto alert = coordinator.BeginInput(route, 64);

    {
        auto guard = coordinator.GuardRoute(route);
        REQUIRE_TRUE(guard.IsTracked());
        const auto guardedState = guard.Snapshot();
        REQUIRE_TRUE(guardedState.pending);
        REQUIRE_TRUE(guardedState.epoch == alert.Epoch());
        REQUIRE_TRUE(guardedState.receivedValue == 64);
        REQUIRE_TRUE(!guard.Resolve(alert.Epoch() - 1));
        REQUIRE_TRUE(guard.Resolve(alert.Epoch()));
    }

    const auto resolved = coordinator.Snapshot(route);
    REQUIRE_TRUE(resolved.has_value());
    REQUIRE_TRUE(!resolved->pending);
    REQUIRE_TRUE(resolved->epoch == alert.Epoch());
}

TEST_CASE(absolute_feedback_coordinator_conditionally_rolls_back_failed_input) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = 1, .position = 2});
    const auto prior = coordinator.BeginInput(route, 12);
    const auto failed = coordinator.BeginInput(route, 34);
    REQUIRE_TRUE(coordinator.Rollback(failed));

    auto state = coordinator.Snapshot(route);
    REQUIRE_TRUE(state.has_value());
    REQUIRE_TRUE(state->pending);
    REQUIRE_TRUE(state->epoch == prior.Epoch());
    REQUIRE_TRUE(state->receivedValue == 12);

    const auto supersededFailure = coordinator.BeginInput(route, 56);
    const auto newer = coordinator.BeginInput(route, 78);
    REQUIRE_TRUE(!coordinator.Rollback(supersededFailure));
    state = coordinator.Snapshot(route);
    REQUIRE_TRUE(state.has_value());
    REQUIRE_TRUE(state->pending);
    REQUIRE_TRUE(state->epoch == newer.Epoch());
    REQUIRE_TRUE(state->receivedValue == 78);
}

TEST_CASE(absolute_feedback_coordinator_capacity_exhaustion_fails_closed) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    for (std::size_t ix = 0; ix < synth::AbsoluteFeedbackCoordinator::kMaxRoutes; ++ix) {
        const auto route = coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = ix, .position = 0});
        REQUIRE_TRUE(route.IsTracked());
    }

    const auto firstRoute = coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = 0, .position = 0});
    const auto firstBefore = coordinator.Snapshot(firstRoute);
    const auto unreservable = coordinator.ReserveRoute({.controllerSlot = 7, .parameterSlot = 9999, .position = 3});
    REQUIRE_TRUE(!unreservable.IsTracked());
    REQUIRE_TRUE(!coordinator.BeginInput(unreservable, 99).IsPublished());

    synth::MessageInBus bus(nullptr, 4);
    synth::EncoderMidiInConfig config;
    config.mode = synth::EncoderMode::Absolute;
    config.turns.push_back({.control = {.channel = 1, .cc = 9}, .slotIx = 9999, .position = 3});
    synth::EncoderMidiInProcessor processor(config, &bus, &coordinator, 7);
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);
    processor.Process(synth::BasicMidi::CC(1, 1, 9, 99));

    synth::MessageIn message;
    REQUIRE_TRUE(!bus.Pop(message, 1));
    REQUIRE_TRUE(thru.count == 0);
    const auto firstAfter = coordinator.Snapshot(firstRoute);
    REQUIRE_TRUE(firstAfter == firstBefore);
}

TEST_CASE(midi_encoder_input_absolute_publishes_matching_epoch_and_rolls_back_rejected_push) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 5, .parameterSlot = 4, .position = 6});
    const auto prior = coordinator.BeginInput(route, 22);
    synth::MessageInBus bus(nullptr, 1);
    REQUIRE_TRUE(bus.Push(synth::MessageIn::Clock(0)));

    synth::EncoderMidiInConfig config;
    config.mode = synth::EncoderMode::Absolute;
    config.turns.push_back({.control = {.channel = 3, .cc = 7}, .slotIx = 4, .position = 6});
    synth::EncoderMidiInProcessor processor(config, &bus, &coordinator, 5);
    processor.SetTimestampProvider([] { return 101; });
    processor.Process(synth::BasicMidi::CC(9999, 3, 7, 64));

    auto state = coordinator.Snapshot(route);
    REQUIRE_TRUE(state.has_value());
    REQUIRE_TRUE(state->epoch == prior.Epoch());
    REQUIRE_TRUE(state->receivedValue == 22);
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 101));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::Clock);

    processor.Process(synth::BasicMidi::CC(9999, 3, 7, 64));
    REQUIRE_TRUE(bus.Pop(message, 101));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(message.absoluteEpoch != 0);
    REQUIRE_NEAR(message.value, 0.5f, 0.000001f);
    state = coordinator.Snapshot(route);
    REQUIRE_TRUE(state.has_value());
    REQUIRE_TRUE(state->pending);
    REQUIRE_TRUE(state->epoch == message.absoluteEpoch);
    REQUIRE_TRUE(state->receivedValue == 64);
}

TEST_CASE(midi_encoder_input_relative_modes_do_not_allocate_or_change_absolute_state) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 6, .parameterSlot = 2, .position = 3});
    const auto prior = coordinator.BeginInput(route, 11);

    for (const auto mode : {synth::EncoderMode::Signed7Bit, synth::EncoderMode::DirectionOnly}) {
        synth::MessageInBus bus(nullptr, 2);
        synth::EncoderMidiInConfig config;
        config.mode = mode;
        config.turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 2, .position = 3});
        synth::EncoderMidiInProcessor processor(config, &bus, &coordinator, 6);
        processor.Process(synth::BasicMidi::CC(1, 0, 1, 127));

        synth::MessageIn message;
        REQUIRE_TRUE(bus.Pop(message, 1));
        REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
        REQUIRE_TRUE(message.absoluteEpoch == 0);
        const auto state = coordinator.Snapshot(route);
        REQUIRE_TRUE(state.has_value());
        REQUIRE_TRUE(state->pending);
        REQUIRE_TRUE(state->epoch == prior.Epoch());
        REQUIRE_TRUE(state->receivedValue == 11);
    }

    const auto next = coordinator.BeginInput(route, 12);
    REQUIRE_TRUE(next.Epoch() == prior.Epoch() + 1);
}

TEST_CASE(midi_encoder_input_absolute_maps_raw_positions_independent_of_turn_step) {
    constexpr std::array rawValues{std::uint8_t{0}, std::uint8_t{64}, std::uint8_t{127}};
    constexpr std::array normalizedValues{0.0f, 0.5f, 1.0f};

    for (const float turnStep : {0.01f, 0.75f}) {
        synth::MessageInBus bus(nullptr, 16);
        synth::EncoderMidiInConfig config;
        config.mode = synth::EncoderMode::Absolute;
        config.turnStep = turnStep;
        config.turns.push_back({.control = {.channel = 3, .cc = 7}, .slotIx = 4, .position = 6});
        synth::EncoderMidiInProcessor processor(config, &bus);
        std::uint64_t nextTimestamp = 101;
        processor.SetTimestampProvider([&nextTimestamp] { return nextTimestamp++; });

        for (std::size_t ix = 0; ix < rawValues.size(); ++ix) {
            processor.Process(synth::BasicMidi::CC(9999, 3, 7, rawValues[ix]));

            synth::MessageIn message;
            REQUIRE_TRUE(bus.Pop(message, 103));
            REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
            REQUIRE_TRUE(message.timestamp == 101 + ix);
            REQUIRE_TRUE(message.slotIx == 4);
            REQUIRE_TRUE(message.position == 6);
            REQUIRE_NEAR(message.value, normalizedValues[ix], 0.000001f);
        }

        synth::MessageIn extra;
        REQUIRE_TRUE(!bus.Pop(extra, 103));
    }
}

TEST_CASE(absolute_encoder_curve_round_trips_every_midi_byte_through_real_processors) {
    synth::MessageInBus bus(nullptr, 128);
    synth::EncoderMidiInConfig input;
    input.mode = synth::EncoderMode::Absolute;
    input.turns.push_back({.control = {.channel = 3, .cc = 7}, .slotIx = 0, .position = 0});
    synth::EncoderMidiInProcessor inputProcessor(input, &bus);

    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto outputProcessor = MakeEncoderOutput(
        synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute, &sender, &ui);

    for (int raw = 0; raw <= 127; ++raw) {
        inputProcessor.Process(synth::BasicMidi::CC(0, 3, 7, static_cast<std::uint8_t>(raw)));
        synth::MessageIn message;
        REQUIRE_TRUE(bus.Pop(message, 0));
        REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
        if (raw == 64) {
            REQUIRE_NEAR(message.value, 0.5f, 0.000001f);
        }

        PublishEncoderCell(ui, 0.0f, message.value, 0);
        const std::size_t beforeOutput = sink.sent.size();
        outputProcessor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink, beforeOutput) == 1);
        REQUIRE_TRUE(LastPositionValue(sink, beforeOutput) == raw);
    }

    sender.Stop();
}

TEST_CASE(absolute_encoder_output_uses_inverse_curve_before_midi_byte_debounce) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    PublishEncoderCell(ui, 0.0f, 0.6f, 0);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto processor = MakeEncoderOutput(
        synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute, &sender, &ui);

    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(LastPositionValue(sink) == 77);
    const std::size_t afterFirst = sink.sent.size();

    PublishEncoderCell(ui, 0.0f, 0.6005f, 0);
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == afterFirst);
    sender.Stop();
}

TEST_CASE(midi_encoder_input_absolute_preserves_mapped_push_and_thru_boundaries) {
    synth::MessageInBus bus(nullptr, 16);
    synth::EncoderMidiInConfig config;
    config.mode = synth::EncoderMode::Absolute;
    config.turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 2, .position = 3});
    config.pushes.push_back({.control = {.channel = 1, .cc = 1}, .slotIx = 2, .position = 3});
    synth::EncoderMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 77; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::CC(1, 0, 1, 0));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(message.timestamp == 77);
    REQUIRE_TRUE(message.slotIx == 2);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_NEAR(message.value, 0.0f, 0.000001f);
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(1, 1, 1, 127));
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamPush);
    REQUIRE_TRUE(message.timestamp == 77);
    REQUIRE_TRUE(message.slotIx == 2);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(1, 1, 1, 0));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(1, 9, 9, 64));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.Channel() == 9);
    REQUIRE_TRUE(thru.last.GetCC() == 9);
    REQUIRE_TRUE(thru.last.GetValue() == 64);
}

TEST_CASE(midi_analog_input_maps_gestures_scene_blend_timestamps_and_thru) {
    synth::MessageInBus bus(nullptr, 16);
    synth::AnalogMidiInConfig config;
    config.gestures.push_back({.control = {.channel = 2, .cc = 3}, .gestureIx = 3});
    config.gestures.push_back({.control = {.channel = 2, .cc = 4}, .gestureIx = 4});
    config.sceneBlend = synth::MidiControlAddress{.channel = 2, .cc = 16};
    synth::AnalogMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 42; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::CC(999, 2, 3, 64));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 42));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.timestamp == 42);
    REQUIRE_TRUE(message.gestureIx == 3);
    REQUIRE_NEAR(message.value, 64.0f / 127.0f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(999, 2, 4, 0));
    REQUIRE_TRUE(bus.Pop(message, 42));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.gestureIx == 4);
    REQUIRE_NEAR(message.value, 0.0f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(999, 2, 16, 127));
    REQUIRE_TRUE(bus.Pop(message, 42));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetSceneBlend);
    REQUIRE_TRUE(message.timestamp == 42);
    REQUIRE_NEAR(message.value, 1.0f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(999, 2, 99, 1));
    REQUIRE_TRUE(!bus.Pop(message, 42));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.Channel() == 2);
    REQUIRE_TRUE(thru.last.GetCC() == 99);

    processor.Process(synth::BasicMidi::Clock(999));
    REQUIRE_TRUE(thru.count == 2);
    REQUIRE_TRUE(thru.last.Status() == synth::BasicMidi::kStatusClock);
}

TEST_CASE(midi_system_button_input_maps_press_release_timestamps_and_thru) {
    synth::MessageInBus bus(nullptr, 16);
    synth::SystemButtonMidiInConfig config;
    config.associations.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 32},
        .press = synth::MessageIn::SetReset(1, true),
        .release = synth::MessageIn::SetReset(1, false),
    });
    config.associations.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 0},
        .press = synth::MessageIn::SetGestureSelect(1, 0, true),
        .release = synth::MessageIn::SetGestureSelect(1, 0, false),
    });
    config.associations.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 33},
        .press = synth::MessageIn::ToggleReset(1),
    });
    synth::SystemButtonMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 77; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::CC(999, 5, 32, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.timestamp == 77);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(message.boolValue);

    processor.Process(synth::BasicMidi::CC(999, 5, 32, 0));
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.timestamp == 77);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    processor.Process(synth::BasicMidi::CC(999, 5, 0, 0));
    REQUIRE_TRUE(bus.Pop(message, 77));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.timestamp == 77);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    processor.Process(synth::BasicMidi::CC(999, 5, 33, 0));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 0);

    processor.Process(synth::BasicMidi::CC(999, 5, 99, 1));
    REQUIRE_TRUE(!bus.Pop(message, 77));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.Channel() == 5);
    REQUIRE_TRUE(thru.last.GetCC() == 99);

    processor.Process(synth::BasicMidi::Clock(999));
    REQUIRE_TRUE(thru.count == 2);
    REQUIRE_TRUE(thru.last.Status() == synth::BasicMidi::kStatusClock);
}

TEST_CASE(midi_system_button_input_matches_typed_note_press_and_both_release_forms) {
    synth::MessageInBus bus(nullptr, 16);
    synth::SystemButtonMidiInConfig config;
    config.associations.push_back({
        .control = synth::MidiControlAddress{
            .channel = 1,
            .cc = 60,
            .type = synth::MidiControlType::Note,
        },
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    });
    synth::SystemButtonMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 88; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::Note(1, 1, 60, 100));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(message.boolValue);

    const synth::BasicMidi zeroVelocityNoteOn(
        1, std::vector<std::uint8_t>{static_cast<std::uint8_t>(synth::BasicMidi::kStatusNote | 1), 60, 0});
    processor.Process(zeroVelocityNoteOn);
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    const synth::BasicMidi nonzeroVelocityNoteOff(
        2,
        std::vector<std::uint8_t>{static_cast<std::uint8_t>(synth::BasicMidi::kStatusNoteOff | 1), 60, 77});
    processor.Process(nonzeroVelocityNoteOff);
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    processor.Process(synth::BasicMidi::CC(3, 1, 60, 127));
    REQUIRE_TRUE(!bus.Pop(message, 88));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.IsCC());
}

TEST_CASE(midi_system_button_input_matches_launchpad_positions_generically) {
    synth::MessageInBus bus(nullptr, 16);
    synth::SystemButtonMidiInConfig config;
    config.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadX,
            .x = 0,
            .y = 7,
        },
        .press = synth::MessageIn::SceneSelect(0, 0),
    });
    config.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadProMk3,
            .x = 0,
            .y = 0,
        },
        .press = synth::MessageIn::SetGestureSelect(0, 0, true),
        .release = synth::MessageIn::SetGestureSelect(0, 0, false),
    });
    config.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadMiniMk3,
            .x = 1,
            .y = 1,
        },
        .press = synth::MessageIn::SetGestureSelect(0, 1, true),
        .release = synth::MessageIn::SetGestureSelect(0, 1, false),
    });
    config.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadX,
            .x = 0,
            .y = -1,
        },
        .press = synth::MessageIn::SelectParamBank(0, 0, 2),
        .release = synth::MessageIn::SelectParamBank(0, 0, 0),
    });
    config.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadX,
            .x = -1,
            .y = 0,
        },
        .press = synth::MessageIn::SceneSelect(0, 7),
    });

    synth::SystemButtonMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 55; });
    CountingMidiInProcessor thru;
    processor.SetThru(&thru);

    processor.Process(synth::BasicMidi::Note(9, 0, 11, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 0);
    REQUIRE_TRUE(message.timestamp == 55);

    processor.Process(synth::BasicMidi::Note(9, 0, 11, 0));
    REQUIRE_TRUE(!bus.Pop(message, 55));
    REQUIRE_TRUE(thru.count == 0);

    const auto gestureNote = synth::LaunchpadPositionToNote(synth::LaunchpadController::LaunchpadProMk3, 0, 0);
    REQUIRE_TRUE(gestureNote.has_value());
    processor.Process(synth::BasicMidi::NoteOff(9, 0, *gestureNote));
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    const auto miniNote = synth::LaunchpadPositionToNote(synth::LaunchpadController::LaunchpadMiniMk3, 1, 1);
    REQUIRE_TRUE(miniNote.has_value());
    processor.Process(synth::BasicMidi::Note(9, 0, *miniNote, 0));
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.gestureIx == 1);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    const auto edgeCc = synth::LaunchpadPositionToNote(synth::LaunchpadController::LaunchpadX, 0, -1);
    REQUIRE_TRUE(edgeCc.has_value());
    processor.Process(synth::BasicMidi::CC(9, 0, *edgeCc, 127));
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(message.bankIx == 2);

    processor.Process(synth::BasicMidi::CC(9, 0, *edgeCc, 0));
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(message.bankIx == 0);

    synth::SystemButtonMidiInConfig orderedConfig;
    orderedConfig.associations.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadX,
            .x = 0,
            .y = -1,
        },
        .press = synth::MessageIn::SceneSelect(0, 4),
    });
    orderedConfig.associations.push_back({
        .control = synth::MidiControlAddress{.channel = 0, .cc = *edgeCc},
        .press = synth::MessageIn::SceneSelect(0, 5),
    });
    processor.SetConfig(orderedConfig);
    processor.Process(synth::BasicMidi::CC(9, 0, *edgeCc, 127));
    REQUIRE_TRUE(bus.Pop(message, 55));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 4);

    processor.Process(synth::BasicMidi::Note(9, 0, 12, 127));
    REQUIRE_TRUE(!bus.Pop(message, 55));
    REQUIRE_TRUE(thru.count == 1);
    REQUIRE_TRUE(thru.last.Status() == synth::BasicMidi::kStatusNote);
    REQUIRE_TRUE(thru.last.GetNote() == 12);

    processor.Process(synth::BasicMidi::CC(9, 0, 12, 127));
    REQUIRE_TRUE(!bus.Pop(message, 55));
    REQUIRE_TRUE(thru.count == 2);
    REQUIRE_TRUE(thru.last.IsCC());
    REQUIRE_TRUE(thru.last.GetCC() == 12);

    const auto proOnlyNote = synth::LaunchpadPositionToNote(synth::LaunchpadController::LaunchpadProMk3, -1, 0);
    REQUIRE_TRUE(proOnlyNote.has_value());
    processor.Process(synth::BasicMidi::Note(9, 0, *proOnlyNote, 127));
    REQUIRE_TRUE(!bus.Pop(message, 55));
    REQUIRE_TRUE(thru.count == 3);
    REQUIRE_TRUE(thru.last.Status() == synth::BasicMidi::kStatusNote);
    REQUIRE_TRUE(thru.last.GetNote() == *proOnlyNote);
}

TEST_CASE(midi_encoder_default_presets_map_row_major_and_trim) {
    synth::EncoderMidiInConfig twister = synth::EncoderMidiInConfig::TwisterDefault(2);
    REQUIRE_TRUE(twister.mode == synth::EncoderMode::Signed7Bit);
    REQUIRE_NEAR(twister.turnStep, 1.0f / 128.0f, 0.000001f);
    REQUIRE_TRUE(twister.turns.size() == 16);
    REQUIRE_TRUE(twister.pushes.size() == 16);
    REQUIRE_TRUE(twister.turns.front().control.channel == 0);
    REQUIRE_TRUE(twister.turns.front().control.cc == 0);
    REQUIRE_TRUE(twister.turns.front().slotIx == 2);
    REQUIRE_TRUE(twister.turns.front().position == 0);
    REQUIRE_TRUE(twister.turns.back().control.channel == 0);
    REQUIRE_TRUE(twister.turns.back().control.cc == 15);
    REQUIRE_TRUE(twister.turns.back().position == 15);
    REQUIRE_TRUE(twister.pushes.front().control.channel == 1);
    REQUIRE_TRUE(twister.pushes.back().control.cc == 15);

    synth::EncoderMidiInConfig wrld = synth::EncoderMidiInConfig::WrldBldrDefault(3);
    REQUIRE_TRUE(wrld.turns.front().control.channel == 0);
    REQUIRE_TRUE(wrld.pushes.front().control.channel == 1);
    REQUIRE_TRUE(wrld.turns.back().slotIx == 3);
    REQUIRE_TRUE(wrld.turns.back().position == 15);

    wrld.KeepFirstPositions(3);
    REQUIRE_TRUE(wrld.turns.size() == 3);
    REQUIRE_TRUE(wrld.pushes.size() == 3);
    REQUIRE_TRUE(wrld.turns.back().control.cc == 2);
}

TEST_CASE(launchpad_position_helpers_match_smart_grid_mapping) {
    using synth::LaunchpadController;

    REQUIRE_TRUE(synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadX, 0, 7));
    REQUIRE_TRUE(synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadX, 8, -1));
    REQUIRE_TRUE(!synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadX, -1, 0));
    REQUIRE_TRUE(synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadMiniMk3, 8, 0));
    REQUIRE_TRUE(!synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadMiniMk3, -1, 0));
    REQUIRE_TRUE(synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadProMk3, -1, 0));
    REQUIRE_TRUE(synth::LaunchpadShapeSupports(LaunchpadController::LaunchpadProMk3, 0, 9));

    auto note = synth::LaunchpadPositionToNote(LaunchpadController::LaunchpadX, 0, 7);
    REQUIRE_TRUE(note.has_value());
    REQUIRE_TRUE(*note == 11);
    note = synth::LaunchpadPositionToNote(LaunchpadController::LaunchpadX, 0, -1);
    REQUIRE_TRUE(note.has_value());
    REQUIRE_TRUE(*note == 91);
    note = synth::LaunchpadPositionToNote(LaunchpadController::LaunchpadX, -1, 0);
    REQUIRE_TRUE(!note.has_value());

    auto position = synth::LaunchpadNoteToPosition(LaunchpadController::LaunchpadX, 11);
    REQUIRE_TRUE(position.has_value());
    REQUIRE_TRUE(position->controller == LaunchpadController::LaunchpadX);
    REQUIRE_TRUE(position->x == 0);
    REQUIRE_TRUE(position->y == 7);

    note = synth::LaunchpadPositionToNote(LaunchpadController::LaunchpadProMk3, -1, 8);
    REQUIRE_TRUE(note.has_value());
    REQUIRE_TRUE(*note == 100);
    position = synth::LaunchpadNoteToPosition(LaunchpadController::LaunchpadProMk3, *note);
    REQUIRE_TRUE(position.has_value());
    REQUIRE_TRUE(position->controller == LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(position->x == -1);
    REQUIRE_TRUE(position->y == 8);
    REQUIRE_TRUE(synth::LaunchpadNoteToPosition(LaunchpadController::LaunchpadProMk3, 110) == std::nullopt);

    REQUIRE_TRUE(synth::LaunchpadProductByte(LaunchpadController::LaunchpadX) == 0x0C);
    REQUIRE_TRUE(synth::LaunchpadProductByte(LaunchpadController::LaunchpadMiniMk3) == 0x0D);
    REQUIRE_TRUE(synth::LaunchpadProductByte(LaunchpadController::LaunchpadProMk3) == 0x0E);
}

TEST_CASE(midi_encoder_input_supports_incomplete_and_multi_slot_maps) {
    synth::MessageInBus bus(nullptr, 16);
    synth::EncoderMidiInConfig config;
    config.turnStep = 0.5f;
    config.turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 0, .position = 2});
    config.turns.push_back({.control = {.channel = 0, .cc = 7}, .slotIx = 4, .position = 3});
    synth::EncoderMidiInProcessor processor(config, &bus);
    processor.SetTimestampProvider([] { return 123; });

    processor.Process(synth::BasicMidi::CC(1, 0, 1, 65));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 123));
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.position == 2);
    REQUIRE_NEAR(message.delta, 0.5f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(1, 0, 7, 62));
    REQUIRE_TRUE(bus.Pop(message, 123));
    REQUIRE_TRUE(message.slotIx == 4);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_NEAR(message.delta, -1.0f, 0.000001f);

    processor.Process(synth::BasicMidi::CC(1, 0, 2, 65));
    REQUIRE_TRUE(!bus.Pop(message, 123));
}

TEST_CASE(system_message_output_info_reports_colors_and_on_state) {
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 4, 3);
    ui.sceneCapacity = 2;
    ui.leftScene.store(0);
    ui.rightScene.store(1);
    ui.sceneBlend.store(0.25f);
    ui.resetHeld.store(true);
    ui.banks[0].connected.store(true);
    ui.banks[0].selected.store(true);
    ui.banks[0].bankColor.Store(synth::Color::Green);
    ui.banks[1].connected.store(true);
    ui.banks[1].selected.store(false);
    ui.banks[1].bankColor.Store(synth::Color::Blue);
    for (std::size_t gestureIx = 0; gestureIx < 4; ++gestureIx) {
        ui.gestures.connected[gestureIx].store(true);
        ui.gestures.selected[gestureIx].store(false);
    }
    ui.gestures.selected[0].store(true);
    ui.gestures.bankAffectingCount[1].store(1);
    ui.gestures.bankAffectingMask[1].store(1u << 0u);
    ui.gestures.bankAffectingCount[2].store(2);
    ui.gestures.bankAffectingMask[2].store((1u << 0u) | (1u << 1u));

    synth::SystemMessageOutputInfo info(&ui);
    synth::SystemMessageOutputState state = info.Evaluate(synth::MessageIn::SelectParamBank(0, 0, 0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Green);

    state = info.Evaluate(synth::MessageIn::SelectParamBank(0, 0, 1));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Blue.AdjustBrightness(0.35f));

    state = info.Evaluate(synth::MessageIn::SelectParamBank(0, 0, 99));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Off);

    const auto nextState = info.Evaluate(synth::MessageIn::NextParamBank(0, 2));
    REQUIRE_TRUE(nextState.color == synth::Color::Off);
    REQUIRE_TRUE(!nextState.isOn);

    const auto prevState = info.Evaluate(synth::MessageIn::PrevParamBank(0, 3));
    REQUIRE_TRUE(prevState.color == synth::Color::Off);
    REQUIRE_TRUE(!prevState.isOn);

    state = info.Evaluate(synth::MessageIn::ToggleReset(0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::White);
    ui.resetHeld.store(false);
    state = info.Evaluate(synth::MessageIn::ToggleReset(0));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Grey);

    ui.randomHeld.store(true);
    state = info.Evaluate(synth::MessageIn::ToggleRandom(0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::White);
    ui.randomHeld.store(false);
    state = info.Evaluate(synth::MessageIn::ToggleRandom(0));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Grey);

    ui.randomModHeld.store(true);
    state = info.Evaluate(synth::MessageIn::ToggleRandomMod(0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::White);
    ui.randomModHeld.store(false);
    state = info.Evaluate(synth::MessageIn::ToggleRandomMod(0));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Grey);

    state = info.Evaluate(synth::MessageIn::SceneSelect(0, 0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Orange.AdjustBrightness(0.5f + 0.5f * (1.0f - 0.25f)));
    state = info.Evaluate(synth::MessageIn::SceneSelect(0, 1));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Green.AdjustBrightness(0.5f + 0.5f * 0.25f));
    ui.leftScene.store(1);
    ui.rightScene.store(1);
    state = info.Evaluate(synth::MessageIn::SceneSelect(0, 1));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Orange.AdjustBrightness(0.5f + 0.5f * (1.0f - 0.25f)));
    state = info.Evaluate(synth::MessageIn::SceneSelect(0, 99));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Off);

    state = info.Evaluate(synth::MessageIn::ToggleGestureSelect(0, 0));
    REQUIRE_TRUE(state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::White);
    state = info.Evaluate(synth::MessageIn::ToggleGestureSelect(0, 1));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Green);
    state = info.Evaluate(synth::MessageIn::ToggleGestureSelect(0, 2));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::White);
    state = info.Evaluate(synth::MessageIn::ToggleGestureSelect(0, 3));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Grey.AdjustBrightness(0.5f));
    state = info.Evaluate(synth::MessageIn::ToggleGestureSelect(0, 99));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Off);

    state = info.Evaluate(synth::MessageIn::ParamPush(0, 0, 0));
    REQUIRE_TRUE(!state.isOn);
    REQUIRE_TRUE(state.color == synth::Color::Off);
}

TEST_CASE(relative_bank_messages_json_round_trip_with_exact_type_names_and_slots) {
    const std::array messages{
        synth::MessageIn::NextParamBank(17, 2),
        synth::MessageIn::PrevParamBank(19, 3),
    };
    const std::array<std::string_view, 2> typeNames{"nextParamBank", "prevParamBank"};

    for (std::size_t ix = 0; ix < messages.size(); ++ix) {
        synth::JsonArena arena(4096);
        const synth::JSON json = synth::ToJSON(arena, messages[ix]);
        REQUIRE_TRUE(!arena.Failed());
        REQUIRE_TRUE(std::string_view(json.Get("type").StringValue()) == typeNames[ix]);
        REQUIRE_TRUE(json.Get("slotIx").IntegerValue() == static_cast<int64_t>(messages[ix].slotIx));

        synth::MessageIn loaded;
        REQUIRE_TRUE(synth::FromJSON(json, loaded));
        REQUIRE_TRUE(loaded.type == messages[ix].type);
        REQUIRE_TRUE(loaded.slotIx == messages[ix].slotIx);
    }
}

TEST_CASE(system_output_processors_debounce_reset_and_render_cc_and_wrld_bldr) {
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 0, 0);
    ui.resetHeld.store(true);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    synth::SystemCcMidiOutConfig ccConfig;
    ccConfig.associations.push_back({
        .control = {.channel = 5, .cc = 32},
        .message = synth::MessageIn::ToggleReset(0),
    });
    synth::SystemCcMidiOutProcessor ccProcessor(ccConfig, &sender, &ui);
    ccProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].Status() == synth::BasicMidi::kStatusCC);
    REQUIRE_TRUE(sink.sent[0].Channel() == 5);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 32);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 127);

    ccProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);

    ui.resetHeld.store(false);
    ccProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[1].GetValue() == 0);

    ccProcessor.Reset();
    ccProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 3);
    REQUIRE_TRUE(sink.sent[2].GetValue() == 0);

    synth::WrldBldrSystemMidiOutConfig wrldConfig;
    wrldConfig.associations.push_back({
        .position = {.channel = 5, .x = 0, .y = 4},
        .message = synth::MessageIn::ToggleReset(0),
    });
    synth::WrldBldrSystemMidiOutProcessor wrldProcessor(wrldConfig, &sender, &ui);
    ui.resetHeld.store(true);
    wrldProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 4);
    REQUIRE_TRUE(sink.sent[3].IsSysEx());
    REQUIRE_TRUE(sink.sent[3].raw[8] == 5);
    REQUIRE_TRUE(sink.sent[3].raw[9] == synth::WrldBldrPositionToCC(0, 4));
    REQUIRE_TRUE(sink.sent[3].raw[10] == synth::Color::White.r / 2);
    REQUIRE_TRUE(sink.sent[3].raw[11] == synth::Color::White.g / 2);
    REQUIRE_TRUE(sink.sent[3].raw[12] == synth::Color::White.b / 2);

    wrldProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 4);

    wrldProcessor.Reset();
    wrldProcessor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 5);
    REQUIRE_TRUE(sink.sent[4].IsSysEx());
}

TEST_CASE(launchpad_color_sysex_uses_controller_product_and_rgb_note) {
    auto midi = synth::LaunchpadColorSysex(7, synth::LaunchpadController::LaunchpadX, 0, 7, synth::Color::White);
    REQUIRE_TRUE(midi.IsSysEx());
    REQUIRE_TRUE(midi.timestamp == 7);
    REQUIRE_TRUE(midi.raw[0] == 0xF0);
    REQUIRE_TRUE(midi.raw[1] == 0x00);
    REQUIRE_TRUE(midi.raw[2] == 0x20);
    REQUIRE_TRUE(midi.raw[3] == 0x29);
    REQUIRE_TRUE(midi.raw[4] == 0x02);
    REQUIRE_TRUE(midi.raw[5] == 0x0C);
    REQUIRE_TRUE(midi.raw[6] == 0x03);
    REQUIRE_TRUE(midi.raw[7] == 0x03);
    REQUIRE_TRUE(midi.raw[8] == 11);
    REQUIRE_TRUE(midi.raw[9] == 127);
    REQUIRE_TRUE(midi.raw[10] == 127);
    REQUIRE_TRUE(midi.raw[11] == 127);

    midi = synth::LaunchpadColorSysex(7, synth::LaunchpadController::LaunchpadMiniMk3, 0, 7,
                                      synth::Color::Orange);
    REQUIRE_TRUE(midi.raw[5] == 0x0D);
    REQUIRE_TRUE(midi.raw[9] == synth::Color::Orange.r / 2);
    REQUIRE_TRUE(midi.raw[10] == synth::Color::Orange.g / 2);
    REQUIRE_TRUE(midi.raw[11] == synth::Color::Orange.b / 2);

    midi = synth::LaunchpadColorSysex(7, synth::LaunchpadController::LaunchpadProMk3, -1, 0,
                                      synth::Color::Green);
    REQUIRE_TRUE(midi.raw[5] == 0x0E);
}

TEST_CASE(launchpad_output_processor_debounces_reset_and_uses_system_info) {
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 0, 0);
    ui.resetHeld.store(true);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    synth::LaunchpadGridMidiOutConfig config;
    config.associations.push_back({
        .position = {.controller = synth::LaunchpadController::LaunchpadX, .x = 0, .y = 7},
        .message = synth::MessageIn::ToggleReset(0),
    });
    synth::LaunchpadGridMidiOutProcessor processor(config, &sender, &ui);
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].raw[5] == 0x0C);
    REQUIRE_TRUE(sink.sent[0].raw[9] == 127);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 1);

    ui.resetHeld.store(false);
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[1].raw[9] == synth::Color::Grey.r / 2);

    processor.Reset();
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 3);
}

TEST_CASE(midi_controller_profile_builds_chained_input_processors) {
    synth::MessageInBus bus(nullptr, 16);
    synth::MidiControllerProfileConfig config;
    synth::EncoderMidiInConfig encoder;
    encoder.turnStep = 0.25f;
    encoder.turns.push_back({.control = {.channel = 0, .cc = 1}, .slotIx = 2, .position = 3});
    config.encoderInput = encoder;
    synth::AnalogMidiInConfig analog;
    analog.gestures.push_back({.control = {.channel = 2, .cc = 4}, .gestureIx = 5});
    config.analogInput = analog;
    config.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 32},
        .wrldBldrPosition = synth::WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 4},
        .press = synth::MessageIn::ToggleReset(0),
        .feedback = synth::MessageIn::ToggleReset(0),
    });

    synth::MidiControllerProfileResult profile =
        synth::CreateMidiControllerProfile(config, &bus, nullptr, nullptr, [] { return 88; });
    REQUIRE_TRUE(profile.input != nullptr);
    REQUIRE_TRUE(profile.inputThru.size() == 3);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(profile.inputThru[2].get()) != nullptr);

    profile.input->Process(synth::BasicMidi::CC(1, 0, 1, 65));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.timestamp == 88);
    REQUIRE_TRUE(message.slotIx == 2);
    REQUIRE_TRUE(message.position == 3);
    REQUIRE_NEAR(message.delta, 0.25f, 0.000001f);

    profile.input->Process(synth::BasicMidi::CC(1, 2, 4, 127));
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.timestamp == 88);
    REQUIRE_TRUE(message.gestureIx == 5);
    REQUIRE_NEAR(message.value, 1.0f, 0.000001f);

    profile.input->Process(synth::BasicMidi::CC(1, 5, 32, 1));
    REQUIRE_TRUE(bus.Pop(message, 88));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.timestamp == 88);
}

TEST_CASE(midi_controller_profile_threads_absolute_route_identity_and_mode) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0, 0);
    PublishEncoderCell(ui, /*displayValue=*/0.9f, /*rawValue=*/0.25f,
                       /*processedEpoch=*/0);

    synth::MessageInBus bus(nullptr, 16);
    synth::AbsoluteFeedbackCoordinator coordinator;
    synth::MidiControllerProfileConfig config;
    config.encoderInput = synth::EncoderMidiInConfig{
        .mode = synth::EncoderMode::Absolute,
        .turns = {{.control = {.channel = 0, .cc = 7}, .slotIx = 0, .position = 0}},
    };
    config.encoderOutput = synth::EncoderMidiOutConfig{
        .protocol = synth::EncoderMidiOutProtocol::Twister,
        .mappings = {{.slotIx = 0, .position = 0, .cc = 7}},
    };

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(3, &sink);
    sender.Start();
    synth::MidiControllerProfileResult profile = synth::CreateMidiControllerProfile(
        config, &bus, &sender, &ui, [] { return 101; }, /*sinkIx=*/3,
        &coordinator, /*controllerSlot=*/6);

    profile.input->Process(synth::BasicMidi::CC(0, 0, 7, 32));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 101));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamSetAbsolute);
    REQUIRE_TRUE(message.absoluteEpoch != 0);
    const auto route = coordinator.ReserveRoute({.controllerSlot = 6, .parameterSlot = 0, .position = 0});
    const auto expectation = coordinator.Snapshot(route);
    REQUIRE_TRUE(expectation.has_value());
    REQUIRE_TRUE(expectation->pending);
    REQUIRE_TRUE(expectation->epoch == message.absoluteEpoch);
    REQUIRE_TRUE(expectation->receivedValue == 32);

    // A second controller addressing the same cell retains independent route
    // state while allocating from the coordinator's one global epoch stream.
    auto secondProfile = synth::CreateMidiControllerProfile(
        config, &bus, &sender, &ui, [] { return 101; }, /*sinkIx=*/3,
        &coordinator, /*controllerSlot=*/7);
    secondProfile.input->Process(synth::BasicMidi::CC(0, 0, 7, 64));
    synth::MessageIn secondMessage;
    REQUIRE_TRUE(bus.Pop(secondMessage, 101));
    REQUIRE_TRUE(secondMessage.absoluteEpoch > message.absoluteEpoch);
    const auto secondRoute =
        coordinator.ReserveRoute({.controllerSlot = 7, .parameterSlot = 0, .position = 0});
    const auto secondExpectation = coordinator.Snapshot(secondRoute);
    REQUIRE_TRUE(secondExpectation.has_value());
    REQUIRE_TRUE(secondExpectation->pending);
    REQUIRE_TRUE(secondExpectation->epoch == secondMessage.absoluteEpoch);
    REQUIRE_TRUE(secondExpectation->receivedValue == 64);

    profile.outputs.front()->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    sender.Stop();
    REQUIRE_TRUE(CountPositionMessages(sink) == 0);
}

TEST_CASE(midi_controller_profile_keeps_relative_and_output_only_feedback_uncoordinated) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0, 0);
    PublishEncoderCell(ui, /*displayValue=*/0.75f, /*rawValue=*/0.25f,
                       /*processedEpoch=*/0);

    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto relativeRoute =
        coordinator.ReserveRoute({.controllerSlot = 4, .parameterSlot = 0, .position = 0});
    synth::MessageInBus bus(nullptr, 16);
    synth::MidiControllerProfileConfig relativeConfig;
    relativeConfig.encoderInput = synth::EncoderMidiInConfig{
        .mode = synth::EncoderMode::DirectionOnly,
        .turns = {{.control = {.channel = 0, .cc = 9}, .slotIx = 0, .position = 0}},
    };
    relativeConfig.encoderOutput = synth::EncoderMidiOutConfig{
        .protocol = synth::EncoderMidiOutProtocol::Twister,
        .mappings = {{.slotIx = 0, .position = 0, .cc = 9}},
    };

    FakeMidiSink relativeSink;
    synth::MidiSender relativeSender;
    relativeSender.SetSink(0, &relativeSink);
    relativeSender.Start();
    auto relativeProfile = synth::CreateMidiControllerProfile(
        relativeConfig, &bus, &relativeSender, &ui, [] { return 102; }, 0,
        &coordinator, /*controllerSlot=*/4);
    relativeProfile.input->Process(synth::BasicMidi::CC(0, 0, 9, 127));
    synth::MessageIn relativeMessage;
    REQUIRE_TRUE(bus.Pop(relativeMessage, 102));
    REQUIRE_TRUE(relativeMessage.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(relativeMessage.absoluteEpoch == 0);
    const auto untouched = coordinator.Snapshot(relativeRoute);
    REQUIRE_TRUE(untouched.has_value());
    REQUIRE_TRUE(!untouched->pending);
    relativeProfile.outputs.front()->Process();
    REQUIRE_TRUE(relativeSender.FlushForTests(std::chrono::milliseconds(500)));
    relativeSender.Stop();
    REQUIRE_TRUE(LastPositionValue(relativeSink).has_value());
    REQUIRE_TRUE(*LastPositionValue(relativeSink) == 95);

    synth::MidiControllerProfileConfig outputOnlyConfig;
    outputOnlyConfig.encoderOutput = relativeConfig.encoderOutput;
    FakeMidiSink outputOnlySink;
    synth::MidiSender outputOnlySender;
    outputOnlySender.SetSink(0, &outputOnlySink);
    outputOnlySender.Start();
    auto outputOnlyProfile = synth::CreateMidiControllerProfile(
        outputOnlyConfig, nullptr, &outputOnlySender, &ui, {}, 0,
        &coordinator, /*controllerSlot=*/5);
    outputOnlyProfile.outputs.front()->Process();
    REQUIRE_TRUE(outputOnlySender.FlushForTests(std::chrono::milliseconds(500)));
    outputOnlySender.Stop();
    REQUIRE_TRUE(LastPositionValue(outputOnlySink).has_value());
    REQUIRE_TRUE(*LastPositionValue(outputOnlySink) == 95);
}

TEST_CASE(generic_controller_profile_derives_same_address_position_only_output_and_honors_overrides) {
    synth::MidiControllerProfileConfig inputOnly;
    inputOnly.encoderInput = synth::EncoderMidiInConfig{
        .mode = synth::EncoderMode::Signed7Bit,
        .turns = {
            {.control = {.channel = 7, .cc = 74}, .slotIx = 0, .position = 0},
            {.control = {.channel = 12, .cc = 3}, .slotIx = 0, .position = 1},
        },
    };

    struct FactoryCase {
        const char* name;
        synth::MidiControllerProfileConfig config;
        std::optional<synth::MidiProfileKind> kind;
        bool expectGeneric;
    };
    synth::MidiControllerProfileConfig noInput;
    synth::MidiControllerProfileConfig explicitTwister = inputOnly;
    explicitTwister.encoderOutput = synth::EncoderMidiOutConfig{
        .protocol = synth::EncoderMidiOutProtocol::Twister,
        .mappings = {{.slotIx = 0, .position = 0, .cc = 74}},
    };
    const std::array cases{
        FactoryCase{"engine Generic", inputOnly, synth::MidiProfileKind::Generic, true},
        FactoryCase{"legacy omitted kind", inputOnly, std::nullopt, false},
        FactoryCase{"non-Generic kind", inputOnly, synth::MidiProfileKind::WrldBldr, false},
        FactoryCase{"missing encoder input", noInput, synth::MidiProfileKind::Generic, false},
        FactoryCase{"explicit specialized output", explicitTwister, synth::MidiProfileKind::Generic, false},
    };
    for (const FactoryCase& testCase : cases) {
        auto profile = synth::CreateMidiControllerProfile(
            testCase.config, nullptr, nullptr, nullptr, {}, 0, nullptr, 0, testCase.kind);
        const bool hasGeneric = profile.outputs.size() == 1 &&
            dynamic_cast<synth::GenericMidiOutProcessor*>(profile.outputs.front().get()) != nullptr;
        REQUIRE_TRUE(hasGeneric == testCase.expectGeneric);
        if (testCase.config.encoderOutput.has_value()) {
            REQUIRE_TRUE(profile.outputs.size() == 1);
            REQUIRE_TRUE(dynamic_cast<synth::TwisterMidiOutProcessor*>(profile.outputs.front().get()) != nullptr);
        } else if (!testCase.expectGeneric) {
            REQUIRE_TRUE(profile.outputs.empty());
        }
    }

    synth::ParameterManager::UIState ui;
    ui.Configure(1, 2, 1, 0, 0, 0);
    PublishEncoderCellAt(ui, 0, 0, 0.25f, 0.9f, 0);
    PublishEncoderCellAt(ui, 0, 1, 0.75f, 0.1f, 0);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto derived = synth::CreateMidiControllerProfile(
        inputOnly, nullptr, &sender, &ui, {}, 0, nullptr, 0, synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(derived.outputs.size() == 1);
    derived.outputs.front()->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[0].Channel() == 7 && sink.sent[0].GetCC() == 74);
    REQUIRE_TRUE(sink.sent[1].Channel() == 12 && sink.sent[1].GetCC() == 3);
    REQUIRE_TRUE(std::all_of(sink.sent.begin(), sink.sent.end(),
        [](const synth::BasicMidi& midi) { return midi.IsCC(); }));
    const std::size_t afterInitial = sink.sent.size();
    derived.outputs.front()->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == afterInitial);
    sender.Stop();

    FakeMidiSink explicitSink;
    synth::MidiSender explicitSender;
    explicitSender.SetSink(0, &explicitSink);
    explicitSender.Start();
    auto explicitProfile = synth::CreateMidiControllerProfile(
        explicitTwister, nullptr, &explicitSender, &ui, {}, 0, nullptr, 0,
        synth::MidiProfileKind::Generic);
    explicitProfile.outputs.front()->Process();
    REQUIRE_TRUE(explicitSender.FlushForTests(std::chrono::milliseconds(500)));
    explicitSender.Stop();
    REQUIRE_TRUE(explicitSink.sent.size() == 4);
    REQUIRE_TRUE(explicitSink.sent[0].Channel() == 1);
    REQUIRE_TRUE(explicitSink.sent[1].Channel() == 2);
    REQUIRE_TRUE(explicitSink.sent[2].Channel() == 5);
    REQUIRE_TRUE(explicitSink.sent[3].Channel() == 0);
    REQUIRE_TRUE(std::none_of(explicitSink.sent.begin(), explicitSink.sent.end(),
        [](const synth::BasicMidi& midi) { return midi.Channel() == 7 && midi.GetCC() == 74; }));
}

TEST_CASE(generic_absolute_encoder_output_uses_same_address_causal_acknowledgement) {
    synth::EncoderMidiInConfig input{
        .mode = synth::EncoderMode::Absolute,
        .turns = {{.control = {.channel = 7, .cc = 74}, .slotIx = 0, .position = 0}},
    };
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0, 0);
    PublishEncoderCell(ui, 0.9f, 0.5f, 0);
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 2, .parameterSlot = 0, .position = 0});
    const auto exact = coordinator.BeginInput(route, 64);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::GenericMidiOutProcessor processor(input, &sender, &ui, 0, &coordinator, 2);

    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.empty());
    REQUIRE_TRUE(coordinator.Snapshot(route)->pending);

    PublishEncoderCell(ui, 0.9f, 0.5f, exact.Epoch());
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.empty());
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.empty());

    const auto rejected = coordinator.BeginInput(route, 96);
    PublishEncoderCell(ui, 0.9f, 0.25f, rejected.Epoch());
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].IsCC());
    REQUIRE_TRUE(sink.sent[0].Channel() == 7);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 74);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 32);
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);

    const auto disconnected = coordinator.BeginInput(route, 80);
    PublishEncoderCell(ui, 0.9f, 0.0f, disconnected.Epoch(), false);
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[1].Channel() == 7 && sink.sent[1].GetCC() == 74);
    REQUIRE_TRUE(sink.sent[1].GetValue() == 0);
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
}

TEST_CASE(generic_absolute_encoder_output_retries_failed_correction_without_state_mutation) {
    synth::EncoderMidiInConfig input{
        .mode = synth::EncoderMode::Absolute,
        .turns = {{.control = {.channel = 12, .cc = 3}, .slotIx = 0, .position = 0}},
    };
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0, 0);
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 7, .parameterSlot = 0, .position = 0});
    const auto alert = coordinator.BeginInput(route, 96);
    PublishEncoderCell(ui, 0.9f, 0.25f, alert.Epoch());
    FakeMidiSink sink;
    synth::MidiSender sender(1);
    sender.SetSink(0, &sink);
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 9, 9, 9)));
    synth::GenericMidiOutProcessor processor(input, &sender, &ui, 0, &coordinator, 7);

    processor.Process();
    REQUIRE_TRUE(coordinator.Snapshot(route)->pending);
    sender.Start();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    const std::size_t beforeRetry = sink.sent.size();
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == beforeRetry + 1);
    REQUIRE_TRUE(sink.sent.back().Channel() == 12 && sink.sent.back().GetCC() == 3);
    REQUIRE_TRUE(sink.sent.back().GetValue() == 32);
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
}

TEST_CASE(generic_relative_encoder_output_uses_display_value_without_epoch_coordination) {
    for (const synth::EncoderMode mode : {
             synth::EncoderMode::Signed7Bit,
             synth::EncoderMode::DirectionOnly,
         }) {
        synth::EncoderMidiInConfig input{
            .mode = mode,
            .turns = {{.control = {.channel = 12, .cc = 3}, .slotIx = 0, .position = 0}},
        };
        synth::ParameterManager::UIState ui;
        ui.Configure(1, 1, 1, 0, 0, 0);
        PublishEncoderCell(ui, 0.25f, 0.9f, 0);
        synth::AbsoluteFeedbackCoordinator coordinator;
        const auto route = coordinator.ReserveRoute({.controllerSlot = 6, .parameterSlot = 0, .position = 0});
        const auto alert = coordinator.BeginInput(route, 100);
        FakeMidiSink sink;
        synth::MidiSender sender;
        sender.SetSink(0, &sink);
        sender.Start();
        synth::GenericMidiOutProcessor processor(input, &sender, &ui, 0, &coordinator, 6);

        processor.Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(sink.sent.size() == 1);
        REQUIRE_TRUE(sink.sent.back().Channel() == 12 && sink.sent.back().GetCC() == 3);
        REQUIRE_TRUE(sink.sent.back().GetValue() == 32);
        PublishEncoderCell(ui, 0.75f, 0.9f, alert.Epoch());
        processor.Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        sender.Stop();
        REQUIRE_TRUE(sink.sent.size() == 2);
        REQUIRE_TRUE(sink.sent.back().Channel() == 12 && sink.sent.back().GetCC() == 3);
        REQUIRE_TRUE(sink.sent.back().GetValue() == 95);
        const auto untouched = coordinator.Snapshot(route);
        REQUIRE_TRUE(untouched.has_value() && untouched->pending);
        REQUIRE_TRUE(untouched->epoch == alert.Epoch());
        REQUIRE_TRUE(untouched->receivedValue == 100);
    }
}

TEST_CASE(generic_untracked_absolute_encoder_output_uses_raw_center_fallback) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    for (std::size_t ix = 0; ix < synth::AbsoluteFeedbackCoordinator::kMaxRoutes; ++ix) {
        REQUIRE_TRUE(coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = ix, .position = 0}).IsTracked());
    }
    synth::EncoderMidiInConfig input{
        .mode = synth::EncoderMode::Absolute,
        .turns = {{.control = {.channel = 7, .cc = 74}, .slotIx = 0, .position = 0}},
    };
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0, 0);
    PublishEncoderCell(ui, 0.9f, 0.25f, 0);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::GenericMidiOutProcessor processor(input, &sender, &ui, 0, &coordinator, 7);
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].Channel() == 7 && sink.sent[0].GetCC() == 74);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 32);
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 1);
}

TEST_CASE(midi_controller_profile_routes_launchpad_only_system_associations) {
    synth::MessageInBus bus(nullptr, 16);
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 0, 0);
    ui.sceneCapacity = 4;
    ui.leftScene.store(3);
    ui.rightScene.store(3);
    ui.sceneBlend.store(0.0f);
    synth::MidiControllerProfileConfig config;
    config.systemMessages.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadX,
            .x = 0,
            .y = 7,
        },
        .press = synth::MessageIn::SceneSelect(0, 3),
        .feedback = synth::MessageIn::SceneSelect(0, 3),
    });

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::MidiControllerProfileResult profile =
        synth::CreateMidiControllerProfile(config, &bus, &sender, &ui, [] { return 91; });
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(profile.input.get()) != nullptr);
    REQUIRE_TRUE(profile.inputThru.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(profile.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(profile.outputs.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::LaunchpadGridMidiOutProcessor*>(profile.outputs[0].get()) != nullptr);

    profile.input->Process(synth::BasicMidi::Note(1, 0, 11, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 91));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 3);
    REQUIRE_TRUE(message.timestamp == 91);

    profile.outputs[0]->Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 1);
    REQUIRE_TRUE(sink.sent[0].IsSysEx());
    REQUIRE_TRUE(sink.sent[0].raw[5] == 0x0C);
    REQUIRE_TRUE(sink.sent[0].raw[8] == 11);
    REQUIRE_TRUE(sink.sent[0].raw[9] == synth::Color::Orange.r / 2);
}

TEST_CASE(midi_controller_profile_builds_independent_outputs_from_shared_system_associations) {
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 0, 0);
    ui.resetHeld.store(true);

    synth::MidiControllerProfileConfig config;
    config.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 32},
        .wrldBldrPosition = synth::WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 4},
        .press = synth::MessageIn::ToggleReset(0),
        .feedback = synth::MessageIn::ToggleReset(0),
    });

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::MidiControllerProfileResult profile =
        synth::CreateMidiControllerProfile(config, nullptr, &sender, &ui);

    REQUIRE_TRUE(profile.outputs.size() == 2);
    profile.outputs[0]->Process();
    profile.outputs[1]->Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[0].Status() == synth::BasicMidi::kStatusCC);
    REQUIRE_TRUE(sink.sent[0].Channel() == 5);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 32);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 127);
    REQUIRE_TRUE(sink.sent[1].IsSysEx());
    REQUIRE_TRUE(sink.sent[1].raw[8] == 5);
    REQUIRE_TRUE(sink.sent[1].raw[9] == synth::WrldBldrPositionToCC(0, 4));
}

TEST_CASE(wrld_bldr_default_profile_maps_encoders_analogs_and_system_buttons) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& parameter = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    manager.SetSceneEndpoints(0, 0);

    synth::MessageInBus bus(&manager, 64);
    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 8;
    options.bankButtonCount = 16;
    options.gestureSelectorCount = 1;
    synth::MidiControllerProfileResult profile =
        synth::CreateWrldBldrDefaultProfile(options, &bus, nullptr, nullptr, [] { return 99; });
    REQUIRE_TRUE(profile.input != nullptr);
    REQUIRE_TRUE(profile.inputThru.size() == 3);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(profile.inputThru[2].get()) != nullptr);

    profile.input->Process(synth::BasicMidi::CC(0, 0, 0, 65));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ParamIncDec);
    REQUIRE_TRUE(message.slotIx == 0);
    REQUIRE_TRUE(message.position == 0);

    profile.input->Process(synth::BasicMidi::CC(0, 2, 0, 127));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetSceneBlend);
    REQUIRE_NEAR(message.value, 1.0f, 0.000001f);

    profile.input->Process(synth::BasicMidi::CC(0, 2, 1, 64));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_NEAR(message.value, 64.0f / 127.0f, 0.000001f);

    profile.input->Process(synth::BasicMidi::CC(0, 14, 0, 32));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureValue);
    REQUIRE_TRUE(message.gestureIx == 1);
    REQUIRE_NEAR(message.value, 32.0f / 127.0f, 0.000001f);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(0, 4), 127));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(0, 4), 0));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(0, 0), 127));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_TRUE(message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(0, 0), 0));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.gestureIx == 0);
    REQUIRE_TRUE(!message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(1, 6), 127));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 1);

    profile.input->Process(synth::BasicMidi::CC(0, 5, synth::WrldBldrPositionToCC(7, 2), 127));
    REQUIRE_TRUE(bus.Pop(message, 99));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(message.bankIx == 15);
    bus.Push(message);
    bus.Process(99);
    REQUIRE_TRUE(slot.SelectedBank() == &bank);
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 0);
}

TEST_CASE(mf_twister_default_profile_maps_encoders_and_input_only_side_buttons) {
    synth::MessageInBus bus(nullptr, 64);
    synth::MfTwisterDefaultProfileOptions options;
    options.visibleEncoderCount = 2;
    options.sideButtons[0] = synth::MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    };
    options.sideButtons[5] = synth::MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SceneSelect(0, 1),
    };

    const synth::MidiControllerProfileConfig config = synth::MfTwisterDefaultProfileConfig(options);
    REQUIRE_TRUE(config.encoderInput.has_value());
    REQUIRE_TRUE(config.encoderOutput.has_value());
    REQUIRE_TRUE(config.encoderOutput->protocol == synth::EncoderMidiOutProtocol::Twister);
    REQUIRE_TRUE(!config.analogInput.has_value());
    REQUIRE_TRUE(config.encoderInput->turns.size() == 2);
    REQUIRE_TRUE(config.encoderInput->pushes.size() == 2);
    REQUIRE_TRUE(config.encoderInput->turns[0].control.channel == 0);
    REQUIRE_TRUE(config.encoderInput->turns[0].control.cc == 0);
    REQUIRE_TRUE(config.encoderInput->turns[1].control.channel == 0);
    REQUIRE_TRUE(config.encoderInput->turns[1].control.cc == 1);
    REQUIRE_TRUE(config.encoderInput->pushes[0].control.channel == 1);
    REQUIRE_TRUE(config.encoderInput->pushes[0].control.cc == 0);
    REQUIRE_TRUE(config.encoderInput->pushes[1].control.channel == 1);
    REQUIRE_TRUE(config.encoderInput->pushes[1].control.cc == 1);
    REQUIRE_TRUE(config.systemMessages.size() == 2);
    REQUIRE_TRUE(config.systemMessages[0].control.has_value());
    REQUIRE_TRUE(config.systemMessages[0].control->channel == 3);
    REQUIRE_TRUE(config.systemMessages[0].control->cc == 8);
    REQUIRE_TRUE(!config.systemMessages[0].outputFeedback);
    REQUIRE_TRUE(config.systemMessages[1].control.has_value());
    REQUIRE_TRUE(config.systemMessages[1].control->channel == 3);
    REQUIRE_TRUE(config.systemMessages[1].control->cc == 13);
    REQUIRE_TRUE(!config.systemMessages[1].outputFeedback);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 2, 1, 0, 0, 0);
    synth::MidiControllerProfileResult profile =
        synth::CreateMfTwisterDefaultProfile(options, &bus, &sender, &ui, [] { return 321; });
    REQUIRE_TRUE(profile.input != nullptr);
    REQUIRE_TRUE(profile.inputThru.size() == 2);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(profile.inputThru[1].get()) != nullptr);
    REQUIRE_TRUE(profile.outputs.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::TwisterMidiOutProcessor*>(profile.outputs[0].get()) != nullptr);

    profile.input->Process(synth::BasicMidi::CC(0, 3, 8, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 321));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 3, 8, 0));
    REQUIRE_TRUE(bus.Pop(message, 321));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    profile.input->Process(synth::BasicMidi::CC(0, 3, 13, 127));
    REQUIRE_TRUE(bus.Pop(message, 321));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 1);

    profile.input->Process(synth::BasicMidi::CC(0, 3, 9, 127));
    REQUIRE_TRUE(!bus.Pop(message, 321));

    profile.outputs[0]->Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    for (const synth::BasicMidi& midi : sink.sent) {
        REQUIRE_TRUE(!(midi.IsCC() && midi.Channel() == 3 && midi.GetCC() >= 8 && midi.GetCC() <= 13));
    }
}

TEST_CASE(wrld_bldr_default_profile_creates_encoder_and_system_outputs) {
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 0, 0);
    ui.resetHeld.store(true);

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 1;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::MidiControllerProfileResult profile =
        synth::CreateWrldBldrDefaultProfile(options, nullptr, &sender, &ui);

    REQUIRE_TRUE(profile.outputs.size() == 3);
    profile.outputs[1]->Process();
    profile.outputs[2]->Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() >= 2);
    bool sawCc = false;
    bool sawSysex = false;
    for (const synth::BasicMidi& midi : sink.sent) {
        sawCc = sawCc || midi.Status() == synth::BasicMidi::kStatusCC;
        sawSysex = sawSysex || midi.IsSysEx();
    }
    REQUIRE_TRUE(sawCc);
    REQUIRE_TRUE(sawSysex);
}

TEST_CASE(launchpad_default_profile_creates_only_system_input_and_grid_output) {
    synth::MessageInBus bus(nullptr, 32);
    synth::ParameterManager::UIState ui;
    ui.Configure(0, 0, 0, 0, 1, 2);
    ui.sceneCapacity = 2;
    ui.leftScene.store(1);
    ui.rightScene.store(1);
    ui.sceneBlend.store(0.0f);
    ui.gestures.connected[0].store(true);
    ui.gestures.selected[0].store(true);
    ui.banks[0].connected.store(true);
    ui.banks[0].selected.store(true);
    ui.banks[0].bankColor.Store(synth::Color::Green);

    synth::LaunchpadDefaultProfileOptions options;
    options.controller = synth::LaunchpadController::LaunchpadMiniMk3;
    options.slotIx = 4;
    options.sceneCount = 2;
    options.bankButtonCount = 2;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig config = synth::LaunchpadDefaultProfileConfig(options);
    REQUIRE_TRUE(!config.encoderInput.has_value());
    REQUIRE_TRUE(!config.encoderOutput.has_value());
    REQUIRE_TRUE(!config.analogInput.has_value());
    REQUIRE_TRUE(config.systemMessages.size() == 6);
    REQUIRE_TRUE(config.systemMessages[0].launchpadPosition.has_value());
    REQUIRE_TRUE(config.systemMessages[0].launchpadPosition->controller == options.controller);
    REQUIRE_TRUE(config.systemMessages[0].launchpadPosition->x == 0);
    REQUIRE_TRUE(config.systemMessages[0].launchpadPosition->y == -1);
    REQUIRE_TRUE(config.systemMessages[2].press.type == synth::MessageIn::Type::SelectParamBank);
    REQUIRE_TRUE(config.systemMessages[2].press.slotIx == 4);
    REQUIRE_TRUE(config.systemMessages[2].press.bankIx == 0);
    REQUIRE_TRUE(config.systemMessages[4].press.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(config.systemMessages[4].release.has_value());
    REQUIRE_TRUE(config.systemMessages[5].press.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(config.systemMessages[5].press.hasBoolValue);
    REQUIRE_TRUE(config.systemMessages[5].press.boolValue);
    REQUIRE_TRUE(config.systemMessages[5].release.has_value());
    REQUIRE_TRUE(config.systemMessages[5].release->type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(config.systemMessages[5].release->hasBoolValue);
    REQUIRE_TRUE(!config.systemMessages[5].release->boolValue);
    REQUIRE_TRUE(config.systemMessages[5].feedback.type == synth::MessageIn::Type::ToggleReset);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::MidiControllerProfileResult profile =
        synth::CreateLaunchpadDefaultProfile(options, &bus, &sender, &ui, [] { return 123; });
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(profile.input.get()) != nullptr);
    REQUIRE_TRUE(profile.inputThru.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(profile.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(profile.outputs.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::LaunchpadGridMidiOutProcessor*>(profile.outputs[0].get()) != nullptr);

    const auto sceneNote = synth::LaunchpadPositionToNote(options.controller, 1, -1);
    REQUIRE_TRUE(sceneNote.has_value());
    profile.input->Process(synth::BasicMidi::Note(0, 0, *sceneNote, 127));
    synth::MessageIn message;
    REQUIRE_TRUE(bus.Pop(message, 123));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(message.sceneIx == 1);
    REQUIRE_TRUE(message.timestamp == 123);

    const auto gestureNote = synth::LaunchpadPositionToNote(options.controller, 0, 0);
    REQUIRE_TRUE(gestureNote.has_value());
    profile.input->Process(synth::BasicMidi::Note(0, 0, *gestureNote, 0));
    REQUIRE_TRUE(bus.Pop(message, 123));
    REQUIRE_TRUE(message.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(message.hasBoolValue);
    REQUIRE_TRUE(!message.boolValue);

    profile.outputs[0]->Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    bool sawSelectedScene = false;
    for (const synth::BasicMidi& midi : sink.sent) {
        if (midi.IsSysEx() && midi.raw[5] == 0x0D && midi.raw[8] == *sceneNote) {
            sawSelectedScene = midi.raw[9] == synth::Color::Orange.r / 2 &&
                               midi.raw[10] == synth::Color::Orange.g / 2 &&
                               midi.raw[11] == synth::Color::Orange.b / 2;
        }
    }
    REQUIRE_TRUE(sawSelectedScene);
}

TEST_CASE(launchpad_default_profiles_skip_unsupported_positions_for_each_controller) {
    const auto expectController = [](synth::LaunchpadController controller, std::size_t expectedCount) {
        synth::LaunchpadDefaultProfileOptions options;
        options.controller = controller;
        options.sceneCount = 10;
        options.bankButtonCount = 11;
        options.gestureSelectorCount = 10;
        const synth::MidiControllerProfileConfig config = synth::LaunchpadDefaultProfileConfig(options);
        REQUIRE_TRUE(config.systemMessages.size() == expectedCount);
        REQUIRE_TRUE(!config.encoderInput.has_value());
        REQUIRE_TRUE(!config.encoderOutput.has_value());
        REQUIRE_TRUE(!config.analogInput.has_value());
        for (const synth::MidiControllerSystemMessageAssociation& association : config.systemMessages) {
            REQUIRE_TRUE(!association.control.has_value());
            REQUIRE_TRUE(!association.wrldBldrPosition.has_value());
            REQUIRE_TRUE(association.launchpadPosition.has_value());
            REQUIRE_TRUE(association.launchpadPosition->controller == controller);
            REQUIRE_TRUE(synth::LaunchpadShapeSupports(controller, association.launchpadPosition->x,
                                                       association.launchpadPosition->y));
        }
    };

    expectController(synth::LaunchpadController::LaunchpadX, 27);
    expectController(synth::LaunchpadController::LaunchpadMiniMk3, 27);
    expectController(synth::LaunchpadController::LaunchpadProMk3, 29);

    synth::LaunchpadDefaultProfileOptions unsupportedReset;
    unsupportedReset.controller = synth::LaunchpadController::LaunchpadX;
    unsupportedReset.sceneCount = 0;
    unsupportedReset.bankButtonCount = 0;
    unsupportedReset.gestureSelectorCount = 0;
    unsupportedReset.resetPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadX, .x = -1, .y = 0};
    REQUIRE_TRUE(synth::LaunchpadDefaultProfileConfig(unsupportedReset).systemMessages.empty());

    synth::LaunchpadDefaultProfileOptions mismatchedReset;
    mismatchedReset.controller = synth::LaunchpadController::LaunchpadMiniMk3;
    mismatchedReset.sceneCount = 0;
    mismatchedReset.bankButtonCount = 0;
    mismatchedReset.gestureSelectorCount = 0;
    mismatchedReset.resetPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadProMk3, .x = -1, .y = 0};
    REQUIRE_TRUE(synth::LaunchpadDefaultProfileConfig(mismatchedReset).systemMessages.empty());

    synth::LaunchpadDefaultProfileOptions proReset;
    proReset.controller = synth::LaunchpadController::LaunchpadProMk3;
    proReset.sceneCount = 0;
    proReset.bankButtonCount = 0;
    proReset.gestureSelectorCount = 0;
    proReset.resetPosition =
        synth::LaunchpadGridPosition{.controller = synth::LaunchpadController::LaunchpadProMk3, .x = -1, .y = 0};
    const synth::MidiControllerProfileConfig proResetConfig = synth::LaunchpadDefaultProfileConfig(proReset);
    REQUIRE_TRUE(proResetConfig.systemMessages.size() == 1);
    REQUIRE_TRUE(proResetConfig.systemMessages[0].launchpadPosition->x == -1);
    REQUIRE_TRUE(proResetConfig.systemMessages[0].launchpadPosition->y == 0);
}

TEST_CASE(midi_sender_delivers_fifo_and_stops_cleanly) {
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 1, 2)));
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 3, 4)));
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 2);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 1);
    REQUIRE_TRUE(sink.sent[1].GetCC() == 3);
}

TEST_CASE(midi_sender_routes_each_sink_index_to_its_own_sink_in_order) {
    FakeMidiSink sinkA;
    FakeMidiSink sinkB;
    synth::MidiSender sender;
    sender.SetSink(0, &sinkA);
    sender.SetSink(1, &sinkB);
    sender.Start();

    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 1, 2)));
    REQUIRE_TRUE(sender.Enqueue(1, synth::BasicMidi::CC(0, 0, 10, 20)));
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 3, 4)));
    REQUIRE_TRUE(sender.Enqueue(1, synth::BasicMidi::CC(0, 0, 30, 40)));
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sinkA.sent.size() == 2);
    REQUIRE_TRUE(sinkA.sent[0].GetCC() == 1);
    REQUIRE_TRUE(sinkA.sent[1].GetCC() == 3);
    REQUIRE_TRUE(sinkB.sent.size() == 2);
    REQUIRE_TRUE(sinkB.sent[0].GetCC() == 10);
    REQUIRE_TRUE(sinkB.sent[1].GetCC() == 30);
}

TEST_CASE(midi_sender_drops_messages_for_null_sink_without_blocking_others) {
    FakeMidiSink sinkB;
    synth::MidiSender sender;
    // Sink 0 is intentionally left null (offline controller).
    sender.SetSink(1, &sinkB);
    sender.Start();

    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 1, 2)));
    REQUIRE_TRUE(sender.Enqueue(1, synth::BasicMidi::CC(0, 0, 10, 20)));
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sinkB.sent.size() == 1);
    REQUIRE_TRUE(sinkB.sent[0].GetCC() == 10);
}

TEST_CASE(midi_sender_enqueue_rejects_out_of_range_sink_index) {
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    REQUIRE_TRUE(!sender.Enqueue(synth::MidiSender::kMaxSinks, synth::BasicMidi::CC(0, 0, 1, 2)));
    sender.FlushForTests(std::chrono::milliseconds(200));
    sender.Stop();

    REQUIRE_TRUE(sink.sent.empty());
}

TEST_CASE(midi_sender_set_sink_swap_mid_stream_delivers_to_new_sink) {
    FakeMidiSink sinkA;
    FakeMidiSink sinkB;
    synth::MidiSender sender;
    sender.SetSink(0, &sinkA);
    sender.Start();

    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 1, 2)));
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sinkA.sent.size() == 1);

    sender.SetSink(0, &sinkB);
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 3, 4)));
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sinkA.sent.size() == 1);
    REQUIRE_TRUE(sinkB.sent.size() == 1);
    REQUIRE_TRUE(sinkB.sent[0].GetCC() == 3);
}

TEST_CASE(twister_output_debounces_reset_and_uses_channels) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 4,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {
        .name = "Cutoff",
        .shortName = "Cut",
        .defaultValue = 0.5f,
        .baseColor = synth::Color::Green,
    });
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(1);
    synth::TwisterMidiOutProcessor processor(config, &sender, ui.get());
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 4);
    REQUIRE_TRUE(sink.sent[0].Channel() == 1);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 0);
    REQUIRE_TRUE(sink.sent[0].GetValue() != 0);
    REQUIRE_TRUE(sink.sent[1].Channel() == 2);
    REQUIRE_TRUE(sink.sent[1].GetValue() == synth::FullBrightnessAnimationValue());
    REQUIRE_TRUE(sink.sent[2].Channel() == 5);
    REQUIRE_TRUE(sink.sent[2].GetValue() == 95);
    REQUIRE_TRUE(sink.sent[3].Channel() == 0);
    REQUIRE_TRUE(sink.sent[3].GetValue() == 64);
    REQUIRE_TRUE(std::none_of(sink.sent.begin(), sink.sent.end(), [](const synth::BasicMidi& midi) {
        return midi.IsCC() && midi.Channel() == 4;
    }));
    const std::size_t afterFirst = sink.sent.size();

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == afterFirst);

    processor.Reset();
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == afterFirst + 4);
    sender.Stop();
}

TEST_CASE(twister_output_skips_unstable_snapshot_without_cache_update) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    ui.slots[0].connected.store(true);
    ui.slots[0].cells[0].revision.store(1);
    ui.slots[0].cells[0].connected.store(true);
    ui.slots[0].cells[0].voiceCount.store(1);
    ui.slots[0].cells[0].values[0].store(0.25f);
    ui.slots[0].cells[0].baseColor.Store(synth::Color::Red);
    ui.slots[0].cells[0].indicatorColors[0].Store(synth::Color::Cyan);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(1);
    synth::TwisterMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(200));
    REQUIRE_TRUE(sink.sent.empty());

    auto& stableCell = ui.slots[0].cells[0];
    stableCell.revision.store(2);
    stableCell.connected.store(true);
    stableCell.voiceCount.store(1);
    stableCell.values[0].store(0.25f);
    stableCell.baseColor.Store(synth::Color::Red);
    stableCell.indicatorColors[0].Store(synth::Color::Cyan);
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 4);
}

TEST_CASE(twister_output_blanks_disconnected_mapped_cells_with_brightness_off_values_once) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(1);
    synth::TwisterMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 4);
    REQUIRE_TRUE(sink.sent[0].Channel() == 1);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 0);
    REQUIRE_TRUE(sink.sent[1].Channel() == 2);
    REQUIRE_TRUE(sink.sent[1].GetValue() == 17);
    REQUIRE_TRUE(sink.sent[2].Channel() == 5);
    REQUIRE_TRUE(sink.sent[2].GetValue() == 65);
    REQUIRE_TRUE(sink.sent[3].Channel() == 0);
    REQUIRE_TRUE(sink.sent[3].GetValue() == 0);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 4);
}

TEST_CASE(twister_output_blanks_mapped_encoder_beyond_visible_cell_capacity_with_brightness_off_values_once) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    auto& liveCell = ui.slots[0].cells[0];
    liveCell.connected.store(true);
    liveCell.voiceCount.store(1);
    liveCell.values[0].store(0.5f);
    liveCell.baseColor.Store(synth::Color::Green);
    liveCell.indicatorColors[0].Store(synth::Color::Cyan);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(2);
    synth::TwisterMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 8);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 0);
    REQUIRE_TRUE(sink.sent[0].GetValue() != 0);
    REQUIRE_TRUE(sink.sent[1].Channel() == 2);
    REQUIRE_TRUE(sink.sent[1].GetCC() == 0);
    REQUIRE_TRUE(sink.sent[1].GetValue() == synth::FullBrightnessAnimationValue());
    REQUIRE_TRUE(sink.sent[4].Channel() == 1);
    REQUIRE_TRUE(sink.sent[4].GetCC() == 1);
    REQUIRE_TRUE(sink.sent[4].GetValue() == 0);
    REQUIRE_TRUE(sink.sent[5].Channel() == 2);
    REQUIRE_TRUE(sink.sent[5].GetCC() == 1);
    REQUIRE_TRUE(sink.sent[5].GetValue() == 17);
    REQUIRE_TRUE(sink.sent[6].Channel() == 5);
    REQUIRE_TRUE(sink.sent[6].GetCC() == 1);
    REQUIRE_TRUE(sink.sent[6].GetValue() == 65);
    REQUIRE_TRUE(sink.sent[7].Channel() == 0);
    REQUIRE_TRUE(sink.sent[7].GetCC() == 1);
    REQUIRE_TRUE(sink.sent[7].GetValue() == 0);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 8);
}

TEST_CASE(twister_output_ignores_unmapped_encoder_without_blanking) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 2, 1, 0, 0);
    auto& mappedCell = ui.slots[0].cells[0];
    mappedCell.connected.store(true);
    mappedCell.voiceCount.store(1);
    mappedCell.values[0].store(0.5f);
    mappedCell.baseColor.Store(synth::Color::Green);
    mappedCell.indicatorColors[0].Store(synth::Color::Cyan);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(1);
    synth::TwisterMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sink.sent.size() == 4);
    for (const synth::BasicMidi& message : sink.sent) {
        REQUIRE_TRUE(message.GetCC() == 0);
    }
}

TEST_CASE(twister_output_uses_full_brightness_for_connected_cells) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    auto& cell = ui.slots[0].cells[0];
    cell.connected.store(true);
    cell.voiceCount.store(1);
    cell.values[0].store(0.25f);
    cell.baseColor.Store(synth::Color::Red);
    cell.indicatorColors[0].Store(synth::Color::Cyan);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::TwisterDefault(0);
    config.KeepFirstPositions(1);
    synth::TwisterMidiOutProcessor processor(config, &sender, &ui);
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    REQUIRE_TRUE(sink.sent.size() == 4);
    REQUIRE_TRUE(sink.sent[1].Channel() == 2);
    REQUIRE_TRUE(sink.sent[1].GetValue() == 47);
    REQUIRE_TRUE(sink.sent[2].Channel() == 5);
    REQUIRE_TRUE(sink.sent[2].GetValue() == 95);
}

TEST_CASE(wrld_bldr_output_sends_value_and_source_derived_sysex) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 1,
        .maxParameters = 1,
        .processLiteAlpha = 1.0f,
        .targetCenterAlpha = 1.0f,
    });
    auto& parameter = manager.CreateParameter(group, {
        .name = "Gain",
        .defaultValue = 0.25f,
        .baseColor = synth::Color::Orange,
        .indicatorColors = {synth::Color::Cyan},
    });
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, parameter);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.SelectBank(&bank);
    parameter.Compute(manager.Scene());
    parameter.ProcessLite();
    auto ui = manager.CreateUIState();
    manager.PopulateUIState(*ui);
    const synth::ui::EncoderDrawState screenState =
        synth::ui::EncoderDrawStateFromParameter(ui->slots[0].cells[0]);
    REQUIRE_TRUE(screenState.baseColor == synth::Color::Orange);
    REQUIRE_TRUE(screenState.voices[0].indicatorColor == synth::Color::Cyan);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::WrldBldrDefault(0);
    config.KeepFirstPositions(1);
    synth::WrldBldrMidiOutProcessor processor(config, &sender, ui.get());
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 3);
    REQUIRE_TRUE(sink.sent[0].Status() == synth::BasicMidi::kStatusCC);
    REQUIRE_TRUE(sink.sent[0].Channel() == 0);
    REQUIRE_TRUE(sink.sent[0].GetCC() == 0);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 32);

    const synth::BasicMidi& button = sink.sent[1];
    REQUIRE_TRUE(button.raw.size() == 14);
    REQUIRE_TRUE(button.raw[0] == 0xF0);
    REQUIRE_TRUE(button.raw[1] == 0x79);
    REQUIRE_TRUE(button.raw[2] == 0x74);
    REQUIRE_TRUE(button.raw[3] == 0x78);
    REQUIRE_TRUE(button.raw[4] == 0x00);
    REQUIRE_TRUE(button.raw[5] == 0x01);
    REQUIRE_TRUE(button.raw[6] == 0x00);
    REQUIRE_TRUE(button.raw[7] == 0x20);
    REQUIRE_TRUE(button.raw[8] == 1);
    REQUIRE_TRUE(button.raw[9] == 0);
    REQUIRE_TRUE(button.raw[10] == synth::Color::Orange.r / 2);
    REQUIRE_TRUE(button.raw[11] == synth::Color::Orange.g / 2);
    REQUIRE_TRUE(button.raw[12] == synth::Color::Orange.b / 2);
    REQUIRE_TRUE(button.raw[13] == 0xF7);

    const synth::BasicMidi& indicator = sink.sent[2];
    REQUIRE_TRUE(indicator.raw.size() == 14);
    REQUIRE_TRUE(indicator.raw[8] == 0);
    REQUIRE_TRUE(indicator.raw[9] == 0);
    REQUIRE_TRUE(indicator.raw[10] == synth::Color::Cyan.r / 2);
    REQUIRE_TRUE(indicator.raw[11] == synth::Color::Cyan.g / 2);
    REQUIRE_TRUE(indicator.raw[12] == synth::Color::Cyan.b / 2);
    REQUIRE_TRUE(indicator.raw[13] == 0xF7);

    const std::size_t afterFirst = sink.sent.size();
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == afterFirst);

    processor.Reset();
    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == afterFirst + 3);
    sender.Stop();
}

TEST_CASE(wrld_bldr_output_blanks_disconnected_mapped_cells_once) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::WrldBldrDefault(0);
    config.KeepFirstPositions(1);
    synth::WrldBldrMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    REQUIRE_TRUE(sink.sent.size() == 3);
    REQUIRE_TRUE(sink.sent[0].Status() == synth::BasicMidi::kStatusCC);
    REQUIRE_TRUE(sink.sent[0].Channel() == 0);
    REQUIRE_TRUE(sink.sent[0].GetValue() == 0);
    REQUIRE_TRUE(sink.sent[1].raw[8] == 1);
    REQUIRE_TRUE(sink.sent[1].raw[10] == 0);
    REQUIRE_TRUE(sink.sent[1].raw[11] == 0);
    REQUIRE_TRUE(sink.sent[1].raw[12] == 0);
    REQUIRE_TRUE(sink.sent[2].raw[8] == 0);
    REQUIRE_TRUE(sink.sent[2].raw[10] == 0);
    REQUIRE_TRUE(sink.sent[2].raw[11] == 0);
    REQUIRE_TRUE(sink.sent[2].raw[12] == 0);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();
    REQUIRE_TRUE(sink.sent.size() == 3);
}

// A mapping whose position is beyond the slot's realized cell capacity (e.g. a
// full 16-encoder profile hosted by an app with fewer physical encoders) has no
// backing cell. It must be driven off -- a blank value CC per mapped position --
// rather than left dark/stale by skipping it entirely.
TEST_CASE(wrld_bldr_output_blanks_positions_beyond_cell_capacity) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);  // one slot, one realized cell (positions >= 1 do not exist)

    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto config = synth::EncoderMidiOutConfig::WrldBldrDefault(0);
    config.KeepFirstPositions(2);  // positions 0 (exists, disconnected) and 1 (beyond capacity)
    REQUIRE_TRUE(config.mappings.size() == 2);
    const std::uint8_t cc0 = config.mappings[0].cc;
    const std::uint8_t cc1 = config.mappings[1].cc;
    synth::WrldBldrMidiOutProcessor processor(config, &sender, &ui);

    processor.Process();
    sender.FlushForTests(std::chrono::milliseconds(500));
    sender.Stop();

    // Both positions -- the disconnected realized cell AND the non-existent one
    // -- emit a zero-value CC on channel 0 (status CC) at their mapped CC.
    bool blanked0 = false;
    bool blanked1 = false;
    for (const auto& midi : sink.sent) {
        if (midi.IsCC() && midi.Channel() == 0 && midi.GetValue() == 0) {
            if (midi.raw[1] == cc0) {
                blanked0 = true;
            }
            if (midi.raw[1] == cc1) {
                blanked1 = true;
            }
        }
    }
    REQUIRE_TRUE(blanked0);
    REQUIRE_TRUE(blanked1);
}

TEST_CASE(absolute_encoder_output_gates_until_acknowledged_and_suppresses_exact_echo_for_both_protocols) {
    for (const synth::EncoderMidiOutProtocol protocol : {
             synth::EncoderMidiOutProtocol::Twister,
             synth::EncoderMidiOutProtocol::WrldBldr,
         }) {
        synth::ParameterManager::UIState ui;
        ui.Configure(1, 1, 1, 0, 0);
        PublishEncoderCell(ui, 0.9f, 0.5f, 0);

        synth::AbsoluteFeedbackCoordinator coordinator;
        const auto route = coordinator.ReserveRoute({.controllerSlot = 2, .parameterSlot = 0, .position = 0});
        const auto alert = coordinator.BeginInput(route, 64);
        FakeMidiSink sink;
        synth::MidiSender sender;
        sender.SetSink(0, &sink);
        sender.Start();
        auto processor = MakeEncoderOutput(protocol, synth::EncoderMode::Absolute, &sender, &ui, &coordinator, 2);

        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink) == 0);
        REQUIRE_TRUE(!sink.sent.empty());
        const auto pending = coordinator.Snapshot(route);
        REQUIRE_TRUE(pending.has_value() && pending->pending);

        PublishEncoderCell(ui, 0.9f, 0.5f, alert.Epoch() + 1);
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink) == 0);
        const auto resolved = coordinator.Snapshot(route);
        REQUIRE_TRUE(resolved.has_value() && !resolved->pending);

        const std::size_t afterResolution = sink.sent.size();
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(sink.sent.size() == afterResolution);
        sender.Stop();
    }
}

TEST_CASE(absolute_encoder_output_forces_rejection_correction_even_when_actual_matches_old_cache) {
    for (const synth::EncoderMidiOutProtocol protocol : {
             synth::EncoderMidiOutProtocol::Twister,
             synth::EncoderMidiOutProtocol::WrldBldr,
         }) {
        synth::ParameterManager::UIState ui;
        ui.Configure(1, 1, 1, 0, 0);
        PublishEncoderCell(ui, 0.8f, 0.25f, 0);
        synth::AbsoluteFeedbackCoordinator coordinator;
        const auto route = coordinator.ReserveRoute({.controllerSlot = 3, .parameterSlot = 0, .position = 0});
        FakeMidiSink sink;
        synth::MidiSender sender;
        sender.SetSink(0, &sink);
        sender.Start();
        auto processor = MakeEncoderOutput(protocol, synth::EncoderMode::Absolute, &sender, &ui, &coordinator, 3);

        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink) == 1);
        REQUIRE_TRUE(LastPositionValue(sink) == 32);
        const std::size_t beforeCorrection = sink.sent.size();

        const auto alert = coordinator.BeginInput(route, 96);
        PublishEncoderCell(ui, 0.8f, 0.25f, alert.Epoch());
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink, beforeCorrection) == 1);
        REQUIRE_TRUE(LastPositionValue(sink, beforeCorrection) == 32);
        const auto resolved = coordinator.Snapshot(route);
        REQUIRE_TRUE(resolved.has_value() && !resolved->pending);

        const std::size_t afterCorrection = sink.sent.size();
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(sink.sent.size() == afterCorrection);
        sender.Stop();
    }
}

TEST_CASE(absolute_encoder_output_resolves_only_latest_rapid_expectation_and_disconnected_blank) {
    for (const synth::EncoderMidiOutProtocol protocol : {
             synth::EncoderMidiOutProtocol::Twister,
             synth::EncoderMidiOutProtocol::WrldBldr,
         }) {
        synth::ParameterManager::UIState ui;
        ui.Configure(1, 1, 1, 0, 0);
        PublishEncoderCell(ui, 0.75f, 0.75f, 0);
        synth::AbsoluteFeedbackCoordinator coordinator;
        const auto route = coordinator.ReserveRoute({.controllerSlot = 4, .parameterSlot = 0, .position = 0});
        const auto first = coordinator.BeginInput(route, 16);
        const auto second = coordinator.BeginInput(route, 48);
        const auto latest = coordinator.BeginInput(route, 80);
        REQUIRE_TRUE(first.Epoch() < second.Epoch() && second.Epoch() < latest.Epoch());

        FakeMidiSink sink;
        synth::MidiSender sender;
        sender.SetSink(0, &sink);
        sender.Start();
        auto processor = MakeEncoderOutput(protocol, synth::EncoderMode::Absolute, &sender, &ui, &coordinator, 4);

        const float byte80Target = std::pow(80.0f / 127.0f,
                                            std::log(0.5f) / std::log(64.0f / 127.0f));
        PublishEncoderCell(ui, 0.75f, byte80Target, second.Epoch());
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink) == 0);
        REQUIRE_TRUE(coordinator.Snapshot(route)->pending);

        PublishEncoderCell(ui, 0.75f, byte80Target, latest.Epoch());
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink) == 0);
        REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);

        const auto disconnected = coordinator.BeginInput(route, 64);
        PublishEncoderCell(ui, 0.75f, 0.0f, disconnected.Epoch(), false);
        const std::size_t beforeBlank = sink.sent.size();
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink, beforeBlank) == 1);
        REQUIRE_TRUE(LastPositionValue(sink, beforeBlank) == 0);
        REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
        sender.Stop();
    }
}

TEST_CASE(absolute_encoder_output_rejects_unstable_revision_without_resolving_or_changing_cache) {
    for (const synth::EncoderMidiOutProtocol protocol : {
             synth::EncoderMidiOutProtocol::Twister,
             synth::EncoderMidiOutProtocol::WrldBldr,
         }) {
        synth::ParameterManager::UIState ui;
        ui.Configure(1, 1, 1, 0, 0);
        PublishEncoderCell(ui, 0.2f, 0.2f, 0);
        synth::AbsoluteFeedbackCoordinator coordinator;
        const auto route = coordinator.ReserveRoute({.controllerSlot = 5, .parameterSlot = 0, .position = 0});
        FakeMidiSink sink;
        synth::MidiSender sender;
        sender.SetSink(0, &sink);
        sender.Start();
        auto processor = MakeEncoderOutput(protocol, synth::EncoderMode::Absolute, &sender, &ui, &coordinator, 5);
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        const std::size_t afterInitial = sink.sent.size();

        const auto alert = coordinator.BeginInput(route, 96);
        auto& cell = ui.slots[0].cells[0];
        cell.revision.store(3, std::memory_order_release);
        cell.rawKnobValue.store(0.6f, std::memory_order_relaxed);
        cell.processedAbsoluteEpoch.store(alert.Epoch(), std::memory_order_relaxed);
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(200)));
        REQUIRE_TRUE(sink.sent.size() == afterInitial);
        REQUIRE_TRUE(coordinator.Snapshot(route)->pending);

        cell.revision.store(4, std::memory_order_release);
        processor->Process();
        REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
        REQUIRE_TRUE(CountPositionMessages(sink, afterInitial) == 1);
        REQUIRE_TRUE(LastPositionValue(sink, afterInitial) == 77);
        REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
        sender.Stop();
    }
}

TEST_CASE(relative_encoder_output_modes_ignore_absolute_raw_epochs_and_continue_post_modulation_feedback) {
    for (const synth::EncoderMidiOutProtocol protocol : {
             synth::EncoderMidiOutProtocol::Twister,
             synth::EncoderMidiOutProtocol::WrldBldr,
         }) {
        for (const synth::EncoderMode mode : {
                 synth::EncoderMode::Signed7Bit,
                 synth::EncoderMode::DirectionOnly,
             }) {
            synth::ParameterManager::UIState ui;
            ui.Configure(1, 1, 1, 0, 0);
            PublishEncoderCell(ui, 0.25f, 0.9f, 0);
            synth::AbsoluteFeedbackCoordinator coordinator;
            const auto route = coordinator.ReserveRoute({.controllerSlot = 6, .parameterSlot = 0, .position = 0});
            const auto alert = coordinator.BeginInput(route, 100);
            FakeMidiSink sink;
            synth::MidiSender sender;
            sender.SetSink(0, &sink);
            sender.Start();
            auto processor = MakeEncoderOutput(protocol, mode, &sender, &ui, &coordinator, 6);

            processor->Process();
            REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
            REQUIRE_TRUE(LastPositionValue(sink) == 32);
            const std::size_t afterFirst = sink.sent.size();

            PublishEncoderCell(ui, 0.75f, 0.9f, alert.Epoch());
            processor->Process();
            REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
            REQUIRE_TRUE(CountPositionMessages(sink, afterFirst) == 1);
            REQUIRE_TRUE(LastPositionValue(sink, afterFirst) == 95);
            const auto unchanged = coordinator.Snapshot(route);
            REQUIRE_TRUE(unchanged.has_value() && unchanged->pending);
            sender.Stop();
        }
    }
}

TEST_CASE(absolute_encoder_output_retries_failed_correction_without_resolving_or_caching) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 7, .parameterSlot = 0, .position = 0});
    const auto alert = coordinator.BeginInput(route, 96);
    PublishEncoderCell(ui, 0.9f, 0.25f, alert.Epoch());

    FakeMidiSink sink;
    synth::MidiSender sender(1);
    sender.SetSink(0, &sink);
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 9, 9, 9)));
    auto processor = MakeEncoderOutput(synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute,
                                       &sender, &ui, &coordinator, 7);
    processor->Process();
    const auto afterFailure = coordinator.Snapshot(route);
    REQUIRE_TRUE(afterFailure.has_value() && afterFailure->pending);

    sender.Start();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    const std::size_t beforeRetry = sink.sent.size();
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink, beforeRetry) == 1);
    REQUIRE_TRUE(LastPositionValue(sink, beforeRetry) == 32);
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
    sender.Stop();
}

TEST_CASE(absolute_encoder_output_retries_failed_ordinary_raw_center_enqueue_without_caching) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    PublishEncoderCell(ui, 0.9f, 0.25f, 0);
    synth::AbsoluteFeedbackCoordinator coordinator;
    coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = 0, .position = 0});
    FakeMidiSink sink;
    synth::MidiSender sender(1);
    sender.SetSink(0, &sink);
    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 9, 9, 9)));
    auto processor = MakeEncoderOutput(synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute,
                                       &sender, &ui, &coordinator, 0);
    processor->Process();

    sender.Start();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    const std::size_t beforeRetry = sink.sent.size();
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink, beforeRetry) == 1);
    REQUIRE_TRUE(LastPositionValue(sink, beforeRetry) == 32);
    sender.Stop();
}

TEST_CASE(concurrent_absolute_alert_and_position_output_linearize_before_alert_or_after_acknowledgement) {
    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    PublishEncoderCell(ui, 0.25f, 0.25f, 0);
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route = coordinator.ReserveRoute({.controllerSlot = 1, .parameterSlot = 0, .position = 0});
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto processor = MakeEncoderOutput(synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute,
                                       &sender, &ui, &coordinator, 1);
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    const std::size_t beforeRace = sink.sent.size();

    PublishEncoderCell(ui, 0.5f, 0.5f, 0);
    auto senderGuard = sender.GuardQueueForTests();
    std::thread output([&] { processor->Process(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!coordinator.RouteGuardHeldForTests(route) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool outputReachedGuardedEnqueue = coordinator.RouteGuardHeldForTests(route);

    if (!outputReachedGuardedEnqueue) {
        senderGuard.Release();
        output.join();
        sender.Stop();
        REQUIRE_TRUE(outputReachedGuardedEnqueue);
    }

    synth::AbsoluteFeedbackCoordinator::InputAlert alert;
    std::thread input([&] { alert = coordinator.BeginInput(route, 96); });
    senderGuard.Release();
    output.join();
    input.join();
    REQUIRE_TRUE(alert.IsPublished());
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink, beforeRace) == 1);
    REQUIRE_TRUE(LastPositionValue(sink, beforeRace) == 64);

    const std::size_t afterBeforeAlertEnqueue = sink.sent.size();
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == afterBeforeAlertEnqueue);
    REQUIRE_TRUE(coordinator.Snapshot(route)->pending);

    PublishEncoderCell(ui, 0.6f, 0.6f, alert.Epoch());
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink, afterBeforeAlertEnqueue) == 1);
    REQUIRE_TRUE(LastPositionValue(sink, afterBeforeAlertEnqueue) == 77);
    REQUIRE_TRUE(!coordinator.Snapshot(route)->pending);
    sender.Stop();
}

TEST_CASE(untracked_absolute_encoder_output_uses_ordinary_raw_center_debounce) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    for (std::size_t ix = 0; ix < synth::AbsoluteFeedbackCoordinator::kMaxRoutes; ++ix) {
        REQUIRE_TRUE(coordinator.ReserveRoute({.controllerSlot = 0, .parameterSlot = ix, .position = 0}).IsTracked());
    }
    REQUIRE_TRUE(!coordinator.ReserveRoute({.controllerSlot = 7, .parameterSlot = 0, .position = 0}).IsTracked());

    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    PublishEncoderCell(ui, 0.9f, 0.25f, 0);
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    auto processor = MakeEncoderOutput(synth::EncoderMidiOutProtocol::Twister, synth::EncoderMode::Absolute,
                                       &sender, &ui, &coordinator, 7);
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(LastPositionValue(sink) == 32);
    const std::size_t afterFirst = sink.sent.size();
    processor->Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(sink.sent.size() == afterFirst);
    sender.Stop();
}

TEST_CASE(tracked_absolute_output_beyond_visible_capacity_stays_safely_gated_while_pending) {
    synth::AbsoluteFeedbackCoordinator coordinator;
    const auto route =
        coordinator.ReserveRoute({.controllerSlot = 2, .parameterSlot = 0, .position = 9});
    const auto alert = coordinator.BeginInput(route, 64);
    REQUIRE_TRUE(alert.IsPublished());

    synth::ParameterManager::UIState ui;
    ui.Configure(1, 1, 1, 0, 0);
    synth::EncoderMidiOutConfig config{
        .protocol = synth::EncoderMidiOutProtocol::Twister,
        .mappings = {{.slotIx = 0, .position = 9, .cc = 9}},
    };
    FakeMidiSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();
    synth::TwisterMidiOutProcessor processor(
        config, &sender, &ui, 0, synth::EncoderMode::Absolute, &coordinator, 2);

    // There is no cell capable of publishing the epoch. Conservatively keep
    // the pending route gated instead of treating a synthetic blank snapshot
    // as an acknowledgement and emitting stale position feedback.
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink) == 0);
    REQUIRE_TRUE(coordinator.Snapshot(route)->pending);

    // Rolling back the unprocessed alert reveals the ordinary blank value.
    // Its first send proves the gated passes did not mutate position debounce.
    REQUIRE_TRUE(coordinator.Rollback(alert));
    const std::size_t beforeBlank = sink.sent.size();
    processor.Process();
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(500)));
    REQUIRE_TRUE(CountPositionMessages(sink, beforeBlank) == 1);
    REQUIRE_TRUE(LastPositionValue(sink, beforeBlank) == 0);
    sender.Stop();
}

TEST_CASE(message_bus_single_producer_single_consumer_threaded_order) {
    constexpr std::size_t kMessages = 1000;
    synth::MessageInBus bus(nullptr, 64);
    std::atomic<bool> producerDone{false};
    std::atomic<bool> failed{false};

    std::thread producer([&] {
        for (std::size_t ix = 0; ix < kMessages; ++ix) {
            const auto message = synth::MessageIn::SetSceneBlend(static_cast<std::uint64_t>(ix), static_cast<float>(ix));
            while (!bus.Push(message)) {
                std::this_thread::yield();
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        std::size_t expected = 0;
        while (expected < kMessages) {
            synth::MessageIn message;
            if (!bus.Pop(message, kMessages)) {
                if (producerDone.load(std::memory_order_acquire) && bus.Size() == 0) {
                    failed.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield();
                continue;
            }
            if (message.timestamp != expected || message.type != synth::MessageIn::Type::SetSceneBlend) {
                failed.store(true, std::memory_order_release);
                return;
            }
            ++expected;
        }
    });

    producer.join();
    consumer.join();
    REQUIRE_TRUE(!failed.load());
    REQUIRE_TRUE(bus.Size() == 0);
}

TEST_CASE(clear_gesture_active_flags_for_active_scene_selection) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    manager.Scene() = {.leftScene = 0, .rightScene = 1, .blend = 0.5f};
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numScenes = 3,
        .maxParameters = 1,
    });
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.2f});
    group.AddParameterStorageBatch(synth::MakeParameterStorageBatch(group.Config(), group.GestureCount(), 1));
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.3f});
    first.SetGestureActive(0, 0, true);
    first.SetGestureActive(1, 0, true);
    first.SetGestureActive(2, 0, true);
    second.SetGestureActive(0, 0, true);
    second.SetGestureActive(1, 0, true);

    manager.ClearGestureActiveFlagsForActiveSceneSelection(0);

    REQUIRE_TRUE(!first.GestureActive(0, 0));
    REQUIRE_TRUE(!first.GestureActive(1, 0));
    REQUIRE_TRUE(first.GestureActive(2, 0));
    REQUIRE_TRUE(!second.GestureActive(0, 0));
    REQUIRE_TRUE(!second.GestureActive(1, 0));
}

namespace {

constexpr std::size_t kSimParams = 3;
constexpr std::size_t kSimVoices = 4;
constexpr std::size_t kSimMods = 3;
constexpr std::size_t kSimGestures = 64;
constexpr std::size_t kSimScenes = 3;
constexpr std::array<synth::PhysicalEncoderId, 5> kSimSlotEncoders{10, 11, 12, 20, 21};

struct SimCell {
    synth::PhysicalEncoderId encoder = 0;
    int parameter = -1;
};

struct SimBank {
    std::vector<SimCell> top;
    std::vector<SimCell> visible;
    int selectedParameter = -1;
};

struct SimParam {
    synth::RangeKind range = synth::RangeKind::Unipolar;
    float defaultValue = 0.0f;
    std::size_t switchValues = 0;
    std::array<float, kSimScenes> sceneCenter{};
    std::array<std::array<float, kSimGestures>, kSimScenes> gestureValue{};
    std::array<synth::GestureMask, kSimScenes> gestureActiveMasks{};
    std::array<int, kSimMods> route{};
    std::array<std::size_t, kSimMods> routeSourceIndices{};
    std::array<std::size_t, kSimMods> sourceRoutePositions{};
    std::size_t activeRouteCount = 0;
    float currentCenter = 0.0f;
    float targetCenter = 0.0f;
    std::array<float, kSimVoices> currentCenterScale{};
    std::array<float, kSimVoices> targetCenterScale{};
    std::array<float, kSimVoices> currentNormalizationOffset{};
    std::array<float, kSimVoices> targetNormalizationOffset{};
    std::array<float, kSimVoices> currentMinValue{};
    std::array<float, kSimVoices> targetMinValue{};
    std::array<float, kSimVoices> currentMaxValue{};
    std::array<float, kSimVoices> targetMaxValue{};
    std::array<std::array<float, kSimMods>, kSimVoices> currentDepth{};
    std::array<std::array<float, kSimMods>, kSimVoices> targetDepth{};
    std::array<float, kSimVoices> cachedKnob{};
    std::array<float, kSimVoices> uiDisplayCenter{};
    std::array<float, kSimVoices> uiDisplaySpreadEnergy{};
};

struct SimLocalSlot {
    std::size_t storageIdentity = 0;
    int parentParameter = -1;
    std::size_t sourceIx = 0;
    bool live = false;
    bool free = false;
    bool pinned = false;
};

struct SimOracle {
    synth::SceneState scene{.leftScene = 0, .rightScene = 1, .blend = 0.25f};
    std::optional<synth::PageOrdinal> activePage = 0;
    int selectedBank = 0;
    bool resetHeld = false;
    bool randomHeld = false;
    bool randomModHeld = false;
    std::array<SimBank, 2> banks;
    std::array<float, kSimGestures> gestureWeight{};
    synth::GestureMask gestureSelectedMask = 0;
    std::array<std::array<float, kSimMods>, kSimVoices> modulatorValue{};
    std::array<SimParam, kSimParams> params;
    std::array<SimLocalSlot, kSimMods> localSlots{};
    std::size_t liveLocalCount = 0;
    std::size_t freeLocalCount = 0;
};

struct SimRandomSamples {
    std::vector<float> values;
    std::vector<float> coins;
    std::vector<std::size_t> indices;
    std::size_t valueIx = 0;
    std::size_t coinIx = 0;
    std::size_t indexIx = 0;
    unsigned diagnosticSeed = 0;
    int diagnosticStep = -1;
    std::string diagnosticAction;

    void Clear() {
        values.clear();
        coins.clear();
        indices.clear();
        valueIx = 0;
        coinIx = 0;
        indexIx = 0;
    }

    void SetDiagnosticContext(unsigned seed, int step, std::string action) {
        diagnosticSeed = seed;
        diagnosticStep = step;
        diagnosticAction = std::move(action);
    }

    [[noreturn]] void FailUnderflow(const char* sampleType) const {
        std::ostringstream oss;
        oss << "seed " << diagnosticSeed << " step " << diagnosticStep << " action " << diagnosticAction
            << " random " << sampleType << " sample underflow";
        throw std::runtime_error(oss.str());
    }

    float PopValue() {
        if (valueIx >= values.size()) {
            FailUnderflow("value");
        }
        return values[valueIx++];
    }

    float PopCoin() {
        if (coinIx >= coins.size()) {
            FailUnderflow("coin");
        }
        return coins[coinIx++];
    }

    std::size_t PopIndex(std::size_t) {
        if (indexIx >= indices.size()) {
            FailUnderflow("index");
        }
        return indices[indexIx++];
    }

    void RequireDrained(unsigned seed, int step, const std::string& action) const {
        if (valueIx != values.size() || coinIx != coins.size() || indexIx != indices.size()) {
            std::ostringstream oss;
            oss << "seed " << seed << " step " << step << " action " << action
                << " random samples not fully consumed values=" << valueIx << "/" << values.size()
                << " coins=" << coinIx << "/" << coins.size()
                << " indices=" << indexIx << "/" << indices.size();
            throw std::runtime_error(oss.str());
        }
    }

    std::string ConsumptionSummary() const {
        std::ostringstream oss;
        oss << "values=" << valueIx << "/" << values.size()
            << ",coins=" << coinIx << "/" << coins.size()
            << ",indices=" << indexIx << "/" << indices.size();
        return oss.str();
    }
};

bool SimGestureActive(const SimParam& parameter, std::size_t sceneIx, std::size_t gestureIx) {
    return (parameter.gestureActiveMasks[sceneIx] & (synth::GestureMask{1} << gestureIx)) != 0;
}

void SimSetGestureActive(SimParam& parameter, std::size_t sceneIx, std::size_t gestureIx, bool active) {
    const synth::GestureMask bit = synth::GestureMask{1} << gestureIx;
    if (active) {
        parameter.gestureActiveMasks[sceneIx] |= bit;
    } else {
        parameter.gestureActiveMasks[sceneIx] &= ~bit;
    }
}

bool SimGestureSelected(const SimOracle& oracle, std::size_t gestureIx) {
    return (oracle.gestureSelectedMask & (synth::GestureMask{1} << gestureIx)) != 0;
}

void SimSetGestureSelected(SimOracle& oracle, std::size_t gestureIx, bool selected) {
    const synth::GestureMask bit = synth::GestureMask{1} << gestureIx;
    if (selected) {
        oracle.gestureSelectedMask |= bit;
    } else {
        oracle.gestureSelectedMask &= ~bit;
    }
}

void SimEnsureRouteActive(SimParam& parameter, std::size_t sourceIx) {
    const std::size_t routeSlot = parameter.sourceRoutePositions[sourceIx];
    if (routeSlot < parameter.activeRouteCount) {
        return;
    }
    const std::size_t destination = parameter.activeRouteCount;
    if (routeSlot != destination) {
        const std::size_t displacedSource = parameter.routeSourceIndices[destination];
        std::swap(parameter.routeSourceIndices[routeSlot], parameter.routeSourceIndices[destination]);
        parameter.sourceRoutePositions[sourceIx] = destination;
        parameter.sourceRoutePositions[displacedSource] = routeSlot;
    }
    ++parameter.activeRouteCount;
}

void SimRemoveActiveRoute(SimParam& parameter, std::size_t routeSlot) {
    const std::size_t lastActive = parameter.activeRouteCount - 1;
    if (routeSlot != lastActive) {
        const std::size_t removedSource = parameter.routeSourceIndices[routeSlot];
        const std::size_t movedSource = parameter.routeSourceIndices[lastActive];
        std::swap(parameter.routeSourceIndices[routeSlot], parameter.routeSourceIndices[lastActive]);
        parameter.sourceRoutePositions[movedSource] = routeSlot;
        parameter.sourceRoutePositions[removedSource] = lastActive;
    }
    --parameter.activeRouteCount;
}

bool SimRouteNeutralAcrossVoices(const SimParam& parameter, std::size_t sourceIx) {
    constexpr float tolerance = 0.000001f;
    for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
        if (std::fabs(parameter.currentDepth[voiceIx][sourceIx]) > tolerance ||
            std::fabs(parameter.targetDepth[voiceIx][sourceIx]) > tolerance) {
            return false;
        }
    }
    return true;
}

void SimPruneNeutralActiveRoutes(SimParam& parameter) {
    for (std::size_t routeSlot = parameter.activeRouteCount; routeSlot-- > 0;) {
        const std::size_t sourceIx = parameter.routeSourceIndices[routeSlot];
        if (SimRouteNeutralAcrossVoices(parameter, sourceIx)) {
            SimRemoveActiveRoute(parameter, routeSlot);
        }
    }
}

synth::Modifier SimCurrentModifier(const SimOracle& oracle) {
    if (oracle.randomModHeld) {
        return synth::Modifier::RandomMod;
    }
    if (oracle.randomHeld) {
        return synth::Modifier::Random;
    }
    if (oracle.resetHeld) {
        return synth::Modifier::Reset;
    }
    return synth::Modifier::None;
}

std::string JoinSamples(const std::vector<std::string>& samples) {
    std::ostringstream oss;
    for (std::size_t ix = 0; ix < samples.size(); ++ix) {
        if (ix > 0) {
            oss << ",";
        }
        oss << samples[ix];
    }
    return oss.str();
}

float SimClamp(float value, synth::RangeKind range) {
    return synth::ClampToRange(value, range);
}

float SimRangeMin(synth::RangeKind range) {
    (void)range;
    return 0.0f;
}

float SimToBipolarPresentation(float normalized) {
    return 2.0f * std::clamp(normalized, 0.0f, 1.0f) - 1.0f;
}

float SimToUIPresentation(float normalized, synth::RangeKind range) {
    return range == synth::RangeKind::Bipolar ? SimToBipolarPresentation(normalized) : normalized;
}

float SimToUISpreadPresentation(float normalizedSpread, synth::RangeKind range) {
    return range == synth::RangeKind::Bipolar ? 2.0f * normalizedSpread : normalizedSpread;
}

float SimBlend(float left, float right, float blend) {
    return left * (1.0f - blend) + right * blend;
}

float SimLinearMap(float minValue, float maxValue, float normalized) {
    return minValue + (maxValue - minValue) * normalized;
}

void SimApplySceneDistribution(float& left, float& right, float blend, float delta, synth::RangeKind range) {
    blend = std::clamp(blend, 0.0f, 1.0f);
    if (&left == &right) {
        left = SimClamp(left + delta, range);
        return;
    }
    if (blend <= 0.0f) {
        left = SimClamp(left + delta, range);
        return;
    }
    if (blend >= 1.0f) {
        right = SimClamp(right + delta, range);
        return;
    }

    const float inverseBlend = 1.0f - blend;
    const float targetBlended = SimClamp(SimBlend(left, right, blend) + delta, range);
    const float proposedLeft = left + delta * inverseBlend;
    const float proposedRight = right + delta * blend;

    if (SimClamp(proposedLeft, range) != proposedLeft) {
        left = SimClamp(proposedLeft, range);
        right = (targetBlended - left * inverseBlend) / blend;
    } else if (SimClamp(proposedRight, range) != proposedRight) {
        right = SimClamp(proposedRight, range);
        left = (targetBlended - right * blend) / inverseBlend;
    } else {
        left = proposedLeft;
        right = proposedRight;
    }
}

void SimDeselect(SimBank& bank) {
    bank.selectedParameter = -1;
    bank.visible = bank.top;
}

SimCell* SimFindCell(SimBank& bank, synth::PhysicalEncoderId encoder) {
    for (auto& cell : bank.visible) {
        if (cell.encoder == encoder) {
            return &cell;
        }
    }
    return nullptr;
}

const SimCell* SimFindCell(const SimBank& bank, synth::PhysicalEncoderId encoder) {
    for (const auto& cell : bank.visible) {
        if (cell.encoder == encoder) {
            return &cell;
        }
    }
    return nullptr;
}

float SimEffectiveGestureWeight(const SimOracle& oracle, const SimParam& parameter, std::size_t gestureIx) {
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    const float leftWeight =
        SimGestureActive(parameter, oracle.scene.leftScene, gestureIx)
            ? oracle.gestureWeight[gestureIx] * (1.0f - blend)
            : 0.0f;
    const float rightWeight =
        SimGestureActive(parameter, oracle.scene.rightScene, gestureIx) ? oracle.gestureWeight[gestureIx] * blend
                                                                        : 0.0f;
    return leftWeight + rightWeight;
}

float SimRawCenter(const SimOracle& oracle, const SimParam& parameter) {
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    const float inverseBlend = 1.0f - blend;
    const float base =
        parameter.sceneCenter[oracle.scene.leftScene] * inverseBlend + parameter.sceneCenter[oracle.scene.rightScene] * blend;

    float weightedMixSum = 0.0f;
    float activeWeightSum = 0.0f;
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        const float effectiveWeight = SimEffectiveGestureWeight(oracle, parameter, gestureIx);
        if (effectiveWeight == 0.0f) {
            continue;
        }
        const float gestureValue = parameter.gestureValue[oracle.scene.leftScene][gestureIx] * inverseBlend +
                                   parameter.gestureValue[oracle.scene.rightScene][gestureIx] * blend;
        const float mix = base * (1.0f - effectiveWeight) + gestureValue * effectiveWeight;
        weightedMixSum += effectiveWeight * mix;
        activeWeightSum += effectiveWeight;
    }

    return activeWeightSum == 0.0f ? base : weightedMixSum / activeWeightSum;
}

float SimGetRaw(const SimOracle& oracle, std::size_t paramIx, std::size_t voiceIx) {
    const SimParam& parameter = oracle.params[paramIx];
    float value = parameter.currentCenter * parameter.currentCenterScale[voiceIx] +
                  parameter.currentNormalizationOffset[voiceIx];
    for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
        value += oracle.modulatorValue[voiceIx][modIx] * parameter.currentDepth[voiceIx][modIx];
    }
    return SimClamp(value, parameter.range);
}

float SimTargetGet(const SimOracle& oracle, std::size_t paramIx, std::size_t voiceIx) {
    const SimParam& parameter = oracle.params[paramIx];
    float value = parameter.targetCenter * parameter.targetCenterScale[voiceIx] +
                  parameter.targetNormalizationOffset[voiceIx];
    for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
        value += oracle.modulatorValue[voiceIx][modIx] * parameter.targetDepth[voiceIx][modIx];
    }
    return SimClamp(value, parameter.range);
}

std::size_t SimSwitchVal(const SimOracle& oracle, std::size_t paramIx, std::size_t voiceIx) {
    const SimParam& parameter = oracle.params[paramIx];
    if (parameter.switchValues <= 1) {
        return 0;
    }
    float normalized = SimTargetGet(oracle, paramIx, voiceIx);
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    const double maxBucket = static_cast<double>(parameter.switchValues - 1);
    return static_cast<std::size_t>(std::clamp(std::round(static_cast<double>(normalized) * maxBucket), 0.0, maxBucket));
}

bool SimHasNonNeutralDepthState(const SimOracle& oracle, const SimParam& parameter) {
    constexpr float tolerance = 0.000001f;
    constexpr float neutralDepthCenter = 0.5f;
    if (std::fabs(parameter.currentCenter - neutralDepthCenter) > tolerance ||
        std::fabs(parameter.targetCenter - neutralDepthCenter) > tolerance) {
        return true;
    }
    for (const float center : parameter.sceneCenter) {
        if (std::fabs(center - neutralDepthCenter) > tolerance) {
            return true;
        }
    }
    for (const synth::GestureMask mask : parameter.gestureActiveMasks) {
        if (mask != 0) {
            return true;
        }
    }
    for (const auto& row : parameter.currentDepth) {
        for (const float depth : row) {
            if (std::fabs(depth) > tolerance) {
                return true;
            }
        }
    }
    for (const auto& row : parameter.targetDepth) {
        for (const float depth : row) {
            if (std::fabs(depth) > tolerance) {
                return true;
            }
        }
    }
    for (const int route : parameter.route) {
        if (route >= 0 && SimHasNonNeutralDepthState(oracle, oracle.params[static_cast<std::size_t>(route)])) {
            return true;
        }
    }
    return false;
}

std::uint32_t SimModulatorsAffectingMask(const SimOracle& oracle, const SimParam& parameter) {
    std::uint32_t mask = 0;
    for (std::size_t modIx = 0; modIx < std::min<std::size_t>(kSimMods, 32); ++modIx) {
        const int route = parameter.route[modIx];
        if (route >= 0 && SimHasNonNeutralDepthState(oracle, oracle.params[static_cast<std::size_t>(route)])) {
            mask |= (std::uint32_t{1} << modIx);
        }
    }
    return mask;
}

synth::GestureMask SimGesturesAffectingMask(const SimOracle& oracle, const SimParam& parameter) {
    synth::GestureMask mask = 0;
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        bool active = false;
        if (blend <= 0.0f) {
            active = SimGestureActive(parameter, oracle.scene.leftScene, gestureIx);
        } else if (blend >= 1.0f) {
            active = SimGestureActive(parameter, oracle.scene.rightScene, gestureIx);
        } else {
            active = SimGestureActive(parameter, oracle.scene.leftScene, gestureIx) ||
                     SimGestureActive(parameter, oracle.scene.rightScene, gestureIx);
        }
        if (active) {
            mask |= (synth::GestureMask{1} << gestureIx);
        }
    }
    return mask;
}

void SimSeedDisplayState(SimOracle& oracle, std::size_t paramIx);

void SimComputeAtDepth(SimOracle& oracle, std::size_t paramIx, std::size_t recursionDepth) {
    SimParam& parameter = oracle.params[paramIx];
    parameter.targetCenter = SimClamp(SimRawCenter(oracle, parameter), parameter.range);

    for (const int route : parameter.route) {
        if (route >= 0) {
            SimComputeAtDepth(oracle, static_cast<std::size_t>(route), recursionDepth + 1);
        }
    }

    constexpr float neutralTolerance = 0.000001f;
    for (std::size_t sourceIx = 0; sourceIx < kSimMods; ++sourceIx) {
        const int route = parameter.route[sourceIx];
        bool targetNonNeutral = false;
        if (route >= 0) {
            for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
                if (std::fabs(synth::ModulationDepthTargetFromKnob(
                                  SimGetRaw(oracle, static_cast<std::size_t>(route), voiceIx))) > neutralTolerance) {
                    targetNonNeutral = true;
                    break;
                }
            }
        }
        bool currentNonNeutral = false;
        if (parameter.sourceRoutePositions[sourceIx] < parameter.activeRouteCount) {
            for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
                if (std::fabs(parameter.currentDepth[voiceIx][sourceIx]) > neutralTolerance) {
                    currentNonNeutral = true;
                    break;
                }
            }
        }
        if (targetNonNeutral || currentNonNeutral) {
            SimEnsureRouteActive(parameter, sourceIx);
        }
    }

    for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
        float weightSum = 0.0f;
        for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
            const int route = parameter.route[modIx];
            const float depth =
                route < 0 ? 0.0f
                          : synth::ModulationDepthTargetFromKnob(
                                SimGetRaw(oracle, static_cast<std::size_t>(route), voiceIx));
            parameter.targetDepth[voiceIx][modIx] = depth;
            weightSum += std::fabs(depth);
        }

        if (weightSum < 1.0f) {
            parameter.targetCenterScale[voiceIx] = 1.0f - weightSum;
        } else {
            parameter.targetCenterScale[voiceIx] = 0.0f;
            for (float& depth : parameter.targetDepth[voiceIx]) {
                depth /= weightSum;
            }
        }

        float normalizationOffset = 0.0f;
        for (const float depth : parameter.targetDepth[voiceIx]) {
            normalizationOffset -= std::min(0.0f, depth);
        }
        parameter.targetNormalizationOffset[voiceIx] = normalizationOffset;

        if (weightSum > 1.0f) {
            parameter.targetMinValue[voiceIx] = SimRangeMin(parameter.range);
            parameter.targetMaxValue[voiceIx] = 1.0f;
        } else {
            float minContribution = 0.0f;
            float maxContribution = 0.0f;
            for (const float depth : parameter.targetDepth[voiceIx]) {
                minContribution += std::min(0.0f, depth);
                maxContribution += std::max(0.0f, depth);
            }
            const float base = parameter.targetCenter * parameter.targetCenterScale[voiceIx] +
                               parameter.targetNormalizationOffset[voiceIx];
            parameter.targetMinValue[voiceIx] = SimClamp(base + minContribution, parameter.range);
            parameter.targetMaxValue[voiceIx] = SimClamp(base + maxContribution, parameter.range);
        }
    }

    if (recursionDepth > 0) {
        parameter.currentCenter = parameter.targetCenter;
        parameter.currentCenterScale = parameter.targetCenterScale;
        parameter.currentNormalizationOffset = parameter.targetNormalizationOffset;
        parameter.currentMinValue = parameter.targetMinValue;
        parameter.currentMaxValue = parameter.targetMaxValue;
        parameter.currentDepth = parameter.targetDepth;
        SimSeedDisplayState(oracle, paramIx);
    }
    SimPruneNeutralActiveRoutes(parameter);
}

void SimComputeAll(SimOracle& oracle) {
    for (std::size_t paramIx = 0; paramIx < kSimParams; ++paramIx) {
        SimComputeAtDepth(oracle, paramIx, 0);
    }
}

std::size_t SimParamIndex(const SimOracle& oracle, const SimParam& parameter) {
    const SimParam* begin = oracle.params.data();
    const SimParam* end = begin + oracle.params.size();
    if (&parameter < begin || &parameter >= end) {
        throw std::logic_error("simulation parameter does not belong to oracle");
    }
    return static_cast<std::size_t>(&parameter - begin);
}

void SimSeedDisplayState(SimOracle& oracle, std::size_t paramIx) {
    SimParam& parameter = oracle.params[paramIx];
    for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
        parameter.cachedKnob[voiceIx] = SimGetRaw(oracle, paramIx, voiceIx);
        parameter.uiDisplayCenter[voiceIx] = parameter.cachedKnob[voiceIx];
        parameter.uiDisplaySpreadEnergy[voiceIx] = 0.0f;
    }
}

void SimSeedDisplayState(SimOracle& oracle, SimParam& parameter) {
    SimSeedDisplayState(oracle, SimParamIndex(oracle, parameter));
}

void SimSnapParameterToTarget(SimOracle& oracle, SimParam& parameter) {
    parameter.currentCenter = parameter.targetCenter;
    parameter.currentCenterScale = parameter.targetCenterScale;
    parameter.currentNormalizationOffset = parameter.targetNormalizationOffset;
    parameter.currentMinValue = parameter.targetMinValue;
    parameter.currentMaxValue = parameter.targetMaxValue;
    parameter.currentDepth = parameter.targetDepth;
    SimPruneNeutralActiveRoutes(parameter);
    SimSeedDisplayState(oracle, parameter);
    for (const int route : parameter.route) {
        if (route >= 0) {
            SimSnapParameterToTarget(oracle, oracle.params[static_cast<std::size_t>(route)]);
        }
    }
}

void SimProcessLitePhase1All(SimOracle& oracle) {
    constexpr float alpha = 0.25f;
    for (std::size_t paramIx = 0; paramIx < oracle.params.size(); ++paramIx) {
        SimParam& parameter = oracle.params[paramIx];
        parameter.currentCenter += alpha * (parameter.targetCenter - parameter.currentCenter);
        for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
            parameter.currentCenterScale[voiceIx] +=
                alpha * (parameter.targetCenterScale[voiceIx] - parameter.currentCenterScale[voiceIx]);
            parameter.currentNormalizationOffset[voiceIx] +=
                alpha * (parameter.targetNormalizationOffset[voiceIx] - parameter.currentNormalizationOffset[voiceIx]);
            parameter.currentMinValue[voiceIx] +=
                alpha * (parameter.targetMinValue[voiceIx] - parameter.currentMinValue[voiceIx]);
            parameter.currentMaxValue[voiceIx] +=
                alpha * (parameter.targetMaxValue[voiceIx] - parameter.currentMaxValue[voiceIx]);
            for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
                parameter.currentDepth[voiceIx][modIx] +=
                    alpha * (parameter.targetDepth[voiceIx][modIx] - parameter.currentDepth[voiceIx][modIx]);
            }
        }
        for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
            const float knob = SimGetRaw(oracle, paramIx, voiceIx);
            parameter.cachedKnob[voiceIx] = knob;
        }
    }
}

void SimProcessLitePhase2All(SimOracle& oracle) {
    constexpr float uiCenterAlpha = synth::kDefaultUiDisplayCenterAlpha;
    constexpr float uiSpreadAlpha = synth::kDefaultUiDisplaySpreadAlpha;
    for (SimParam& parameter : oracle.params) {
        for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
            const float knob = parameter.cachedKnob[voiceIx];
            parameter.uiDisplayCenter[voiceIx] += uiCenterAlpha * (knob - parameter.uiDisplayCenter[voiceIx]);
            const float residual = knob - parameter.uiDisplayCenter[voiceIx];
            parameter.uiDisplaySpreadEnergy[voiceIx] +=
                uiSpreadAlpha * ((residual * residual) - parameter.uiDisplaySpreadEnergy[voiceIx]);
        }
    }
}

void SimProcessLiteAll(SimOracle& oracle) {
    SimProcessLitePhase1All(oracle);
    SimProcessLitePhase2All(oracle);
}

void SimOpenModulationView(SimOracle& oracle, SimBank& bank, int paramIx) {
    if (kSimMods > kSimSlotEncoders.size() - 1) {
        throw std::logic_error("simulation slot has too many modulators");
    }
    for (std::size_t cellIx = 0; cellIx < kSimMods; ++cellIx) {
        if (oracle.params[static_cast<std::size_t>(paramIx)].route[cellIx] < 0) {
            return;
        }
    }

    bank.selectedParameter = paramIx;
    bank.visible.clear();

    for (std::size_t cellIx = 0; cellIx < kSimMods; ++cellIx) {
        bank.visible.push_back({
            .encoder = kSimSlotEncoders[cellIx],
            .parameter = oracle.params[static_cast<std::size_t>(paramIx)].route[cellIx],
        });
    }
    bank.visible.push_back({
        .encoder = kSimSlotEncoders.back(),
        .parameter = paramIx,
    });
}

void SimHandleIncDec(SimOracle& oracle, SimParam& parameter, float delta) {
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    auto armSelectedGesture = [&](std::size_t sceneIx, std::size_t gestureIx) {
        if (SimGestureActive(parameter, sceneIx, gestureIx)) {
            return false;
        }
        parameter.gestureValue[sceneIx][gestureIx] = parameter.sceneCenter[sceneIx];
        SimSetGestureActive(parameter, sceneIx, gestureIx, true);
        return true;
    };

    bool armedGesture = false;
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        if (!SimGestureSelected(oracle, gestureIx)) {
            continue;
        }
        if (blend <= 0.0f) {
            armedGesture = armSelectedGesture(oracle.scene.leftScene, gestureIx) || armedGesture;
        } else if (blend >= 1.0f) {
            armedGesture = armSelectedGesture(oracle.scene.rightScene, gestureIx) || armedGesture;
        } else {
            armedGesture = armSelectedGesture(oracle.scene.leftScene, gestureIx) || armedGesture;
            if (oracle.scene.rightScene != oracle.scene.leftScene) {
                armedGesture = armSelectedGesture(oracle.scene.rightScene, gestureIx) || armedGesture;
            }
        }
    }

    if (armedGesture) {
        return;
    }

    float activeEffectiveWeightSum = 0.0f;
    float baseShareNumerator = 0.0f;
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        const float effectiveWeight = SimEffectiveGestureWeight(oracle, parameter, gestureIx);
        if (effectiveWeight == 0.0f) {
            continue;
        }
        activeEffectiveWeightSum += effectiveWeight;
        baseShareNumerator += effectiveWeight * (1.0f - effectiveWeight);
    }

    if (activeEffectiveWeightSum == 0.0f) {
        SimApplySceneDistribution(parameter.sceneCenter[oracle.scene.leftScene],
                                  parameter.sceneCenter[oracle.scene.rightScene], blend, delta, parameter.range);
        return;
    }

    SimApplySceneDistribution(parameter.sceneCenter[oracle.scene.leftScene], parameter.sceneCenter[oracle.scene.rightScene],
                              blend, delta * (baseShareNumerator / activeEffectiveWeightSum), parameter.range);

    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        const float effectiveWeight = SimEffectiveGestureWeight(oracle, parameter, gestureIx);
        if (effectiveWeight == 0.0f) {
            continue;
        }
        const float gestureDelta = delta * ((effectiveWeight * effectiveWeight) / activeEffectiveWeightSum);
        SimApplySceneDistribution(parameter.gestureValue[oracle.scene.leftScene][gestureIx],
                                  parameter.gestureValue[oracle.scene.rightScene][gestureIx], blend, gestureDelta,
                                  parameter.range);
    }
}

float SimDrawRandomValue(SimRandomSamples& randomSamples, std::mt19937& rng, std::vector<std::string>* samples) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    const float value = dist(rng);
    randomSamples.values.push_back(value);
    if (samples != nullptr) {
        samples->push_back("value=" + std::to_string(value));
    }
    return value;
}

float SimDrawRandomCoin(SimRandomSamples& randomSamples, std::mt19937& rng, std::vector<std::string>* samples) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    const float coin = dist(rng);
    randomSamples.coins.push_back(coin);
    if (samples != nullptr) {
        samples->push_back("coin=" + std::to_string(coin));
    }
    return coin;
}

std::size_t SimDrawRandomIndex(SimRandomSamples& randomSamples, std::mt19937& rng, std::size_t exclusiveMax,
                               std::vector<std::string>* samples) {
    const std::size_t rawIndex = static_cast<std::size_t>(rng());
    randomSamples.indices.push_back(rawIndex);
    const std::size_t index = exclusiveMax == 0 ? 0 : rawIndex % exclusiveMax;
    if (samples != nullptr) {
        samples->push_back("slot=" + std::to_string(index) + "/" + std::to_string(rawIndex));
    }
    return index;
}

void SimRandomizeValue(SimOracle& oracle, SimParam& parameter, float value) {
    const std::size_t paramIx = SimParamIndex(oracle, parameter);
    const float target = SimLinearMap(SimRangeMin(parameter.range), 1.0f, std::clamp(value, 0.0f, 1.0f));
    SimComputeAtDepth(oracle, paramIx, 0);
    SimSnapParameterToTarget(oracle, parameter);
    const float delta = target - SimTargetGet(oracle, paramIx, 0);
    SimHandleIncDec(oracle, parameter, delta);
    SimComputeAtDepth(oracle, paramIx, 0);
    SimSnapParameterToTarget(oracle, parameter);
}

void SimResetDepthToNeutral(SimOracle& oracle, SimParam& parameter) {
    for (const int route : parameter.route) {
        if (route >= 0) {
            SimResetDepthToNeutral(oracle, oracle.params[static_cast<std::size_t>(route)]);
        }
    }

    for (auto& row : parameter.currentDepth) {
        row.fill(0.0f);
    }
    for (auto& row : parameter.targetDepth) {
        row.fill(0.0f);
    }
    parameter.activeRouteCount = 0;

    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    auto resetScene = [&](std::size_t sceneIx) {
        parameter.sceneCenter[sceneIx] = 0.5f;
        parameter.gestureActiveMasks[sceneIx] = 0;
    };

    if (blend <= 0.0f) {
        resetScene(oracle.scene.leftScene);
    } else if (blend >= 1.0f) {
        resetScene(oracle.scene.rightScene);
    } else {
        resetScene(oracle.scene.leftScene);
        if (oracle.scene.rightScene != oracle.scene.leftScene) {
            resetScene(oracle.scene.rightScene);
        }
    }

    parameter.currentCenter = 0.5f;
    parameter.targetCenter = 0.5f;
    parameter.currentCenterScale.fill(1.0f);
    parameter.targetCenterScale.fill(1.0f);
    parameter.currentNormalizationOffset.fill(0.0f);
    parameter.targetNormalizationOffset.fill(0.0f);
    parameter.currentMinValue.fill(0.5f);
    parameter.targetMinValue.fill(0.5f);
    parameter.currentMaxValue.fill(0.5f);
    parameter.targetMaxValue.fill(0.5f);
    SimSeedDisplayState(oracle, parameter);
}

void SimRevertToDefault(SimOracle& oracle, SimParam& parameter) {
    for (const int route : parameter.route) {
        if (route >= 0) {
            SimResetDepthToNeutral(oracle, oracle.params[static_cast<std::size_t>(route)]);
        }
    }

    for (auto& row : parameter.currentDepth) {
        row.fill(0.0f);
    }
    for (auto& row : parameter.targetDepth) {
        row.fill(0.0f);
    }
    parameter.activeRouteCount = 0;

    const float defaultValue = SimClamp(parameter.defaultValue, parameter.range);
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    auto resetScene = [&](std::size_t sceneIx) {
        parameter.sceneCenter[sceneIx] = defaultValue;
        parameter.gestureActiveMasks[sceneIx] = 0;
    };

    if (blend <= 0.0f) {
        resetScene(oracle.scene.leftScene);
    } else if (blend >= 1.0f) {
        resetScene(oracle.scene.rightScene);
    } else {
        resetScene(oracle.scene.leftScene);
        if (oracle.scene.rightScene != oracle.scene.leftScene) {
            resetScene(oracle.scene.rightScene);
        }
    }

    parameter.currentCenter = defaultValue;
    parameter.targetCenter = defaultValue;
    parameter.currentCenterScale.fill(1.0f);
    parameter.targetCenterScale.fill(1.0f);
    parameter.currentNormalizationOffset.fill(0.0f);
    parameter.targetNormalizationOffset.fill(0.0f);
    parameter.currentMinValue.fill(defaultValue);
    parameter.targetMinValue.fill(defaultValue);
    parameter.currentMaxValue.fill(defaultValue);
    parameter.targetMaxValue.fill(defaultValue);
    SimSeedDisplayState(oracle, parameter);
}

bool SimEncoderIsPhysical(synth::PhysicalEncoderId encoder) {
    return std::find(kSimSlotEncoders.begin(), kSimSlotEncoders.end(), encoder) != kSimSlotEncoders.end();
}

void SimHandlePress(SimOracle& oracle, synth::PhysicalEncoderId encoder) {
    if (!SimEncoderIsPhysical(encoder)) {
        return;
    }
    SimBank& bank = oracle.banks[static_cast<std::size_t>(oracle.selectedBank)];
    SimCell* cell = SimFindCell(bank, encoder);
    if (cell == nullptr) {
        return;
    }
    if (bank.selectedParameter >= 0 && cell->parameter == bank.selectedParameter) {
        SimDeselect(bank);
        return;
    }
    if (cell->parameter >= 0) {
        SimOpenModulationView(oracle, bank, cell->parameter);
    }
}

void SimHandleTick(SimOracle& oracle, synth::PhysicalEncoderId encoder, float delta) {
    if (!SimEncoderIsPhysical(encoder)) {
        return;
    }
    SimBank& bank = oracle.banks[static_cast<std::size_t>(oracle.selectedBank)];
    SimCell* cell = SimFindCell(bank, encoder);
    if (cell == nullptr || cell->parameter < 0) {
        return;
    }
    SimHandleIncDec(oracle, oracle.params[static_cast<std::size_t>(cell->parameter)], delta);
}

void SimRandomMod(SimOracle& oracle, SimParam& parameter, SimRandomSamples& randomSamples, std::mt19937& rng,
                  std::vector<std::string>* samples) {
    if (kSimMods == 0) {
        return;
    }

    while (SimDrawRandomCoin(randomSamples, rng, samples) < 0.5f) {
        const std::size_t modIx = SimDrawRandomIndex(randomSamples, rng, kSimMods, samples);
        const int route = parameter.route[modIx];
        if (route < 0) {
            return;
        }
        SimRandomizeValue(oracle, oracle.params[static_cast<std::size_t>(route)],
                          SimDrawRandomValue(randomSamples, rng, samples));
    }
}

void SimHandleModifierPress(SimOracle& oracle, synth::PhysicalEncoderId encoder, SimRandomSamples& randomSamples,
                            std::mt19937& rng, std::vector<std::string>* samples) {
    if (!SimEncoderIsPhysical(encoder)) {
        return;
    }
    SimBank& bank = oracle.banks[static_cast<std::size_t>(oracle.selectedBank)];
    SimCell* cell = SimFindCell(bank, encoder);
    if (cell == nullptr || cell->parameter < 0) {
        return;
    }
    SimParam& parameter = oracle.params[static_cast<std::size_t>(cell->parameter)];
    switch (SimCurrentModifier(oracle)) {
    case synth::Modifier::None:
        break;
    case synth::Modifier::Reset:
        SimRevertToDefault(oracle, parameter);
        break;
    case synth::Modifier::Random:
        SimRandomizeValue(oracle, parameter, SimDrawRandomValue(randomSamples, rng, samples));
        break;
    case synth::Modifier::RandomMod:
        SimRandomMod(oracle, parameter, randomSamples, rng, samples);
        break;
    }
}

void SimApplyModifierToBank(SimOracle& oracle, int bankIx, SimRandomSamples& randomSamples, std::mt19937& rng,
                            std::vector<std::string>* samples) {
    std::vector<int> visited;
    for (const SimCell& cell : oracle.banks[static_cast<std::size_t>(bankIx)].top) {
        if (cell.parameter < 0 || std::find(visited.begin(), visited.end(), cell.parameter) != visited.end()) {
            continue;
        }
        visited.push_back(cell.parameter);
        SimParam& parameter = oracle.params[static_cast<std::size_t>(cell.parameter)];
        switch (SimCurrentModifier(oracle)) {
        case synth::Modifier::None:
            break;
        case synth::Modifier::Reset:
            SimRevertToDefault(oracle, parameter);
            break;
        case synth::Modifier::Random:
            SimRandomizeValue(oracle, parameter, SimDrawRandomValue(randomSamples, rng, samples));
            break;
        case synth::Modifier::RandomMod:
            SimRandomMod(oracle, parameter, randomSamples, rng, samples);
            break;
        }
    }
}

void SimSelectBank(SimOracle& oracle, int bankIx) {
    if (oracle.selectedBank != bankIx) {
        SimDeselect(oracle.banks[static_cast<std::size_t>(oracle.selectedBank)]);
    }
    oracle.selectedBank = bankIx;
}

void SimNavigateBank(SimOracle& oracle, std::size_t slotIx, synth::BankDirection direction,
                     SimRandomSamples& randomSamples, std::mt19937& randomRng,
                     std::vector<std::string>* samples) {
    if (slotIx != 0) {
        return;
    }
    if (SimCurrentModifier(oracle) != synth::Modifier::None) {
        SimApplyModifierToBank(oracle, oracle.selectedBank, randomSamples, randomRng, samples);
        return;
    }
    const int bankCount = static_cast<int>(oracle.banks.size());
    oracle.selectedBank = direction == synth::BankDirection::Next
        ? (oracle.selectedBank + 1) % bankCount
        : (oracle.selectedBank == 0 ? bankCount - 1 : oracle.selectedBank - 1);
}

std::vector<unsigned> SimSeedsFromEnvironment() {
    const char* env = std::getenv("SYNTH_RANDOM_SEEDS");
    if (env == nullptr || *env == '\0') {
        return {0x51A7u, 0xC0FFEEu, 0xA11CEu};
    }

    std::vector<unsigned> seeds;
    std::stringstream input(env);
    std::string token;
    while (std::getline(input, token, ',')) {
        if (!token.empty()) {
            seeds.push_back(static_cast<unsigned>(std::stoul(token, nullptr, 0)));
        }
    }
    return seeds.empty() ? std::vector<unsigned>{0x51A7u} : seeds;
}

int SimStepsFromEnvironmentOrDefault(int defaultSteps) {
    const char* env = std::getenv("SYNTH_RANDOM_STEPS");
    return env == nullptr || *env == '\0' ? std::max(1, defaultSteps) : std::max(1, std::atoi(env));
}

int SimStepsFromEnvironment() {
    return SimStepsFromEnvironmentOrDefault(160);
}

void SimFail(unsigned seed, int step, const std::string& action, const std::string& field, float expected, float actual) {
    std::ostringstream oss;
    oss << "seed " << seed << " step " << step << " action " << action << " " << field << " expected " << expected
        << " got " << actual;
    throw std::runtime_error(oss.str());
}

void SimFailBool(unsigned seed, int step, const std::string& action, const std::string& field) {
    std::ostringstream oss;
    oss << "seed " << seed << " step " << step << " action " << action << " mismatch in " << field;
    throw std::runtime_error(oss.str());
}

void SimCheckNear(unsigned seed, int step, const std::string& action, const std::string& field, float expected,
                  float actual, float tolerance = 0.0002f) {
    if (std::fabs(expected - actual) > tolerance) {
        SimFail(seed, step, action, field, expected, actual);
    }
}

std::string SimParamField(const synth::Parameter& parameter, std::size_t paramIx, const std::string& field) {
    std::ostringstream oss;
    oss << "paramIx=" << paramIx << " paramId=" << parameter.Id() << " " << field;
    return oss.str();
}

void SimCheck(const SimOracle& oracle, const std::array<synth::Parameter*, kSimParams>& params,
              const std::array<synth::Bank*, 2>& banks, const synth::ParameterGroup& group,
              const synth::BankSlot& slot, const synth::ParameterManager& manager, unsigned seed, int step,
              const std::string& action) {
    if (manager.ActivePageOrdinal() != oracle.activePage) {
        SimFailBool(seed, step, action, "active page ordinal");
    }
    if (manager.ResetHeld() != oracle.resetHeld) {
        SimFailBool(seed, step, action, "manager reset held");
    }
    if (manager.RandomHeld() != oracle.randomHeld) {
        SimFailBool(seed, step, action, "manager random held");
    }
    if (manager.RandomModHeld() != oracle.randomModHeld) {
        SimFailBool(seed, step, action, "manager random-mod held");
    }
    if (manager.GetCurrentModifier() != SimCurrentModifier(oracle)) {
        SimFailBool(seed, step, action, "manager current modifier");
    }
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        if (group.GestureSelected(gestureIx) != SimGestureSelected(oracle, gestureIx)) {
            std::ostringstream oss;
            oss << "seed " << seed << " step " << step << " action " << action << " group gestureIx=" << gestureIx
                << " selected expected " << SimGestureSelected(oracle, gestureIx) << " got "
                << group.GestureSelected(gestureIx);
            throw std::runtime_error(oss.str());
        }
        SimCheckNear(seed, step, action, "group gestureIx=" + std::to_string(gestureIx) + " weight",
                     oracle.gestureWeight[gestureIx], manager.GestureValue(gestureIx));
    }
    if (slot.SelectedBank() != banks[static_cast<std::size_t>(oracle.selectedBank)]) {
        SimFailBool(seed, step, action, "slot selectedBank=" + std::to_string(oracle.selectedBank));
    }

    for (std::size_t paramIx = 0; paramIx < kSimParams; ++paramIx) {
        const SimParam& expected = oracle.params[paramIx];
        const synth::Parameter& actual = *params[paramIx];
        for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
            SimCheckNear(seed, step, action,
                         SimParamField(actual, paramIx, "sceneIx=" + std::to_string(sceneIx) + " center"),
                         expected.sceneCenter[sceneIx], actual.SceneCenter(sceneIx));
            for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                const std::string gestureField = "sceneIx=" + std::to_string(sceneIx) +
                                                 " gestureIx=" + std::to_string(gestureIx);
                SimCheckNear(seed, step, action, SimParamField(actual, paramIx, gestureField + " value"),
                             expected.gestureValue[sceneIx][gestureIx],
                             actual.GestureValue(sceneIx, gestureIx));
                if (SimGestureActive(expected, sceneIx, gestureIx) != actual.GestureActive(sceneIx, gestureIx)) {
                    SimFailBool(seed, step, action, SimParamField(actual, paramIx, gestureField + " active"));
                }
            }
        }
        for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
            const int route = expected.route[modIx];
            const synth::Parameter* expectedRoute = route < 0 ? nullptr : params[static_cast<std::size_t>(route)];
            if (actual.ModulationDepthParameter(modIx) != expectedRoute) {
                SimFailBool(seed, step, action,
                            SimParamField(actual, paramIx, "modIx=" + std::to_string(modIx) + " route"));
            }
        }
        SimCheckNear(seed, step, action, SimParamField(actual, paramIx, "target center"), expected.targetCenter,
                     actual.TargetCenter());
        SimCheckNear(seed, step, action, SimParamField(actual, paramIx, "current center"), expected.currentCenter,
                     actual.CurrentCenter());
        RequireRouteBijection(actual, kSimMods);
        if (actual.ActiveRouteCount() != expected.activeRouteCount) {
            SimFail(seed, step, action, SimParamField(actual, paramIx, "active route count"),
                    static_cast<float>(expected.activeRouteCount), static_cast<float>(actual.ActiveRouteCount()));
        }
        for (std::size_t routeSlot = 0; routeSlot < kSimMods; ++routeSlot) {
            const std::size_t expectedSourceIx = expected.routeSourceIndices[routeSlot];
            const std::size_t actualSourceIx = actual.RouteSourceIndex(routeSlot);
            if (actualSourceIx != expectedSourceIx) {
                SimFailBool(seed, step, action,
                            SimParamField(actual, paramIx,
                                          "routeSlot=" + std::to_string(routeSlot) +
                                              " expected stable source=" + std::to_string(expectedSourceIx) +
                                              " actual stable source=" + std::to_string(actualSourceIx)));
            }
            const std::size_t actualRouteSlot = actual.RoutePositionForSource(expectedSourceIx);
            if (actualRouteSlot != expected.sourceRoutePositions[expectedSourceIx]) {
                SimFailBool(seed, step, action,
                            SimParamField(actual, paramIx,
                                          "stable source=" + std::to_string(expectedSourceIx) +
                                              " expected route slot=" +
                                              std::to_string(expected.sourceRoutePositions[expectedSourceIx]) +
                                              " actual route slot=" + std::to_string(actualRouteSlot)));
            }
        }
        for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
            const std::string voiceField = "voiceIx=" + std::to_string(voiceIx);
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " target center scale"),
                         expected.targetCenterScale[voiceIx],
                         actual.TargetCenterScale(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " current center scale"),
                         expected.currentCenterScale[voiceIx],
                         actual.CurrentCenterScale(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " target normalization offset"),
                         expected.targetNormalizationOffset[voiceIx],
                         actual.TargetNormalizationOffset(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " current normalization offset"),
                         expected.currentNormalizationOffset[voiceIx],
                         actual.CurrentNormalizationOffset(voiceIx));
            for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
                const std::string modField = voiceField + " modIx=" + std::to_string(modIx);
                SimCheckNear(seed, step, action, SimParamField(actual, paramIx, modField + " target depth"),
                             expected.targetDepth[voiceIx][modIx],
                             actual.TargetDepthForSource(voiceIx, modIx));
                SimCheckNear(seed, step, action, SimParamField(actual, paramIx, modField + " current depth"),
                             expected.currentDepth[voiceIx][modIx],
                             actual.CurrentDepthForSource(voiceIx, modIx));
            }
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " raw"),
                         SimGetRaw(oracle, paramIx, voiceIx), actual.GetRaw(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " cached knob"),
                         expected.cachedKnob[voiceIx], actual.CachedKnobValue(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " ui display center"),
                         expected.uiDisplayCenter[voiceIx], actual.UIDisplayCenter(voiceIx));
            SimCheckNear(seed, step, action, SimParamField(actual, paramIx, voiceField + " ui display spread"),
                         std::sqrt(std::max(0.0f, expected.uiDisplaySpreadEnergy[voiceIx])),
                         actual.UIDisplaySpread(voiceIx));
        }
    }

    for (std::size_t bankIx = 0; bankIx < banks.size(); ++bankIx) {
        const SimBank& expected = oracle.banks[bankIx];
        const synth::Bank& actual = *banks[bankIx];
        if (actual.ShowingModulation() != (expected.selectedParameter >= 0)) {
            SimFailBool(seed, step, action, "bankIx=" + std::to_string(bankIx) + " mode");
        }
        const synth::Parameter* expectedSelected =
            expected.selectedParameter < 0 ? nullptr : params[static_cast<std::size_t>(expected.selectedParameter)];
        if (actual.SelectedParameter() != expectedSelected) {
            SimFailBool(seed, step, action, "bankIx=" + std::to_string(bankIx) + " selected parameter");
        }
        for (const synth::PhysicalEncoderId encoder : kSimSlotEncoders) {
            const SimCell* cell = SimFindCell(expected, encoder);
            const synth::Parameter* expectedVisible =
                cell == nullptr || cell->parameter < 0 ? nullptr : params[static_cast<std::size_t>(cell->parameter)];
            if (actual.VisibleParameter(encoder) != expectedVisible) {
                SimFailBool(seed, step, action,
                            "bankIx=" + std::to_string(bankIx) + " encoder=" + std::to_string(encoder) +
                                " visible parameter");
            }
        }
    }
}

void SimCheckUIState(const SimOracle& oracle, const synth::ParameterManager::UIState& ui, unsigned seed, int step,
                     const std::string& action) {
    if (ui.leftScene.load() != oracle.scene.leftScene || ui.rightScene.load() != oracle.scene.rightScene) {
        SimFailBool(seed, step, action, "ui scene endpoints");
    }
    SimCheckNear(seed, step, action, "ui scene blend", oracle.scene.blend, ui.sceneBlend.load());
    if (ui.resetHeld.load(std::memory_order_relaxed) != oracle.resetHeld) {
        SimFailBool(seed, step, action, "ui reset held");
    }
    if (ui.randomHeld.load(std::memory_order_relaxed) != oracle.randomHeld) {
        SimFailBool(seed, step, action, "ui random held");
    }
    if (ui.randomModHeld.load(std::memory_order_relaxed) != oracle.randomModHeld) {
        SimFailBool(seed, step, action, "ui random-mod held");
    }

    const SimBank& bank = oracle.banks[static_cast<std::size_t>(oracle.selectedBank)];
    if (!ui.slots[0].connected.load() ||
        ui.slots[0].showingModulationView.load() != (bank.selectedParameter >= 0)) {
        SimFailBool(seed, step, action, "ui selected bank/view state");
    }
    for (std::size_t bankIx = 0; bankIx < ui.bankCapacity; ++bankIx) {
        if (!ui.banks[bankIx].connected.load() ||
            ui.banks[bankIx].selected.load() != (bankIx == static_cast<std::size_t>(oracle.selectedBank))) {
            SimFailBool(seed, step, action, "ui bankIx=" + std::to_string(bankIx) + " selected state");
        }
    }
    const std::array<synth::Color, 4> defaultIndicators{
        synth::Color::Grey, synth::Color::Grey, synth::Color::Grey, synth::Color::Grey};
    for (std::size_t position = 0; position < kSimSlotEncoders.size(); ++position) {
        const SimCell* cell = SimFindCell(bank, kSimSlotEncoders[position]);
        const synth::Parameter::UIState& actual = ui.slots[0].cells[position];
        const bool expectedConnected = cell != nullptr && cell->parameter >= 0;
        if (actual.connected.load() != expectedConnected) {
            SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " connected");
        }
        if (!expectedConnected) {
            if (actual.switchValues.load() != 0 || actual.modulatorsAffectingMask.load() != 0 ||
                actual.gesturesAffectingMask.load() != 0) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " disconnected switches/masks");
            }
        }
        if (expectedConnected) {
            const std::size_t paramIx = static_cast<std::size_t>(cell->parameter);
            const SimParam& expected = oracle.params[paramIx];
            if (actual.bipolar.load() != (expected.range == synth::RangeKind::Bipolar)) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " bipolar");
            }
            if (actual.baseColor.Load() != synth::Color::Grey) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " color");
            }
            const std::size_t expectedSwitchValues = expected.switchValues;
            const std::uint32_t expectedModulatorMask = SimModulatorsAffectingMask(oracle, expected);
            const synth::GestureMask expectedGestureMask = SimGesturesAffectingMask(oracle, expected);
            if (actual.switchValues.load() != expectedSwitchValues) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " switch values");
            }
            if (actual.modulatorsAffectingMask.load() != expectedModulatorMask) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " modulator mask");
            }
            if (actual.gesturesAffectingMask.load() != expectedGestureMask) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " gesture mask");
            }
            if (actual.modulatorColorCount.load() != kSimMods || actual.gestureColorCount.load() != kSimGestures) {
                SimFailBool(seed, step, action, "ui position=" + std::to_string(position) + " color counts");
            }
            for (std::size_t sourceIx = 0; sourceIx < kSimMods; ++sourceIx) {
                if (actual.modulatorSourceColors[sourceIx].Load() != synth::Color::Off) {
                    SimFailBool(seed, step, action,
                                "ui position=" + std::to_string(position) +
                                    " source color=" + std::to_string(sourceIx));
                }
            }
            for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                if (actual.gestureColors[gestureIx].Load() != synth::Color::Off) {
                    SimFailBool(seed, step, action,
                                "ui position=" + std::to_string(position) +
                                    " gesture color=" + std::to_string(gestureIx));
                }
            }
            for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
                SimCheckNear(seed, step, action,
                             "ui position=" + std::to_string(position) + " voice=" + std::to_string(voiceIx),
                             SimToUIPresentation(expected.uiDisplayCenter[voiceIx], expected.range),
                             actual.values[voiceIx].load());
                const float expectedSpread = expected.switchValues > 1
                                                 ? 0.0f
                                                 : SimToUISpreadPresentation(
                                                       std::sqrt(std::max(0.0f, expected.uiDisplaySpreadEnergy[voiceIx])),
                                                       expected.range);
                SimCheckNear(seed, step, action,
                             "ui position=" + std::to_string(position) + " spread=" + std::to_string(voiceIx),
                             expectedSpread,
                             actual.spreadValues[voiceIx].load());
                SimCheckNear(seed, step, action,
                             "ui position=" + std::to_string(position) + " min=" + std::to_string(voiceIx),
                             SimToUIPresentation(expected.currentMinValue[voiceIx], expected.range),
                             actual.minValues[voiceIx].load());
                SimCheckNear(seed, step, action,
                             "ui position=" + std::to_string(position) + " max=" + std::to_string(voiceIx),
                             SimToUIPresentation(expected.currentMaxValue[voiceIx], expected.range),
                             actual.maxValues[voiceIx].load());
                const std::size_t expectedSwitchValue = SimSwitchVal(oracle, paramIx, voiceIx);
                if (actual.switchValue[voiceIx].load() != expectedSwitchValue) {
                    SimFailBool(seed, step, action,
                                "ui position=" + std::to_string(position) + " switch=" + std::to_string(voiceIx));
                }
                if (actual.indicatorColors[voiceIx].Load() != defaultIndicators[voiceIx]) {
                    SimFailBool(seed, step, action,
                                "ui position=" + std::to_string(position) + " indicator=" + std::to_string(voiceIx));
                }
            }
        }
    }
    if (ui.gestures.gestureCapacity != kSimGestures) {
        SimFailBool(seed, step, action, "ui gesture capacity");
    }
    for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
        if (ui.gestures.selected[gestureIx].load() != SimGestureSelected(oracle, gestureIx)) {
            SimFailBool(seed, step, action, "ui gesture selected");
        }
        SimCheckNear(seed, step, action, "ui gesture value", oracle.gestureWeight[gestureIx],
                     ui.gestures.values[gestureIx].load());
    }
}

void SimInitializeOracle(SimOracle& oracle) {
    oracle.selectedBank = 0;
    oracle.resetHeld = false;
    oracle.randomHeld = false;
    oracle.randomModHeld = false;
    const std::array<float, kSimParams> defaults{0.35f, 0.55f, 0.4f};
    const std::array<synth::RangeKind, kSimParams> ranges{
        synth::RangeKind::Unipolar,
        synth::RangeKind::Bipolar,
        synth::RangeKind::Bipolar,
    };
    const std::array<std::size_t, kSimParams> switchValues{5, 0, 3};

    for (std::size_t paramIx = 0; paramIx < kSimParams; ++paramIx) {
        SimParam& parameter = oracle.params[paramIx];
        parameter.defaultValue = defaults[paramIx];
        parameter.range = ranges[paramIx];
        parameter.switchValues = switchValues[paramIx];
        const float defaultValue = SimClamp(defaults[paramIx], ranges[paramIx]);
        parameter.sceneCenter.fill(defaultValue);
        for (auto& row : parameter.gestureValue) {
            row.fill(defaultValue);
        }
        parameter.gestureActiveMasks.fill(0);
        parameter.route.fill(-1);
        for (std::size_t sourceIx = 0; sourceIx < kSimMods; ++sourceIx) {
            parameter.routeSourceIndices[sourceIx] = sourceIx;
            parameter.sourceRoutePositions[sourceIx] = sourceIx;
        }
        parameter.activeRouteCount = 0;
        parameter.currentCenter = defaultValue;
        parameter.targetCenter = defaultValue;
        parameter.currentCenterScale.fill(1.0f);
        parameter.targetCenterScale.fill(1.0f);
        parameter.currentNormalizationOffset.fill(0.0f);
        parameter.targetNormalizationOffset.fill(0.0f);
        parameter.currentMinValue.fill(defaultValue);
        parameter.targetMinValue.fill(defaultValue);
        parameter.currentMaxValue.fill(defaultValue);
        parameter.targetMaxValue.fill(defaultValue);
        for (auto& row : parameter.currentDepth) {
            row.fill(0.0f);
        }
        for (auto& row : parameter.targetDepth) {
            row.fill(0.0f);
        }
    }

    oracle.params[0].route[0] = 1;
    oracle.params[0].route[1] = 2;
    oracle.params[1].route[0] = 2;
    oracle.banks[0].top = {
        {.encoder = 10, .parameter = 0},
        {.encoder = 11, .parameter = 1},
        {.encoder = 12, .parameter = 2},
    };
    oracle.banks[1].top = {
        {.encoder = 20, .parameter = 2},
        {.encoder = 21, .parameter = 0},
    };
    SimDeselect(oracle.banks[0]);
    SimDeselect(oracle.banks[1]);
    for (std::size_t paramIx = 0; paramIx < oracle.params.size(); ++paramIx) {
        SimSeedDisplayState(oracle, paramIx);
    }
}

void SimSetLessSelectedScene(SimOracle& oracle, std::size_t sceneIx) {
    if (sceneIx >= kSimScenes) {
        return;
    }
    const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
    if (blend <= 0.5f) {
        oracle.scene.rightScene = sceneIx;
    } else {
        oracle.scene.leftScene = sceneIx;
    }
}

void SimSnapAllToTarget(SimOracle& oracle) {
    for (std::size_t paramIx = 0; paramIx < oracle.params.size(); ++paramIx) {
        SimSnapParameterToTarget(oracle, oracle.params[paramIx]);
    }
}

void SimComputeAllAndSnap(SimOracle& oracle) {
    SimComputeAll(oracle);
    SimSnapAllToTarget(oracle);
}

struct SimPatchSnapshot {
    std::array<SimParam, kSimParams> params;
};

SimPatchSnapshot SimCapturePatchSnapshot(const SimOracle& oracle) {
    return {.params = oracle.params};
}

void SimApplyPatchSnapshot(SimOracle& oracle, const SimPatchSnapshot& snapshot) {
    for (std::size_t paramIx = 0; paramIx < kSimParams; ++paramIx) {
        SimParam& target = oracle.params[paramIx];
        const SimParam& saved = snapshot.params[paramIx];
        target.sceneCenter = saved.sceneCenter;
        target.gestureValue = saved.gestureValue;
        target.gestureActiveMasks = saved.gestureActiveMasks;
    }
    SimComputeAllAndSnap(oracle);
}

void SimApplyNewPatch(SimOracle& oracle) {
    const auto banks = oracle.banks;
    const int selectedBank = oracle.selectedBank;
    const auto modulatorValue = oracle.modulatorValue;
    SimInitializeOracle(oracle);
    oracle.banks = banks;
    oracle.selectedBank = selectedBank;
    oracle.modulatorValue = modulatorValue;
    oracle.scene = {.leftScene = 0, .rightScene = 1, .blend = 0.25f};
    oracle.activePage = 0;
    oracle.gestureWeight.fill(0.0f);
    oracle.gestureSelectedMask = 0;
    SimComputeAllAndSnap(oracle);
}

std::size_t SimFindLatestPatchInDirectory(
    const std::vector<std::pair<std::filesystem::path, SimPatchSnapshot>>& versions,
    const std::filesystem::path& patchDir) {
    for (std::size_t reverseIx = versions.size(); reverseIx > 0; --reverseIx) {
        const std::size_t ix = reverseIx - 1;
        if (versions[ix].first.parent_path() == patchDir) {
            return ix;
        }
    }
    throw std::runtime_error("saved patch directory not found: " + patchDir.string());
}

} // namespace

TEST_CASE(randomized_parameter_modulation_simulation) {
    const std::vector<unsigned> seeds = SimSeedsFromEnvironment();
    const int steps = SimStepsFromEnvironment();

    for (const unsigned seed : seeds) {
        synth::ParameterManager manager;
        manager.SetGestureCount(kSimGestures);
        auto& group = manager.CreateGroup({
            .numVoices = kSimVoices,
            .numModulators = kSimMods,
            .numScenes = kSimScenes,
            .maxParameters = kSimParams,
            .processLiteAlpha = 0.25f,
            .targetCenterAlpha = 1.0f,
        });
        MarkAllModulatorsConnectedForUi(group);
        auto& carrier = manager.CreateParameter(group, {
            .name = "Carrier",
            .defaultValue = 0.35f,
            .switchValues = 5,
        });
        auto& depthA = manager.CreateParameter(
            group, {.name = "DepthA", .defaultValue = 0.55f, .range = synth::RangeKind::Bipolar});
        auto& depthB = manager.CreateParameter(
            group, {
                .name = "DepthB",
                .defaultValue = 0.4f,
                .range = synth::RangeKind::Bipolar,
                .switchValues = 3,
            });
        REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depthA));
        REQUIRE_TRUE(carrier.AssignModulationDepth(1, &depthB));
        REQUIRE_TRUE(depthA.AssignModulationDepth(0, &depthB));

        auto& auxGroup = manager.CreateGroup({
            .numVoices = 1,
            .numScenes = kSimScenes,
            .maxParameters = 1,
        });
        auto& aux = manager.CreateParameter(auxGroup, {.name = "Aux", .defaultValue = 0.25f});
        REQUIRE_TRUE(manager.NumGroups() == 2);
        REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
        manager.SetSceneBlend(0.25f);

        auto& pageA = manager.CreatePage("A");
        auto& pageB = manager.CreatePage("B");
        auto& pageAux = manager.CreatePage("Aux");
        manager.AssignParameterToPage(pageA.ordinal, carrier);
        manager.AssignParameterToPage(pageB.ordinal, depthA);
        manager.AssignParameterToPage(pageAux.ordinal, aux);
        manager.SelectActivePage(pageA.ordinal);

        auto& bankA = manager.CreateBank();
        bankA.AddMapping(10, carrier);
        bankA.AddMapping(11, depthA);
        bankA.AddMapping(12, depthB);
        auto& bankB = manager.CreateBank();
        bankB.AddMapping(20, depthB);
        bankB.AddMapping(21, carrier);
        auto& slot = manager.CreateBankSlot();
        for (const synth::PhysicalEncoderId encoder : {10u, 11u, 12u, 20u, 21u}) {
            slot.AddPhysicalEncoder(encoder);
        }
        slot.SelectBank(&bankA);

        SimOracle oracle;
        SimInitializeOracle(oracle);
        const std::array<synth::Parameter*, kSimParams> params{&carrier, &depthA, &depthB};
        const std::array<synth::Bank*, 2> banks{&bankA, &bankB};
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> deltaDist(-0.18f, 0.18f);
        std::uniform_real_distribution<float> unipolarDist(0.0f, 1.0f);
        const std::array<synth::PhysicalEncoderId, 6> encoders{10, 11, 12, 20, 21, 99};
        SimRandomSamples randomSamples;
        std::mt19937 randomRng(seed ^ 0x5A17A11Du);
        manager.SetRandomSource(
            [&randomSamples]() { return randomSamples.PopValue(); },
            [&randomSamples]() { return randomSamples.PopCoin(); },
            [&randomSamples](std::size_t max) { return randomSamples.PopIndex(max); });

        SimCheck(oracle, params, banks, group, slot, manager, seed, -1, "initial");

        for (int step = 0; step < steps; ++step) {
            std::string action;
            auto handlePress = [&](synth::PhysicalEncoderId encoder, const std::string& prefix) {
                randomSamples.Clear();
                std::vector<std::string> samples;
                if (SimCurrentModifier(oracle) == synth::Modifier::None) {
                    action = prefix + " press " + std::to_string(encoder);
                    manager.HandlePress(encoder);
                    SimHandlePress(oracle, encoder);
                    return;
                }
                SimHandleModifierPress(oracle, encoder, randomSamples, randomRng, &samples);
                action = prefix + " modifier press " + std::to_string(encoder) + " samples=" + JoinSamples(samples);
                manager.HandlePress(encoder);
                randomSamples.RequireDrained(seed, step, action);
            };
            auto handleMomentaryModifierPress = [&](synth::Modifier modifier, synth::PhysicalEncoderId encoder,
                                                    const std::string& prefix) {
                randomSamples.Clear();
                std::vector<std::string> samples;
                switch (modifier) {
                case synth::Modifier::Reset:
                    manager.SetResetHeld(true);
                    oracle.resetHeld = true;
                    break;
                case synth::Modifier::Random:
                    manager.SetRandomHeld(true);
                    oracle.randomHeld = true;
                    break;
                case synth::Modifier::RandomMod:
                    manager.SetRandomModHeld(true);
                    oracle.randomModHeld = true;
                    break;
                case synth::Modifier::None:
                    break;
                }
                SimHandleModifierPress(oracle, encoder, randomSamples, randomRng, &samples);
                action = prefix + " press " + std::to_string(encoder) + " samples=" + JoinSamples(samples);
                manager.HandlePress(encoder);
                randomSamples.RequireDrained(seed, step, action);
                switch (modifier) {
                case synth::Modifier::Reset:
                    manager.SetResetHeld(false);
                    oracle.resetHeld = false;
                    break;
                case synth::Modifier::Random:
                    manager.SetRandomHeld(false);
                    oracle.randomHeld = false;
                    break;
                case synth::Modifier::RandomMod:
                    manager.SetRandomModHeld(false);
                    oracle.randomModHeld = false;
                    break;
                case synth::Modifier::None:
                    break;
                }
            };
            switch (rng() % 19) {
            case 0: {
                const auto encoder = encoders[rng() % encoders.size()];
                const float delta = deltaDist(rng);
                action = "turn encoder " + std::to_string(encoder);
                manager.HandleTick(encoder, delta);
                SimHandleTick(oracle, encoder, delta);
                break;
            }
            case 1: {
                const auto encoder = encoders[rng() % encoders.size()];
                handlePress(encoder, "encoder");
                break;
            }
            case 2: {
                const auto encoder = encoders[rng() % encoders.size()];
                handleMomentaryModifierPress(synth::Modifier::Reset, encoder, "reset");
                break;
            }
            case 3: {
                const auto encoder = encoders[rng() % encoders.size()];
                handleMomentaryModifierPress(synth::Modifier::Random, encoder, "random");
                break;
            }
            case 4: {
                const auto encoder = encoders[rng() % encoders.size()];
                handleMomentaryModifierPress(synth::Modifier::RandomMod, encoder, "random-mod");
                break;
            }
            case 5: {
                const bool held = (rng() % 2) == 0;
                action = std::string("set reset held ") + (held ? "true" : "false");
                manager.SetResetHeld(held);
                oracle.resetHeld = held;
                break;
            }
            case 6: {
                const bool held = (rng() % 2) == 0;
                action = std::string("set random held ") + (held ? "true" : "false");
                manager.SetRandomHeld(held);
                oracle.randomHeld = held;
                break;
            }
            case 7: {
                const bool held = (rng() % 2) == 0;
                action = std::string("set random-mod held ") + (held ? "true" : "false");
                manager.SetRandomModHeld(held);
                oracle.randomModHeld = held;
                break;
            }
            case 8: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "select gesture " + std::to_string(gestureIx);
                manager.SelectGesture(gestureIx);
                SimSetGestureSelected(oracle, gestureIx, true);
                break;
            }
            case 9: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "deselect gesture " + std::to_string(gestureIx);
                manager.DeselectGesture(gestureIx);
                SimSetGestureSelected(oracle, gestureIx, false);
                break;
            }
            case 10: {
                const std::size_t gestureIx = rng() % kSimGestures;
                const float value = unipolarDist(rng);
                action = "set gesture value " + std::to_string(gestureIx);
                manager.SetGestureValue(gestureIx, value);
                oracle.gestureWeight[gestureIx] = value;
                break;
            }
            case 11: {
                const synth::PageOrdinal ordinal = rng() % 2 == 0 ? pageA.ordinal : pageB.ordinal;
                action = "select page " + std::to_string(ordinal);
                manager.SelectActivePage(ordinal);
                oracle.activePage = ordinal;
                break;
            }
            case 12: {
                const int bankIx = static_cast<int>(rng() % 2);
                randomSamples.Clear();
                std::vector<std::string> samples;
                if (SimCurrentModifier(oracle) == synth::Modifier::None) {
                    action = "select bank " + std::to_string(bankIx);
                    REQUIRE_TRUE(manager.SelectBankForSlot(0, static_cast<std::size_t>(bankIx)));
                    SimSelectBank(oracle, bankIx);
                } else {
                    SimApplyModifierToBank(oracle, bankIx, randomSamples, randomRng, &samples);
                    action = "modified bank " + std::to_string(bankIx) + " samples=" + JoinSamples(samples);
                    REQUIRE_TRUE(manager.SelectBankForSlot(0, static_cast<std::size_t>(bankIx)));
                    randomSamples.RequireDrained(seed, step, action);
                }
                break;
            }
            case 13: {
                const std::size_t left = rng() % kSimScenes;
                const std::size_t right = rng() % kSimScenes;
                action = "change scene";
                REQUIRE_TRUE(manager.SetSceneEndpoints(left, right));
                oracle.scene.leftScene = left;
                oracle.scene.rightScene = right;
                break;
            }
            case 14: {
                const float blend = unipolarDist(rng);
                action = "change blend";
                manager.SetSceneBlend(blend);
                oracle.scene.blend = blend;
                break;
            }
            case 15: {
                const std::size_t voiceIx = rng() % kSimVoices;
                const std::size_t modIx = rng() % kSimMods;
                const float value = unipolarDist(rng);
                action = "change modulator";
                group.GetModulators().Value(voiceIx, modIx) = value;
                oracle.modulatorValue[voiceIx][modIx] = value;
                break;
            }
            case 16:
                action = "compute";
                for (synth::Parameter* parameter : params) {
                    parameter->Compute(manager.Scene());
                }
                SimComputeAll(oracle);
                break;
            case 17:
                action = "process lite";
                for (synth::Parameter* parameter : params) {
                    parameter->ProcessLite();
                }
                SimProcessLiteAll(oracle);
                break;
            default: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "clear active gesture " + std::to_string(gestureIx);
                manager.ClearGestureActiveFlagsForActiveSceneSelection(gestureIx);
                const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
                for (auto& parameter : oracle.params) {
                    if (blend <= 0.0f) {
                        SimSetGestureActive(parameter, oracle.scene.leftScene, gestureIx, false);
                    } else if (blend >= 1.0f) {
                        SimSetGestureActive(parameter, oracle.scene.rightScene, gestureIx, false);
                    } else {
                        SimSetGestureActive(parameter, oracle.scene.leftScene, gestureIx, false);
                        if (oracle.scene.rightScene != oracle.scene.leftScene) {
                            SimSetGestureActive(parameter, oracle.scene.rightScene, gestureIx, false);
                        }
                    }
                }
                break;
            }
            }

            SimCheck(oracle, params, banks, group, slot, manager, seed, step, action);
        }
    }
}

TEST_CASE(randomized_message_bus_ui_state_simulation) {
    const std::vector<unsigned> seeds = SimSeedsFromEnvironment();
    const int steps = SimStepsFromEnvironmentOrDefault(250);

    for (const unsigned seed : seeds) {
        synth::ParameterManager manager;
        manager.SetGestureCount(kSimGestures);
        auto& group = manager.CreateGroup({
            .numVoices = kSimVoices,
            .numModulators = kSimMods,
            .numScenes = kSimScenes,
            .maxParameters = kSimParams,
            .processLiteAlpha = 0.25f,
            .targetCenterAlpha = 1.0f,
        });
        MarkAllModulatorsConnectedForUi(group);
        auto& carrier = manager.CreateParameter(group, {
            .name = "Carrier",
            .defaultValue = 0.35f,
            .switchValues = 5,
        });
        auto& depthA = manager.CreateParameter(
            group, {.name = "DepthA", .defaultValue = 0.55f, .range = synth::RangeKind::Bipolar});
        auto& depthB = manager.CreateParameter(
            group, {
                .name = "DepthB",
                .defaultValue = 0.4f,
                .range = synth::RangeKind::Bipolar,
                .switchValues = 3,
            });
        REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depthA));
        REQUIRE_TRUE(carrier.AssignModulationDepth(1, &depthB));
        REQUIRE_TRUE(depthA.AssignModulationDepth(0, &depthB));

        auto& auxGroup = manager.CreateGroup({
            .numVoices = 1,
            .numScenes = kSimScenes,
            .maxParameters = 1,
        });
        auto& aux = manager.CreateParameter(auxGroup, {.name = "Aux", .defaultValue = 0.25f});
        (void)aux;
        REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
        manager.SetSceneBlend(0.25f);

        auto& pageA = manager.CreatePage("A");
        auto& pageB = manager.CreatePage("B");
        manager.AssignParameterToPage(pageA.ordinal, carrier);
        manager.AssignParameterToPage(pageB.ordinal, depthA);
        manager.SelectActivePage(pageA.ordinal);

        auto& bankA = manager.CreateBank();
        bankA.AddMapping(10, carrier);
        bankA.AddMapping(11, depthA);
        bankA.AddMapping(12, depthB);
        auto& bankB = manager.CreateBank();
        bankB.AddMapping(20, depthB);
        bankB.AddMapping(21, carrier);
        auto& slot = manager.CreateBankSlot();
        for (const synth::PhysicalEncoderId encoder : {10u, 11u, 12u, 20u, 21u}) {
            slot.AddPhysicalEncoder(encoder);
        }
        slot.SelectBank(&bankA);

        const auto gridRange = synth::GridRange::Create(-2, 1, 6, 9);
        REQUIRE_TRUE(gridRange.has_value());
        synth::GridManager gridManager;
        const std::size_t gridA = *gridManager.CreateGrid(*gridRange);
        const std::size_t gridB = *gridManager.CreateGrid(*gridRange);
        const std::size_t gridSlot = *gridManager.CreateSlot(*gridRange);
        std::vector<std::string> gridEvents;
        REQUIRE_TRUE(gridManager.GridAt(gridA)->RegisterCell(
            -1, 7, std::make_unique<RecordingGridCell>(&gridEvents, 0)));
        REQUIRE_TRUE(gridManager.GridAt(gridB)->RegisterCell(
            -1, 7, std::make_unique<RecordingGridCell>(&gridEvents, 1)));
        int selectedGridOracle = -1;
        std::vector<std::string> gridEventsOracle;

        synth::MessageInBus bus(&manager, 256);
        bus.SetGridManager(&gridManager);
        auto ui = manager.CreateUIState();
        SimOracle oracle;
        SimInitializeOracle(oracle);
        const std::array<synth::Parameter*, kSimParams> params{&carrier, &depthA, &depthB};
        const std::array<synth::Bank*, 2> banks{&bankA, &bankB};
        const std::array<synth::PhysicalEncoderId, 6> encoders{10, 11, 12, 20, 21, 99};
        std::mt19937 rng(seed ^ 0xB05u);
        std::uniform_real_distribution<float> deltaDist(-0.18f, 0.18f);
        std::uniform_real_distribution<float> unipolarDist(0.0f, 1.0f);
        std::uint64_t timestamp = 1;
        SimRandomSamples randomSamples;
        std::mt19937 randomRng(seed ^ 0xBADC0DEu);
        manager.SetRandomSource(
            [&randomSamples]() { return randomSamples.PopValue(); },
            [&randomSamples]() { return randomSamples.PopCoin(); },
            [&randomSamples](std::size_t max) { return randomSamples.PopIndex(max); });

        SimCheck(oracle, params, banks, group, slot, manager, seed, -1, "initial bus");
        manager.PopulateUIState(*ui);
        SimCheckUIState(oracle, *ui, seed, -1, "initial bus ui");

        REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(timestamp, 32, true)));
        REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(timestamp, 32, 0.4f)));
        REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureSelect(timestamp, 63, true)));
        REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(timestamp, 63, 0.8f)));
        REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(timestamp, 0, 0, 0.05f)));
        bus.Process(timestamp++);
        SimSetGestureSelected(oracle, 32, true);
        oracle.gestureWeight[32] = 0.4f;
        SimSetGestureSelected(oracle, 63, true);
        oracle.gestureWeight[63] = 0.8f;
        SimHandleTick(oracle, encoders[0], 0.05f);
        SimCheck(oracle, params, banks, group, slot, manager, seed, -1,
                 "deterministic gestures=32,63 random-consumption(values=0/0,coins=0/0,indices=0/0)");
        manager.PopulateUIState(*ui);
        SimCheckUIState(oracle, *ui, seed, -1,
                        "deterministic gestures=32,63 ui random-consumption(values=0/0,coins=0/0,indices=0/0)");
        REQUIRE_TRUE((ui->slots[0].cells[0].gesturesAffectingMask.load() & (synth::GestureMask{1} << 32)) != 0);
        REQUIRE_TRUE((ui->slots[0].cells[0].gesturesAffectingMask.load() & (synth::GestureMask{1} << 63)) != 0);

        for (int step = 0; step < steps; ++step) {
            std::string action;
            randomSamples.Clear();
            auto modifierName = [](synth::Modifier modifier) {
                switch (modifier) {
                case synth::Modifier::None:
                    return "none";
                case synth::Modifier::Reset:
                    return "reset";
                case synth::Modifier::Random:
                    return "random";
                case synth::Modifier::RandomMod:
                    return "random-mod";
                }
                return "unknown";
            };
            auto setOracleHeld = [&](synth::Modifier modifier, bool held) {
                switch (modifier) {
                case synth::Modifier::Reset:
                    oracle.resetHeld = held;
                    break;
                case synth::Modifier::Random:
                    oracle.randomHeld = held;
                    break;
                case synth::Modifier::RandomMod:
                    oracle.randomModHeld = held;
                    break;
                case synth::Modifier::None:
                    break;
                }
            };
            auto setMessage = [&](synth::Modifier modifier, bool held) {
                switch (modifier) {
                case synth::Modifier::Reset:
                    return synth::MessageIn::SetReset(timestamp, held);
                case synth::Modifier::Random:
                    return synth::MessageIn::SetRandom(timestamp, held);
                case synth::Modifier::RandomMod:
                    return synth::MessageIn::SetRandomMod(timestamp, held);
                case synth::Modifier::None:
                    break;
                }
                return synth::MessageIn::Clock(timestamp);
            };
            auto handleBusPress = [&](std::size_t position, const std::string& prefix) {
                randomSamples.Clear();
                std::vector<std::string> samples;
                const synth::Modifier modifier = SimCurrentModifier(oracle);
                if (modifier == synth::Modifier::None) {
                    action = prefix + " press position " + std::to_string(position);
                    SimHandlePress(oracle, encoders[position]);
                } else {
                    SimHandleModifierPress(oracle, encoders[position], randomSamples, randomRng, &samples);
                    action = prefix + " " + modifierName(modifier) + " press position " + std::to_string(position) +
                             " samples=" + JoinSamples(samples);
                }
                REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(timestamp, 0, position)));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
            };
            auto handleMomentaryBusPress = [&](synth::Modifier modifier, std::size_t position) {
                randomSamples.Clear();
                std::vector<std::string> samples;
                setOracleHeld(modifier, true);
                SimHandleModifierPress(oracle, encoders[position], randomSamples, randomRng, &samples);
                action = std::string("bus ") + modifierName(modifier) + " press position " +
                         std::to_string(position) + " samples=" + JoinSamples(samples);
                REQUIRE_TRUE(bus.Push(setMessage(modifier, true)));
                REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamPush(timestamp, 0, position)));
                REQUIRE_TRUE(bus.Push(setMessage(modifier, false)));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
                setOracleHeld(modifier, false);
            };
            auto handleMomentaryBusBank = [&](synth::Modifier modifier, int bankIx) {
                randomSamples.Clear();
                std::vector<std::string> samples;
                setOracleHeld(modifier, true);
                SimApplyModifierToBank(oracle, bankIx, randomSamples, randomRng, &samples);
                action = std::string("bus ") + modifierName(modifier) + " modified bank " + std::to_string(bankIx) +
                         " samples=" + JoinSamples(samples);
                REQUIRE_TRUE(bus.Push(setMessage(modifier, true)));
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(timestamp, 0, static_cast<std::size_t>(bankIx))));
                REQUIRE_TRUE(bus.Push(setMessage(modifier, false)));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
                setOracleHeld(modifier, false);
            };
            switch (rng() % 32) {
            case 0: {
                const std::size_t position = rng() % encoders.size();
                const float delta = deltaDist(rng);
                action = "bus turn position " + std::to_string(position);
                REQUIRE_TRUE(bus.Push(synth::MessageIn::ParamIncDec(timestamp, 0, position, delta)));
                bus.Process(timestamp);
                if (SimCurrentModifier(oracle) == synth::Modifier::None) {
                    SimHandleTick(oracle, encoders[position], delta);
                }
                break;
            }
            case 1: {
                const std::size_t position = rng() % encoders.size();
                handleBusPress(position, "bus");
                break;
            }
            case 2: {
                const std::size_t position = rng() % encoders.size();
                handleMomentaryBusPress(synth::Modifier::Reset, position);
                break;
            }
            case 3: {
                const std::size_t position = rng() % encoders.size();
                handleMomentaryBusPress(synth::Modifier::Random, position);
                break;
            }
            case 4: {
                const std::size_t position = rng() % encoders.size();
                handleMomentaryBusPress(synth::Modifier::RandomMod, position);
                break;
            }
            case 5: {
                const int bankIx = static_cast<int>(rng() % 2);
                handleMomentaryBusBank(synth::Modifier::Reset, bankIx);
                break;
            }
            case 6: {
                const int bankIx = static_cast<int>(rng() % 2);
                handleMomentaryBusBank(synth::Modifier::Random, bankIx);
                break;
            }
            case 7: {
                const int bankIx = static_cast<int>(rng() % 2);
                handleMomentaryBusBank(synth::Modifier::RandomMod, bankIx);
                break;
            }
            case 8: {
                const bool held = (rng() % 2) == 0;
                action = std::string("bus set reset held ") + (held ? "true" : "false");
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetReset(timestamp, held)));
                bus.Process(timestamp);
                oracle.resetHeld = held;
                break;
            }
            case 9: {
                const bool held = (rng() % 2) == 0;
                action = std::string("bus set random held ") + (held ? "true" : "false");
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandom(timestamp, held)));
                bus.Process(timestamp);
                oracle.randomHeld = held;
                break;
            }
            case 10: {
                const bool held = (rng() % 2) == 0;
                action = std::string("bus set random-mod held ") + (held ? "true" : "false");
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetRandomMod(timestamp, held)));
                bus.Process(timestamp);
                oracle.randomModHeld = held;
                break;
            }
            case 11: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "bus toggle gesture " + std::to_string(gestureIx);
                REQUIRE_TRUE(bus.Push(synth::MessageIn::ToggleGestureSelect(timestamp, gestureIx)));
                bus.Process(timestamp);
                SimSetGestureSelected(oracle, gestureIx, !SimGestureSelected(oracle, gestureIx));
                break;
            }
            case 12: {
                const std::size_t gestureIx = rng() % kSimGestures;
                const float value = unipolarDist(rng);
                action = "bus set gesture value " + std::to_string(gestureIx);
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetGestureValue(timestamp, gestureIx, value)));
                bus.Process(timestamp);
                oracle.gestureWeight[gestureIx] = value;
                break;
            }
            case 13: {
                const int bankIx = static_cast<int>(rng() % 2);
                randomSamples.Clear();
                std::vector<std::string> samples;
                if (SimCurrentModifier(oracle) == synth::Modifier::None) {
                    action = "bus select bank " + std::to_string(bankIx);
                    SimSelectBank(oracle, bankIx);
                } else {
                    SimApplyModifierToBank(oracle, bankIx, randomSamples, randomRng, &samples);
                    action = std::string("bus ") + modifierName(SimCurrentModifier(oracle)) + " modified bank " +
                             std::to_string(bankIx) + " samples=" + JoinSamples(samples);
                }
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectParamBank(timestamp, 0, static_cast<std::size_t>(bankIx))));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
                break;
            }
            case 14: {
                const std::size_t sceneIx = rng() % kSimScenes;
                action = "bus scene";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(timestamp, sceneIx)));
                bus.Process(timestamp);
                SimSetLessSelectedScene(oracle, sceneIx);
                break;
            }
            case 15: {
                const float blend = unipolarDist(rng);
                action = "bus blend";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetSceneBlend(timestamp, blend)));
                bus.Process(timestamp);
                oracle.scene.blend = blend;
                break;
            }
            case 16: {
                const std::size_t voiceIx = rng() % kSimVoices;
                const std::size_t modIx = rng() % kSimMods;
                const float value = unipolarDist(rng);
                action = "change modulator";
                group.GetModulators().Value(voiceIx, modIx) = value;
                oracle.modulatorValue[voiceIx][modIx] = value;
                break;
            }
            case 17:
                action = "compute";
                for (synth::Parameter* parameter : params) {
                    parameter->Compute(manager.Scene());
                }
                SimComputeAll(oracle);
                break;
            case 18:
                action = "process lite";
                for (synth::Parameter* parameter : params) {
                    parameter->ProcessLite();
                }
                SimProcessLiteAll(oracle);
                break;
            case 19: {
                action = "bus inert transport";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::Clock(timestamp)));
                REQUIRE_TRUE(bus.Push(synth::MessageIn::Start(timestamp)));
                REQUIRE_TRUE(bus.Push(synth::MessageIn::Stop(timestamp)));
                bus.Process(timestamp);
                break;
            }
            case 20: {
                action = "bus invalid scene";
                const std::size_t previousLeft = manager.Scene().leftScene;
                const std::size_t previousRight = manager.Scene().rightScene;
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SceneSelect(timestamp, kSimScenes + 1)));
                bus.Process(timestamp);
                REQUIRE_TRUE(manager.Scene().leftScene == previousLeft);
                REQUIRE_TRUE(manager.Scene().rightScene == previousRight);
                break;
            }
            case 21: {
                selectedGridOracle = static_cast<int>(rng() % 2);
                action = "bus select grid " + std::to_string(selectedGridOracle);
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectGrid(
                    timestamp, gridSlot, static_cast<std::size_t>(selectedGridOracle))));
                bus.Process(timestamp);
                break;
            }
            case 22: {
                const std::uint8_t velocity = static_cast<std::uint8_t>(rng() % 256);
                action = "bus grid press";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPress(timestamp, gridSlot, -1, 7, velocity)));
                bus.Process(timestamp);
                if (selectedGridOracle >= 0) {
                    gridEventsOracle.push_back("press:" + std::to_string(selectedGridOracle) + ":" +
                                               std::to_string(velocity));
                }
                break;
            }
            case 23: {
                action = "bus grid release";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::GridRelease(timestamp, gridSlot, -1, 7)));
                bus.Process(timestamp);
                if (selectedGridOracle >= 0) {
                    gridEventsOracle.push_back("release:" + std::to_string(selectedGridOracle));
                }
                break;
            }
            case 24: {
                const std::uint8_t pressure = static_cast<std::uint8_t>(rng() % 256);
                action = "bus grid pressure";
                REQUIRE_TRUE(bus.Push(
                    synth::MessageIn::GridPressureChange(timestamp, gridSlot, -1, 7, pressure)));
                bus.Process(timestamp);
                if (selectedGridOracle >= 0) {
                    gridEventsOracle.push_back("pressure:" + std::to_string(selectedGridOracle) + ":" +
                                               std::to_string(pressure));
                }
                break;
            }
            case 25: {
                const bool missingSlot = (rng() % 2) == 0;
                action = missingSlot ? "bus select missing grid slot" : "bus select missing grid";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SelectGrid(
                    timestamp, missingSlot ? 99 : gridSlot, missingSlot ? gridA : 99)));
                bus.Process(timestamp);
                break;
            }
            case 26: {
                action = "bus grid event missing slot";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPress(timestamp, 99, -1, 7, 127)));
                bus.Process(timestamp);
                break;
            }
            case 27: {
                const int x = (rng() % 2) == 0 ? -100 : 100;
                action = "bus grid event signed out-of-range coordinate";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::GridPressureChange(timestamp, gridSlot, x, 7, 127)));
                bus.Process(timestamp);
                break;
            }
            case 28:
            case 29: {
                const synth::BankDirection direction = (rng() % 2) == 0
                    ? synth::BankDirection::Next
                    : synth::BankDirection::Previous;
                std::vector<std::string> samples;
                SimNavigateBank(oracle, 0, direction, randomSamples, randomRng, &samples);
                action = std::string("bus ") +
                    (direction == synth::BankDirection::Next ? "next bank" : "previous bank") +
                    " samples=" + JoinSamples(samples);
                REQUIRE_TRUE(bus.Push(direction == synth::BankDirection::Next
                                          ? synth::MessageIn::NextParamBank(timestamp, 0)
                                          : synth::MessageIn::PrevParamBank(timestamp, 0)));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
                break;
            }
            case 30: {
                const synth::BankDirection direction = (rng() % 2) == 0
                    ? synth::BankDirection::Next
                    : synth::BankDirection::Previous;
                constexpr std::size_t invalidSlot = 99;
                std::vector<std::string> samples;
                SimNavigateBank(oracle, invalidSlot, direction, randomSamples, randomRng, &samples);
                action = std::string("bus invalid slot ") + std::to_string(invalidSlot) + " " +
                    (direction == synth::BankDirection::Next ? "next bank" : "previous bank") +
                    " modifier=" + modifierName(SimCurrentModifier(oracle)) +
                    " samples=" + JoinSamples(samples);
                randomSamples.SetDiagnosticContext(seed, step, action);
                REQUIRE_TRUE(bus.Push(direction == synth::BankDirection::Next
                                          ? synth::MessageIn::NextParamBank(timestamp, invalidSlot)
                                          : synth::MessageIn::PrevParamBank(timestamp, invalidSlot)));
                bus.Process(timestamp);
                randomSamples.RequireDrained(seed, step, action);
                break;
            }
            default: {
                const std::uint8_t pressure = static_cast<std::uint8_t>(rng() % 256);
                const float blend = unipolarDist(rng);
                action = "bus interleaved parameter and grid messages";
                REQUIRE_TRUE(bus.Push(synth::MessageIn::SetSceneBlend(timestamp, blend)));
                REQUIRE_TRUE(bus.Push(
                    synth::MessageIn::GridPressureChange(timestamp, gridSlot, -1, 7, pressure)));
                bus.Process(timestamp);
                oracle.scene.blend = blend;
                if (selectedGridOracle >= 0) {
                    gridEventsOracle.push_back("pressure:" + std::to_string(selectedGridOracle) + ":" +
                                               std::to_string(pressure));
                }
                break;
            }
            }
            ++timestamp;
            action += " random-consumption(" + randomSamples.ConsumptionSummary() + ")";
            SimCheck(oracle, params, banks, group, slot, manager, seed, step, action);
            const synth::Grid* expectedGrid = selectedGridOracle < 0
                                                  ? nullptr
                                                  : gridManager.GridAt(static_cast<std::size_t>(selectedGridOracle));
            REQUIRE_TRUE(gridManager.SlotAt(gridSlot)->SelectedGrid() == expectedGrid);
            REQUIRE_TRUE(gridEvents == gridEventsOracle);
            if (step % 11 == 0) {
                manager.PopulateUIState(*ui);
                SimCheckUIState(oracle, *ui, seed, step, action);
            }
        }
    }
}

TEST_CASE(message_bus_sparse_lifecycle_model_tracks_pins_collection_reuse_and_patch_load) {
    constexpr unsigned seed = 0x5A17C0DEu;
    synth::ParameterManager manager;
    REQUIRE_TRUE(manager.SetGestureCount(64));
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 2,
                                       .numScenes = 2,
                                       .maxParameters = 2,
                                       .targetCenterAlpha = 1.0f});
    MarkAllModulatorsConnectedForUi(group);
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.25f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.75f});
    group.AddParameterStorageBatch(synth::MakeParameterStorageBatch(group.Config(), group.GestureCount(), 2));
    REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
    manager.SetSceneBlend(0.25f);

    auto& bank = manager.CreateBank();
    bank.AddMapping(10, first);
    bank.AddMapping(20, second);
    auto& slot = manager.CreateBankSlot();
    for (const synth::PhysicalEncoderId encoder : {10u, 11u, 12u, 20u}) {
        slot.AddPhysicalEncoder(encoder);
    }
    slot.SelectBank(&bank);
    synth::MessageInBus bus(&manager, 64);

    SimOracle oracle;
    auto fail = [&](int step, const std::string& action, const std::string& field,
                    std::size_t expected, std::size_t actual) {
        std::ostringstream oss;
        oss << "seed " << seed << " step " << step << " action " << action
            << " random-consumption(values=0/0,coins=0/0,indices=0/0) " << field
            << " expected " << expected << " got " << actual;
        throw std::runtime_error(oss.str());
    };
    auto checkCounts = [&](int step, const std::string& action) {
        if (group.LiveLocalParameterCount() != oracle.liveLocalCount) {
            fail(step, action, "live local count", oracle.liveLocalCount, group.LiveLocalParameterCount());
        }
        if (group.FreeLocalParameterSlotCount() != oracle.freeLocalCount) {
            fail(step, action, "free local count", oracle.freeLocalCount, group.FreeLocalParameterSlotCount());
        }
    };
    std::uint64_t timestamp = 1;
    auto push = [&](const synth::MessageIn& message) {
        REQUIRE_TRUE(bus.Push(message));
        bus.Process(timestamp);
        ++timestamp;
    };

    push(synth::MessageIn::ParamPush(timestamp, 0, 0)); // open First
    oracle.liveLocalCount = 2;
    for (std::size_t sourceIx = 0; sourceIx < 2; ++sourceIx) {
        oracle.localSlots[sourceIx] = {
            .storageIdentity = 2 + sourceIx,
            .parentParameter = 0,
            .sourceIx = sourceIx,
            .live = true,
            .free = false,
            .pinned = true,
        };
    }
    checkCounts(0, "open first modulation view");
    REQUIRE_TRUE(bank.ShowingModulation());
    synth::Parameter* firstSource0 = bank.VisibleParameter(10);
    synth::Parameter* firstSource1 = bank.VisibleParameter(11);
    REQUIRE_TRUE(firstSource0 == &group.ParameterByLocalIndex(oracle.localSlots[0].storageIdentity));
    REQUIRE_TRUE(firstSource1 == &group.ParameterByLocalIndex(oracle.localSlots[1].storageIdentity));

    push(synth::MessageIn::SetReset(timestamp, true));
    push(synth::MessageIn::ParamPush(timestamp, 0, 0)); // reset visible local through the bus
    push(synth::MessageIn::SetReset(timestamp, false));
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0); // the model's pinned guard is observable
    checkCounts(1, "reset pinned local and collect");

    push(synth::MessageIn::ParamPush(timestamp, 0, 3)); // close First view
    oracle.liveLocalCount = 0;
    oracle.freeLocalCount = 2;
    for (SimLocalSlot& local : oracle.localSlots) {
        local.live = false;
        local.free = true;
        local.pinned = false;
        local.parentParameter = -1;
    }
    checkCounts(2, "close first view and collect neutral locals");
    REQUIRE_TRUE(!bank.ShowingModulation());

    push(synth::MessageIn::ParamPush(timestamp, 0, 3)); // open Second using recycled slots
    oracle.liveLocalCount = 2;
    oracle.freeLocalCount = 0;
    oracle.localSlots[1].parentParameter = 1;
    oracle.localSlots[1].sourceIx = 0;
    oracle.localSlots[1].live = true;
    oracle.localSlots[1].free = false;
    oracle.localSlots[1].pinned = true;
    oracle.localSlots[0].parentParameter = 1;
    oracle.localSlots[0].sourceIx = 1;
    oracle.localSlots[0].live = true;
    oracle.localSlots[0].free = false;
    oracle.localSlots[0].pinned = true;
    checkCounts(3, "open second view with distinct-parent reuse");
    REQUIRE_TRUE(bank.VisibleParameter(10) == firstSource1);
    REQUIRE_TRUE(bank.VisibleParameter(11) == firstSource0);

    push(synth::MessageIn::SetGestureSelect(timestamp, 63, true));
    push(synth::MessageIn::SetGestureValue(timestamp, 63, 1.0f));
    push(synth::MessageIn::ParamIncDec(timestamp, 0, 0, 0.1f)); // arm gesture 63
    push(synth::MessageIn::ParamIncDec(timestamp, 0, 0, 0.1f)); // edit the armed gesture
    synth::Parameter* retained = bank.VisibleParameter(10);
    REQUIRE_TRUE(retained != nullptr);
    REQUIRE_TRUE(retained->GestureActive(0, 63));
    REQUIRE_TRUE(retained->GestureActive(1, 63));
    REQUIRE_TRUE(retained->GestureValue(0, 63) != 0.5f || retained->GestureValue(1, 63) != 0.5f);
    const float savedGesture0 = retained->GestureValue(0, 63);
    const float savedGesture1 = retained->GestureValue(1, 63);

    push(synth::MessageIn::ParamPush(timestamp, 0, 3)); // close Second view
    oracle.liveLocalCount = 1;
    oracle.freeLocalCount = 1;
    oracle.localSlots[1].pinned = false;
    oracle.localSlots[0].live = false;
    oracle.localSlots[0].free = true;
    oracle.localSlots[0].pinned = false;
    oracle.localSlots[0].parentParameter = -1;
    checkCounts(4, "close second view and retain non-default gesture route");
    REQUIRE_TRUE(second.ModulationDepthParameter(0) == retained);
    REQUIRE_TRUE(second.ModulationDepthParameter(1) == nullptr);

    synth::JsonArena patchArena(262144);
    synth::MidiInstrumentConfig instrument;
    synth::AudioDeviceState audio;
    const synth::JSON patch = synth::BuildPatchJSON(patchArena, "Lifecycle", manager, instrument, audio);
    REQUIRE_TRUE(!patchArena.Failed());
    manager.RevertAllToDefaults();
    oracle.liveLocalCount = 0;
    oracle.freeLocalCount = 2;
    checkCounts(5, "revert all and collect");
    REQUIRE_TRUE(second.ModulationDepthParameter(0) == nullptr);

    REQUIRE_TRUE(synth::LoadPatchJSON(patch, manager, instrument, &audio));
    oracle.liveLocalCount = 1;
    oracle.freeLocalCount = 1;
    checkCounts(6, "patch load rematerializes retained route and collects omissions");
    synth::Parameter* loaded = second.ModulationDepthParameter(0);
    REQUIRE_TRUE(loaded != nullptr);
    REQUIRE_TRUE(loaded->GestureActive(0, 63));
    REQUIRE_TRUE(loaded->GestureActive(1, 63));
    REQUIRE_NEAR(loaded->GestureValue(0, 63), savedGesture0, 0.000001f);
    REQUIRE_NEAR(loaded->GestureValue(1, 63), savedGesture1, 0.000001f);
}

TEST_CASE(randomized_patch_lifecycle_simulation) {
    const std::vector<unsigned> seeds = SimSeedsFromEnvironment();
    const int steps = SimStepsFromEnvironmentOrDefault(260);

    for (const unsigned seed : seeds) {
        const std::filesystem::path tempRoot =
            std::filesystem::temp_directory_path() / ("sheaf-synth-patch-random-" + std::to_string(seed));
        std::filesystem::remove_all(tempRoot);
        std::filesystem::create_directories(tempRoot);

        synth::ParameterManager manager;
        manager.SetGestureCount(kSimGestures);
        auto& group = manager.CreateGroup({
            .numVoices = kSimVoices,
            .numModulators = kSimMods,
            .numScenes = kSimScenes,
            .maxParameters = kSimParams,
            .processLiteAlpha = 0.25f,
            .targetCenterAlpha = 1.0f,
        });
        MarkAllModulatorsConnectedForUi(group);
        auto& carrier = manager.CreateParameter(group, {
            .name = "Carrier",
            .defaultValue = 0.35f,
            .switchValues = 5,
        });
        auto& depthA = manager.CreateParameter(
            group, {.name = "DepthA", .defaultValue = 0.55f, .range = synth::RangeKind::Bipolar});
        auto& depthB = manager.CreateParameter(
            group, {
                .name = "DepthB",
                .defaultValue = 0.4f,
                .range = synth::RangeKind::Bipolar,
                .switchValues = 3,
            });
        REQUIRE_TRUE(carrier.AssignModulationDepth(0, &depthA));
        REQUIRE_TRUE(carrier.AssignModulationDepth(1, &depthB));
        REQUIRE_TRUE(depthA.AssignModulationDepth(0, &depthB));
        REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
        manager.SetSceneBlend(0.25f);

        auto& pageA = manager.CreatePage("A");
        auto& pageB = manager.CreatePage("B");
        manager.AssignParameterToPage(pageA.ordinal, carrier);
        manager.AssignParameterToPage(pageB.ordinal, depthA);
        manager.SelectActivePage(pageA.ordinal);

        auto& bankA = manager.CreateBank();
        bankA.AddMapping(10, carrier);
        bankA.AddMapping(11, depthA);
        bankA.AddMapping(12, depthB);
        auto& bankB = manager.CreateBank();
        bankB.AddMapping(20, depthB);
        bankB.AddMapping(21, carrier);
        auto& slot = manager.CreateBankSlot();
        for (const synth::PhysicalEncoderId encoder : {10u, 11u, 12u, 20u, 21u}) {
            slot.AddPhysicalEncoder(encoder);
        }
        slot.SelectBank(&bankA);
        manager.CaptureDefaultControlState();

        synth::WrldBldrDefaultProfileOptions midiOptions;
        midiOptions.visibleEncoderCount = 5;
        midiOptions.sceneCount = kSimScenes;
        midiOptions.bankButtonCount = 2;
        midiOptions.gestureSelectorCount = kSimGestures;
        const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(midiOptions);
        const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
        synth::MidiInstrumentConfig instrument = defaultInstrument;
        synth::AudioDeviceState defaultAudioDevice;
        synth::AudioDeviceState audioDevice;
        synth::PatchMessageInBus inputBus(32);
        synth::MessageOutBus outputBus(32);
        synth::PatchManager patchManager(&inputBus, &outputBus);

        SimOracle oracle;
        SimInitializeOracle(oracle);
        const std::array<synth::Parameter*, kSimParams> params{&carrier, &depthA, &depthB};
        const std::array<synth::Bank*, 2> banks{&bankA, &bankB};
        const std::array<synth::PhysicalEncoderId, 6> encoders{10, 11, 12, 20, 21, 99};
        std::vector<std::pair<std::filesystem::path, SimPatchSnapshot>> savedVersions;
        std::optional<std::filesystem::path> expectedCurrentPatchDir;
        std::mt19937 rng(seed ^ 0x9A7C4u);
        std::uniform_real_distribution<float> deltaDist(-0.24f, 0.24f);
        std::uniform_real_distribution<float> unipolarDist(0.0f, 1.0f);
        int patchNameCounter = 0;
        int writeCounter = 0;

        auto processPatchMessages = [&] {
            synth::PatchMessageIn message;
            while (inputBus.Pop(message)) {
                const synth::PatchApplyStatus status =
                    synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                             audioDevice, defaultAudioDevice, outputBus);
                REQUIRE_TRUE(status == synth::PatchApplyStatus::Applied ||
                             status == synth::PatchApplyStatus::Reverted ||
                             status == synth::PatchApplyStatus::Serialized);
            }
        };

        auto completePendingSave = [&](const SimPatchSnapshot& snapshot) {
            processPatchMessages();
            const auto now = std::chrono::system_clock::from_time_t(1700001000 + writeCounter++);
            const synth::PatchCommandResult completion = patchManager.ProcessResponses(now);
            REQUIRE_TRUE(completion.status == synth::PatchCommandStatus::Written);
            savedVersions.push_back({completion.path, snapshot});
            expectedCurrentPatchDir = completion.path.parent_path();
            REQUIRE_TRUE(patchManager.CurrentPatchDirectory().has_value());
            REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == *expectedCurrentPatchDir);
            return completion.path;
        };

        auto saveAsCurrentOracle = [&] {
            const std::filesystem::path patchDir = tempRoot / ("Patch-" + std::to_string(patchNameCounter++));
            const SimPatchSnapshot snapshot = SimCapturePatchSnapshot(oracle);
            const synth::PatchCommandResult result = patchManager.SavePatchAs(patchDir);
            REQUIRE_TRUE(result.status == synth::PatchCommandStatus::Pending);
            completePendingSave(snapshot);
        };

        auto saveCurrentOracle = [&] {
            if (!expectedCurrentPatchDir.has_value()) {
                saveAsCurrentOracle();
                return;
            }
            const SimPatchSnapshot snapshot = SimCapturePatchSnapshot(oracle);
            const synth::PatchCommandResult result = patchManager.SavePatch();
            REQUIRE_TRUE(result.status == synth::PatchCommandStatus::Pending);
            completePendingSave(snapshot);
        };

        auto loadPatchPath = [&](const std::filesystem::path& path, const SimPatchSnapshot& snapshot,
                                const std::filesystem::path& expectedDir) {
            const synth::PatchCommandResult result = patchManager.LoadPatch(path);
            REQUIRE_TRUE(result.status == synth::PatchCommandStatus::Ok);
            processPatchMessages();
            SimApplyPatchSnapshot(oracle, snapshot);
            expectedCurrentPatchDir = expectedDir;
            REQUIRE_TRUE(patchManager.CurrentPatchDirectory().has_value());
            REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == expectedDir);
        };

        saveAsCurrentOracle();
        SimCheck(oracle, params, banks, group, slot, manager, seed, -1, "initial patch save");

        for (int step = 0; step < steps; ++step) {
            std::string action;
            switch (rng() % 22) {
            case 0:
            case 1:
            case 2: {
                const auto encoder = encoders[rng() % encoders.size()];
                const float delta = deltaDist(rng);
                action = "patch turn encoder " + std::to_string(encoder);
                manager.HandleTick(encoder, delta);
                SimHandleTick(oracle, encoder, delta);
                break;
            }
            case 3: {
                const auto encoder = encoders[rng() % encoders.size()];
                action = "patch press encoder " + std::to_string(encoder);
                manager.HandlePress(encoder);
                SimHandlePress(oracle, encoder);
                break;
            }
            case 4: {
                const auto encoder = encoders[rng() % encoders.size()];
                action = "patch reset press " + std::to_string(encoder);
                SimRandomSamples resetSamples;
                std::mt19937 resetRng(seed ^ static_cast<unsigned>(step));
                manager.SetResetHeld(true);
                oracle.resetHeld = true;
                SimHandleModifierPress(oracle, encoder, resetSamples, resetRng, nullptr);
                manager.HandlePress(encoder);
                resetSamples.RequireDrained(seed, step, action);
                manager.SetResetHeld(false);
                oracle.resetHeld = false;
                break;
            }
            case 5: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "patch select gesture " + std::to_string(gestureIx);
                manager.SelectGesture(gestureIx);
                SimSetGestureSelected(oracle, gestureIx, true);
                break;
            }
            case 6: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "patch deselect gesture " + std::to_string(gestureIx);
                manager.DeselectGesture(gestureIx);
                SimSetGestureSelected(oracle, gestureIx, false);
                break;
            }
            case 7: {
                const std::size_t gestureIx = rng() % kSimGestures;
                const float value = unipolarDist(rng);
                action = "patch set gesture value " + std::to_string(gestureIx);
                manager.SetGestureValue(gestureIx, value);
                oracle.gestureWeight[gestureIx] = value;
                break;
            }
            case 8: {
                const int bankIx = static_cast<int>(rng() % 2);
                action = "patch select bank " + std::to_string(bankIx);
                slot.SelectBank(banks[static_cast<std::size_t>(bankIx)]);
                SimSelectBank(oracle, bankIx);
                break;
            }
            case 9: {
                const std::size_t sceneIx = rng() % kSimScenes;
                action = "patch scene";
                REQUIRE_TRUE(manager.SetLessSelectedScene(sceneIx));
                SimSetLessSelectedScene(oracle, sceneIx);
                break;
            }
            case 10: {
                const float blend = unipolarDist(rng);
                action = "patch blend";
                manager.SetSceneBlend(blend);
                oracle.scene.blend = blend;
                break;
            }
            case 11: {
                const std::size_t voiceIx = rng() % kSimVoices;
                const std::size_t modIx = rng() % kSimMods;
                const float value = unipolarDist(rng);
                action = "patch modulator";
                group.GetModulators().Value(voiceIx, modIx) = value;
                oracle.modulatorValue[voiceIx][modIx] = value;
                break;
            }
            case 12:
                action = "patch compute";
                for (synth::Parameter* parameter : params) {
                    parameter->Compute(manager.Scene());
                }
                SimComputeAll(oracle);
                break;
            case 13:
                action = "patch process lite";
                for (synth::Parameter* parameter : params) {
                    parameter->ProcessLite();
                }
                SimProcessLiteAll(oracle);
                break;
            case 14: {
                const std::size_t gestureIx = rng() % kSimGestures;
                action = "patch clear active gesture " + std::to_string(gestureIx);
                manager.ClearGestureActiveFlagsForActiveSceneSelection(gestureIx);
                const float blend = std::clamp(oracle.scene.blend, 0.0f, 1.0f);
                for (auto& parameter : oracle.params) {
                    if (blend <= 0.0f) {
                        SimSetGestureActive(parameter, oracle.scene.leftScene, gestureIx, false);
                    } else if (blend >= 1.0f) {
                        SimSetGestureActive(parameter, oracle.scene.rightScene, gestureIx, false);
                    } else {
                        SimSetGestureActive(parameter, oracle.scene.leftScene, gestureIx, false);
                        if (oracle.scene.rightScene != oracle.scene.leftScene) {
                            SimSetGestureActive(parameter, oracle.scene.rightScene, gestureIx, false);
                        }
                    }
                }
                break;
            }
            case 15:
            case 16:
                action = "patch save";
                saveCurrentOracle();
                break;
            case 17:
                action = "patch save as";
                saveAsCurrentOracle();
                break;
            case 18: {
                action = "patch load directory";
                const std::size_t versionIx = rng() % savedVersions.size();
                const std::filesystem::path dir = savedVersions[versionIx].first.parent_path();
                const std::size_t latestIx = SimFindLatestPatchInDirectory(savedVersions, dir);
                loadPatchPath(dir, savedVersions[latestIx].second, dir);
                break;
            }
            case 19: {
                action = "patch load version";
                const std::size_t versionIx = rng() % savedVersions.size();
                const auto& [path, snapshot] = savedVersions[versionIx];
                loadPatchPath(path, snapshot, path.parent_path());
                break;
            }
            case 20:
                action = "patch revert";
                if (!expectedCurrentPatchDir.has_value()) {
                    REQUIRE_TRUE(patchManager.RevertPatch().status == synth::PatchCommandStatus::Ok);
                    processPatchMessages();
                    SimApplyNewPatch(oracle);
                } else {
                    const std::size_t latestIx = SimFindLatestPatchInDirectory(savedVersions, *expectedCurrentPatchDir);
                    REQUIRE_TRUE(patchManager.RevertPatch().status == synth::PatchCommandStatus::Ok);
                    processPatchMessages();
                    SimApplyPatchSnapshot(oracle, savedVersions[latestIx].second);
                }
                break;
            default:
                action = "patch new";
                REQUIRE_TRUE(patchManager.NewPatch().status == synth::PatchCommandStatus::Ok);
                processPatchMessages();
                expectedCurrentPatchDir.reset();
                SimApplyNewPatch(oracle);
                REQUIRE_TRUE(!patchManager.CurrentPatchDirectory().has_value());
                break;
            }

            SimCheck(oracle, params, banks, group, slot, manager, seed, step, action);
        }

        std::filesystem::remove_all(tempRoot);
    }
}

TEST_CASE(randomized_patch_lifecycle_preserves_recursive_local_modulation_depths) {
    struct ParamValueSnapshot {
        std::array<float, kSimScenes> sceneCenter{};
        std::array<std::array<float, kSimGestures>, kSimScenes> gestureValue{};
        std::array<std::array<bool, kSimGestures>, kSimScenes> gestureActive{};
    };

    struct RecursivePatchSnapshot {
        std::vector<ParamValueSnapshot> values;
    };

    auto captureValues = [](const std::vector<synth::Parameter*>& parameters) {
        RecursivePatchSnapshot snapshot;
        snapshot.values.resize(parameters.size());
        for (std::size_t paramIx = 0; paramIx < parameters.size(); ++paramIx) {
            for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
                snapshot.values[paramIx].sceneCenter[sceneIx] = parameters[paramIx]->SceneCenter(sceneIx);
                for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                    snapshot.values[paramIx].gestureValue[sceneIx][gestureIx] =
                        parameters[paramIx]->GestureValue(sceneIx, gestureIx);
                    snapshot.values[paramIx].gestureActive[sceneIx][gestureIx] =
                        parameters[paramIx]->GestureActive(sceneIx, gestureIx);
                }
            }
        }
        return snapshot;
    };

    auto checkValues = [](const std::vector<synth::Parameter*>& parameters, const RecursivePatchSnapshot& expected,
                          unsigned seed, int step, const std::string& action) {
        for (std::size_t paramIx = 0; paramIx < parameters.size(); ++paramIx) {
            for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
                SimCheckNear(seed, step, action,
                             "recursive paramIx=" + std::to_string(paramIx) + " sceneIx=" +
                                 std::to_string(sceneIx) + " center",
                             expected.values[paramIx].sceneCenter[sceneIx],
                             parameters[paramIx]->SceneCenter(sceneIx));
                for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                    const std::string field = "recursive paramIx=" + std::to_string(paramIx) + " sceneIx=" +
                                              std::to_string(sceneIx) + " gestureIx=" +
                                              std::to_string(gestureIx);
                    SimCheckNear(seed, step, action, field + " value",
                                 expected.values[paramIx].gestureValue[sceneIx][gestureIx],
                                 parameters[paramIx]->GestureValue(sceneIx, gestureIx));
                    if (expected.values[paramIx].gestureActive[sceneIx][gestureIx] !=
                        parameters[paramIx]->GestureActive(sceneIx, gestureIx)) {
                        SimFailBool(seed, step, action, field + " active");
                    }
                }
            }
        }
    };

    auto findLatestRecursive = [](const std::vector<std::pair<std::filesystem::path, RecursivePatchSnapshot>>& versions,
                                  const std::filesystem::path& patchDir) {
        for (std::size_t reverseIx = versions.size(); reverseIx > 0; --reverseIx) {
            const std::size_t ix = reverseIx - 1;
            if (versions[ix].first.parent_path() == patchDir) {
                return ix;
            }
        }
        throw std::runtime_error("recursive patch directory not found: " + patchDir.string());
    };

    const std::vector<unsigned> seeds = SimSeedsFromEnvironment();
    const int steps = SimStepsFromEnvironmentOrDefault(220);

    for (const unsigned seed : seeds) {
        const std::filesystem::path tempRoot =
            std::filesystem::temp_directory_path() / ("sheaf-synth-recursive-patch-random-" + std::to_string(seed));
        std::filesystem::remove_all(tempRoot);
        std::filesystem::create_directories(tempRoot);

        synth::ParameterManager manager;
        manager.SetGestureCount(kSimGestures);
        auto& group = manager.CreateGroup({
            .numVoices = kSimVoices,
            .numModulators = kSimMods,
            .numScenes = kSimScenes,
            .maxParameters = 8,
            .processLiteAlpha = 0.25f,
            .targetCenterAlpha = 1.0f,
        });
        MarkAllModulatorsConnectedForUi(group);
        auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.35f});
        auto& resonance = manager.CreateParameter(
            group, {.name = "Resonance", .defaultValue = 0.55f, .range = synth::RangeKind::Bipolar});
        auto& cutoffLfo = cutoff.EnsureModulationDepth(
            0, {.name = "Cutoff LFO", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
        auto& cutoffEnv = cutoff.EnsureModulationDepth(
            1, {.name = "Cutoff Env", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
        auto& lfoCurve = cutoffLfo.EnsureModulationDepth(
            2, {.name = "Cutoff LFO Curve", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
        auto& resonanceLfo = resonance.EnsureModulationDepth(
            0, {.name = "Resonance LFO", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
        REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
        manager.SetSceneBlend(0.25f);
        manager.CaptureDefaultControlState();

        std::vector<synth::Parameter*> tracked{&cutoff, &resonance, &cutoffLfo, &cutoffEnv, &lfoCurve, &resonanceLfo};
        auto refreshTrackedTopology = [&] {
            synth::Parameter* liveCutoffLfo = cutoff.EnsureModulationDepth(0);
            synth::Parameter* liveCutoffEnv = cutoff.EnsureModulationDepth(1);
            synth::Parameter* liveResonanceLfo = resonance.EnsureModulationDepth(0);
            REQUIRE_TRUE(liveCutoffLfo != nullptr);
            REQUIRE_TRUE(liveCutoffEnv != nullptr);
            REQUIRE_TRUE(liveResonanceLfo != nullptr);
            synth::Parameter* liveLfoCurve = liveCutoffLfo->EnsureModulationDepth(2);
            REQUIRE_TRUE(liveLfoCurve != nullptr);
            tracked = {&cutoff, &resonance, liveCutoffLfo, liveCutoffEnv, liveLfoCurve, liveResonanceLfo};
        };
        RecursivePatchSnapshot expected = captureValues(tracked);
        const RecursivePatchSnapshot defaultExpected = expected;

        synth::WrldBldrDefaultProfileOptions midiOptions;
        midiOptions.visibleEncoderCount = 5;
        midiOptions.sceneCount = kSimScenes;
        midiOptions.bankButtonCount = 2;
        midiOptions.gestureSelectorCount = kSimGestures;
        const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(midiOptions);
        const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
        synth::MidiInstrumentConfig instrument = defaultInstrument;
        synth::AudioDeviceState defaultAudioDevice;
        synth::AudioDeviceState audioDevice;
        synth::PatchMessageInBus inputBus(32);
        synth::MessageOutBus outputBus(32);
        synth::PatchManager patchManager(&inputBus, &outputBus);
        std::vector<std::pair<std::filesystem::path, RecursivePatchSnapshot>> savedVersions;
        std::optional<std::filesystem::path> expectedCurrentPatchDir;
        std::mt19937 rng(seed ^ 0xD33F5u);
        std::uniform_real_distribution<float> unipolarDist(0.0f, 1.0f);
        int patchNameCounter = 0;
        int writeCounter = 0;

        auto processPatchMessages = [&] {
            synth::PatchMessageIn message;
            while (inputBus.Pop(message)) {
                const synth::PatchApplyStatus status =
                    synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                             audioDevice, defaultAudioDevice, outputBus);
                REQUIRE_TRUE(status == synth::PatchApplyStatus::Applied ||
                             status == synth::PatchApplyStatus::Reverted ||
                             status == synth::PatchApplyStatus::Serialized);
            }
            refreshTrackedTopology();
        };

        auto completePendingSave = [&](const RecursivePatchSnapshot& snapshot) {
            processPatchMessages();
            const auto now = std::chrono::system_clock::from_time_t(1700005000 + writeCounter++);
            const synth::PatchCommandResult completion = patchManager.ProcessResponses(now);
            REQUIRE_TRUE(completion.status == synth::PatchCommandStatus::Written);
            savedVersions.push_back({completion.path, snapshot});
            expectedCurrentPatchDir = completion.path.parent_path();
        };

        auto saveAsExpected = [&] {
            const std::filesystem::path patchDir = tempRoot / ("Patch-" + std::to_string(patchNameCounter++));
            const synth::PatchCommandResult result = patchManager.SavePatchAs(patchDir);
            REQUIRE_TRUE(result.status == synth::PatchCommandStatus::Pending);
            completePendingSave(expected);
        };

        auto saveExpected = [&] {
            if (!expectedCurrentPatchDir.has_value()) {
                saveAsExpected();
                return;
            }
            const synth::PatchCommandResult result = patchManager.SavePatch();
            REQUIRE_TRUE(result.status == synth::PatchCommandStatus::Pending);
            completePendingSave(expected);
        };

        saveAsExpected();
        checkValues(tracked, expected, seed, -1, "initial recursive save");

        for (int step = 0; step < steps; ++step) {
            std::string action;
            switch (rng() % 12) {
            case 0:
            case 1:
            case 2: {
                const std::size_t paramIx = rng() % tracked.size();
                const std::size_t sceneIx = rng() % kSimScenes;
                const float value = unipolarDist(rng);
                action = "recursive set scene";
                tracked[paramIx]->SceneCenter(sceneIx) = value;
                expected.values[paramIx].sceneCenter[sceneIx] = value;
                manager.ComputeAllParameters();
                break;
            }
            case 3: {
                const std::size_t paramIx = rng() % tracked.size();
                const std::size_t sceneIx = rng() % kSimScenes;
                const std::size_t gestureIx = rng() % kSimGestures;
                const float value = unipolarDist(rng);
                action = "recursive set gesture value";
                tracked[paramIx]->GestureValue(sceneIx, gestureIx) = value;
                expected.values[paramIx].gestureValue[sceneIx][gestureIx] = value;
                manager.ComputeAllParameters();
                break;
            }
            case 4: {
                const std::size_t paramIx = rng() % tracked.size();
                const std::size_t sceneIx = rng() % kSimScenes;
                const std::size_t gestureIx = rng() % kSimGestures;
                const bool active = (rng() % 2) == 0;
                action = "recursive set gesture active";
                tracked[paramIx]->SetGestureActive(sceneIx, gestureIx, active);
                expected.values[paramIx].gestureActive[sceneIx][gestureIx] = active;
                manager.ComputeAllParameters();
                break;
            }
            case 5:
                action = "recursive compute";
                manager.ComputeAllParameters();
                break;
            case 6:
                action = "recursive save";
                saveExpected();
                break;
            case 7:
                action = "recursive save as";
                saveAsExpected();
                break;
            case 8: {
                action = "recursive load directory";
                const std::size_t versionIx = rng() % savedVersions.size();
                const std::filesystem::path dir = savedVersions[versionIx].first.parent_path();
                const std::size_t latestIx = findLatestRecursive(savedVersions, dir);
                REQUIRE_TRUE(patchManager.LoadPatch(dir).status == synth::PatchCommandStatus::Ok);
                processPatchMessages();
                expectedCurrentPatchDir = dir;
                expected = savedVersions[latestIx].second;
                checkValues(tracked, expected, seed, step, action);
                continue;
            }
            case 9: {
                action = "recursive load version";
                const std::size_t versionIx = rng() % savedVersions.size();
                REQUIRE_TRUE(patchManager.LoadPatch(savedVersions[versionIx].first).status == synth::PatchCommandStatus::Ok);
                processPatchMessages();
                expectedCurrentPatchDir = savedVersions[versionIx].first.parent_path();
                expected = savedVersions[versionIx].second;
                checkValues(tracked, expected, seed, step, action);
                continue;
            }
            case 10:
                action = "recursive revert";
                if (!expectedCurrentPatchDir.has_value()) {
                    REQUIRE_TRUE(patchManager.RevertPatch().status == synth::PatchCommandStatus::Ok);
                    processPatchMessages();
                    expected = defaultExpected;
                } else {
                    const std::size_t latestIx = findLatestRecursive(savedVersions, *expectedCurrentPatchDir);
                    REQUIRE_TRUE(patchManager.RevertPatch().status == synth::PatchCommandStatus::Ok);
                    processPatchMessages();
                    expected = savedVersions[latestIx].second;
                }
                break;
            default:
                action = "recursive new";
                REQUIRE_TRUE(patchManager.NewPatch().status == synth::PatchCommandStatus::Ok);
                processPatchMessages();
                expectedCurrentPatchDir.reset();
                expected = defaultExpected;
                break;
            }

            checkValues(tracked, expected, seed, step, action);
        }

        std::filesystem::remove_all(tempRoot);
    }
}

TEST_CASE(randomized_recursive_modulation_ui_tree_round_trips_into_fresh_initialization) {
    struct TreeEntry {
        std::string path;
        std::string name;
        std::array<float, kSimScenes> sceneCenter{};
        std::array<std::array<float, kSimGestures>, kSimScenes> gestureValue{};
        std::array<std::array<bool, kSimGestures>, kSimScenes> gestureActive{};
    };

    auto captureParameter = [](auto& self, const synth::Parameter& parameter, const std::string& path,
                               std::vector<TreeEntry>& entries) -> void {
        TreeEntry entry;
        entry.path = path;
        entry.name = parameter.Name();
        for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
            entry.sceneCenter[sceneIx] = parameter.SceneCenter(sceneIx);
            for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                entry.gestureValue[sceneIx][gestureIx] = parameter.GestureValue(sceneIx, gestureIx);
                entry.gestureActive[sceneIx][gestureIx] = parameter.GestureActive(sceneIx, gestureIx);
            }
        }
        entries.push_back(entry);
        for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
            if (const synth::Parameter* child = parameter.ModulationDepthParameter(modIx); child != nullptr) {
                self(self, *child, path + "/" + std::to_string(modIx), entries);
            }
        }
    };

    auto captureTree = [&](const std::array<synth::Parameter*, 2>& roots) {
        std::vector<TreeEntry> entries;
        for (std::size_t rootIx = 0; rootIx < roots.size(); ++rootIx) {
            captureParameter(captureParameter, *roots[rootIx], std::to_string(rootIx), entries);
        }
        std::sort(entries.begin(), entries.end(), [](const TreeEntry& lhs, const TreeEntry& rhs) {
            return lhs.path < rhs.path;
        });
        return entries;
    };

    auto defaultValueForPath = [](const std::string& path) {
        if (path == "0") {
            return 0.25f;
        }
        if (path == "1") {
            return 0.5f;
        }
        return 0.5f;
    };

    auto parameterHasPersistedState = [&](auto& self, const synth::Parameter& parameter,
                                          const std::string& path) -> bool {
        constexpr float tolerance = 0.000001f;
        const float defaultValue = defaultValueForPath(path);
        for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
            if (std::fabs(parameter.SceneCenter(sceneIx) - defaultValue) > tolerance) {
                return true;
            }
            for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                if (std::fabs(parameter.GestureValue(sceneIx, gestureIx) - defaultValue) > tolerance ||
                    parameter.GestureActive(sceneIx, gestureIx)) {
                    return true;
                }
            }
        }
        for (std::size_t voiceIx = 0; voiceIx < kSimVoices; ++voiceIx) {
            for (const float depth : parameter.CurrentDepthSlots(voiceIx)) {
                if (std::fabs(depth) > tolerance) {
                    return true;
                }
            }
            for (const float depth : parameter.TargetDepthSlots(voiceIx)) {
                if (std::fabs(depth) > tolerance) {
                    return true;
                }
            }
            if (std::fabs(parameter.CurrentCenterScale(voiceIx) - 1.0f) > tolerance ||
                std::fabs(parameter.TargetCenterScale(voiceIx) - 1.0f) > tolerance ||
                std::fabs(parameter.CurrentNormalizationOffset(voiceIx)) > tolerance ||
                std::fabs(parameter.TargetNormalizationOffset(voiceIx)) > tolerance) {
                return true;
            }
        }
        for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
            if (const synth::Parameter* child = parameter.ModulationDepthParameter(modIx); child != nullptr) {
                if (self(self, *child, path + "/" + std::to_string(modIx))) {
                    return true;
                }
            }
        }
        return false;
    };

    auto capturePersistedTree = [&](const std::array<synth::Parameter*, 2>& roots) {
        auto capturePersistedParameter = [&](auto& self, const synth::Parameter& parameter,
                                             const std::string& path, bool force,
                                             std::vector<TreeEntry>& entries) -> void {
            if (!force && !parameterHasPersistedState(parameterHasPersistedState, parameter, path)) {
                return;
            }
            TreeEntry entry;
            entry.path = path;
            entry.name = parameter.Name();
            for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
                entry.sceneCenter[sceneIx] = parameter.SceneCenter(sceneIx);
                for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                    entry.gestureValue[sceneIx][gestureIx] = parameter.GestureValue(sceneIx, gestureIx);
                    entry.gestureActive[sceneIx][gestureIx] = parameter.GestureActive(sceneIx, gestureIx);
                }
            }
            entries.push_back(entry);

            for (std::size_t modIx = 0; modIx < kSimMods; ++modIx) {
                const std::string key = std::to_string(modIx);
                const synth::Parameter* child = parameter.ModulationDepthParameter(modIx);
                if (child != nullptr) {
                    self(self, *child, path + "/" + key, false, entries);
                }
            }
        };

        std::vector<TreeEntry> entries;
        for (std::size_t rootIx = 0; rootIx < roots.size(); ++rootIx) {
            capturePersistedParameter(capturePersistedParameter, *roots[rootIx], std::to_string(rootIx), true,
                                      entries);
        }
        std::sort(entries.begin(), entries.end(), [](const TreeEntry& lhs, const TreeEntry& rhs) {
            return lhs.path < rhs.path;
        });
        return entries;
    };

    auto assertTreeMatches = [&](const std::array<synth::Parameter*, 2>& roots, const std::vector<TreeEntry>& expected,
                                 unsigned seed, int step, const std::string& action) {
        const std::vector<TreeEntry> actual = captureTree(roots);
        if (actual.size() != expected.size()) {
            SimFailBool(seed, step, action, "recursive ui tree entry count");
        }
        for (std::size_t entryIx = 0; entryIx < expected.size(); ++entryIx) {
            const TreeEntry& exp = expected[entryIx];
            const TreeEntry& got = actual[entryIx];
            if (exp.path != got.path || exp.name != got.name) {
                SimFailBool(seed, step, action, "recursive ui tree path/name");
            }
            for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
                SimCheckNear(seed, step, action, exp.path + " scene", exp.sceneCenter[sceneIx],
                             got.sceneCenter[sceneIx]);
                for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                    SimCheckNear(seed, step, action, exp.path + " gesture value",
                                 exp.gestureValue[sceneIx][gestureIx],
                                 got.gestureValue[sceneIx][gestureIx]);
                    if (exp.gestureActive[sceneIx][gestureIx] != got.gestureActive[sceneIx][gestureIx]) {
                        SimFailBool(seed, step, action, exp.path + " gesture active");
                    }
                }
            }
        }
    };

    auto assertPersistedTreeMatches = [&](const std::array<synth::Parameter*, 2>& roots,
                                          const std::vector<TreeEntry>& expected,
                                          unsigned seed, int step, const std::string& action) {
        const std::vector<TreeEntry> actual = capturePersistedTree(roots);
        if (actual.size() != expected.size()) {
            SimFailBool(seed, step, action, "recursive persisted ui tree entry count");
        }
        for (std::size_t entryIx = 0; entryIx < expected.size(); ++entryIx) {
            const TreeEntry& exp = expected[entryIx];
            const TreeEntry& got = actual[entryIx];
            if (exp.path != got.path || exp.name != got.name) {
                SimFailBool(seed, step, action, "recursive persisted ui tree path/name");
            }
            for (std::size_t sceneIx = 0; sceneIx < kSimScenes; ++sceneIx) {
                SimCheckNear(seed, step, action, exp.path + " persisted scene", exp.sceneCenter[sceneIx],
                             got.sceneCenter[sceneIx]);
                for (std::size_t gestureIx = 0; gestureIx < kSimGestures; ++gestureIx) {
                    SimCheckNear(seed, step, action, exp.path + " persisted gesture value",
                                 exp.gestureValue[sceneIx][gestureIx],
                                 got.gestureValue[sceneIx][gestureIx]);
                    if (exp.gestureActive[sceneIx][gestureIx] != got.gestureActive[sceneIx][gestureIx]) {
                        SimFailBool(seed, step, action, exp.path + " persisted gesture active");
                    }
                }
            }
        }
    };

    struct Rig {
        synth::ParameterManager manager;
        synth::ParameterMessageOutBus outputBus{64};
        synth::ParameterGroup* group = nullptr;
        synth::Bank* bank = nullptr;
        synth::BankSlot* slot = nullptr;
        synth::Parameter* phase = nullptr;
        synth::Parameter* shape = nullptr;
    };

    auto buildRig = [](Rig& rig) {
        rig.manager.SetGestureCount(kSimGestures);
        rig.manager.SetParameterMessageOutBus(&rig.outputBus);
        auto& group = rig.manager.CreateGroup({
            .numVoices = kSimVoices,
            .numModulators = kSimMods,
            .numScenes = kSimScenes,
            .maxParameters = 2,
            .processLiteAlpha = 0.3f,
            .targetCenterAlpha = 1.0f,
        });
        rig.group = &group;
        group.GetModulators().Metadata(0) = {
            .name = "Sweep",
            .shortName = "Swp",
            .sourceColor = synth::Color::Cyan,
            .connected = true,
        };
        group.GetModulators().Metadata(1) = {
            .name = "Envelope",
            .shortName = "Env",
            .sourceColor = synth::Color::Orange,
            .connected = true,
        };
        group.GetModulators().Metadata(2) = {
            .name = "LFO",
            .shortName = "LFO",
            .sourceColor = synth::Color::Green,
            .connected = true,
        };
        rig.phase = &rig.manager.CreateParameter(group, {.name = "Osc Phase", .defaultValue = 0.25f});
        rig.shape = &rig.manager.CreateParameter(group, {.name = "Osc Shape", .defaultValue = 0.5f});
        auto& bank = rig.manager.CreateBank();
        rig.bank = &bank;
        bank.AddMapping(10, *rig.phase);
        bank.AddMapping(11, *rig.shape);
        auto& slot = rig.manager.CreateBankSlot();
        rig.slot = &slot;
        for (synth::PhysicalEncoderId encoder : {10u, 11u, 12u, 13u}) {
            slot.AddPhysicalEncoder(encoder);
        }
        slot.SelectBank(&bank);
        REQUIRE_TRUE(rig.manager.SetSceneEndpoints(0, 1));
        rig.manager.SetSceneBlend(0.25f);
        rig.manager.CaptureDefaultControlState();
    };

    auto processStorageRequests = [](Rig& rig) {
        synth::ParameterMessageOut message;
        while (rig.outputBus.Pop(message)) {
            REQUIRE_TRUE(message.type == synth::ParameterMessageOut::Type::ParameterStorageBatchNeeded);
            REQUIRE_TRUE(message.group != nullptr);
            message.group->AddParameterStorageBatch(
                synth::MakeParameterStorageBatch(message.group->Config(), message.group->GestureCount(),
                                                 message.requestedParameters));
        }
    };

    auto dirtyTargetBeforeLoad = [](Rig& rig) {
        synth::Parameter* phaseSweep = rig.phase->EnsureModulationDepth(0);
        REQUIRE_TRUE(phaseSweep != nullptr);
        synth::Parameter* phaseSweepLfo = phaseSweep->EnsureModulationDepth(2);
        REQUIRE_TRUE(phaseSweepLfo != nullptr);
        synth::Parameter* shapeEnv = rig.shape->EnsureModulationDepth(1);
        REQUIRE_TRUE(shapeEnv != nullptr);
        rig.phase->SceneCenter(0) = 0.93f;
        phaseSweep->SceneCenter(0) = 0.81f;
        phaseSweep->GestureValue(1, 0) = 0.265f;
        phaseSweep->SetGestureActive(1, 0, true);
        phaseSweepLfo->SceneCenter(0) = 0.19f;
        shapeEnv->SceneCenter(1) = 0.58f;
        rig.manager.ComputeAllParameters();
    };

    const std::vector<unsigned> seeds = SimSeedsFromEnvironment();
    const int steps = SimStepsFromEnvironmentOrDefault(320);
    const std::array<synth::PhysicalEncoderId, 4> encoders{10, 11, 12, 13};

    for (const unsigned seed : seeds) {
        Rig source;
        buildRig(source);
        processStorageRequests(source);
        std::size_t maxPersistedEntryCount = 0;
        std::mt19937 rng(seed ^ 0xB16B00B5u);
        std::uniform_real_distribution<float> deltaDist(-0.35f, 0.35f);
        std::uniform_real_distribution<float> valueDist(0.0f, 1.0f);

        for (int step = 0; step < steps; ++step) {
            std::string action;
            switch (rng() % 10) {
            case 0:
            case 1: {
                const synth::PhysicalEncoderId encoder = encoders[rng() % encoders.size()];
                action = "recursive ui press";
                source.slot->HandlePress(encoder);
                processStorageRequests(source);
                break;
            }
            case 2:
            case 3: {
                const synth::PhysicalEncoderId encoder = encoders[rng() % encoders.size()];
                action = "recursive ui tick";
                source.slot->HandleTick(encoder, source.manager.Scene(), deltaDist(rng));
                source.manager.ComputeAllParameters();
                break;
            }
            case 4:
                action = "recursive ui scene";
                REQUIRE_TRUE(source.manager.SetSceneEndpoints(rng() % kSimScenes, rng() % kSimScenes));
                source.manager.SetSceneBlend(valueDist(rng));
                source.manager.ComputeAllParameters();
                break;
            case 5: {
                action = "recursive ui gesture select";
                const std::size_t gestureIx = rng() % kSimGestures;
                source.manager.ToggleGestureSelected(gestureIx);
                source.manager.SetGestureValue(gestureIx, valueDist(rng));
                source.manager.ComputeAllParameters();
                break;
            }
            case 6: {
                action = "recursive ui reset press";
                const synth::PhysicalEncoderId encoder = encoders[rng() % encoders.size()];
                source.manager.SetResetHeld(true);
                source.slot->HandlePress(encoder);
                source.manager.SetResetHeld(false);
                source.manager.ComputeAllParameters();
                break;
            }
            default:
                action = "recursive ui compute";
                source.manager.ComputeAllParameters();
                break;
            }

            if (step % 9 == 0) {
                std::array<synth::Parameter*, 2> sourceRoots{source.phase, source.shape};
                synth::JsonArena arena(262144);
                synth::JSON saved;
                do {
                    saved = source.manager.ParameterValuesToJSON(arena);
                    if (!saved.IsNull() && !arena.Failed()) {
                        break;
                    }
                    arena.GrowAndReset();
                } while (arena.Capacity() <= synth::JsonArena::kDefaultCapacity);
                REQUIRE_TRUE(!saved.IsNull());
                REQUIRE_TRUE(!arena.Failed());
                const std::vector<TreeEntry> expected = capturePersistedTree(sourceRoots);
                maxPersistedEntryCount = std::max(maxPersistedEntryCount, expected.size());

                Rig target;
                buildRig(target);
                const std::size_t targetStorage = std::max<std::size_t>(128, expected.size() * (kSimMods + 1));
                target.group->AddParameterStorageBatch(
                    synth::MakeParameterStorageBatch(target.group->Config(), target.group->GestureCount(), targetStorage));
                REQUIRE_TRUE(target.manager.LoadParameterValuesFromJSON(saved));
                std::array<synth::Parameter*, 2> targetRoots{target.phase, target.shape};
                assertTreeMatches(targetRoots, expected, seed, step, action);

                Rig dirtyTarget;
                buildRig(dirtyTarget);
                dirtyTarget.group->AddParameterStorageBatch(
                    synth::MakeParameterStorageBatch(dirtyTarget.group->Config(), dirtyTarget.group->GestureCount(),
                                                     targetStorage));
                dirtyTargetBeforeLoad(dirtyTarget);
                REQUIRE_TRUE(dirtyTarget.manager.LoadParameterValuesFromJSON(saved));
                std::array<synth::Parameter*, 2> dirtyTargetRoots{dirtyTarget.phase, dirtyTarget.shape};
                assertPersistedTreeMatches(dirtyTargetRoots, expected, seed, step, action);
            }
        }
        REQUIRE_TRUE(maxPersistedEntryCount > 2);
    }
}

TEST_CASE(parameter_values_json_round_trips_values_by_name_and_live_mod_slot) {
    synth::ParameterManager source;
    source.SetGestureCount(2);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 3,
        .maxParameters = 4,
        .targetCenterAlpha = 1.0f,
    });
    auto& cutoff = source.CreateParameter(sourceGroup, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& resonance = source.CreateParameter(sourceGroup, {.name = "Resonance", .defaultValue = 0.4f});
    auto& depth = cutoff.EnsureModulationDepth(0, {.name = "Cutoff LFO Depth", .defaultValue = 0.0f});

    cutoff.SceneCenter(0) = 0.11f;
    cutoff.SceneCenter(1) = 0.22f;
    cutoff.SceneCenter(2) = 0.33f;
    cutoff.GestureValue(0, 0) = 0.41f;
    cutoff.GestureValue(0, 1) = 0.42f;
    cutoff.GestureValue(1, 0) = 0.51f;
    cutoff.GestureValue(1, 1) = 0.52f;
    cutoff.GestureValue(2, 0) = 0.61f;
    cutoff.GestureValue(2, 1) = 0.62f;
    cutoff.SetGestureActive(0, 0, true);
    cutoff.SetGestureActive(1, 1, true);
    cutoff.SetGestureActive(2, 0, true);
    depth.SceneCenter(0) = 0.71f;
    depth.SceneCenter(1) = 0.72f;
    depth.SceneCenter(2) = 0.73f;
    depth.GestureValue(1, 0) = 0.81f;
    depth.SetGestureActive(1, 0, true);
    resonance.SceneCenter(0) = 0.91f;

    synth::JsonArena arena(32768);
    synth::JSON saved = source.ParameterValuesToJSON(arena);
    REQUIRE_TRUE(!saved.Get("Cutoff").IsNull());
    REQUIRE_TRUE(!saved.Get("Resonance").IsNull());
    REQUIRE_TRUE(!saved.Get("Cutoff").Get("modDepths").Get("0").IsNull());
    REQUIRE_TRUE(saved.Get("Cutoff").Get("modDepths").Get("1").IsNull());

    synth::ParameterManager target;
    target.SetGestureCount(2);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 3,
        .maxParameters = 4,
        .targetCenterAlpha = 1.0f,
    });
    auto& targetResonance = target.CreateParameter(targetGroup, {.name = "Resonance", .defaultValue = 0.4f});
    auto& targetCutoff = target.CreateParameter(targetGroup, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& targetDepth = targetCutoff.EnsureModulationDepth(0, {.name = "Different Child Name", .defaultValue = 0.0f});

    REQUIRE_TRUE(target.LoadParameterValuesFromJSON(saved));

    REQUIRE_NEAR(targetCutoff.SceneCenter(0), 0.11f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.SceneCenter(1), 0.22f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.SceneCenter(2), 0.33f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(0, 0), 0.41f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(0, 1), 0.42f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(1, 0), 0.51f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(1, 1), 0.52f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(2, 0), 0.61f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(2, 1), 0.62f, 0.000001f);
    REQUIRE_TRUE(targetCutoff.GestureActive(0, 0));
    REQUIRE_TRUE(!targetCutoff.GestureActive(0, 1));
    REQUIRE_TRUE(!targetCutoff.GestureActive(1, 0));
    REQUIRE_TRUE(targetCutoff.GestureActive(1, 1));
    REQUIRE_TRUE(targetCutoff.GestureActive(2, 0));
    REQUIRE_TRUE(!targetCutoff.GestureActive(2, 1));
    REQUIRE_NEAR(targetDepth.SceneCenter(0), 0.71f, 0.000001f);
    REQUIRE_NEAR(targetDepth.SceneCenter(1), 0.72f, 0.000001f);
    REQUIRE_NEAR(targetDepth.SceneCenter(2), 0.73f, 0.000001f);
    REQUIRE_NEAR(targetDepth.GestureValue(1, 0), 0.81f, 0.000001f);
    REQUIRE_TRUE(targetDepth.GestureActive(1, 0));
    REQUIRE_NEAR(targetResonance.SceneCenter(0), 0.91f, 0.000001f);
    REQUIRE_NEAR(targetResonance.GetRaw(0), 0.91f, 0.000001f);
    synth::Parameter::UIState resonanceUI(1);
    targetResonance.PopulateUIState(resonanceUI);
    REQUIRE_NEAR(resonanceUI.values[0].load(), 0.91f, 0.000001f);
}

TEST_CASE(parameter_values_json_materializes_saved_recursive_mod_depths_from_code_slots) {
    synth::ParameterManager source;
    source.SetGestureCount(1);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 2,
        .maxParameters = 8,
    });
    sourceGroup.GetModulators().Metadata(0) = {
        .name = "Sweep",
        .shortName = "Swp",
        .sourceColor = synth::Color::Cyan,
        .connected = true,
    };
    sourceGroup.GetModulators().Metadata(2) = {
        .name = "LFO",
        .shortName = "LFO",
        .sourceColor = synth::Color::Green,
        .connected = true,
    };
    auto& sourcePhase = source.CreateParameter(sourceGroup, {.name = "Osc Phase", .defaultValue = 0.25f});
    auto& sourceSweepDepth = sourcePhase.EnsureModulationDepth(
        0, {.name = "Osc Phase Sweep", .shortName = "Swp", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& sourceNestedLfoDepth = sourceSweepDepth.EnsureModulationDepth(
        2, {.name = "Osc Phase Sweep LFO", .shortName = "LFO", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    sourceSweepDepth.SceneCenter(0) = 0.45f;
    sourceSweepDepth.SceneCenter(1) = 0.375f;
    sourceNestedLfoDepth.SceneCenter(0) = 0.72f;
    sourceNestedLfoDepth.GestureValue(1, 0) = 0.335f;
    sourceNestedLfoDepth.SetGestureActive(1, 0, true);

    synth::JsonArena arena(65536);
    synth::JSON saved = source.ParameterValuesToJSON(arena);
    REQUIRE_TRUE(!saved.Get("Osc Phase").Get("modDepths").Get("0").IsNull());
    REQUIRE_TRUE(!saved.Get("Osc Phase").Get("modDepths").Get("0").Get("modDepths").Get("2").IsNull());

    synth::ParameterManager target;
    target.SetGestureCount(1);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 2,
        .maxParameters = 8,
    });
    targetGroup.GetModulators().Metadata(0) = sourceGroup.GetModulators().Metadata(0);
    targetGroup.GetModulators().Metadata(2) = sourceGroup.GetModulators().Metadata(2);
    auto& targetPhase = target.CreateParameter(targetGroup, {.name = "Osc Phase", .defaultValue = 0.25f});

    REQUIRE_TRUE(target.LoadParameterValuesFromJSON(saved));

    synth::Parameter* targetSweepDepth = targetPhase.ModulationDepthParameter(0);
    REQUIRE_TRUE(targetSweepDepth != nullptr);
    synth::Parameter* targetNestedLfoDepth = targetSweepDepth->ModulationDepthParameter(2);
    REQUIRE_TRUE(targetNestedLfoDepth != nullptr);
    REQUIRE_TRUE(targetSweepDepth->Name() == "Osc Phase Sweep");
    REQUIRE_TRUE(targetNestedLfoDepth->Name() == "Osc Phase Sweep LFO");
    REQUIRE_NEAR(targetSweepDepth->SceneCenter(0), 0.45f, 0.000001f);
    REQUIRE_NEAR(targetSweepDepth->SceneCenter(1), 0.375f, 0.000001f);
    REQUIRE_NEAR(targetNestedLfoDepth->SceneCenter(0), 0.72f, 0.000001f);
    REQUIRE_NEAR(targetNestedLfoDepth->GestureValue(1, 0), 0.335f, 0.000001f);
    REQUIRE_TRUE(targetNestedLfoDepth->GestureActive(1, 0));
    synth::Parameter::UIState sweepDepthUI(1);
    targetSweepDepth->PopulateUIState(sweepDepthUI);
    REQUIRE_TRUE((sweepDepthUI.modulatorsAffectingMask.load() & (1u << 2)) != 0);
}

TEST_CASE(parameter_values_json_load_resets_dirty_lazy_modulation_branches_before_applying) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 3,
        .numScenes = 2,
        .maxParameters = 8,
    });
    group.GetModulators().Metadata(0) = {
        .name = "Sweep",
        .shortName = "Swp",
        .sourceColor = synth::Color::Cyan,
        .connected = true,
    };
    group.GetModulators().Metadata(2) = {
        .name = "LFO",
        .shortName = "LFO",
        .sourceColor = synth::Color::Green,
        .connected = true,
    };
    auto& phase = manager.CreateParameter(group, {.name = "Osc Phase", .defaultValue = 0.25f});
    manager.CaptureDefaultControlState();

    synth::JsonArena cleanArena(65536);
    synth::JSON clean = manager.ParameterValuesToJSON(cleanArena);
    REQUIRE_TRUE(!cleanArena.Failed());
    REQUIRE_TRUE(clean.Get("Osc Phase").Get("modDepths").Get("0").IsNull());

    auto& sweepDepth = phase.EnsureModulationDepth(
        0, {.name = "Osc Phase Sweep", .shortName = "Swp", .defaultValue = 0.5f, .range = synth::RangeKind::Bipolar});
    auto& nestedLfoDepth = sweepDepth.EnsureModulationDepth(
        2, {.name = "Osc Phase Sweep LFO", .shortName = "LFO", .defaultValue = 0.5f,
            .range = synth::RangeKind::Bipolar});
    phase.SceneCenter(0) = 0.91f;
    sweepDepth.SceneCenter(0) = 0.77f;
    sweepDepth.SetGestureActive(1, 0, true);
    sweepDepth.GestureValue(1, 0) = 0.28f;
    nestedLfoDepth.SceneCenter(0) = 0.17f;

    REQUIRE_TRUE(manager.LoadParameterValuesFromJSON(clean));

    REQUIRE_NEAR(phase.SceneCenter(0), 0.25f, 0.000001f);
    REQUIRE_NEAR(sweepDepth.SceneCenter(0), 0.5f, 0.000001f);
    REQUIRE_NEAR(sweepDepth.SceneCenter(1), 0.5f, 0.000001f);
    REQUIRE_TRUE(!sweepDepth.GestureActive(1, 0));
    REQUIRE_NEAR(sweepDepth.GestureValue(1, 0), 0.5f, 0.000001f);
    REQUIRE_NEAR(nestedLfoDepth.SceneCenter(0), 0.5f, 0.000001f);

    synth::JsonArena afterLoadArena(65536);
    synth::JSON afterLoad = manager.ParameterValuesToJSON(afterLoadArena);
    REQUIRE_TRUE(!afterLoadArena.Failed());
    REQUIRE_TRUE(afterLoad.Get("Osc Phase").Get("modDepths").Get("0").IsNull());
}

TEST_CASE(parameter_values_json_persists_inactive_depth_gesture_values) {
    synth::ParameterManager source;
    source.SetGestureCount(1);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    sourceGroup.GetModulators().Metadata(0) = {
        .name = "Sweep",
        .shortName = "Swp",
        .sourceColor = synth::Color::Cyan,
        .connected = true,
    };
    auto& sourcePhase = source.CreateParameter(sourceGroup, {.name = "Osc Phase", .defaultValue = 0.25f});
    auto& sourceSweepDepth = sourcePhase.EnsureModulationDepth(
        0, {.name = "Osc Phase Sweep", .shortName = "Swp", .defaultValue = 0.0f,
            .range = synth::RangeKind::Bipolar});
    sourceSweepDepth.GestureValue(0, 0) = 0.42f;
    REQUIRE_TRUE(!sourceSweepDepth.GestureActive(0, 0));

    synth::JsonArena arena(32768);
    synth::JSON saved = source.ParameterValuesToJSON(arena);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(!saved.Get("Osc Phase").Get("modDepths").Get("0").IsNull());

    synth::ParameterManager target;
    target.SetGestureCount(1);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 1,
        .maxParameters = 2,
    });
    targetGroup.GetModulators().Metadata(0) = sourceGroup.GetModulators().Metadata(0);
    auto& targetPhase = target.CreateParameter(targetGroup, {.name = "Osc Phase", .defaultValue = 0.25f});

    REQUIRE_TRUE(target.LoadParameterValuesFromJSON(saved));
    synth::Parameter* targetSweepDepth = targetPhase.ModulationDepthParameter(0);
    REQUIRE_TRUE(targetSweepDepth != nullptr);
    REQUIRE_NEAR(targetSweepDepth->GestureValue(0, 0), 0.42f, 0.000001f);
    REQUIRE_TRUE(!targetSweepDepth->GestureActive(0, 0));
}

TEST_CASE(parameter_values_json_ignores_unknown_names_and_materializes_saved_depth_slots) {
    synth::JsonArena arena(65536);
    synth::JSON root = arena.Object();
    synth::JSON cutoff = arena.Object();
    synth::JSON centers = arena.Array();
    centers.AppendNew(arena.Real(0.35));
    centers.AppendNew(arena.Real(0.45));
    cutoff.SetNew("sceneCenters", centers);
    synth::JSON modDepths = arena.Object();
    synth::JSON missingChild = arena.Object();
    synth::JSON childCenters = arena.Array();
    childCenters.AppendNew(arena.Real(0.8));
    childCenters.AppendNew(arena.Real(0.9));
    missingChild.SetNew("sceneCenters", childCenters);
    modDepths.SetNew("1", missingChild);
    cutoff.SetNew("modDepths", modDepths);
    root.SetNew("Cutoff", cutoff);
    root.SetNew("Unknown Parameter", cutoff);

    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 2,
        .maxParameters = 3,
    });
    auto& cutoffParam = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    auto& resonance = manager.CreateParameter(group, {.name = "Resonance", .defaultValue = 0.6f});

    REQUIRE_TRUE(manager.LoadParameterValuesFromJSON(root));

    REQUIRE_NEAR(cutoffParam.SceneCenter(0), 0.35f, 0.000001f);
    REQUIRE_NEAR(cutoffParam.SceneCenter(1), 0.45f, 0.000001f);
    REQUIRE_NEAR(resonance.SceneCenter(0), 0.6f, 0.000001f);
    REQUIRE_NEAR(resonance.SceneCenter(1), 0.6f, 0.000001f);
    synth::Parameter* loadedChild = cutoffParam.ModulationDepthParameter(1);
    REQUIRE_TRUE(loadedChild != nullptr);
    REQUIRE_NEAR(loadedChild->SceneCenter(0), 0.8f, 0.000001f);
    REQUIRE_NEAR(loadedChild->SceneCenter(1), 0.9f, 0.000001f);
    REQUIRE_TRUE(loadedChild->Name() == "Cutoff Mod Depth 2");
    REQUIRE_TRUE(manager.ParameterCount() == 2);
}

TEST_CASE(parameter_values_json_shape_mismatches_leave_defaults_after_load_reset) {
    synth::ParameterManager manager;
    manager.SetGestureCount(2);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 2,
        .maxParameters = 1,
    });
    auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    cutoff.SceneCenter(0) = 0.11f;
    cutoff.SceneCenter(1) = 0.22f;
    cutoff.GestureValue(0, 0) = 0.31f;
    cutoff.GestureValue(0, 1) = 0.32f;
    cutoff.GestureValue(1, 0) = 0.41f;
    cutoff.GestureValue(1, 1) = 0.42f;
    cutoff.SetGestureActive(0, 0, true);
    cutoff.SetGestureActive(1, 1, true);

    synth::JsonArena arena(65536);
    synth::JSON root = arena.Object();
    synth::JSON value = arena.Object();
    synth::JSON badCenters = arena.Array();
    badCenters.AppendNew(arena.Real(0.9));
    value.SetNew("sceneCenters", badCenters);
    synth::JSON badGestureValues = arena.Array();
    synth::JSON badGestureRow0 = arena.Array();
    badGestureRow0.AppendNew(arena.Real(0.9));
    badGestureRow0.AppendNew(arena.Real(0.8));
    synth::JSON badGestureRow1 = arena.Array();
    badGestureRow1.AppendNew(arena.Real(0.7));
    badGestureValues.AppendNew(badGestureRow0);
    badGestureValues.AppendNew(badGestureRow1);
    value.SetNew("gestureValues", badGestureValues);
    synth::JSON badGestureActive = arena.Array();
    synth::JSON activeRow0 = arena.Array();
    activeRow0.AppendNew(arena.Boolean(false));
    activeRow0.AppendNew(arena.Boolean(false));
    synth::JSON activeRow1 = arena.Array();
    activeRow1.AppendNew(arena.Boolean(false));
    activeRow1.AppendNew(arena.Boolean(false));
    badGestureActive.AppendNew(activeRow0);
    badGestureActive.AppendNew(activeRow1);
    value.SetNew("gestureActive", badGestureActive);
    root.SetNew("Cutoff", value);

    REQUIRE_TRUE(manager.LoadParameterValuesFromJSON(root));

    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.2f, 0.000001f);
    REQUIRE_NEAR(cutoff.SceneCenter(1), 0.2f, 0.000001f);
    REQUIRE_NEAR(cutoff.GestureValue(0, 0), 0.2f, 0.000001f);
    REQUIRE_NEAR(cutoff.GestureValue(0, 1), 0.2f, 0.000001f);
    REQUIRE_NEAR(cutoff.GestureValue(1, 0), 0.2f, 0.000001f);
    REQUIRE_NEAR(cutoff.GestureValue(1, 1), 0.2f, 0.000001f);
    REQUIRE_TRUE(!cutoff.GestureActive(0, 0));
    REQUIRE_TRUE(!cutoff.GestureActive(0, 1));
    REQUIRE_TRUE(!cutoff.GestureActive(1, 0));
    REQUIRE_TRUE(!cutoff.GestureActive(1, 1));

    cutoff.SetGestureActive(0, 0, true);
    cutoff.SetGestureActive(1, 1, true);
    synth::JSON rootWithBadActive = arena.Object();
    synth::JSON valueWithBadActive = arena.Object();
    synth::JSON badActive = arena.Array();
    synth::JSON badActiveRow0 = arena.Array();
    badActiveRow0.AppendNew(arena.Boolean(false));
    badActiveRow0.AppendNew(arena.Boolean(false));
    synth::JSON badActiveRow1 = arena.Array();
    badActiveRow1.AppendNew(arena.Boolean(false));
    badActive.AppendNew(badActiveRow0);
    badActive.AppendNew(badActiveRow1);
    valueWithBadActive.SetNew("gestureActive", badActive);
    rootWithBadActive.SetNew("Cutoff", valueWithBadActive);

    REQUIRE_TRUE(manager.LoadParameterValuesFromJSON(rootWithBadActive));
    REQUIRE_TRUE(!cutoff.GestureActive(0, 0));
    REQUIRE_TRUE(!cutoff.GestureActive(0, 1));
    REQUIRE_TRUE(!cutoff.GestureActive(1, 0));
    REQUIRE_TRUE(!cutoff.GestureActive(1, 1));
}

TEST_CASE(midi_profile_config_json_round_trips_wrld_bldr_defaults_and_rebuilds_processors) {
    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 4;
    options.sceneCount = 3;
    options.bankButtonCount = 5;
    options.gestureSelectorCount = 2;
    const synth::MidiControllerProfileConfig source = synth::WrldBldrDefaultProfileConfig(options);

    synth::JsonArena arena(262144);
    synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    char* dumped = json.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);

    synth::JsonArena parseArena(4096);
    synth::JSON parsedJson = parseArena.Loads(dumped);
    while (parsedJson.IsNull() && parseArena.Failed()) {
        parseArena.GrowAndReset();
        parsedJson = parseArena.Loads(dumped);
    }
    std::free(dumped);

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(parsedJson, loaded));
    REQUIRE_TRUE(loaded.encoderInput.has_value());
    REQUIRE_TRUE(loaded.encoderOutput.has_value());
    REQUIRE_TRUE(loaded.analogInput.has_value());
    REQUIRE_TRUE(loaded.encoderInput->mode == synth::EncoderMode::Signed7Bit);
    REQUIRE_TRUE(loaded.encoderInput->turns.size() == 4);
    REQUIRE_TRUE(loaded.encoderInput->pushes.size() == 4);
    REQUIRE_TRUE(loaded.encoderInput->turns[3].control.channel == 0);
    REQUIRE_TRUE(loaded.encoderInput->turns[3].control.cc == synth::EncoderPositionToCC(3));
    REQUIRE_TRUE(loaded.encoderInput->pushes[3].control.channel == 1);
    REQUIRE_TRUE(loaded.encoderInput->pushes[3].slotIx == options.slotIx);
    REQUIRE_TRUE(loaded.encoderInput->pushes[3].position == 3);
    REQUIRE_NEAR(loaded.encoderInput->turnStep, 1.0f / 128.0f, 0.000001f);
    REQUIRE_TRUE(loaded.encoderOutput->mappings.size() == 4);
    REQUIRE_TRUE(loaded.encoderOutput->mappings[3].cc == synth::EncoderPositionToCC(3));
    REQUIRE_TRUE(loaded.analogInput->sceneBlend.has_value());
    REQUIRE_TRUE(loaded.analogInput->sceneBlend->channel == 2);
    REQUIRE_TRUE(loaded.analogInput->sceneBlend->cc == 0);
    REQUIRE_TRUE(!loaded.analogInput->gestures.empty());
    REQUIRE_TRUE(loaded.analogInput->gestures[0].control.channel == 2);
    REQUIRE_TRUE(loaded.analogInput->gestures[0].control.cc == 1);
    REQUIRE_TRUE(loaded.analogInput->gestures[0].gestureIx == 0);
    REQUIRE_TRUE(loaded.systemMessages.size() == 13);
    REQUIRE_TRUE(loaded.systemMessages[0].control.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].control->channel == 5);
    REQUIRE_TRUE(loaded.systemMessages[0].wrldBldrPosition.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].press.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages[0].release.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].release->type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages[0].feedback.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages[0].feedback.hasBoolValue);
    REQUIRE_TRUE(loaded.systemMessages[0].feedback.boolValue);
    REQUIRE_TRUE(loaded.systemMessages.back().press.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(loaded.systemMessages.back().press.gestureIx == 1);
    REQUIRE_TRUE(loaded.systemMessages.back().release.has_value());
    REQUIRE_TRUE(loaded.systemMessages.back().release->gestureIx == 1);
    REQUIRE_TRUE(loaded.systemMessages.back().feedback.type == synth::MessageIn::Type::SetGestureSelect);
    REQUIRE_TRUE(loaded.systemMessages.back().feedback.gestureIx == 1);
    REQUIRE_TRUE(loaded.systemMessages.back().feedback.hasBoolValue);
    REQUIRE_TRUE(loaded.systemMessages.back().feedback.boolValue);

    synth::MidiControllerProfileResult result =
        synth::CreateMidiControllerProfile(loaded, nullptr, nullptr, nullptr, [] { return 0; });
    REQUIRE_TRUE(dynamic_cast<synth::EncoderMidiInProcessor*>(result.input.get()) != nullptr);
    REQUIRE_TRUE(result.inputThru.size() == 3);
    REQUIRE_TRUE(dynamic_cast<synth::AnalogMidiInProcessor*>(result.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(result.inputThru[1].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(result.inputThru[2].get()) != nullptr);
    REQUIRE_TRUE(result.outputs.size() == 3);
    REQUIRE_TRUE(dynamic_cast<synth::WrldBldrMidiOutProcessor*>(result.outputs[0].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::SystemCcMidiOutProcessor*>(result.outputs[1].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::WrldBldrSystemMidiOutProcessor*>(result.outputs[2].get()) != nullptr);
}

TEST_CASE(midi_control_address_type_round_trips_and_legacy_profiles_default_to_cc) {
    synth::MidiControllerProfileConfig source;
    source.encoderInput = synth::EncoderMidiInConfig{};
    source.encoderInput->pushes.push_back({
        .control = {.channel = 1, .cc = 60, .type = synth::MidiControlType::Note},
        .slotIx = 2,
        .position = 3,
    });

    synth::JsonArena arena(16384);
    const synth::JSON json = synth::ToJSON(arena, source);
    const synth::JSON serializedControl =
        json.Get("encoderInput").Get("pushes").GetAt(0).Get("control");
    REQUIRE_TRUE(!serializedControl.Get("type").IsNull());
    REQUIRE_TRUE(std::string_view(serializedControl.Get("type").StringValue()) == "note");

    synth::MidiControllerProfileConfig roundTripped;
    REQUIRE_TRUE(synth::FromJSON(json, roundTripped));
    REQUIRE_TRUE(roundTripped.encoderInput.has_value());
    REQUIRE_TRUE(roundTripped.encoderInput->pushes.front().control.type == synth::MidiControlType::Note);

    synth::JsonArena legacyArena(16384);
    synth::JSON legacyRoot = legacyArena.Object();
    legacyRoot.SetNew("schema", legacyArena.String("synth.midiControllerProfileConfig"));
    legacyRoot.SetNew("schemaVersion", legacyArena.Integer(1));
    synth::JSON legacyEncoderInput = legacyArena.Object();
    legacyEncoderInput.SetNew("mode", legacyArena.String("signed7Bit"));
    legacyEncoderInput.SetNew("turnStep", legacyArena.Real(1.0 / 128.0));
    legacyEncoderInput.SetNew("turns", legacyArena.Array());
    synth::JSON legacyPushes = legacyArena.Array();
    synth::JSON legacyPush = legacyArena.Object();
    synth::JSON legacyControl = legacyArena.Object();
    legacyControl.SetNew("channel", legacyArena.Integer(1));
    legacyControl.SetNew("cc", legacyArena.Integer(60));
    legacyPush.SetNew("control", legacyControl);
    legacyPush.SetNew("slotIx", legacyArena.Integer(2));
    legacyPush.SetNew("position", legacyArena.Integer(3));
    legacyPushes.AppendNew(legacyPush);
    legacyEncoderInput.SetNew("pushes", legacyPushes);
    legacyRoot.SetNew("encoderInput", legacyEncoderInput);
    legacyRoot.SetNew("encoderOutput", legacyArena.Null());
    legacyRoot.SetNew("analogInput", legacyArena.Null());
    legacyRoot.SetNew("systemMessages", legacyArena.Array());

    synth::MidiControllerProfileConfig legacyLoaded;
    REQUIRE_TRUE(synth::FromJSON(legacyRoot, legacyLoaded));
    REQUIRE_TRUE(legacyLoaded.encoderInput.has_value());
    REQUIRE_TRUE(legacyLoaded.encoderInput->pushes.front().control.type == synth::MidiControlType::Cc);
}

TEST_CASE(midi_control_address_json_rejects_unknown_type_without_mutating_target) {
    synth::JsonArena arena(4096);
    synth::JSON json = arena.Object();
    json.SetNew("channel", arena.Integer(3));
    json.SetNew("cc", arena.Integer(9));
    json.SetNew("type", arena.String("pitch"));

    synth::MidiControlAddress target{
        .channel = 7,
        .cc = 74,
        .type = synth::MidiControlType::Note,
    };
    const synth::MidiControlAddress unchanged = target;
    REQUIRE_TRUE(!synth::FromJSON(json, target));
    REQUIRE_TRUE(target == unchanged);
}

TEST_CASE(generic_note_system_control_does_not_create_cc_feedback_output) {
    synth::MidiControllerProfileConfig config;
    config.systemMessages.push_back({
        .control = synth::MidiControlAddress{
            .channel = 1,
            .cc = 60,
            .type = synth::MidiControlType::Note,
        },
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
        .feedback = synth::MessageIn::SetReset(0, true),
        .outputFeedback = true,
    });

    synth::MidiControllerProfileResult result = synth::CreateMidiControllerProfile(
        config, nullptr, nullptr, nullptr, {}, 0, nullptr, 0, synth::MidiProfileKind::Generic);
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(result.input.get()) != nullptr);
    REQUIRE_TRUE(result.outputs.empty());
}

static_assert(static_cast<int>(synth::EncoderMode::Signed7Bit) == 0);
static_assert(static_cast<int>(synth::EncoderMode::DirectionOnly) == 1);
static_assert(static_cast<int>(synth::EncoderMode::Absolute) == 2);

TEST_CASE(encoder_mode_contract_defaults_to_signed_7_bit) {
    const synth::EncoderMidiInConfig config;
    REQUIRE_TRUE(config.mode == synth::EncoderMode::Signed7Bit);
    REQUIRE_NEAR(config.turnStep, 1.0f / 128.0f, 0.000001f);
}

TEST_CASE(encoder_mode_json_round_trips_absolute_and_writes_new_field_only) {
    synth::EncoderMidiInConfig source;
    source.mode = synth::EncoderMode::Absolute;
    source.turnStep = 0.25f;

    synth::JsonArena arena(4096);
    const synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string_view(json.Get("mode").StringValue()) == "absolute");
    REQUIRE_TRUE(json.Get("relativeMode").IsNull());

    synth::EncoderMidiInConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.mode == synth::EncoderMode::Absolute);
    REQUIRE_NEAR(loaded.turnStep, 0.25f, 0.000001f);
}

TEST_CASE(encoder_mode_json_loads_legacy_field_and_migrates_on_save) {
    synth::JsonArena arena(4096);
    synth::JSON legacy = arena.Object();
    legacy.SetNew("relativeMode", arena.String("directionOnly"));
    legacy.SetNew("turnStep", arena.Real(0.125));
    legacy.SetNew("turns", arena.Array());
    legacy.SetNew("pushes", arena.Array());

    synth::EncoderMidiInConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(legacy, loaded));
    REQUIRE_TRUE(loaded.mode == synth::EncoderMode::DirectionOnly);

    synth::JSON migrated = synth::ToJSON(arena, loaded);
    REQUIRE_TRUE(std::string_view(migrated.Get("mode").StringValue()) == "directionOnly");
    REQUIRE_TRUE(migrated.Get("relativeMode").IsNull());
}

TEST_CASE(encoder_mode_json_new_field_is_authoritative) {
    synth::JsonArena arena(4096);
    synth::JSON both = arena.Object();
    both.SetNew("mode", arena.String("signed7Bit"));
    both.SetNew("relativeMode", arena.String("directionOnly"));
    both.SetNew("turnStep", arena.Real(0.125));
    both.SetNew("turns", arena.Array());
    both.SetNew("pushes", arena.Array());

    synth::EncoderMidiInConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(both, loaded));
    REQUIRE_TRUE(loaded.mode == synth::EncoderMode::Signed7Bit);

    synth::JSON invalid = arena.Object();
    invalid.SetNew("mode", arena.String("not-a-mode"));
    invalid.SetNew("relativeMode", arena.String("directionOnly"));
    invalid.SetNew("turnStep", arena.Real(0.125));
    invalid.SetNew("turns", arena.Array());
    invalid.SetNew("pushes", arena.Array());
    loaded.mode = synth::EncoderMode::Absolute;
    REQUIRE_TRUE(!synth::FromJSON(invalid, loaded));
    REQUIRE_TRUE(loaded.mode == synth::EncoderMode::Absolute);

    synth::JSON nullMode = arena.Object();
    nullMode.SetNew("mode", arena.Null());
    nullMode.SetNew("relativeMode", arena.String("directionOnly"));
    nullMode.SetNew("turnStep", arena.Real(0.125));
    nullMode.SetNew("turns", arena.Array());
    nullMode.SetNew("pushes", arena.Array());
    REQUIRE_TRUE(!synth::FromJSON(nullMode, loaded));
    REQUIRE_TRUE(loaded.mode == synth::EncoderMode::Absolute);
}

TEST_CASE(midi_profile_config_json_migrates_legacy_shift_actions_to_reset) {
    synth::JsonArena arena(262144);
    synth::MidiControllerProfileConfig profile = synth::WrldBldrDefaultProfileConfig({});
    synth::JSON root = synth::ToJSON(arena, profile);
    char* dumped = root.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);
    std::string text(dumped);
    std::free(dumped);

    std::size_t pos = text.find("toggleReset");
    REQUIRE_TRUE(pos != std::string::npos);
    text.replace(pos, std::string("toggleReset").size(), "toggleShift");

    pos = text.find("toggleReset");
    REQUIRE_TRUE(pos != std::string::npos);
    text.replace(pos, std::string("toggleReset").size(), "setShift");

    synth::JsonArena parseArena(262144);
    synth::JSON parsed = parseArena.Loads(text.c_str());
    while (parsed.IsNull() && parseArena.Failed()) {
        parseArena.GrowAndReset();
        parsed = parseArena.Loads(text.c_str());
    }

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(parsed, loaded));
    REQUIRE_TRUE(!loaded.systemMessages.empty());
    REQUIRE_TRUE(loaded.systemMessages.front().press.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages.front().press.hasBoolValue);
    REQUIRE_TRUE(loaded.systemMessages.front().press.boolValue);
    REQUIRE_TRUE(loaded.systemMessages.front().release.has_value());
    REQUIRE_TRUE(loaded.systemMessages.front().release->type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages.front().release->hasBoolValue);
    REQUIRE_TRUE(!loaded.systemMessages.front().release->boolValue);
}

TEST_CASE(wrld_bldr_default_profile_maps_reset_random_and_random_mod_aux_buttons) {
    synth::MidiControllerProfileConfig profile = synth::WrldBldrDefaultProfileConfig({});
    auto hasMessageAt = [&](std::uint8_t x, std::uint8_t y, synth::MessageIn::Type type) {
        const std::uint8_t cc = synth::WrldBldrPositionToCC(x, y);
        return std::any_of(profile.systemMessages.begin(), profile.systemMessages.end(), [&](const auto& assoc) {
            return assoc.control.has_value() && assoc.control->channel == 5 && assoc.control->cc == cc &&
                   assoc.press.type == type && assoc.press.hasBoolValue && assoc.press.boolValue &&
                   assoc.release.has_value() && assoc.release->type == type && assoc.release->hasBoolValue &&
                   !assoc.release->boolValue;
        });
    };
    REQUIRE_TRUE(hasMessageAt(0, 4, synth::MessageIn::Type::ToggleReset));
    REQUIRE_TRUE(hasMessageAt(1, 4, synth::MessageIn::Type::ToggleRandom));
    REQUIRE_TRUE(hasMessageAt(2, 4, synth::MessageIn::Type::ToggleRandomMod));
}

TEST_CASE(midi_profile_config_json_round_trips_mf_twister_side_buttons) {
    synth::MfTwisterDefaultProfileOptions options;
    options.visibleEncoderCount = 3;
    options.sideButtons[0] = synth::MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SetReset(0, true),
        .release = synth::MessageIn::SetReset(0, false),
    };
    options.sideButtons[5] = synth::MidiControllerSystemMessageAssociation{
        .press = synth::MessageIn::SceneSelect(0, 2),
    };
    const synth::MidiControllerProfileConfig source = synth::MfTwisterDefaultProfileConfig(options);

    synth::JsonArena arena(262144);
    synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string(json.Get("encoderOutput").Get("protocol").StringValue()) == "twister");
    REQUIRE_TRUE(!json.Get("systemMessages").GetAt(0).Get("outputFeedback").IsNull());
    REQUIRE_TRUE(!json.Get("systemMessages").GetAt(0).Get("outputFeedback").BooleanValue());
    REQUIRE_TRUE(!json.Get("systemMessages").GetAt(1).Get("outputFeedback").IsNull());
    REQUIRE_TRUE(!json.Get("systemMessages").GetAt(1).Get("outputFeedback").BooleanValue());

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.encoderInput.has_value());
    REQUIRE_TRUE(loaded.encoderOutput.has_value());
    REQUIRE_TRUE(loaded.encoderOutput->protocol == synth::EncoderMidiOutProtocol::Twister);
    REQUIRE_TRUE(!loaded.analogInput.has_value());
    REQUIRE_TRUE(loaded.encoderInput->turns.size() == 3);
    REQUIRE_TRUE(loaded.systemMessages.size() == 2);
    REQUIRE_TRUE(loaded.systemMessages[0].control.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].control->channel == 3);
    REQUIRE_TRUE(loaded.systemMessages[0].control->cc == 8);
    REQUIRE_TRUE(loaded.systemMessages[0].press.type == synth::MessageIn::Type::ToggleReset);
    REQUIRE_TRUE(loaded.systemMessages[0].release.has_value());
    REQUIRE_TRUE(loaded.systemMessages[1].control->cc == 13);
    REQUIRE_TRUE(loaded.systemMessages[1].press.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(loaded.systemMessages[1].press.sceneIx == 2);
    REQUIRE_TRUE(!loaded.systemMessages[0].outputFeedback);
    REQUIRE_TRUE(!loaded.systemMessages[1].outputFeedback);

    synth::MidiControllerProfileResult rebuilt =
        synth::CreateMidiControllerProfile(loaded, nullptr, nullptr, nullptr, [] { return 0; });
    REQUIRE_TRUE(dynamic_cast<synth::EncoderMidiInProcessor*>(rebuilt.input.get()) != nullptr);
    REQUIRE_TRUE(rebuilt.inputThru.size() == 2);
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(rebuilt.inputThru[0].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::RealtimeMidiInProcessor*>(rebuilt.inputThru[1].get()) != nullptr);
    REQUIRE_TRUE(rebuilt.outputs.size() == 1);
    REQUIRE_TRUE(dynamic_cast<synth::TwisterMidiOutProcessor*>(rebuilt.outputs[0].get()) != nullptr);
    for (const auto& output : rebuilt.outputs) {
        REQUIRE_TRUE(dynamic_cast<synth::SystemCcMidiOutProcessor*>(output.get()) == nullptr);
    }
}

TEST_CASE(midi_profile_config_json_defaults_new_midi_fields_for_older_profiles) {
    synth::JsonArena arena(262144);
    synth::JSON root = arena.Object();
    root.SetNew("schema", arena.String("synth.midiControllerProfileConfig"));
    root.SetNew("schemaVersion", arena.Integer(1));
    root.SetNew("encoderInput", arena.Null());

    synth::JSON encoderOutput = arena.Object();
    synth::JSON mappings = arena.Array();
    synth::JSON mapping = arena.Object();
    mapping.SetNew("slotIx", arena.Integer(0));
    mapping.SetNew("position", arena.Integer(0));
    mapping.SetNew("cc", arena.Integer(synth::EncoderPositionToCC(0)));
    mappings.AppendNew(mapping);
    encoderOutput.SetNew("mappings", mappings);
    encoderOutput.SetNew("wrldBldrColorBudgetPerProcess", arena.Integer(4));
    root.SetNew("encoderOutput", encoderOutput);
    root.SetNew("analogInput", arena.Null());

    synth::JSON systemMessages = arena.Array();
    synth::JSON association = arena.Object();
    association.SetNew("control", synth::ToJSON(arena, synth::MidiControlAddress{.channel = 5, .cc = 32}));
    association.SetNew("wrldBldrPosition", arena.Null());
    association.SetNew("launchpadPosition", arena.Null());
    association.SetNew("press", synth::ToJSON(arena, synth::MessageIn::ToggleReset(0)));
    association.SetNew("release", arena.Null());
    association.SetNew("feedback", synth::ToJSON(arena, synth::MessageIn::SetReset(0, true)));
    systemMessages.AppendNew(association);
    root.SetNew("systemMessages", systemMessages);

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(root, loaded));
    REQUIRE_TRUE(loaded.encoderOutput.has_value());
    REQUIRE_TRUE(loaded.encoderOutput->protocol == synth::EncoderMidiOutProtocol::WrldBldr);
    REQUIRE_TRUE(loaded.systemMessages.size() == 1);
    REQUIRE_TRUE(loaded.systemMessages[0].outputFeedback);

    synth::MidiControllerProfileResult rebuilt =
        synth::CreateMidiControllerProfile(loaded, nullptr, nullptr, nullptr, [] { return 0; });
    REQUIRE_TRUE(rebuilt.outputs.size() == 2);
    REQUIRE_TRUE(dynamic_cast<synth::WrldBldrMidiOutProcessor*>(rebuilt.outputs[0].get()) != nullptr);
    REQUIRE_TRUE(dynamic_cast<synth::SystemCcMidiOutProcessor*>(rebuilt.outputs[1].get()) != nullptr);
}

TEST_CASE(midi_profile_config_json_round_trips_launchpad_system_positions) {
    synth::MidiControllerProfileConfig source;
    source.systemMessages.push_back({
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadMiniMk3,
            .x = 1,
            .y = 1,
        },
        .press = synth::MessageIn::SceneSelect(0, 2),
        .feedback = synth::MessageIn::SceneSelect(0, 2),
    });
    source.systemMessages.push_back({
        .control = synth::MidiControlAddress{.channel = 5, .cc = 32},
        .wrldBldrPosition = synth::WrldBldrSystemPosition{.channel = 5, .x = 0, .y = 4},
        .launchpadPosition = synth::LaunchpadGridPosition{
            .controller = synth::LaunchpadController::LaunchpadProMk3,
            .x = -1,
            .y = 0,
        },
        .press = synth::MessageIn::ToggleReset(0),
        .release = synth::MessageIn::SetReset(0, false),
        .feedback = synth::MessageIn::ToggleReset(0),
    });

    synth::JsonArena arena(262144);
    synth::JSON json = synth::ToJSON(arena, source);
    REQUIRE_TRUE(!arena.Failed());
    synth::JSON firstLaunchpad = json.Get("systemMessages").GetAt(0).Get("launchpadPosition");
    REQUIRE_TRUE(!firstLaunchpad.IsNull());
    REQUIRE_TRUE(std::string(firstLaunchpad.Get("controller").StringValue()) == "launchpadMiniMk3");
    REQUIRE_TRUE(firstLaunchpad.Get("x").IntegerValue() == 1);
    REQUIRE_TRUE(firstLaunchpad.Get("y").IntegerValue() == 1);
    synth::JSON secondLaunchpad = json.Get("systemMessages").GetAt(1).Get("launchpadPosition");
    REQUIRE_TRUE(!secondLaunchpad.IsNull());
    REQUIRE_TRUE(std::string(secondLaunchpad.Get("controller").StringValue()) == "launchpadProMk3");

    synth::MidiControllerProfileConfig loaded;
    REQUIRE_TRUE(synth::FromJSON(json, loaded));
    REQUIRE_TRUE(loaded.systemMessages.size() == 2);
    REQUIRE_TRUE(!loaded.systemMessages[0].control.has_value());
    REQUIRE_TRUE(!loaded.systemMessages[0].wrldBldrPosition.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].launchpadPosition.has_value());
    REQUIRE_TRUE(loaded.systemMessages[0].launchpadPosition->controller ==
                 synth::LaunchpadController::LaunchpadMiniMk3);
    REQUIRE_TRUE(loaded.systemMessages[0].launchpadPosition->x == 1);
    REQUIRE_TRUE(loaded.systemMessages[0].launchpadPosition->y == 1);
    REQUIRE_TRUE(loaded.systemMessages[0].press.type == synth::MessageIn::Type::SceneSelect);
    REQUIRE_TRUE(loaded.systemMessages[0].press.sceneIx == 2);
    REQUIRE_TRUE(loaded.systemMessages[1].control.has_value());
    REQUIRE_TRUE(loaded.systemMessages[1].wrldBldrPosition.has_value());
    REQUIRE_TRUE(loaded.systemMessages[1].launchpadPosition.has_value());
    REQUIRE_TRUE(loaded.systemMessages[1].launchpadPosition->controller ==
                 synth::LaunchpadController::LaunchpadProMk3);
    REQUIRE_TRUE(loaded.systemMessages[1].launchpadPosition->x == -1);
    REQUIRE_TRUE(loaded.systemMessages[1].launchpadPosition->y == 0);
    REQUIRE_TRUE(loaded.systemMessages[1].release.has_value());

    synth::MidiControllerProfileResult rebuilt =
        synth::CreateMidiControllerProfile(loaded, nullptr, nullptr, nullptr, [] { return 0; });
    REQUIRE_TRUE(dynamic_cast<synth::SystemButtonMidiInProcessor*>(rebuilt.input.get()) != nullptr);
    REQUIRE_TRUE(rebuilt.outputs.size() == 4);
    std::size_t launchpadOutputCount = 0;
    for (const auto& output : rebuilt.outputs) {
        if (dynamic_cast<synth::LaunchpadGridMidiOutProcessor*>(output.get()) != nullptr) {
            ++launchpadOutputCount;
        }
    }
    REQUIRE_TRUE(launchpadOutputCount == 2);

    synth::JsonArena invalidArena(32768);
    synth::MidiControllerProfileConfig invalidSource = source;
    invalidSource.systemMessages[0].launchpadPosition->x = -1;
    synth::JSON invalid = synth::ToJSON(invalidArena, invalidSource);
    REQUIRE_TRUE(!invalidArena.Failed());
    synth::MidiControllerProfileConfig unchanged = source;
    REQUIRE_TRUE(!synth::FromJSON(invalid, unchanged));
    REQUIRE_TRUE(unchanged.systemMessages.size() == source.systemMessages.size());
    REQUIRE_TRUE(unchanged.systemMessages[0].launchpadPosition.has_value());
    REQUIRE_TRUE(unchanged.systemMessages[0].launchpadPosition->x == 1);

    const auto makeProfileWithLaunchpadX = [](synth::JsonArena& profileArena, std::int64_t x) {
        synth::JSON root = profileArena.Object();
        root.SetNew("schema", profileArena.String("synth.midiControllerProfileConfig"));
        root.SetNew("schemaVersion", profileArena.Integer(1));
        synth::JSON systemMessages = profileArena.Array();
        synth::JSON association = profileArena.Object();
        association.SetNew("control", profileArena.Null());
        association.SetNew("wrldBldrPosition", profileArena.Null());
        synth::JSON launchpadPosition = profileArena.Object();
        launchpadPosition.SetNew("controller", profileArena.String("launchpadX"));
        launchpadPosition.SetNew("x", profileArena.Integer(x));
        launchpadPosition.SetNew("y", profileArena.Integer(1));
        association.SetNew("launchpadPosition", launchpadPosition);
        association.SetNew("press", synth::ToJSON(profileArena, synth::MessageIn::SceneSelect(0, 2)));
        association.SetNew("release", profileArena.Null());
        association.SetNew("feedback", synth::ToJSON(profileArena, synth::MessageIn::SceneSelect(0, 2)));
        systemMessages.AppendNew(association);
        root.SetNew("systemMessages", systemMessages);
        return root;
    };

    synth::JsonArena tooLargeArena(4096);
    synth::JSON tooLarge = makeProfileWithLaunchpadX(tooLargeArena, 4294967297LL);
    REQUIRE_TRUE(!tooLargeArena.Failed());
    unchanged = source;
    REQUIRE_TRUE(!synth::FromJSON(tooLarge, unchanged));
    REQUIRE_TRUE(unchanged.systemMessages[0].launchpadPosition->x == 1);

    synth::JsonArena tooSmallArena(4096);
    synth::JSON tooSmall = makeProfileWithLaunchpadX(tooSmallArena, -4294967295LL);
    REQUIRE_TRUE(!tooSmallArena.Failed());
    unchanged = source;
    REQUIRE_TRUE(!synth::FromJSON(tooSmall, unchanged));
    REQUIRE_TRUE(unchanged.systemMessages[0].launchpadPosition->x == 1);
}

TEST_CASE(midi_profile_config_json_rejects_invalid_values_without_mutating_target) {
    synth::MidiControllerProfileConfig target = synth::WrldBldrDefaultProfileConfig({});
    const std::size_t originalTurnCount = target.encoderInput->turns.size();
    const std::size_t originalSystemMessageCount = target.systemMessages.size();

    synth::JsonArena arena(4096);
    synth::JSON root = arena.Object();
    root.SetNew("schema", arena.String("synth.midiControllerProfileConfig"));
    root.SetNew("schemaVersion", arena.Integer(1));
    synth::JSON encoderInput = arena.Object();
    encoderInput.SetNew("relativeMode", arena.String("signed7Bit"));
    encoderInput.SetNew("turnStep", arena.Real(0.01));
    synth::JSON turns = arena.Array();
    synth::JSON badTurn = arena.Object();
    synth::JSON badControl = arena.Object();
    badControl.SetNew("channel", arena.Integer(16));
    badControl.SetNew("cc", arena.Integer(1));
    badTurn.SetNew("control", badControl);
    badTurn.SetNew("slotIx", arena.Integer(0));
    badTurn.SetNew("position", arena.Integer(0));
    turns.AppendNew(badTurn);
    encoderInput.SetNew("turns", turns);
    encoderInput.SetNew("pushes", arena.Array());
    root.SetNew("encoderInput", encoderInput);
    root.SetNew("encoderOutput", arena.Null());
    root.SetNew("analogInput", arena.Null());
    root.SetNew("systemMessages", arena.Array());

    REQUIRE_TRUE(!synth::FromJSON(root, target));
    REQUIRE_TRUE(target.encoderInput.has_value());
    REQUIRE_TRUE(target.encoderInput->turns.size() == originalTurnCount);

    synth::JsonArena turnStepArena(4096);
    synth::JSON badTurnStepRoot = turnStepArena.Object();
    badTurnStepRoot.SetNew("schema", turnStepArena.String("synth.midiControllerProfileConfig"));
    badTurnStepRoot.SetNew("schemaVersion", turnStepArena.Integer(1));
    synth::JSON badTurnStepInput = turnStepArena.Object();
    badTurnStepInput.SetNew("relativeMode", turnStepArena.String("signed7Bit"));
    badTurnStepInput.SetNew("turnStep", turnStepArena.Real(0.0));
    badTurnStepInput.SetNew("turns", turnStepArena.Array());
    badTurnStepInput.SetNew("pushes", turnStepArena.Array());
    badTurnStepRoot.SetNew("encoderInput", badTurnStepInput);
    badTurnStepRoot.SetNew("encoderOutput", turnStepArena.Null());
    badTurnStepRoot.SetNew("analogInput", turnStepArena.Null());
    badTurnStepRoot.SetNew("systemMessages", turnStepArena.Array());
    REQUIRE_TRUE(!synth::FromJSON(badTurnStepRoot, target));
    REQUIRE_TRUE(target.encoderInput.has_value());
    REQUIRE_TRUE(target.encoderInput->turns.size() == originalTurnCount);

    synth::JSON missingSchema = arena.Object();
    missingSchema.SetNew("schemaVersion", arena.Integer(1));
    missingSchema.SetNew("systemMessages", arena.Array());
    REQUIRE_TRUE(!synth::FromJSON(missingSchema, target));
    REQUIRE_TRUE(target.encoderInput.has_value());
    REQUIRE_TRUE(target.encoderInput->turns.size() == originalTurnCount);

    synth::JSON wrongVersion = arena.Object();
    wrongVersion.SetNew("schema", arena.String("synth.midiControllerProfileConfig"));
    wrongVersion.SetNew("schemaVersion", arena.Integer(3));
    wrongVersion.SetNew("systemMessages", arena.Array());
    REQUIRE_TRUE(!synth::FromJSON(wrongVersion, target));
    REQUIRE_TRUE(target.systemMessages.size() == originalSystemMessageCount);

    synth::JsonArena systemArena(4096);
    synth::JSON badSystemRoot = systemArena.Object();
    badSystemRoot.SetNew("schema", systemArena.String("synth.midiControllerProfileConfig"));
    badSystemRoot.SetNew("schemaVersion", systemArena.Integer(1));
    badSystemRoot.SetNew("encoderInput", systemArena.Null());
    badSystemRoot.SetNew("encoderOutput", systemArena.Null());
    badSystemRoot.SetNew("analogInput", systemArena.Null());
    synth::JSON systemMessages = systemArena.Array();
    synth::JSON association = systemArena.Object();
    association.SetNew("control", synth::ToJSON(systemArena, synth::MidiControlAddress{.channel = 5, .cc = 32}));
    association.SetNew("wrldBldrPosition", systemArena.Null());
    association.SetNew("press", synth::ToJSON(systemArena, synth::MessageIn::ToggleReset(99)));
    association.SetNew("release", systemArena.Null());
    synth::JSON badFeedback = systemArena.Object();
    badFeedback.SetNew("type", systemArena.String("notARealMessage"));
    badFeedback.SetNew("slotIx", systemArena.Integer(0));
    badFeedback.SetNew("position", systemArena.Integer(0));
    badFeedback.SetNew("gestureIx", systemArena.Integer(0));
    badFeedback.SetNew("bankIx", systemArena.Integer(0));
    badFeedback.SetNew("sceneIx", systemArena.Integer(0));
    badFeedback.SetNew("value", systemArena.Real(0.0));
    badFeedback.SetNew("delta", systemArena.Real(0.0));
    badFeedback.SetNew("boolValue", systemArena.Boolean(false));
    badFeedback.SetNew("hasBoolValue", systemArena.Boolean(false));
    association.SetNew("feedback", badFeedback);
    systemMessages.AppendNew(association);
    badSystemRoot.SetNew("systemMessages", systemMessages);
    REQUIRE_TRUE(!synth::FromJSON(badSystemRoot, target));
    REQUIRE_TRUE(target.systemMessages.size() == originalSystemMessageCount);
}

TEST_CASE(patch_json_serializes_parameter_values_only) {
    synth::ParameterManager source;
    source.SetGestureCount(1);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& sourceCutoff = source.CreateParameter(sourceGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    sourceCutoff.SceneCenter(0) = 0.45f;
    sourceCutoff.SceneCenter(1) = 0.55f;
    sourceCutoff.GestureValue(1, 0) = 0.75f;
    sourceCutoff.SetGestureActive(1, 0, true);

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 2;
    options.sceneCount = 2;
    options.bankButtonCount = 2;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig midiProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(midiProfile, "input-device-id", "output-device-id");

    synth::JsonArena arena(262144);
    synth::JSON root = synth::BuildPatchJSON(arena, "Patch A", source, instrument);
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string(root.Get("schema").StringValue()) == "sheaf.synth.patch");
    REQUIRE_TRUE(root.Get("schemaVersion").IntegerValue() == 1);
    REQUIRE_TRUE(std::string(root.Get("patchName").StringValue()) == "Patch A");
    REQUIRE_TRUE(!root.Get("parameterValues").Get("Cutoff").IsNull());
    REQUIRE_TRUE(root.Get("midiInstrument").IsNull());
    REQUIRE_TRUE(root.Get("audioDevice").IsNull());
}

TEST_CASE(patch_json_loads_parameter_values_without_mutating_runtime_configuration) {
    synth::ParameterManager source;
    source.SetGestureCount(1);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& sourceCutoff = source.CreateParameter(sourceGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    sourceCutoff.SceneCenter(0) = 0.45f;
    sourceCutoff.SceneCenter(1) = 0.55f;
    sourceCutoff.GestureValue(1, 0) = 0.75f;
    sourceCutoff.SetGestureActive(1, 0, true);

    synth::JsonArena arena(262144);
    synth::JSON root = synth::BuildPatchJSON(arena, "Patch A", source, synth::MidiInstrumentConfig{});
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(root.Get("midiInstrument").IsNull());
    REQUIRE_TRUE(root.Get("audioDevice").IsNull());

    synth::ParameterManager target;
    target.SetGestureCount(1);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& targetCutoff = target.CreateParameter(targetGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    auto& targetNewParameter = target.CreateParameter(targetGroup, {.name = "New Parameter", .defaultValue = 0.33f});
    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 2;
    options.sceneCount = 2;
    options.bankButtonCount = 2;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig midiProfile = synth::WrldBldrDefaultProfileConfig(options);
    synth::MidiInstrumentConfig loadedInstrument =
        MakeInstrumentFromProfile(midiProfile, "keep-input", "keep-output");
    synth::AudioDeviceState loadedAudioDevice{.outputDeviceName = "Keep Output", .inputDeviceName = "Keep Input"};

    REQUIRE_TRUE(synth::LoadPatchJSON(root, target, loadedInstrument, &loadedAudioDevice));
    REQUIRE_NEAR(targetCutoff.SceneCenter(0), 0.45f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.SceneCenter(1), 0.55f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(1, 0), 0.75f, 0.000001f);
    REQUIRE_TRUE(targetCutoff.GestureActive(1, 0));
    REQUIRE_NEAR(targetNewParameter.SceneCenter(0), 0.33f, 0.000001f);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.encoderInput->turns.size() == 2);
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.systemMessages.size() == 8);
    REQUIRE_TRUE(loadedInstrument.controllers[0].input.identifier == "keep-input");
    REQUIRE_TRUE(loadedInstrument.controllers[0].output.identifier == "keep-output");
    REQUIRE_TRUE(loadedAudioDevice.outputDeviceName == "Keep Output");
    REQUIRE_TRUE(loadedAudioDevice.inputDeviceName == "Keep Input");
}

TEST_CASE(patch_json_ignores_legacy_midi_and_audio_sections_even_when_invalid) {
    synth::ParameterManager source;
    source.SetGestureCount(0);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& sourceCutoff = source.CreateParameter(sourceGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    sourceCutoff.SceneCenter(0) = 0.71f;

    synth::JsonArena arena(262144);
    synth::JSON root = arena.Object();
    root.SetNew("schema", arena.String("sheaf.synth.patch"));
    root.SetNew("schemaVersion", arena.Integer(1));
    root.SetNew("patchName", arena.String("Legacy Patch"));
    root.SetNew("parameterValues", source.ParameterValuesToJSON(arena));
    root.SetNew("midiInstrument", arena.Array());
    root.SetNew("audioDevice", arena.Array());
    REQUIRE_TRUE(synth::ValidatePatchJSON(root));

    synth::ParameterManager target;
    target.SetGestureCount(0);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& targetCutoff = target.CreateParameter(targetGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    synth::MidiInstrumentConfig loadedInstrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState loadedAudioDevice{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};

    REQUIRE_TRUE(synth::LoadPatchJSON(root, target, loadedInstrument, &loadedAudioDevice));
    REQUIRE_NEAR(targetCutoff.SceneCenter(0), 0.71f, 0.000001f);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(loadedInstrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(loadedAudioDevice.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(loadedAudioDevice.inputDeviceName == "Keep In");
}

TEST_CASE(patch_json_rejects_invalid_roots_without_mutating_runtime_configuration) {
    synth::ParameterManager manager;
    manager.SetGestureCount(0);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 1,
    });
    auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});

    synth::MidiInstrumentConfig targetInstrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "in", "out");
    const std::size_t originalTurnCount = targetInstrument.controllers[0].config.encoderInput->turns.size();

    synth::JsonArena arena(65536);
    synth::JSON wrongSchema = arena.Object();
    wrongSchema.SetNew("schema", arena.String("wrong.schema"));
    wrongSchema.SetNew("schemaVersion", arena.Integer(1));
    wrongSchema.SetNew("patchName", arena.String("Patch"));
    REQUIRE_TRUE(!synth::LoadPatchJSON(wrongSchema, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers.size() == 1);
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");

    synth::JSON wrongVersion = arena.Object();
    wrongVersion.SetNew("schema", arena.String("sheaf.synth.patch"));
    wrongVersion.SetNew("schemaVersion", arena.Integer(2));
    wrongVersion.SetNew("patchName", arena.String("Patch"));
    REQUIRE_TRUE(!synth::LoadPatchJSON(wrongVersion, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].output.identifier == "out");

    synth::JSON missingPatchName = arena.Object();
    missingPatchName.SetNew("schema", arena.String("sheaf.synth.patch"));
    missingPatchName.SetNew("schemaVersion", arena.Integer(1));
    REQUIRE_TRUE(!synth::LoadPatchJSON(missingPatchName, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);

    synth::JSON nonObjectRoot = arena.Array();
    REQUIRE_TRUE(!synth::LoadPatchJSON(nonObjectRoot, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");
    REQUIRE_TRUE(targetInstrument.controllers[0].output.identifier == "out");

    cutoff.SceneCenter(0) = 0.44f;
    synth::JSON badParameterValues = arena.Object();
    badParameterValues.SetNew("schema", arena.String("sheaf.synth.patch"));
    badParameterValues.SetNew("schemaVersion", arena.Integer(1));
    badParameterValues.SetNew("patchName", arena.String("Patch"));
    badParameterValues.SetNew("parameterValues", arena.Array());
    synth::JSON validMidiInstrument = synth::ToJSON(arena, targetInstrument);
    badParameterValues.SetNew("midiInstrument", validMidiInstrument);
    REQUIRE_TRUE(!synth::LoadPatchJSON(badParameterValues, manager, targetInstrument));
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.44f, 0.000001f);
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");
    REQUIRE_TRUE(targetInstrument.controllers[0].output.identifier == "out");

    synth::JSON missingParameterValues = arena.Object();
    missingParameterValues.SetNew("schema", arena.String("sheaf.synth.patch"));
    missingParameterValues.SetNew("schemaVersion", arena.Integer(1));
    missingParameterValues.SetNew("patchName", arena.String("Patch"));
    missingParameterValues.SetNew("midiInstrument", validMidiInstrument);
    REQUIRE_TRUE(!synth::LoadPatchJSON(missingParameterValues, manager, targetInstrument));
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.44f, 0.000001f);
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");
    REQUIRE_TRUE(targetInstrument.controllers[0].output.identifier == "out");

    // midiInstrument is now legacy-only: absence is valid, and loading still
    // leaves runtime configuration untouched.
    synth::JsonArena legacyArena(65536);
    synth::JSON missingInstrument = legacyArena.Object();
    missingInstrument.SetNew("schema", legacyArena.String("sheaf.synth.patch"));
    missingInstrument.SetNew("schemaVersion", legacyArena.Integer(1));
    missingInstrument.SetNew("patchName", legacyArena.String("Patch"));
    missingInstrument.SetNew("parameterValues", manager.ParameterValuesToJSON(legacyArena));
    REQUIRE_TRUE(synth::LoadPatchJSON(missingInstrument, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers.size() == 1);
    REQUIRE_TRUE(targetInstrument.controllers[0].config.encoderInput->turns.size() == originalTurnCount);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");
    REQUIRE_TRUE(targetInstrument.controllers[0].output.identifier == "out");
    REQUIRE_TRUE(synth::ValidatePatchJSON(missingInstrument));

    // Legacy midiInstrument/audioDevice sections are ignored even if invalid.
    synth::JsonArena invalidLegacyArena(65536);
    synth::JSON invalidInstrumentRoot = invalidLegacyArena.Object();
    invalidInstrumentRoot.SetNew("schema", invalidLegacyArena.String("sheaf.synth.patch"));
    invalidInstrumentRoot.SetNew("schemaVersion", invalidLegacyArena.Integer(1));
    invalidInstrumentRoot.SetNew("patchName", invalidLegacyArena.String("Patch"));
    invalidInstrumentRoot.SetNew("parameterValues", manager.ParameterValuesToJSON(invalidLegacyArena));
    synth::JSON invalidMidiInstrument = invalidLegacyArena.Object();
    invalidMidiInstrument.SetNew("schema", invalidLegacyArena.String("wrong.schema"));
    invalidMidiInstrument.SetNew("schemaVersion", invalidLegacyArena.Integer(1));
    invalidMidiInstrument.SetNew("controllers", invalidLegacyArena.Array());
    invalidInstrumentRoot.SetNew("midiInstrument", invalidMidiInstrument);
    invalidInstrumentRoot.SetNew("audioDevice", invalidLegacyArena.Array());
    REQUIRE_TRUE(synth::LoadPatchJSON(invalidInstrumentRoot, manager, targetInstrument));
    REQUIRE_TRUE(targetInstrument.controllers.size() == 1);
    REQUIRE_TRUE(targetInstrument.controllers[0].input.identifier == "in");
    REQUIRE_TRUE(synth::ValidatePatchJSON(invalidInstrumentRoot));
}

TEST_CASE(patch_file_versioning_writes_collision_safe_sortable_versions) {
    const auto now = std::chrono::system_clock::from_time_t(1700000000);
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-patch-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(tempRoot);

    const std::filesystem::path first = synth::SavePatchVersion(tempRoot, "Patch A", "{\"save\":1}", now);
    const std::filesystem::path second = synth::SavePatchVersion(tempRoot, "Patch A", "{\"save\":2}", now);
    const std::string baseName = synth::TimestampPatchFilename(now);
    const std::string firstSuffix = "-000.json";
    const auto suffixPos = baseName.rfind(firstSuffix);
    REQUIRE_TRUE(suffixPos != std::string::npos);
    const std::string stem = baseName.substr(0, suffixPos);
    REQUIRE_TRUE(first != second);
    REQUIRE_TRUE(first.filename().string() == baseName);
    REQUIRE_TRUE(second.filename().string() == stem + "-001.json");
    REQUIRE_TRUE(std::filesystem::exists(first));
    REQUIRE_TRUE(std::filesystem::exists(second));

    const std::optional<std::filesystem::path> latest = synth::LatestPatchVersion(synth::PatchDirectory(tempRoot, "Patch A"));
    REQUIRE_TRUE(latest.has_value());
    REQUIRE_TRUE(latest->filename() == second.filename());
    REQUIRE_TRUE(synth::LoadPatchVersionText(first) == "{\"save\":1}");
    REQUIRE_TRUE(synth::LoadPatchVersionText(second) == "{\"save\":2}");

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(patch_file_round_trips_real_patch_json_through_latest_version) {
    synth::ParameterManager source;
    source.SetGestureCount(1);
    auto& sourceGroup = source.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& cutoff = source.CreateParameter(sourceGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    auto& depth = cutoff.EnsureModulationDepth(0, {.name = "Cutoff LFO", .defaultValue = 0.5f});
    cutoff.SceneCenter(0) = 0.61f;
    cutoff.GestureValue(1, 0) = 0.72f;
    cutoff.SetGestureActive(1, 0, true);
    depth.SceneCenter(1) = 0.31f;

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 2;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig midiProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig instrument = MakeInstrumentFromProfile(midiProfile, "saved-input", "saved-output");

    synth::JsonArena buildArena(262144);
    const synth::JSON patch = synth::BuildPatchJSON(buildArena, "Round Trip", source, instrument);
    REQUIRE_TRUE(!patch.IsNull());
    REQUIRE_TRUE(!buildArena.Failed());
    char* dumped = patch.Dumps(JSON_ENCODE_ANY);
    REQUIRE_TRUE(dumped != nullptr);
    const std::string jsonText(dumped);
    std::free(dumped);

    const auto now = std::chrono::system_clock::from_time_t(1700000500);
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-real-patch-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(tempRoot);
    const std::filesystem::path saved = synth::SavePatchVersion(tempRoot, "Round Trip", jsonText, now);

    const std::optional<std::filesystem::path> latest =
        synth::LatestPatchVersion(synth::PatchDirectory(tempRoot, "Round Trip"));
    REQUIRE_TRUE(latest.has_value());
    REQUIRE_TRUE(*latest == saved);

    const std::string loadedText = synth::LoadPatchVersionText(*latest);
    synth::JsonArena parseArena(262144);
    synth::JSON loadedRoot = parseArena.Loads(loadedText.c_str());
    REQUIRE_TRUE(!loadedRoot.IsNull());
    REQUIRE_TRUE(!parseArena.Failed());

    synth::ParameterManager target;
    target.SetGestureCount(1);
    auto& targetGroup = target.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 3,
    });
    auto& targetCutoff = target.CreateParameter(targetGroup, {.name = "Cutoff", .defaultValue = 0.2f});
    auto& targetDepth = targetCutoff.EnsureModulationDepth(0, {.name = "Cutoff LFO", .defaultValue = 0.0f});
    auto& added = target.CreateParameter(targetGroup, {.name = "New Default", .defaultValue = 0.44f});

    synth::MidiInstrumentConfig loadedInstrument =
        MakeInstrumentFromProfile(midiProfile, "keep-input", "keep-output");
    REQUIRE_TRUE(synth::LoadPatchJSON(loadedRoot, target, loadedInstrument));
    REQUIRE_NEAR(targetCutoff.SceneCenter(0), 0.61f, 0.000001f);
    REQUIRE_NEAR(targetCutoff.GestureValue(1, 0), 0.72f, 0.000001f);
    REQUIRE_TRUE(targetCutoff.GestureActive(1, 0));
    REQUIRE_NEAR(targetDepth.SceneCenter(1), 0.31f, 0.000001f);
    REQUIRE_NEAR(added.SceneCenter(0), 0.44f, 0.000001f);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.encoderInput.has_value());
    REQUIRE_TRUE(loadedInstrument.controllers[0].config.encoderInput->turns.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].input.identifier == "keep-input");
    REQUIRE_TRUE(loadedInstrument.controllers[0].output.identifier == "keep-output");

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(runtime_config_json_round_trips_midi_and_audio) {
    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "input-id", "output-id");
    synth::AudioDeviceState audio{.outputDeviceName = "Interface Out", .inputDeviceName = "Interface In"};
    const synth::SyncConfig sync{
        .sendClock = true,
        .receiveClock = false,
        .sendTransport = true,
        .receiveTransport = true,
        .ppqn = 96,
    };

    synth::JsonArena arena(262144);
    synth::JSON root = synth::BuildRuntimeConfigJSON(arena, instrument, audio, sync);
    REQUIRE_TRUE(!root.IsNull());
    REQUIRE_TRUE(!arena.Failed());
    REQUIRE_TRUE(std::string(root.Get("schema").StringValue()) == synth::kRuntimeConfigSchema);
    REQUIRE_TRUE(root.Get("schemaVersion").IntegerValue() == synth::kRuntimeConfigSchemaVersion);
    REQUIRE_TRUE(root.Get("patchName").IsNull());
    REQUIRE_TRUE(root.Get("parameterValues").IsNull());
    REQUIRE_TRUE(synth::ValidateRuntimeConfigJSON(root));

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(root, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].input.identifier == "input-id");
    REQUIRE_TRUE(loadedInstrument.controllers[0].output.identifier == "output-id");
    REQUIRE_TRUE(loadedAudio.outputDeviceName == "Interface Out");
    REQUIRE_TRUE(loadedAudio.inputDeviceName == "Interface In");
    REQUIRE_TRUE(loadedSync == sync);
}

TEST_CASE(runtime_config_v1_migrates_to_default_sync_and_v2_save_contains_exact_sync_fields) {
    const synth::MidiInstrumentConfig sourceInstrument;
    const synth::AudioDeviceState sourceAudio{.outputDeviceName = "Out", .inputDeviceName = "In"};

    synth::JsonArena defaultArena(1024);
    const synth::JSON defaultJson = synth::ToJSON(defaultArena, synth::SyncConfig{});
    REQUIRE_TRUE(!defaultJson.Get("sendClock").BooleanValue());
    REQUIRE_TRUE(!defaultJson.Get("receiveClock").BooleanValue());
    REQUIRE_TRUE(!defaultJson.Get("sendTransport").BooleanValue());
    REQUIRE_TRUE(!defaultJson.Get("receiveTransport").BooleanValue());
    REQUIRE_TRUE(defaultJson.Get("ppqn").IntegerValue() == 24);
    synth::SyncConfig roundTrippedDefault{.sendClock = true, .ppqn = 960};
    REQUIRE_TRUE(synth::FromJSON(defaultJson, roundTrippedDefault));
    REQUIRE_TRUE(roundTrippedDefault == synth::SyncConfig{});

    synth::JsonArena v1Arena(8192);
    synth::JSON v1 = v1Arena.Object();
    v1.SetNew("schema", v1Arena.String(synth::kRuntimeConfigSchema));
    v1.SetNew("schemaVersion", v1Arena.Integer(1));
    v1.SetNew("midiInstrument", synth::ToJSON(v1Arena, sourceInstrument));
    v1.SetNew("audioDevice", synth::ToJSON(v1Arena, sourceAudio));

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync{
        .sendClock = true,
        .receiveClock = true,
        .sendTransport = true,
        .receiveTransport = true,
        .ppqn = 960,
    };
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(v1, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedAudio.outputDeviceName == sourceAudio.outputDeviceName);
    REQUIRE_TRUE(loadedAudio.inputDeviceName.empty());
    REQUIRE_TRUE(loadedSync == synth::SyncConfig{});

    const synth::SyncConfig savedSync{
        .sendClock = true,
        .receiveClock = true,
        .sendTransport = false,
        .receiveTransport = true,
        .ppqn = 48,
    };
    synth::JsonArena v2Arena(8192);
    const synth::JSON v2 = synth::BuildRuntimeConfigJSON(
        v2Arena, sourceInstrument, sourceAudio, savedSync);
    REQUIRE_TRUE(v2.Get("schemaVersion").IntegerValue() == synth::kRuntimeConfigSchemaVersion);
    const synth::JSON sync = v2.Get("sync");
    REQUIRE_TRUE(sync.Size() == 5);
    REQUIRE_TRUE(sync.Get("sendClock").BooleanValue());
    REQUIRE_TRUE(sync.Get("receiveClock").BooleanValue());
    REQUIRE_TRUE(!sync.Get("sendTransport").BooleanValue());
    REQUIRE_TRUE(sync.Get("receiveTransport").BooleanValue());
    REQUIRE_TRUE(sync.Get("ppqn").IntegerValue() == 48);
}

TEST_CASE(runtime_config_v2_rejects_every_missing_or_wrong_sync_field_atomically) {
    const std::string prefix =
        R"({"schema":"sheaf.synth.runtime-config","schemaVersion":2,"midiInstrument":{"schema":"synth.midiInstrument","schemaVersion":1,"controllers":[]},"audioDevice":{},"sync":)";
    const auto requireAtomicRejection = [](const std::string& text) {
        synth::JsonArena arena(16384);
        const synth::JSON invalid = arena.Loads(text.c_str());
        REQUIRE_TRUE(!invalid.IsNull());

        synth::MidiInstrumentConfig instrument =
            MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
        const synth::MidiInstrumentConfig originalInstrument = instrument;
        synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
        const synth::AudioDeviceState originalAudio = audio;
        synth::SyncConfig sync{
            .sendClock = true,
            .receiveClock = true,
            .sendTransport = true,
            .receiveTransport = true,
            .ppqn = 960,
        };
        const synth::SyncConfig originalSync = sync;

        REQUIRE_TRUE(!synth::LoadRuntimeConfigJSON(invalid, instrument, audio, sync));
        REQUIRE_TRUE(instrument.controllers.size() == originalInstrument.controllers.size());
        REQUIRE_TRUE(instrument.controllers[0].input.identifier ==
                     originalInstrument.controllers[0].input.identifier);
        REQUIRE_TRUE(audio == originalAudio);
        REQUIRE_TRUE(sync == originalSync);
    };

    requireAtomicRejection(
        R"({"schema":"sheaf.synth.runtime-config","schemaVersion":2,"midiInstrument":{"schema":"synth.midiInstrument","schemaVersion":1,"controllers":[]},"audioDevice":{}})");

    const std::vector<std::string> invalidSyncObjects = {
        R"(null)",
        R"({"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false})",
        R"({"sendClock":0,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":"false","sendTransport":false,"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":[],"receiveTransport":false,"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":{},"ppqn":24})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":24.0})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":true})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":-1})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":0})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":961})",
        R"({"sendClock":false,"receiveClock":false,"sendTransport":false,"receiveTransport":false,"ppqn":24,"extra":0})",
    };

    for (const std::string& syncObject : invalidSyncObjects) {
        const std::string text = prefix + syncObject + "}";
        requireAtomicRejection(text);
    }
}

TEST_CASE(runtime_config_v2_audio_device_input_name_dropped_on_migration) {
    const synth::MidiInstrumentConfig instrument;
    const synth::AudioDeviceState sourceAudio{.outputDeviceName = "Interface Out",
                                               .inputDeviceName = "BlackHole 2ch"};
    const synth::SyncConfig sync{.ppqn = 24};

    synth::JsonArena arena(8192);
    synth::JSON v2 = arena.Object();
    v2.SetNew("schema", arena.String(synth::kRuntimeConfigSchema));
    v2.SetNew("schemaVersion", arena.Integer(2));
    v2.SetNew("midiInstrument", synth::ToJSON(arena, instrument));
    v2.SetNew("audioDevice", synth::ToJSON(arena, sourceAudio));
    v2.SetNew("sync", synth::ToJSON(arena, sync));

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(v2, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedAudio.inputDeviceName.empty());
    REQUIRE_TRUE(loadedAudio.outputDeviceName == sourceAudio.outputDeviceName);
}

TEST_CASE(runtime_config_v3_audio_device_input_name_preserved) {
    const synth::MidiInstrumentConfig instrument;
    const synth::AudioDeviceState sourceAudio{.outputDeviceName = "Interface Out",
                                               .inputDeviceName = "BlackHole 2ch"};
    const synth::SyncConfig sync{.ppqn = 24};

    synth::JsonArena arena(8192);
    const synth::JSON v3 = synth::BuildRuntimeConfigJSON(arena, instrument, sourceAudio, sync);
    REQUIRE_TRUE(v3.Get("schemaVersion").IntegerValue() == synth::kRuntimeConfigSchemaVersion);

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(v3, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedAudio.inputDeviceName == sourceAudio.inputDeviceName);
    REQUIRE_TRUE(loadedAudio.outputDeviceName == sourceAudio.outputDeviceName);
}

TEST_CASE(runtime_config_v2_sync_section_still_loads) {
    const synth::MidiInstrumentConfig instrument;
    const synth::AudioDeviceState sourceAudio{.outputDeviceName = "Out", .inputDeviceName = ""};
    const synth::SyncConfig sync{
        .sendClock = true,
        .receiveClock = true,
        .sendTransport = false,
        .receiveTransport = true,
        .ppqn = 480,
    };

    synth::JsonArena arena(8192);
    synth::JSON v2 = arena.Object();
    v2.SetNew("schema", arena.String(synth::kRuntimeConfigSchema));
    v2.SetNew("schemaVersion", arena.Integer(2));
    v2.SetNew("midiInstrument", synth::ToJSON(arena, instrument));
    v2.SetNew("audioDevice", synth::ToJSON(arena, sourceAudio));
    v2.SetNew("sync", synth::ToJSON(arena, sync));

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    REQUIRE_TRUE(synth::LoadRuntimeConfigJSON(v2, loadedInstrument, loadedAudio, loadedSync));
    REQUIRE_TRUE(loadedSync == sync);
}

TEST_CASE(runtime_config_load_invalid_schema_preserves_targets) {
    synth::JsonArena arena(1024);
    synth::JSON invalid = arena.Object();
    invalid.SetNew("schema", arena.String("wrong"));
    invalid.SetNew("schemaVersion", arena.Integer(1));

    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
    synth::SyncConfig sync{.sendClock = true, .ppqn = 96};

    REQUIRE_TRUE(!synth::LoadRuntimeConfigJSON(invalid, instrument, audio, sync));
    REQUIRE_TRUE(!synth::ValidateRuntimeConfigJSON(invalid));
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(instrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(audio.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(audio.inputDeviceName == "Keep In");
    REQUIRE_TRUE((sync == synth::SyncConfig{.sendClock = true, .ppqn = 96}));
}

TEST_CASE(runtime_config_load_invalid_midi_preserves_targets) {
    synth::JsonArena arena(4096);
    synth::JSON invalid = arena.Object();
    invalid.SetNew("schema", arena.String(synth::kRuntimeConfigSchema));
    invalid.SetNew("schemaVersion", arena.Integer(synth::kRuntimeConfigSchemaVersion));
    synth::JSON invalidMidi = arena.Object();
    invalidMidi.SetNew("schema", arena.String("wrong.midi"));
    invalidMidi.SetNew("schemaVersion", arena.Integer(1));
    invalidMidi.SetNew("controllers", arena.Array());
    invalid.SetNew("midiInstrument", invalidMidi);
    invalid.SetNew("audioDevice", synth::ToJSON(arena, synth::AudioDeviceState{
        .outputDeviceName = "New Out",
        .inputDeviceName = "New In",
    }));
    synth::JSON syncJson = arena.Object();
    syncJson.SetNew("sendClock", arena.Boolean(false));
    syncJson.SetNew("receiveClock", arena.Boolean(false));
    syncJson.SetNew("sendTransport", arena.Boolean(false));
    syncJson.SetNew("receiveTransport", arena.Boolean(false));
    syncJson.SetNew("ppqn", arena.Integer(24));
    invalid.SetNew("sync", syncJson);

    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
    synth::SyncConfig sync{.receiveClock = true, .ppqn = 48};

    REQUIRE_TRUE(!synth::LoadRuntimeConfigJSON(invalid, instrument, audio, sync));
    REQUIRE_TRUE(!synth::ValidateRuntimeConfigJSON(invalid));
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(instrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(audio.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(audio.inputDeviceName == "Keep In");
    REQUIRE_TRUE((sync == synth::SyncConfig{.receiveClock = true, .ppqn = 48}));
}

TEST_CASE(runtime_config_load_invalid_audio_preserves_targets) {
    synth::MidiInstrumentConfig replacement =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "new-in", "new-out");

    synth::JsonArena arena(262144);
    synth::JSON invalid = arena.Object();
    invalid.SetNew("schema", arena.String(synth::kRuntimeConfigSchema));
    invalid.SetNew("schemaVersion", arena.Integer(synth::kRuntimeConfigSchemaVersion));
    invalid.SetNew("midiInstrument", synth::ToJSON(arena, replacement));
    synth::JSON invalidAudio = arena.Object();
    invalidAudio.SetNew("outputDeviceName", arena.Integer(3));
    invalid.SetNew("audioDevice", invalidAudio);
    synth::JSON syncJson = arena.Object();
    syncJson.SetNew("sendClock", arena.Boolean(false));
    syncJson.SetNew("receiveClock", arena.Boolean(false));
    syncJson.SetNew("sendTransport", arena.Boolean(false));
    syncJson.SetNew("receiveTransport", arena.Boolean(false));
    syncJson.SetNew("ppqn", arena.Integer(24));
    invalid.SetNew("sync", syncJson);
    REQUIRE_TRUE(!arena.Failed());

    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
    synth::SyncConfig sync{.sendTransport = true, .ppqn = 120};

    REQUIRE_TRUE(!synth::LoadRuntimeConfigJSON(invalid, instrument, audio, sync));
    REQUIRE_TRUE(!synth::ValidateRuntimeConfigJSON(invalid));
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(instrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(audio.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(audio.inputDeviceName == "Keep In");
    REQUIRE_TRUE((sync == synth::SyncConfig{.sendTransport = true, .ppqn = 120}));
}

TEST_CASE(runtime_config_file_invalid_schema_returns_invalid_and_preserves_targets) {
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-runtime-config-invalid-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot);
    const std::filesystem::path configFile = tempRoot / "config.json";
    {
        std::ofstream out(configFile, std::ios::binary | std::ios::trunc);
        out << R"({"schema":"wrong","schemaVersion":1,"midiInstrument":{},"audioDevice":{}})";
    }

    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
    synth::SyncConfig sync{.receiveTransport = true, .ppqn = 192};

    const synth::RuntimeConfigFileStatus status = synth::LoadRuntimeConfigFile(configFile, instrument, audio, sync);
    REQUIRE_TRUE(status == synth::RuntimeConfigFileStatus::Invalid);
    REQUIRE_TRUE(std::string(synth::RuntimeConfigFileStatusName(status)) == "Invalid");
    REQUIRE_TRUE(std::string(synth::RuntimeConfigFileStatusName(synth::RuntimeConfigFileStatus::IOError)) == "IOError");
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(instrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(audio.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(audio.inputDeviceName == "Keep In");
    REQUIRE_TRUE((sync == synth::SyncConfig{.receiveTransport = true, .ppqn = 192}));

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(runtime_config_file_missing_preserves_targets) {
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-runtime-config-missing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(tempRoot);
    const std::filesystem::path configFile = tempRoot / "config.json";

    synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "keep-in", "keep-out");
    synth::AudioDeviceState audio{.outputDeviceName = "Keep Out", .inputDeviceName = "Keep In"};
    synth::SyncConfig sync{.sendClock = true, .ppqn = 12};

    const synth::RuntimeConfigFileStatus status = synth::LoadRuntimeConfigFile(configFile, instrument, audio, sync);
    REQUIRE_TRUE(status == synth::RuntimeConfigFileStatus::Missing);
    REQUIRE_TRUE(std::string(synth::RuntimeConfigFileStatusName(status)) == "Missing");
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "keep-in");
    REQUIRE_TRUE(instrument.controllers[0].output.identifier == "keep-out");
    REQUIRE_TRUE(audio.outputDeviceName == "Keep Out");
    REQUIRE_TRUE(audio.inputDeviceName == "Keep In");
    REQUIRE_TRUE((sync == synth::SyncConfig{.sendClock = true, .ppqn = 12}));

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(runtime_config_atomic_save_writes_config_and_cleans_temp) {
    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-runtime-config-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::remove_all(tempRoot);
    const std::filesystem::path configFile = tempRoot / "nested" / "config.json";

    const synth::MidiInstrumentConfig instrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "saved-in", "saved-out");
    const synth::AudioDeviceState audio{.outputDeviceName = "Saved Out", .inputDeviceName = "Saved In"};
    const synth::SyncConfig sync{.sendClock = true, .receiveTransport = true, .ppqn = 48};

    const synth::RuntimeConfigFileStatus saveStatus = synth::SaveRuntimeConfigFile(configFile, instrument, audio, sync);
    REQUIRE_TRUE(saveStatus == synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(std::string(synth::RuntimeConfigFileStatusName(saveStatus)) == "Ok");
    REQUIRE_TRUE(std::filesystem::exists(configFile));

    for (const auto& entry : std::filesystem::directory_iterator(configFile.parent_path())) {
        REQUIRE_TRUE(entry.path().extension() != ".tmp");
    }

    synth::MidiInstrumentConfig loadedInstrument;
    synth::AudioDeviceState loadedAudio;
    synth::SyncConfig loadedSync;
    const synth::RuntimeConfigFileStatus loadStatus = synth::LoadRuntimeConfigFile(
        configFile, loadedInstrument, loadedAudio, loadedSync);
    REQUIRE_TRUE(loadStatus == synth::RuntimeConfigFileStatus::Ok);
    REQUIRE_TRUE(loadedInstrument.controllers.size() == 1);
    REQUIRE_TRUE(loadedInstrument.controllers[0].input.identifier == "saved-in");
    REQUIRE_TRUE(loadedInstrument.controllers[0].output.identifier == "saved-out");
    REQUIRE_TRUE(loadedAudio.outputDeviceName == "Saved Out");
    REQUIRE_TRUE(loadedAudio.inputDeviceName == "Saved In");
    REQUIRE_TRUE(loadedSync == sync);

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(revert_all_to_defaults_resets_values_controls_and_existing_depths_only) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 2,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.25f});
    auto& depth = cutoff.EnsureModulationDepth(0, {.name = "Cutoff LFO", .defaultValue = 0.5f});
    manager.SetSceneEndpoints(0, 1);
    manager.SetSceneBlend(0.35f);
    manager.SetGestureValue(0, 0.4f);
    manager.SelectGesture(0);
    manager.CaptureDefaultControlState();

    cutoff.SceneCenter(0) = 0.91f;
    cutoff.SceneCenter(1) = 0.83f;
    cutoff.GestureValue(1, 0) = 0.73f;
    cutoff.SetGestureActive(1, 0, true);
    depth.SceneCenter(0) = 0.44f;
    depth.GestureValue(1, 0) = 0.22f;
    manager.SetSceneBlend(1.0f);
    manager.SetResetHeld(true);
    manager.DeselectGesture(0);

    manager.RevertAllToDefaults();
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.25f, 0.000001f);
    REQUIRE_NEAR(cutoff.SceneCenter(1), 0.25f, 0.000001f);
    REQUIRE_NEAR(cutoff.GestureValue(1, 0), 0.25f, 0.000001f);
    REQUIRE_TRUE(!cutoff.GestureActive(1, 0));
    REQUIRE_NEAR(depth.SceneCenter(0), 0.5f, 0.000001f);
    REQUIRE_NEAR(depth.GestureValue(1, 0), 0.5f, 0.000001f);
    REQUIRE_TRUE(cutoff.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(manager.ParameterCount() == 1);
    REQUIRE_TRUE(manager.Scene().leftScene == 0);
    REQUIRE_TRUE(manager.Scene().rightScene == 1);
    REQUIRE_NEAR(manager.Scene().blend, 0.35f, 0.000001f);
    REQUIRE_TRUE(!manager.ResetHeld());
    REQUIRE_TRUE(manager.GestureSelected(0));
    REQUIRE_NEAR(manager.GestureValue(0), 0.4f, 0.000001f);
}

TEST_CASE(patch_messages_serialize_load_and_revert_initialized_state) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    manager.CaptureDefaultControlState();

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 2;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig defaultInstrument;  // zero controllers: valid default
    synth::MidiInstrumentConfig instrument = MakeInstrumentFromProfile(defaultProfile, "in-a", "out-a");
    synth::AudioDeviceState defaultAudioDevice;
    synth::AudioDeviceState audioDevice{.outputDeviceName = "Speakers", .inputDeviceName = "Microphone"};
    synth::MessageOutBus outputBus(4);

    cutoff.SceneCenter(0) = 0.66f;
    const auto status = synth::ApplyPatchMessage(
        synth::PatchMessageIn::SerializeToJSON(42, "Patch A"), manager, instrument, defaultInstrument,
        audioDevice, defaultAudioDevice, outputBus);
    REQUIRE_TRUE(status == synth::PatchApplyStatus::Serialized);
    synth::MessageOut out;
    REQUIRE_TRUE(outputBus.Pop(out));
    REQUIRE_TRUE(out.requestId == 42);
    REQUIRE_TRUE(out.document.arena != nullptr);
    REQUIRE_TRUE(synth::ValidatePatchJSON(out.document.root));
    REQUIRE_TRUE(out.document.root.Get("midiInstrument").IsNull());
    REQUIRE_TRUE(out.document.root.Get("audioDevice").IsNull());
    REQUIRE_TRUE(synth::ApplyPatchMessage(
                     synth::PatchMessageIn::SerializeToJSON(43, "Too Small"), manager, instrument, defaultInstrument,
                     audioDevice, defaultAudioDevice, outputBus,
                     synth::PatchSerializationContext{.initialArenaCapacity = 1, .maxArenaCapacity = 1}) ==
                 synth::PatchApplyStatus::ArenaExhausted);

    cutoff.SceneCenter(0) = 0.1f;
    instrument.controllers[0].input.identifier = "changed";
    audioDevice.outputDeviceName = "changed";
    audioDevice.inputDeviceName = "also changed";
    REQUIRE_TRUE(synth::ApplyPatchMessage(
                     synth::PatchMessageIn::LoadFromJSON(out.document), manager, instrument, defaultInstrument,
                     audioDevice, defaultAudioDevice, outputBus) ==
                 synth::PatchApplyStatus::Applied);
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.66f, 0.000001f);
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "changed");
    REQUIRE_TRUE(audioDevice.outputDeviceName == "changed");
    REQUIRE_TRUE(audioDevice.inputDeviceName == "also changed");

    cutoff.SceneCenter(0) = 0.99f;
    instrument.controllers[0].input.identifier = "still changed";
    audioDevice.outputDeviceName = "still changed";
    audioDevice.inputDeviceName = "still also changed";
    REQUIRE_TRUE(synth::ApplyPatchMessage(
                     synth::PatchMessageIn::RevertAllToDefault(), manager, instrument, defaultInstrument,
                     audioDevice, defaultAudioDevice, outputBus) ==
                 synth::PatchApplyStatus::Reverted);
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.2f, 0.000001f);
    REQUIRE_TRUE(instrument.controllers.size() == 1);
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "still changed");
    REQUIRE_TRUE(audioDevice.outputDeviceName == "still changed");
    REQUIRE_TRUE(audioDevice.inputDeviceName == "still also changed");
}

TEST_CASE(apply_patch_message_revert_preserves_non_empty_runtime_configuration) {
    synth::ParameterManager manager;
    manager.SetGestureCount(0);
    manager.CreateGroup({.numVoices = 1, .numModulators = 0, .numScenes = 1, .maxParameters = 1});
    manager.CaptureDefaultControlState();

    const synth::MidiInstrumentConfig defaultInstrument =
        MakeInstrumentFromProfile(synth::WrldBldrDefaultProfileConfig({}), "default-in", "default-out");
    synth::MidiInstrumentConfig instrument = defaultInstrument;

    // Perturb: rename+re-endpoint the existing controller, and add a second.
    synth::MidiControllerSlot renamed = instrument.controllers[0];
    renamed.name = "renamed";
    renamed.input.identifier = "perturbed-in";
    REQUIRE_TRUE(instrument.ReplaceController(0, renamed));
    synth::MidiControllerSlot extra;
    extra.name = "extra";
    extra.kind = synth::MidiProfileKind::Launchpad;
    REQUIRE_TRUE(instrument.AddController(extra));
    REQUIRE_TRUE(instrument.controllers.size() == 2);

    synth::AudioDeviceState defaultAudioDevice;
    synth::AudioDeviceState audioDevice;
    synth::MessageOutBus outputBus(4);

    REQUIRE_TRUE(synth::ApplyPatchMessage(synth::PatchMessageIn::RevertAllToDefault(), manager, instrument,
                                          defaultInstrument, audioDevice, defaultAudioDevice, outputBus) ==
                 synth::PatchApplyStatus::Reverted);

    REQUIRE_TRUE(instrument.controllers.size() == 2);
    REQUIRE_TRUE(instrument.controllers[0].name == "renamed");
    REQUIRE_TRUE(instrument.controllers[0].input.identifier == "perturbed-in");
    REQUIRE_TRUE(instrument.controllers[1].name == "extra");
}

TEST_CASE(apply_patch_message_load_absent_audio_device_section_leaves_state_untouched) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    manager.CaptureDefaultControlState();

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 2;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
    synth::MidiInstrumentConfig instrument = defaultInstrument;
    synth::AudioDeviceState defaultAudioDevice;
    // Both device names empty, so BuildPatchJSON omits the "audioDevice" key.
    synth::AudioDeviceState emptyAudioDevice;
    synth::MessageOutBus outputBus(4);

    const auto serializeStatus = synth::ApplyPatchMessage(
        synth::PatchMessageIn::SerializeToJSON(1, "No Device"), manager, instrument, defaultInstrument,
        emptyAudioDevice, defaultAudioDevice, outputBus);
    REQUIRE_TRUE(serializeStatus == synth::PatchApplyStatus::Serialized);
    synth::MessageOut out;
    REQUIRE_TRUE(outputBus.Pop(out));
    REQUIRE_TRUE(out.document.root.Get("audioDevice").IsNull());

    synth::AudioDeviceState untouchedAudioDevice{.outputDeviceName = "Preexisting", .inputDeviceName = "Existing"};
    REQUIRE_TRUE(synth::ApplyPatchMessage(
                     synth::PatchMessageIn::LoadFromJSON(out.document), manager, instrument, defaultInstrument,
                     untouchedAudioDevice, defaultAudioDevice, outputBus) ==
                 synth::PatchApplyStatus::Applied);
    REQUIRE_TRUE(untouchedAudioDevice.outputDeviceName == "Preexisting");
    REQUIRE_TRUE(untouchedAudioDevice.inputDeviceName == "Existing");
}

TEST_CASE(apply_patch_message_reuses_caller_arena) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    manager.CaptureDefaultControlState();

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 2;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
    synth::MidiInstrumentConfig instrument = MakeInstrumentFromProfile(defaultProfile, "in-a", "out-a");
    synth::AudioDeviceState defaultAudioDevice;
    synth::AudioDeviceState audioDevice{.outputDeviceName = "Speakers", .inputDeviceName = "Microphone"};
    synth::MessageOutBus outputBus(4);

    synth::JsonArena arena(64 * 1024);
    synth::PatchSerializationContext context;
    context.arena = &arena;

    // Consume-before-reuse: the caller-owned arena is reset on every
    // ApplyPatchMessage call, so the document from a prior serialize must be
    // fully popped and read before the next call reuses the same arena.
    const auto first = synth::ApplyPatchMessage(
        synth::PatchMessageIn::SerializeToJSON(1, "A"), manager, instrument, defaultInstrument,
        audioDevice, defaultAudioDevice, outputBus, context);
    REQUIRE_TRUE(first == synth::PatchApplyStatus::Serialized);

    synth::MessageOut out;
    REQUIRE_TRUE(outputBus.Pop(out));
    REQUIRE_TRUE(out.requestId == 1);
    REQUIRE_TRUE(synth::ValidatePatchJSON(out.document.root));
    REQUIRE_TRUE(std::string(out.document.root.Get("patchName").StringValue()) == "A");

    const auto second = synth::ApplyPatchMessage(
        synth::PatchMessageIn::SerializeToJSON(2, "B"), manager, instrument, defaultInstrument,
        audioDevice, defaultAudioDevice, outputBus, context);
    REQUIRE_TRUE(second == synth::PatchApplyStatus::Serialized);  // arena reused after prior document consumed

    REQUIRE_TRUE(outputBus.Pop(out));
    REQUIRE_TRUE(out.requestId == 2);
    REQUIRE_TRUE(synth::ValidatePatchJSON(out.document.root));
    REQUIRE_TRUE(std::string(out.document.root.Get("patchName").StringValue()) == "B");
}

TEST_CASE(apply_patch_message_reports_exhaustion_without_growing_caller_arena) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 1,
        .numScenes = 2,
        .maxParameters = 2,
    });
    manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    manager.CaptureDefaultControlState();

    synth::WrldBldrDefaultProfileOptions options;
    options.visibleEncoderCount = 1;
    options.sceneCount = 2;
    options.bankButtonCount = 1;
    options.gestureSelectorCount = 1;
    const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig(options);
    const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
    synth::MidiInstrumentConfig instrument = MakeInstrumentFromProfile(defaultProfile, "in-a", "out-a");
    synth::AudioDeviceState defaultAudioDevice;
    synth::AudioDeviceState audioDevice{.outputDeviceName = "Speakers", .inputDeviceName = "Microphone"};
    synth::MessageOutBus outputBus(4);

    synth::JsonArena tiny(64);  // far too small for any patch document
    synth::PatchSerializationContext context;
    context.arena = &tiny;

    const auto status = synth::ApplyPatchMessage(
        synth::PatchMessageIn::SerializeToJSON(3, "C"), manager, instrument, defaultInstrument,
        audioDevice, defaultAudioDevice, outputBus, context);
    REQUIRE_TRUE(status == synth::PatchApplyStatus::ArenaExhausted);
    REQUIRE_TRUE(tiny.Capacity() == 64);  // caller's arena was not grown/reallocated
}

TEST_CASE(patch_manager_save_load_revert_lifecycle_uses_messages_and_current_directory) {
    synth::PatchMessageInBus inputBus(8);
    synth::MessageOutBus outputBus(8);
    synth::PatchManager patchManager(&inputBus, &outputBus);

    synth::ParameterManager manager;
    manager.SetGestureCount(0);
    auto& group = manager.CreateGroup({
        .numVoices = 1,
        .numModulators = 0,
        .numScenes = 1,
        .maxParameters = 2,
    });
    auto& cutoff = manager.CreateParameter(group, {.name = "Cutoff", .defaultValue = 0.2f});
    manager.CaptureDefaultControlState();

    const synth::MidiControllerProfileConfig defaultProfile = synth::WrldBldrDefaultProfileConfig({});
    const synth::MidiInstrumentConfig defaultInstrument = MakeInstrumentFromProfile(defaultProfile);
    synth::MidiInstrumentConfig instrument = defaultInstrument;
    synth::AudioDeviceState defaultAudioDevice;
    synth::AudioDeviceState audioDevice;

    const auto tempRoot = std::filesystem::temp_directory_path() /
                          ("sheaf-synth-patch-manager-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path patchDir = tempRoot / "Patch A";
    std::filesystem::remove_all(tempRoot);

    REQUIRE_TRUE(patchManager.SavePatch().status == synth::PatchCommandStatus::NeedsSaveAsPath);

    cutoff.SceneCenter(0) = 0.72f;
    synth::PatchCommandResult saveAs = patchManager.SavePatchAs(patchDir);
    REQUIRE_TRUE(saveAs.status == synth::PatchCommandStatus::Pending);
    REQUIRE_TRUE(patchManager.SavePatchAs(tempRoot / "Patch B").status == synth::PatchCommandStatus::Busy);
    const std::filesystem::path busyExistingPatchDir = tempRoot / "Already Exists While Busy";
    std::filesystem::create_directories(busyExistingPatchDir);
    REQUIRE_TRUE(patchManager.SavePatchAs(busyExistingPatchDir).status == synth::PatchCommandStatus::Busy);
    synth::PatchMessageIn message;
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(message.type == synth::PatchMessageIn::Type::SerializeToJSON);
    REQUIRE_TRUE(message.requestId == saveAs.requestId);
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Serialized);
    synth::PatchCommandResult written = patchManager.ProcessResponses(std::chrono::system_clock::from_time_t(1700000100));
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::Written);
    REQUIRE_TRUE(patchManager.CurrentPatchDirectory().has_value());
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);
    REQUIRE_TRUE(std::filesystem::exists(written.path));
    const std::filesystem::path firstVersion = written.path;

    REQUIRE_TRUE(patchManager.SavePatchAs(patchDir).status == synth::PatchCommandStatus::AlreadyExists);

    cutoff.SceneCenter(0) = 0.81f;
    synth::PatchCommandResult overwriteSaveAs = patchManager.SavePatchAsOverwrite(patchDir);
    REQUIRE_TRUE(overwriteSaveAs.status == synth::PatchCommandStatus::Pending);
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(message.type == synth::PatchMessageIn::Type::SerializeToJSON);
    REQUIRE_TRUE(message.requestId == overwriteSaveAs.requestId);
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Serialized);
    written = patchManager.ProcessResponses(std::chrono::system_clock::from_time_t(1700000101));
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::Written);
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);
    REQUIRE_TRUE(std::filesystem::exists(written.path));
    REQUIRE_TRUE(written.path.parent_path() == patchDir);

    const std::filesystem::path racedPatchDir = tempRoot / "Race Patch";
    saveAs = patchManager.SavePatchAs(racedPatchDir);
    REQUIRE_TRUE(saveAs.status == synth::PatchCommandStatus::Pending);
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Serialized);
    std::filesystem::create_directories(racedPatchDir);
    written = patchManager.ProcessResponses(std::chrono::system_clock::from_time_t(1700000102));
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::AlreadyExists);
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);

    cutoff.SceneCenter(0) = 0.84f;
    synth::PatchCommandResult save = patchManager.SavePatch();
    REQUIRE_TRUE(save.status == synth::PatchCommandStatus::Pending);
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Serialized);
    written = patchManager.ProcessResponses(std::chrono::system_clock::from_time_t(1700000102));
    REQUIRE_TRUE(written.status == synth::PatchCommandStatus::Written);
    REQUIRE_TRUE(written.path != firstVersion);

    cutoff.SceneCenter(0) = 0.1f;
    REQUIRE_TRUE(patchManager.LoadPatch(firstVersion).status == synth::PatchCommandStatus::Ok);
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(message.type == synth::PatchMessageIn::Type::LoadFromJSON);
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Applied);
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.72f, 0.000001f);
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);

    REQUIRE_TRUE(patchManager.LoadPatch(tempRoot / "missing").status == synth::PatchCommandStatus::NotFound);
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);
    const std::filesystem::path invalidPatchDir = tempRoot / "Invalid";
    std::filesystem::create_directories(invalidPatchDir);
    const std::filesystem::path invalidVersion = invalidPatchDir / "20231114T221640Z-000.json";
    {
        std::ofstream out(invalidVersion);
        out << R"({"schema":"sheaf.synth.patch","schemaVersion":999,"patchName":"Invalid"})";
    }
    REQUIRE_TRUE(patchManager.LoadPatch(invalidVersion).status == synth::PatchCommandStatus::InvalidPatch);
    REQUIRE_TRUE(*patchManager.CurrentPatchDirectory() == patchDir);

    cutoff.SceneCenter(0) = 0.0f;
    REQUIRE_TRUE(patchManager.RevertPatch().status == synth::PatchCommandStatus::Ok);
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(message.type == synth::PatchMessageIn::Type::LoadFromJSON);
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Applied);
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.84f, 0.000001f);

    REQUIRE_TRUE(patchManager.NewPatch().status == synth::PatchCommandStatus::Ok);
    REQUIRE_TRUE(!patchManager.CurrentPatchDirectory().has_value());
    REQUIRE_TRUE(inputBus.Pop(message));
    REQUIRE_TRUE(message.type == synth::PatchMessageIn::Type::RevertAllToDefault);
    REQUIRE_TRUE(synth::ApplyPatchMessage(message, manager, instrument, defaultInstrument,
                                          audioDevice, defaultAudioDevice, outputBus) == synth::PatchApplyStatus::Reverted);
    REQUIRE_NEAR(cutoff.SceneCenter(0), 0.2f, 0.000001f);

    std::filesystem::remove_all(tempRoot);
}

TEST_CASE(invalid_indices_throw) {
    synth::Modulators modulators(1, 2);
    const float shortDepths[] = {1.0f};

    bool threw = false;
    try {
        (void)modulators.Value(1, 0);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)modulators.Value(0, 2);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        (void)modulators.Apply(0, std::span<const float>(shortDepths));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    synth::Gestures gestures(1);
    threw = false;
    try {
        gestures.Select(1, true);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);

    threw = false;
    try {
        synth::ParameterManager manager;
    manager.SetGestureCount(2);
        manager.CreateGroup({.numVoices = 0, .numScenes = 1, .maxParameters = 1});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(compute_all_targets_preserves_process_lite_slew) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 0,
                                       .numScenes = 1,
                                       .maxParameters = 4,
                                       .processLiteAlpha = 0.1f,
                                       .targetCenterAlpha = 1.0f});
    auto& parameter = manager.CreateParameter(group, {.name = "SlewProbe", .defaultValue = 0.0f});
    manager.ComputeAllParameters();
    REQUIRE_NEAR(parameter.GetRaw(0), 0.0f, 1e-6f);

    parameter.SceneCenter(0) = 1.0f;

    manager.ComputeAllTargets();
    const float afterTargets = parameter.GetRaw(0);
    REQUIRE_NEAR(afterTargets, 0.0f, 1e-6f);  // current not snapped

    parameter.ProcessLite();
    const float afterOneSlew = parameter.GetRaw(0);
    REQUIRE_TRUE(afterOneSlew > 0.0f);
    REQUIRE_TRUE(afterOneSlew < 1.0f);        // approaching, not jumped

    manager.ComputeAllParameters();           // existing API still snaps
    REQUIRE_NEAR(parameter.GetRaw(0), 1.0f, 1e-4f);
}

namespace {

float FullScanApply(const synth::Modulators& modulators, std::size_t voiceIx,
                    std::span<const float> depthsBySource) {
    float sum = 0.0f;
    for (std::size_t sourceIx = 0; sourceIx < depthsBySource.size(); ++sourceIx) {
        sum += modulators.Value(voiceIx, sourceIx) * depthsBySource[sourceIx];
    }
    return sum;
}

void RequireRouteBijection(const synth::Parameter& parameter, std::size_t sourceCount) {
    const auto sources = parameter.ActiveRouteSourceIndices();
    REQUIRE_TRUE(sources.size() == parameter.ActiveRouteCount());
    std::vector<bool> seen(sourceCount, false);
    for (std::size_t slot = 0; slot < sourceCount; ++slot) {
        const std::size_t sourceIx = parameter.RouteSourceIndex(slot);
        REQUIRE_TRUE(sourceIx < sourceCount);
        REQUIRE_TRUE(!seen[sourceIx]);
        seen[sourceIx] = true;
        REQUIRE_TRUE(parameter.RoutePositionForSource(sourceIx) == slot);
    }
}

void RequireFullScanCurrentMatch(const synth::Parameter& parameter) {
    const std::size_t sourceCount = parameter.Group().Config().numModulators;
    RequireRouteBijection(parameter, sourceCount);
    std::vector<float> depthsBySource(sourceCount, 0.0f);
    for (std::size_t voiceIx = 0; voiceIx < parameter.Group().Config().numVoices; ++voiceIx) {
        for (std::size_t sourceIx = 0; sourceIx < sourceCount; ++sourceIx) {
            depthsBySource[sourceIx] = parameter.CurrentDepthForSource(voiceIx, sourceIx);
        }
        const float expected = synth::ClampToRange(
            parameter.CurrentCenter() * parameter.CurrentCenterScale(voiceIx) +
                parameter.CurrentNormalizationOffset(voiceIx) +
                FullScanApply(parameter.Group().GetModulators(), voiceIx, depthsBySource),
            parameter.Range());
        REQUIRE_NEAR(parameter.GetRaw(voiceIx), expected, 0.000001f);
    }
}

}  // namespace

TEST_CASE(modulators_apply_active_uses_explicit_stable_source_indices) {
    synth::Modulators modulators(1, 4);
    modulators.Value(0, 0) = 0.2f;
    modulators.Value(0, 1) = -0.4f;
    modulators.Value(0, 2) = 0.7f;
    modulators.Value(0, 3) = 0.9f;
    const std::array<float, 2> depths = {0.5f, -0.25f};
    const std::array<std::size_t, 2> sources = {3, 0};
    const std::array<float, 4> fullDepths = {-0.25f, 0.0f, 0.0f, 0.5f};

    REQUIRE_NEAR(modulators.ApplyActive(0, depths, sources),
                 FullScanApply(modulators, 0, fullDepths), 0.000001f);

    bool threw = false;
    try {
        (void)modulators.ApplyActive(0, depths, std::span<const std::size_t>(sources).first(1));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    REQUIRE_TRUE(threw);
}

TEST_CASE(active_modulation_routes_preserve_identity_and_settling_tail) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({.numVoices = 2,
                                       .numModulators = 4,
                                       .numScenes = 2,
                                       .maxParameters = 16,
                                       .processLiteAlpha = 1.0f,
                                       .targetCenterAlpha = 1.0f});
    group.GetModulators().Metadata(0) = {.name = "Zero", .shortName = "Z", .sourceColor = synth::Color::Red};
    group.GetModulators().Metadata(1) = {.name = "One", .shortName = "O", .sourceColor = synth::Color::Orange};
    group.GetModulators().Metadata(2) = {.name = "Two", .shortName = "T", .sourceColor = synth::Color::Green};
    group.GetModulators().Metadata(3) = {.name = "Three", .shortName = "H", .sourceColor = synth::Color::Cyan};
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth3 = carrier.EnsureModulationDepth(3);
    auto* depth0 = carrier.EnsureModulationDepth(0);
    auto* depth2 = carrier.EnsureModulationDepth(2);
    auto* depth1 = carrier.EnsureModulationDepth(1);
    REQUIRE_TRUE(depth3 != nullptr);
    REQUIRE_TRUE(depth0 != nullptr);
    REQUIRE_TRUE(depth2 != nullptr);
    REQUIRE_TRUE(depth1 != nullptr);
    group.GetModulators().Value(0, 0) = -0.25f;
    group.GetModulators().Value(0, 2) = 0.5f;
    group.GetModulators().Value(0, 3) = 0.75f;
    group.GetModulators().Value(1, 0) = 0.4f;
    group.GetModulators().Value(1, 2) = -0.6f;
    group.GetModulators().Value(1, 3) = 0.1f;

    depth3->SceneCenter(0) = 0.75f;
    depth3->SceneCenter(1) = 0.75f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 1);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[0] == 3);
    RequireFullScanCurrentMatch(carrier);

    depth0->SceneCenter(0) = 0.7f;
    depth0->SceneCenter(1) = 0.7f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 2);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[0] == 3);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[1] == 0);

    depth2->SceneCenter(0) = 0.8f;
    depth2->SceneCenter(1) = 0.8f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 3);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[2] == 2);
    RequireFullScanCurrentMatch(carrier);

    depth1->SceneCenter(0) = 0.65f;
    depth1->SceneCenter(1) = 0.65f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 4);
    RequireFullScanCurrentMatch(carrier);

    depth0->SceneCenter(0) = 0.5f;
    depth0->SceneCenter(1) = 0.5f;
    depth0->GestureValue(0, 0) = 0.6f;  // latent persisted state must retain source key 0
    depth1->SceneCenter(0) = 0.5f;
    depth1->SceneCenter(1) = 0.5f;
    manager.ComputeAllTargets();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 4);
    carrier.ProcessLite();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 4);
    manager.ComputeAllTargets();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 2);
    REQUIRE_TRUE(carrier.RoutePositionForSource(0) >= carrier.ActiveRouteCount());
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[0] == 3);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[1] == 2);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(2) == depth2);
    REQUIRE_TRUE(depth0->Name() == "Carrier Zero");
    REQUIRE_TRUE(depth2->Name() == "Carrier Two");
    synth::Parameter::UIState ui(2, 4, 1);
    carrier.PopulateUIState(ui);
    REQUIRE_TRUE(ui.modulatorSourceColors[0].Load() == synth::Color::Red);
    REQUIRE_TRUE(ui.modulatorSourceColors[2].Load() == synth::Color::Green);
    synth::JsonArena arena(16384);
    const synth::JSON values = carrier.ToValueJSON(arena);
    REQUIRE_TRUE(!values.Get("modDepths").Get("0").IsNull());
    REQUIRE_TRUE(values.Get("modDepths").Get("1").IsNull());
    REQUIRE_TRUE(!values.Get("modDepths").Get("2").IsNull());
    REQUIRE_TRUE(!values.Get("modDepths").Get("3").IsNull());
    RequireFullScanCurrentMatch(carrier);
}

TEST_CASE(active_modulation_route_union_keeps_source_with_only_voice_one_nonzero) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 2,
                                       .numModulators = 3,
                                       .numScenes = 1,
                                       .maxParameters = 12,
                                       .processLiteAlpha = 1.0f,
                                       .targetCenterAlpha = 1.0f});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.5f});
    auto* depth = carrier.EnsureModulationDepth(2);
    REQUIRE_TRUE(depth != nullptr);
    auto* nested = depth->EnsureModulationDepth(0);
    REQUIRE_TRUE(nested != nullptr);
    nested->SceneCenter(0) = 0.75f;
    group.GetModulators().Value(0, 0) = 0.5f;
    group.GetModulators().Value(1, 0) = 1.0f;
    group.GetModulators().Value(0, 2) = -0.8f;
    group.GetModulators().Value(1, 2) = 0.6f;

    manager.ComputeAllParameters();

    REQUIRE_NEAR(carrier.CurrentDepthForSource(0, 2), 0.0f, 0.000001f);
    REQUIRE_TRUE(std::fabs(carrier.CurrentDepthForSource(1, 2)) > 0.000001f);
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 1);
    REQUIRE_TRUE(carrier.ActiveRouteSourceIndices()[0] == 2);
    RequireFullScanCurrentMatch(carrier);
}

TEST_CASE(active_modulation_routes_randomized_full_scan_oracle_and_work_bound) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 2,
                                       .numModulators = 4,
                                       .numScenes = 2,
                                       .maxParameters = 16,
                                       .processLiteAlpha = 0.25f,
                                       .targetCenterAlpha = 1.0f});
    synth::ParameterProcessingObserver work;
    group.SetProcessingObserverForTests(&work);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.35f});
    std::array<synth::Parameter*, 4> depths{};
    for (std::size_t sourceIx = 0; sourceIx < depths.size(); ++sourceIx) {
        depths[sourceIx] = carrier.EnsureModulationDepth(sourceIx);
        REQUIRE_TRUE(depths[sourceIx] != nullptr);
    }
    std::mt19937 random(0x5a17eU);
    std::uniform_int_distribution<int> sourceDistribution(0, 3);
    std::uniform_int_distribution<int> valueDistribution(0, 4);
    std::uniform_real_distribution<float> modulatorDistribution(-1.0f, 1.0f);

    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 0);
    for (std::size_t step = 0; step < 128; ++step) {
        const std::size_t sourceIx = static_cast<std::size_t>(sourceDistribution(random));
        const float knob = 0.5f + 0.1f * static_cast<float>(valueDistribution(random) - 2);
        depths[sourceIx]->SceneCenter(step & 1U) = knob;
        for (std::size_t voiceIx = 0; voiceIx < 2; ++voiceIx) {
            for (std::size_t modIx = 0; modIx < 4; ++modIx) {
                group.GetModulators().Value(voiceIx, modIx) = modulatorDistribution(random);
            }
        }

        REQUIRE_TRUE(manager.SetSceneEndpoints(0, 1));
        manager.SetSceneBlend(static_cast<float>(step % 5) * 0.25f);
        manager.ComputeAllTargets();
        const std::size_t visitsBefore = work.activeRouteVisits;
        carrier.ProcessLite();
        REQUIRE_TRUE(work.activeRouteVisits - visitsBefore == carrier.ActiveRouteCount() * 2);
        RequireFullScanCurrentMatch(carrier);
    }
}

TEST_CASE(neutral_local_collection_reclaims_leaf_and_preserves_high_water_accounting) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 2,
                                       .numScenes = 1,
                                       .maxParameters = 5});
    group.GetModulators().Metadata(0) = {.name = "Old", .shortName = "Old", .sourceColor = synth::Color::Red};
    group.GetModulators().Metadata(1) = {.name = "New", .shortName = "New", .sourceColor = synth::Color::Cyan};
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .shortName = "Car", .defaultValue = 0.4f});
    auto& other = manager.CreateParameter(group, {.name = "Other", .shortName = "Oth", .defaultValue = 0.6f});
    synth::Parameter* oldLocal = &carrier.EnsureModulationDepth(
        0, {.name = "Old Local",
            .shortName = "Old",
            .defaultValue = 0.5f,
            .range = synth::RangeKind::Bipolar,
            .switchValues = 7,
            .baseColor = synth::Color::Red,
            .indicatorColors = {synth::Color::Orange}});
    REQUIRE_TRUE(oldLocal != nullptr);
    const synth::Parameter* carrierAddress = &carrier;
    const synth::Parameter* otherAddress = &other;
    const std::size_t recycledStorageIx = group.ParameterCount() - 1;
    REQUIRE_TRUE(&group.ParameterByLocalIndex(recycledStorageIx) == oldLocal);

    const std::size_t highWater = group.ParameterCount();
    const std::size_t availableBefore = group.AvailableParameterSlots();
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 1);
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 1);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(group.ParameterCount() == highWater);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 1);
    REQUIRE_TRUE(group.AvailableParameterSlots() == availableBefore + 1);

    synth::Parameter* reused = other.EnsureModulationDepth(1);
    REQUIRE_TRUE(reused != nullptr);
    REQUIRE_TRUE(group.ParameterCount() == highWater);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 1);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 0);
    REQUIRE_TRUE(&carrier == carrierAddress);
    REQUIRE_TRUE(&other == otherAddress);
    REQUIRE_TRUE(&group.ParameterByLocalIndex(recycledStorageIx) == reused);
    REQUIRE_TRUE(reused->Name() == "Other New");
    REQUIRE_TRUE(reused->ShortName() == "New");
    REQUIRE_TRUE(reused->BaseColor() == synth::Color::Cyan);
    REQUIRE_TRUE(reused->Range() == synth::RangeKind::Bipolar);
    REQUIRE_TRUE(reused->SwitchValues() == 0);
    REQUIRE_NEAR(reused->SceneCenter(0), 0.5f, 0.000001f);
    REQUIRE_NEAR(reused->CurrentCenter(), 0.5f, 0.000001f);
    REQUIRE_NEAR(reused->TargetCenter(), 0.5f, 0.000001f);
    REQUIRE_NEAR(reused->CurrentCenterScale(0), 1.0f, 0.000001f);
    REQUIRE_NEAR(reused->TargetCenterScale(0), 1.0f, 0.000001f);
    REQUIRE_NEAR(reused->CurrentNormalizationOffset(0), 0.0f, 0.000001f);
    REQUIRE_NEAR(reused->TargetNormalizationOffset(0), 0.0f, 0.000001f);
    REQUIRE_TRUE(reused->ActiveRouteCount() == 0);
    REQUIRE_TRUE(reused->RouteSourceIndex(0) == 0);
    REQUIRE_TRUE(reused->RouteSourceIndex(1) == 1);
    REQUIRE_TRUE(reused->ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(reused->ModulationDepthParameter(1) == nullptr);
    REQUIRE_NEAR(reused->GestureValue(0, 32), 0.5f, 0.000001f);
    REQUIRE_NEAR(reused->GestureValue(0, 63), 0.5f, 0.000001f);
    REQUIRE_TRUE(!reused->GestureActive(0, 32));
    REQUIRE_TRUE(!reused->GestureActive(0, 63));
    synth::Parameter::UIState ui(1, 2, 64);
    reused->PopulateUIState(ui);
    REQUIRE_NEAR(ui.values[0].load(), 0.0f, 0.000001f);
    REQUIRE_NEAR(ui.spreadValues[0].load(), 0.0f, 0.000001f);
    REQUIRE_NEAR(ui.minValues[0].load(), 0.0f, 0.000001f);
    REQUIRE_NEAR(ui.maxValues[0].load(), 0.0f, 0.000001f);
    REQUIRE_TRUE(ui.modulatorsAffectingMask.load() == 0);
    REQUIRE_TRUE(ui.gesturesAffectingMask.load() == 0);
}

TEST_CASE(neutral_local_collection_retains_non_default_scene_state) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 2, .maxParameters = 2});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(1) = 0.6f;

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);
}

TEST_CASE(neutral_local_collection_retains_inactive_latent_gesture_value) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 2});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->GestureValue(0, 63) = 0.6f;
    REQUIRE_TRUE(!depth->GestureActive(0, 63));

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);
}

TEST_CASE(neutral_local_collection_retains_active_gesture_at_default_value) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 2});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SetGestureActive(0, 32, true);

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);
}

TEST_CASE(neutral_local_collection_retains_unsnapped_runtime_state) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 1,
                                       .numScenes = 1,
                                       .maxParameters = 2,
                                       .targetCenterAlpha = 1.0f});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 0.75f;
    manager.ComputeAllParameters();
    depth->SceneCenter(0) = 0.5f;

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);
}

TEST_CASE(neutral_local_collection_retains_nonzero_normalization_state) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 1,
                                       .numScenes = 1,
                                       .maxParameters = 3,
                                       .targetCenterAlpha = 1.0f});
    group.GetModulators().Value(0, 0) = 1.0f;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    auto* nested = depth->EnsureModulationDepth(0);
    REQUIRE_TRUE(nested != nullptr);
    nested->SceneCenter(0) = 0.25f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(depth->CurrentNormalizationOffset(0) > 0.0f);
    REQUIRE_TRUE(depth->TargetNormalizationOffset(0) > 0.0f);

    depth->ClearModulationDepths();
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);

    REQUIRE_TRUE(depth->AssignModulationDepth(0, nested));
    depth->RevertAllToDefault();
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 2);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
}

TEST_CASE(neutral_local_collection_retains_parent_with_non_collectible_child) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 3});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    auto* nested = depth->EnsureModulationDepth(1);
    REQUIRE_TRUE(nested != nullptr);
    nested->GestureValue(0, 0) = 0.6f;

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == depth);
    REQUIRE_TRUE(depth->ModulationDepthParameter(1) == nested);
}

TEST_CASE(neutral_local_collection_collapses_recursive_subtree_bottom_up) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 3});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    REQUIRE_TRUE(depth->EnsureModulationDepth(1) != nullptr);
    const std::size_t highWater = group.ParameterCount();

    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 2);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(group.ParameterCount() == highWater);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 2);
}

TEST_CASE(neutral_local_collection_detaches_child_while_parent_route_finishes_settling) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1,
                                       .numModulators = 1,
                                       .numScenes = 1,
                                       .maxParameters = 2,
                                       .processLiteAlpha = 0.5f,
                                       .targetCenterAlpha = 1.0f});
    group.GetModulators().Value(0, 0) = 1.0f;
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 0.75f;
    manager.ComputeAllParameters();
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 1);
    REQUIRE_TRUE(std::fabs(carrier.CurrentDepthForSource(0, 0)) > 0.000001f);

    depth->SceneCenter(0) = 0.5f;
    manager.ComputeAllTargets();
    REQUIRE_NEAR(carrier.TargetDepthForSource(0, 0), 0.0f, 0.000001f);
    REQUIRE_TRUE(std::fabs(carrier.CurrentDepthForSource(0, 0)) > 0.000001f);
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 1);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 1);

    for (std::size_t step = 0; step < 32 && carrier.ActiveRouteCount() != 0; ++step) {
        carrier.ProcessLite();
        manager.ComputeAllTargets();
    }
    REQUIRE_TRUE(carrier.ActiveRouteCount() == 0);
    REQUIRE_NEAR(carrier.CurrentDepthForSource(0, 0), 0.0f, 0.000001f);
}

TEST_CASE(modulation_view_pins_visible_locals_until_deselect_boundary) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 3});
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.AddPhysicalEncoder(12);
    slot.SelectBank(&bank);

    slot.HandlePress(10);
    REQUIRE_TRUE(bank.ShowingModulation());
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 2);
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == bank.VisibleParameter(10));
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == bank.VisibleParameter(11));

    bank.Deselect();
    REQUIRE_TRUE(!bank.ShowingModulation());
    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(carrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 2);
}

TEST_CASE(multi_level_modulation_view_balances_pins_and_reuses_the_collapsed_subtree) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 6});
    MarkAllModulatorsConnectedForUi(group);
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    auto& other = manager.CreateParameter(group, {.name = "Other", .defaultValue = 0.6f});
    auto& bank = manager.CreateBank();
    bank.AddMapping(10, carrier);
    auto& slot = manager.CreateBankSlot();
    slot.AddPhysicalEncoder(10);
    slot.AddPhysicalEncoder(11);
    slot.SelectBank(&bank);

    slot.HandlePress(10);
    synth::Parameter* first = carrier.ModulationDepthParameter(0);
    REQUIRE_TRUE(first != nullptr);
    slot.HandlePress(10);
    synth::Parameter* second = first->ModulationDepthParameter(0);
    REQUIRE_TRUE(second != nullptr);
    slot.HandlePress(10);
    synth::Parameter* third = second->ModulationDepthParameter(0);
    REQUIRE_TRUE(third != nullptr);
    REQUIRE_TRUE(bank.SelectedParameter() == second);
    REQUIRE_TRUE(bank.VisibleParameter(10) == third);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 3);
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
    const std::size_t highWater = group.ParameterCount();

    bank.Deselect();

    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 3);
    synth::Parameter* reused = other.EnsureModulationDepth(0);
    REQUIRE_TRUE(reused == first || reused == second || reused == third);
    REQUIRE_TRUE(group.ParameterCount() == highWater);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 1);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 2);
}

TEST_CASE(revert_all_collects_neutral_local_topology_at_control_boundary) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 2});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.4f});
    manager.CaptureDefaultControlState();
    auto* depth = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(depth != nullptr);
    depth->SceneCenter(0) = 0.75f;

    manager.RevertAllToDefaults();

    REQUIRE_TRUE(carrier.ModulationDepthParameter(0) == nullptr);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 1);
}

TEST_CASE(neutral_local_reuse_stays_bounded_beyond_configured_capacity) {
    synth::ParameterManager manager;
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 1, .numScenes = 1, .maxParameters = 2});
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.25f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.75f});
    group.AddParameterStorageBatch(synth::MakeParameterStorageBatch(group.Config(), group.GestureCount(), 1));

    for (std::size_t iteration = 0; iteration < group.Config().maxParameters * 4; ++iteration) {
        synth::Parameter& parent = (iteration & 1U) == 0 ? first : second;
        REQUIRE_TRUE(parent.EnsureModulationDepth(0) != nullptr);
        REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 1);
        REQUIRE_TRUE(parent.ModulationDepthParameter(0) == nullptr);
    }
    REQUIRE_TRUE(group.ParameterCount() == 3);
    REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 1);
}

TEST_CASE(randomized_neutral_local_collection_reuses_slots_without_stale_topology) {
    synth::ParameterManager manager;
    manager.SetGestureCount(64);
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 2, .maxParameters = 5});
    auto& first = manager.CreateParameter(group, {.name = "First", .defaultValue = 0.25f});
    auto& second = manager.CreateParameter(group, {.name = "Second", .defaultValue = 0.75f});
    auto& third = manager.CreateParameter(group, {.name = "Third", .defaultValue = 0.5f});
    std::array<synth::Parameter*, 3> parents = {&first, &second, &third};
    std::mt19937 random(0x74c011ecU);

    for (std::size_t step = 0; step < 128; ++step) {
        synth::Parameter& parent = *parents[random() % parents.size()];
        const std::size_t sourceIx = random() % 2;
        synth::Parameter* local = parent.EnsureModulationDepth(sourceIx);
        REQUIRE_TRUE(local != nullptr);
        if ((random() & 3U) == 0) {
            local->GestureValue(1, 63) = 0.75f;
            REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 0);
            REQUIRE_TRUE(parent.ModulationDepthParameter(sourceIx) == local);
            local->RevertAllToDefault();
        }
        REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 1);
        REQUIRE_TRUE(parent.ModulationDepthParameter(sourceIx) == nullptr);
        REQUIRE_TRUE(group.LiveLocalParameterCount() == 0);
        REQUIRE_TRUE(group.ParameterCount() <= 4);
    }
    REQUIRE_TRUE(group.FreeLocalParameterSlotCount() == 1);
}

TEST_CASE(patch_load_collection_preserves_high_gesture_nested_state_and_collects_default_omissions) {
    synth::ParameterManager source;
    source.SetGestureCount(64);
    auto& sourceGroup = source.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 4});
    auto& sourceCarrier = source.CreateParameter(sourceGroup, {.name = "Carrier", .defaultValue = 0.25f});
    auto* sourceDepth = sourceCarrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(sourceDepth != nullptr);
    auto* sourceNested = sourceDepth->EnsureModulationDepth(1);
    REQUIRE_TRUE(sourceNested != nullptr);
    sourceDepth->GestureValue(0, 32) = 0.7f;
    sourceDepth->SetGestureActive(0, 32, true);
    sourceNested->GestureValue(0, 63) = 0.8f;
    sourceNested->SetGestureActive(0, 63, true);

    synth::JsonArena patchArena(262144);
    synth::MidiInstrumentConfig instrument;
    synth::AudioDeviceState audio;
    synth::JSON patch = synth::BuildPatchJSON(patchArena, "GC", source, instrument, audio);
    REQUIRE_TRUE(!patchArena.Failed());

    synth::ParameterManager target;
    target.SetGestureCount(64);
    auto& targetGroup = target.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 5});
    auto& targetCarrier = target.CreateParameter(targetGroup, {.name = "Carrier", .defaultValue = 0.25f});
    REQUIRE_TRUE(targetCarrier.EnsureModulationDepth(1) != nullptr);  // absent/default dirty topology
    REQUIRE_TRUE(synth::LoadPatchJSON(patch, target, instrument, &audio));

    auto* targetDepth = targetCarrier.ModulationDepthParameter(0);
    REQUIRE_TRUE(targetDepth != nullptr);
    auto* targetNested = targetDepth->ModulationDepthParameter(1);
    REQUIRE_TRUE(targetNested != nullptr);
    REQUIRE_NEAR(targetDepth->GestureValue(0, 32), 0.7f, 0.000001f);
    REQUIRE_TRUE(targetDepth->GestureActive(0, 32));
    REQUIRE_NEAR(targetNested->GestureValue(0, 63), 0.8f, 0.000001f);
    REQUIRE_TRUE(targetNested->GestureActive(0, 63));
    REQUIRE_TRUE(targetCarrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(targetGroup.LiveLocalParameterCount() == 2);

    const float outputBeforeRematerialization = targetCarrier.GetRaw(0);
    synth::Parameter* rematerialized = targetCarrier.EnsureModulationDepth(1);
    REQUIRE_TRUE(rematerialized != nullptr);
    target.ComputeAllParameters();
    REQUIRE_NEAR(targetCarrier.GetRaw(0), outputBeforeRematerialization, 0.000001f);
    REQUIRE_TRUE(rematerialized->Name() == "Carrier Mod Depth 2");
    REQUIRE_NEAR(rematerialized->SceneCenter(0), 0.5f, 0.000001f);
    REQUIRE_NEAR(rematerialized->GestureValue(0, 32), 0.5f, 0.000001f);
    REQUIRE_NEAR(rematerialized->GestureValue(0, 63), 0.5f, 0.000001f);
    REQUIRE_TRUE(!rematerialized->GestureActive(0, 32));
    REQUIRE_TRUE(!rematerialized->GestureActive(0, 63));
    REQUIRE_TRUE(rematerialized->ActiveRouteCount() == 0);
    REQUIRE_TRUE(targetGroup.CollectNeutralLocalParameters() == 1);
    REQUIRE_TRUE(targetCarrier.ModulationDepthParameter(1) == nullptr);
    REQUIRE_TRUE(targetGroup.LiveLocalParameterCount() == 2);
}

TEST_CASE(eligible_collection_preserves_semantic_parameter_json) {
    synth::ParameterManager manager;
    manager.SetGestureCount(1);
    auto& group = manager.CreateGroup({.numVoices = 1, .numModulators = 2, .numScenes = 1, .maxParameters = 4});
    auto& carrier = manager.CreateParameter(group, {.name = "Carrier", .defaultValue = 0.25f});
    auto* neutral = carrier.EnsureModulationDepth(0);
    REQUIRE_TRUE(neutral != nullptr);
    auto* retained = carrier.EnsureModulationDepth(1);
    REQUIRE_TRUE(retained != nullptr);
    retained->GestureValue(0, 0) = 0.75f;

    synth::JsonArena beforeArena(65536);
    const synth::JSON before = manager.ParameterValuesToJSON(beforeArena);
    REQUIRE_TRUE(!beforeArena.Failed());
    REQUIRE_TRUE(group.CollectNeutralLocalParameters() == 1);
    synth::JsonArena afterArena(65536);
    const synth::JSON after = manager.ParameterValuesToJSON(afterArena);
    REQUIRE_TRUE(!afterArena.Failed());
    REQUIRE_TRUE(JsonSemanticallyEqual(before, after));
}

namespace {

// Regression for slog-2: MidiSender's worker thread (Run()) must tag itself
// with ThreadId::MidiSender so log messages produced while sending (and any
// future thread-identity-sensitive code on that thread) observe the correct
// identity. This sink records synth::GetCurrentThreadId() as observed from
// inside Send(), which runs on the sender's worker thread.
struct RecordingMidiOutputSink final : synth::IMidiOutputSink {
    std::mutex mutex;
    std::optional<synth::ThreadId> observedThreadId;

    void Send(const synth::BasicMidi&) override {
        std::lock_guard lock(mutex);
        observedThreadId = synth::GetCurrentThreadId();
    }
};

}  // namespace

TEST_CASE(midi_sender_run_tags_worker_thread_with_midi_sender_id) {
    RecordingMidiOutputSink sink;
    synth::MidiSender sender;
    sender.SetSink(0, &sink);
    sender.Start();

    REQUIRE_TRUE(sender.Enqueue(0, synth::BasicMidi::CC(0, 0, 1, 64)));
    REQUIRE_TRUE(sender.FlushForTests(std::chrono::milliseconds(1000)));

    sender.Stop();

    std::lock_guard lock(sink.mutex);
    REQUIRE_TRUE(sink.observedThreadId.has_value());
    REQUIRE_TRUE(*sink.observedThreadId == synth::ThreadId::MidiSender);
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
