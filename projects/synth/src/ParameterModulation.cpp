#include "synth/ParameterModulation.hpp"

#include "synth/ButtonGrid.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace synth {

namespace detail {

namespace {

constexpr double kAbsoluteCoefficientTolerance = 1e-10;
constexpr double kAbsoluteTargetTolerance = 1e-5;

bool AccumulateAbsoluteLocation(AbsoluteEditWorkspace& workspace,
                                float* storage, double coefficient) noexcept {
    if (coefficient == 0.0) {
        return true;
    }
    for (std::size_t ix = 0; ix < workspace.locationCount; ++ix) {
        if (workspace.locations[ix].storage == storage) {
            workspace.locations[ix].coefficient += coefficient;
            return true;
        }
    }
    if (workspace.locationCount >= workspace.locations.size()) {
        return false;
    }
    workspace.locations[workspace.locationCount++] = {storage, coefficient};
    return true;
}

}  // namespace

bool TryBuildAbsoluteEditLocations(
    float& leftBaseStorage, float& rightBaseStorage, double sceneBlend,
    std::span<const AbsoluteGestureContribution> gestures,
    AbsoluteEditWorkspace& workspace) noexcept {
    workspace.locationCount = 0;
    if (!std::isfinite(sceneBlend) || gestures.size() > kAbsoluteMaxGestureContributions) {
        return false;
    }
    const double blend = std::clamp(sceneBlend, 0.0, 1.0);
    const double inverseBlend = 1.0 - blend;

    double activeWeightSum = 0.0;
    double baseShareNumerator = 0.0;
    for (const AbsoluteGestureContribution& gesture : gestures) {
        if (gesture.leftStorage == nullptr || gesture.rightStorage == nullptr ||
            !std::isfinite(gesture.effectiveWeight) || gesture.effectiveWeight < 0.0 ||
            gesture.effectiveWeight > 1.0) {
            workspace.locationCount = 0;
            return false;
        }
        activeWeightSum += gesture.effectiveWeight;
        baseShareNumerator += gesture.effectiveWeight * (1.0 - gesture.effectiveWeight);
    }
    if (!std::isfinite(activeWeightSum) || !std::isfinite(baseShareNumerator)) {
        workspace.locationCount = 0;
        return false;
    }

    const double baseCoefficient = activeWeightSum == 0.0 ? 1.0 : baseShareNumerator / activeWeightSum;
    if (!AccumulateAbsoluteLocation(workspace, &leftBaseStorage, baseCoefficient * inverseBlend) ||
        !AccumulateAbsoluteLocation(workspace, &rightBaseStorage, baseCoefficient * blend)) {
        workspace.locationCount = 0;
        return false;
    }

    if (activeWeightSum != 0.0) {
        for (const AbsoluteGestureContribution& gesture : gestures) {
            const double gestureCoefficient =
                gesture.effectiveWeight * gesture.effectiveWeight / activeWeightSum;
            if (!AccumulateAbsoluteLocation(
                    workspace, gesture.leftStorage, gestureCoefficient * inverseBlend) ||
                !AccumulateAbsoluteLocation(
                    workspace, gesture.rightStorage, gestureCoefficient * blend)) {
                workspace.locationCount = 0;
                return false;
            }
        }
    }

    double coefficientSum = 0.0;
    bool locationsValid = workspace.locationCount != 0;
    for (std::size_t ix = 0; ix < workspace.locationCount; ++ix) {
        const AbsoluteEditLocation& location = workspace.locations[ix];
        locationsValid = locationsValid && location.storage != nullptr &&
                         std::isfinite(location.coefficient) && location.coefficient > 0.0;
        coefficientSum += location.coefficient;
    }
    if (!locationsValid || !std::isfinite(coefficientSum) ||
        std::fabs(coefficientSum - 1.0) > kAbsoluteCoefficientTolerance) {
        workspace.locationCount = 0;
        return false;
    }
    return true;
}

std::vector<AbsoluteEditLocation> BuildAbsoluteEditLocations(
    float& leftBaseStorage, float& rightBaseStorage, double sceneBlend,
    std::span<const AbsoluteGestureContribution> gestures) {
    AbsoluteEditWorkspace workspace;
    if (!TryBuildAbsoluteEditLocations(
            leftBaseStorage, rightBaseStorage, sceneBlend, gestures, workspace)) {
        throw std::invalid_argument("invalid absolute edit contribution system");
    }
    return {workspace.locations.begin(),
            workspace.locations.begin() + static_cast<std::ptrdiff_t>(workspace.locationCount)};
}

bool ProjectAbsoluteTarget(AbsoluteEditWorkspace& workspace,
                           double minimum, double maximum, double target) noexcept {
    if (workspace.locationCount > workspace.locations.size()) {
        return false;
    }
    const std::span<AbsoluteEditLocation> locations(
        workspace.locations.data(), workspace.locationCount);
    if (locations.empty() || !std::isfinite(minimum) || !std::isfinite(maximum) ||
        !std::isfinite(target) || !(minimum < maximum) || target < minimum || target > maximum) {
        return false;
    }

    double coefficientSum = 0.0;
    double current = 0.0;
    for (std::size_t ix = 0; ix < locations.size(); ++ix) {
        const AbsoluteEditLocation& location = locations[ix];
        if (location.storage == nullptr || !std::isfinite(location.coefficient) ||
            !(location.coefficient > 0.0) || !std::isfinite(*location.storage) ||
            *location.storage < minimum || *location.storage > maximum) {
            return false;
        }
        for (std::size_t prior = 0; prior < ix; ++prior) {
            if (locations[prior].storage == location.storage) {
                return false;
            }
        }
        coefficientSum += location.coefficient;
        current += location.coefficient * static_cast<double>(*location.storage);
    }
    if (!std::isfinite(coefficientSum) ||
        std::fabs(coefficientSum - 1.0) > kAbsoluteCoefficientTolerance) {
        return false;
    }

    std::fill_n(workspace.fixed.begin(), locations.size(), false);
    if (target == minimum || target == maximum) {
        std::fill_n(workspace.projected.begin(), locations.size(), target);
    } else if (target == current) {
        for (std::size_t ix = 0; ix < locations.size(); ++ix) {
            workspace.projected[ix] = *locations[ix].storage;
        }
    } else {
        const bool movingUp = target > current;
        std::size_t freeCount = locations.size();
        bool solved = false;
        for (std::size_t iteration = 0; iteration <= locations.size(); ++iteration) {
            double fixedContribution = 0.0;
            double freeBaseContribution = 0.0;
            double freeCoefficientSquareSum = 0.0;
            for (std::size_t ix = 0; ix < locations.size(); ++ix) {
                const double coefficient = locations[ix].coefficient;
                if (workspace.fixed[ix]) {
                    fixedContribution += coefficient * workspace.projected[ix];
                } else {
                    freeBaseContribution += coefficient * static_cast<double>(*locations[ix].storage);
                    freeCoefficientSquareSum += coefficient * coefficient;
                }
            }
            if (freeCount == 0 || !(freeCoefficientSquareSum > 0.0) ||
                !std::isfinite(freeCoefficientSquareSum)) {
                break;
            }

            const double lambda =
                (target - fixedContribution - freeBaseContribution) / freeCoefficientSquareSum;
            if (!std::isfinite(lambda)) {
                break;
            }
            bool saturated = false;
            for (std::size_t ix = 0; ix < locations.size(); ++ix) {
                if (workspace.fixed[ix]) {
                    continue;
                }
                const double candidate = static_cast<double>(*locations[ix].storage) +
                                         lambda * locations[ix].coefficient;
                if ((movingUp && candidate > maximum) || (!movingUp && candidate < minimum)) {
                    workspace.fixed[ix] = true;
                    workspace.projected[ix] = movingUp ? maximum : minimum;
                    --freeCount;
                    saturated = true;
                } else {
                    workspace.projected[ix] = candidate;
                }
            }
            if (!saturated) {
                solved = true;
                break;
            }
        }
        if (!solved) {
            return false;
        }
    }

    double roundedEffective = 0.0;
    for (std::size_t ix = 0; ix < locations.size(); ++ix) {
        if (!std::isfinite(workspace.projected[ix]) || workspace.projected[ix] < minimum ||
            workspace.projected[ix] > maximum) {
            return false;
        }
        workspace.rounded[ix] =
            static_cast<float>(std::clamp(workspace.projected[ix], minimum, maximum));
        if (!std::isfinite(workspace.rounded[ix]) || workspace.rounded[ix] < minimum ||
            workspace.rounded[ix] > maximum) {
            return false;
        }
        roundedEffective +=
            locations[ix].coefficient * static_cast<double>(workspace.rounded[ix]);
    }
    if (std::fabs(roundedEffective - target) > kAbsoluteTargetTolerance) {
        return false;
    }
    for (std::size_t ix = 0; ix < locations.size(); ++ix) {
        *locations[ix].storage = workspace.rounded[ix];
    }
    return true;
}

bool ProjectAbsoluteTarget(std::span<AbsoluteEditLocation> locations,
                           double minimum, double maximum, double target) {
    if (locations.size() > kAbsoluteMaxEditLocations) {
        return false;
    }
    AbsoluteEditWorkspace workspace;
    workspace.locationCount = locations.size();
    std::copy(locations.begin(), locations.end(), workspace.locations.begin());
    return ProjectAbsoluteTarget(workspace, minimum, maximum, target);
}

}  // namespace detail

namespace {

// Local modulation-depth controls are intentionally not addressable through ParameterManager::ParameterById.
constexpr ParameterId kLocalParameterId = std::numeric_limits<ParameterId>::max();
constexpr float kModulationNeutralTolerance = 0.000001f;
// A local depth is a normalized bipolar control, so its standard zero-depth value is the knob center.
constexpr float kNeutralModulationDepthCenter = 0.5f;

GestureMask GestureCountMask(std::size_t count) {
    if (count >= std::numeric_limits<GestureMask>::digits) {
        return std::numeric_limits<GestureMask>::max();
    }
    return count == 0 ? GestureMask{0} : (GestureMask{1} << count) - GestureMask{1};
}

template <class Fn>
void ForEachGestureBit(GestureMask mask, Fn&& fn) {
    while (mask != 0) {
        const std::size_t gestureIx = std::countr_zero(mask);
        mask &= mask - 1;
        fn(gestureIx);
    }
}

void ValidateProcessingRates(double referenceRate, double processingRate) {
    if (!(std::isfinite(referenceRate) && referenceRate > 0.0 && std::isfinite(processingRate) &&
          processingRate > 0.0)) {
        throw std::invalid_argument("processing timing rates must be positive and finite");
    }
}

void ValidateOnePoleAlpha(float alpha) {
    if (!(alpha >= 0.0f && alpha <= 1.0f)) {
        throw std::invalid_argument("one-pole alpha must be in [0,1]");
    }
}

void ValidateProcessingTiming(const ParameterProcessingTiming& timing) {
    ValidateOnePoleAlpha(timing.processLiteAlpha);
    ValidateOnePoleAlpha(timing.uiDisplayCenterAlpha);
    ValidateOnePoleAlpha(timing.uiDisplaySpreadAlpha);
    if (timing.targetComputeIntervalSamples == 0) {
        throw std::invalid_argument("target compute interval must be positive");
    }
}

ParameterGroupConfig ValidateConfig(ParameterGroupConfig config) {
    if (!config.IsValid()) {
        throw std::invalid_argument("invalid parameter group config");
    }
    return config;
}

template <typename T>
std::span<T> ArenaSlice(std::vector<T>& arena, std::size_t offset, std::size_t count) {
    if (count == 0) {
        return {};
    }
    return std::span<T>(arena.data() + offset, count);
}

float Blend(float left, float right, float blend) {
    return left * (1.0f - blend) + right * blend;
}

bool OutsideRange(float value, RangeKind range) {
    return ClampToRange(value, range) != value;
}

float RangeMin(RangeKind) {
    return 0.0f;
}

float RangeMax(RangeKind) {
    return 1.0f;
}

float LinearMap(float minValue, float maxValue, float normalized) {
    return minValue + (maxValue - minValue) * normalized;
}

float ExponentialMap(float minValue, float maxValue, float normalized) {
    if (minValue <= 0.0f || maxValue <= 0.0f) {
        throw std::invalid_argument("exponential mapping endpoints must be positive");
    }
    return minValue * std::pow(maxValue / minValue, normalized);
}

float ZeroBasedExponentialMapFromMidpoint(float maxValue, float midpointValue, float normalized) {
    const float base = ZeroBasedExponentialBaseFromMidpoint(midpointValue, maxValue);
    return ZeroBasedExponentialMap(normalized, base, maxValue);
}

float ToBipolarPresentation(float normalized) {
    return 2.0f * std::clamp(normalized, 0.0f, 1.0f) - 1.0f;
}

float ToUIPresentation(float normalized, RangeKind range) {
    return range == RangeKind::Bipolar ? ToBipolarPresentation(normalized) : normalized;
}

float ToUISpreadPresentation(float normalizedSpread, RangeKind range) {
    return range == RangeKind::Bipolar ? 2.0f * normalizedSpread : normalizedSpread;
}

ParameterConfig ResolveParameterAppearance(ParameterConfig config, std::size_t numVoices) {
    if (config.indicatorColors.empty()) {
        config.indicatorColors.assign(numVoices, config.baseColor);
    } else if (config.indicatorColors.size() == 1) {
        const Color indicatorColor = config.indicatorColors.front();
        config.indicatorColors.assign(numVoices, indicatorColor);
    } else if (config.indicatorColors.size() != numVoices) {
        throw std::invalid_argument("indicator color count must be zero, one, or the group voice count");
    }
    return config;
}

void ApplySceneDistribution(float& left, float& right, float blend, float delta, RangeKind range) {
    blend = std::clamp(blend, 0.0f, 1.0f);
    if (&left == &right) {
        left = ClampToRange(left + delta, range);
        return;
    }

    if (blend <= 0.0f) {
        left = ClampToRange(left + delta, range);
        return;
    }
    if (blend >= 1.0f) {
        right = ClampToRange(right + delta, range);
        return;
    }

    const float inverseBlend = 1.0f - blend;
    const float targetBlended = ClampToRange(Blend(left, right, blend) + delta, range);
    const float proposedLeft = left + delta * inverseBlend;
    const float proposedRight = right + delta * blend;

    if (OutsideRange(proposedLeft, range)) {
        left = ClampToRange(proposedLeft, range);
        right = (targetBlended - left * inverseBlend) / blend;
    } else if (OutsideRange(proposedRight, range)) {
        right = ClampToRange(proposedRight, range);
        left = (targetBlended - right * blend) / inverseBlend;
    } else {
        left = proposedLeft;
        right = proposedRight;
    }
}

bool IsJsonArray(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Array;
}

bool IsJsonObject(JSON json) {
    return json.m_node != nullptr && json.m_node->m_type == JsonType::Object;
}

const JsonMember* JsonObjectMembers(JSON json) {
    if (!IsJsonObject(json)) {
        return nullptr;
    }
    return static_cast<const JsonMember*>(json.m_node->m_container.m_entries);
}

bool ParseDecimalIndex(std::string_view text, std::size_t& result) {
    if (text.empty()) {
        return false;
    }
    result = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

} // namespace

float ConvertOnePoleAlpha(float referenceAlpha, double referenceRate, double processingRate) {
    ValidateProcessingRates(referenceRate, processingRate);
    ValidateOnePoleAlpha(referenceAlpha);
    return static_cast<float>(1.0 - std::pow(1.0 - static_cast<double>(referenceAlpha),
                                            referenceRate / processingRate));
}

std::size_t ConvertSampleInterval(std::size_t referenceInterval, double referenceRate, double processingRate) {
    ValidateProcessingRates(referenceRate, processingRate);
    if (referenceInterval == 0) {
        throw std::invalid_argument("sample interval must be positive");
    }
    const double converted = std::round(static_cast<double>(referenceInterval) * processingRate / referenceRate);
    if (!(std::isfinite(converted) && converted <= static_cast<double>(std::numeric_limits<std::size_t>::max()))) {
        throw std::invalid_argument("sample interval conversion overflow");
    }
    return std::max<std::size_t>(1, static_cast<std::size_t>(converted));
}

float ClampToRange(float value, RangeKind range) {
    (void)range;
    return std::clamp(value, 0.0f, 1.0f);
}

bool ParameterGroupConfig::IsValid() const {
    return numVoices > 0 && numScenes > 0 && maxParameters > 0 && targetComputeIntervalSamples > 0 &&
           processLiteAlpha >= 0.0f && processLiteAlpha <= 1.0f && targetCenterAlpha >= 0.0f &&
           targetCenterAlpha <= 1.0f && uiDisplayCenterAlpha >= 0.0f &&
           uiDisplayCenterAlpha <= 1.0f && uiDisplaySpreadAlpha >= 0.0f && uiDisplaySpreadAlpha <= 1.0f;
}

ParameterStorageBatch::ParameterStorageBatch(const ParameterGroupConfig& config, std::size_t gestureCount,
                                             std::size_t capacity)
    : numVoices(config.numVoices),
      numModulators(config.numModulators),
      numScenes(config.numScenes),
      gestureCount(gestureCount),
      capacity(capacity),
      currentCenterScaleArena(capacity * config.numVoices),
      targetCenterScaleArena(capacity * config.numVoices),
      currentNormalizationOffsetArena(capacity * config.numVoices),
      targetNormalizationOffsetArena(capacity * config.numVoices),
      currentMinValueArena(capacity * config.numVoices),
      targetMinValueArena(capacity * config.numVoices),
      currentMaxValueArena(capacity * config.numVoices),
      targetMaxValueArena(capacity * config.numVoices),
      currentDepthArena(capacity * config.numVoices * config.numModulators),
      targetDepthArena(capacity * config.numVoices * config.numModulators),
      routeSourceIndexArena(capacity * config.numModulators),
      sourceRoutePositionArena(capacity * config.numModulators),
      currentKnobValueArena(capacity * config.numVoices),
      uiDisplayCenterArena(capacity * config.numVoices),
      uiDisplaySpreadEnergyArena(capacity * config.numVoices),
      modulationDepthArena(capacity * config.numModulators, nullptr),
      sceneCenterArena(capacity * config.numScenes),
      gestureValueArena(capacity * config.numScenes * gestureCount),
      gestureActiveMaskArena(capacity * config.numScenes, 0) {
    parameters.reserve(capacity);
}

bool ParameterStorageBatch::Compatible(const ParameterGroupConfig& config, std::size_t liveGestureCount) const {
    return numVoices == config.numVoices && numModulators == config.numModulators &&
           numScenes == config.numScenes && gestureCount == liveGestureCount && capacity > 0;
}

std::unique_ptr<ParameterStorageBatch> MakeParameterStorageBatch(const ParameterGroupConfig& config,
                                                                 std::size_t gestureCount,
                                                                 std::size_t capacity) {
    return std::make_unique<ParameterStorageBatch>(config, gestureCount, capacity);
}

Modulators::Modulators(std::size_t voices, std::size_t modulators)
    : numVoices_(voices),
      numModulators_(modulators),
      values_(voices * modulators, 0.0f),
      metadata_(modulators),
      sourcePointers_(voices * modulators, nullptr) {}

float& Modulators::Value(std::size_t voiceIx, std::size_t modIx) {
    return values_.at(Index(voiceIx, modIx));
}

float Modulators::Value(std::size_t voiceIx, std::size_t modIx) const {
    return values_.at(Index(voiceIx, modIx));
}

float Modulators::Apply(std::size_t voiceIx, std::span<const float> depths) const {
    if (voiceIx >= numVoices_) {
        throw std::out_of_range("modulator voice index out of range");
    }
    if (depths.size() != numModulators_) {
        throw std::invalid_argument("modulation depth row size does not match modulator count");
    }

    const std::size_t rowStart = voiceIx * numModulators_;
    float result = 0.0f;
    for (std::size_t modIx = 0; modIx < numModulators_; ++modIx) {
        result += values_[rowStart + modIx] * depths[modIx];
    }
    return result;
}

float Modulators::ApplyActive(std::size_t voiceIx, std::span<const float> activeDepths,
                              std::span<const std::size_t> sourceIndices) const {
    if (voiceIx >= numVoices_) {
        throw std::out_of_range("modulator voice index out of range");
    }
    if (activeDepths.size() != sourceIndices.size()) {
        throw std::invalid_argument("active depth and source index counts differ");
    }

    const std::size_t rowStart = voiceIx * numModulators_;
    float result = 0.0f;
    for (std::size_t slot = 0; slot < activeDepths.size(); ++slot) {
        if (sourceIndices[slot] >= numModulators_) {
            throw std::out_of_range("modulator source index out of range");
        }
        result += values_[rowStart + sourceIndices[slot]] * activeDepths[slot];
    }
    return result;
}

void Modulators::SetModulationSource(std::size_t modIx, std::span<float* const> sourcePointers,
                                     ModulatorMetadata metadata) {
    if (modIx >= numModulators_) {
        throw std::out_of_range("modulator index out of range");
    }
    if (sourcePointers.size() != numVoices_) {
        throw std::invalid_argument("modulation source pointer count does not match voice count");
    }
    if (metadata.connected) {
        for (float* sourcePointer : sourcePointers) {
            if (sourcePointer == nullptr) {
                throw std::invalid_argument("connected modulation source pointer must not be null");
            }
        }
    }

    metadata_[modIx] = std::move(metadata);
    for (std::size_t voiceIx = 0; voiceIx < numVoices_; ++voiceIx) {
        sourcePointers_[voiceIx * numModulators_ + modIx] = sourcePointers[voiceIx];
    }
}

void Modulators::UpdateModValues() {
    for (std::size_t modIx = 0; modIx < numModulators_; ++modIx) {
        if (!metadata_[modIx].connected) {
            continue;
        }
        for (std::size_t voiceIx = 0; voiceIx < numVoices_; ++voiceIx) {
            const std::size_t index = voiceIx * numModulators_ + modIx;
            if (sourcePointers_[index] != nullptr) {
                values_[index] = *sourcePointers_[index];
            }
        }
    }
}

ModulatorMetadata& Modulators::Metadata(std::size_t modIx) {
    return metadata_.at(modIx);
}

const ModulatorMetadata& Modulators::Metadata(std::size_t modIx) const {
    return metadata_.at(modIx);
}

std::size_t Modulators::Index(std::size_t voiceIx, std::size_t modIx) const {
    if (voiceIx >= numVoices_) {
        throw std::out_of_range("modulator voice index out of range");
    }
    if (modIx >= numModulators_) {
        throw std::out_of_range("modulator index out of range");
    }
    return voiceIx * numModulators_ + modIx;
}

Gestures::Gestures(std::size_t gestures)
    : values_(gestures, 0.0f),
      metadata_(gestures) {
    if (gestures > std::numeric_limits<GestureMask>::digits) {
        throw std::invalid_argument("gesture count exceeds 64-bit selector capacity");
    }
}

float& Gestures::Value(std::size_t gestureIx) {
    CheckIndex(gestureIx);
    return values_[gestureIx];
}

float Gestures::Value(std::size_t gestureIx) const {
    CheckIndex(gestureIx);
    return values_[gestureIx];
}

void Gestures::Select(std::size_t gestureIx, bool selected) {
    CheckIndex(gestureIx);
    const GestureMask bit = GestureMask{1} << gestureIx;
    if (selected) {
        selectedMask_ |= bit;
    } else {
        selectedMask_ &= ~bit;
    }
}

bool Gestures::Selected(std::size_t gestureIx) const {
    CheckIndex(gestureIx);
    return (selectedMask_ & (GestureMask{1} << gestureIx)) != 0;
}

void Gestures::ClearSelection() {
    selectedMask_ = 0;
}

GestureMetadata& Gestures::Metadata(std::size_t gestureIx) {
    CheckIndex(gestureIx);
    return metadata_[gestureIx];
}

const GestureMetadata& Gestures::Metadata(std::size_t gestureIx) const {
    CheckIndex(gestureIx);
    return metadata_[gestureIx];
}

void Gestures::CheckIndex(std::size_t gestureIx) const {
    if (gestureIx >= values_.size()) {
        throw std::out_of_range("gesture index out of range");
    }
}

ParameterGroup::ParameterGroup(ParameterGroupConfig config, ParameterManager& manager, std::size_t gestureCount)
    : config_(ValidateConfig(config)),
      manager_(&manager),
      gestureCount_(gestureCount),
      modulators_(config.numVoices, config.numModulators),
      parameterCount_(0) {
    parameters_.reserve(config_.maxParameters);
    topLevelParameters_.reserve(config_.maxParameters);
    recycledLocalSlots_.reserve(config_.maxParameters);
    currentCenterScaleArena_.resize(config_.maxParameters * config_.numVoices);
    targetCenterScaleArena_.resize(config_.maxParameters * config_.numVoices);
    currentNormalizationOffsetArena_.resize(config_.maxParameters * config_.numVoices);
    targetNormalizationOffsetArena_.resize(config_.maxParameters * config_.numVoices);
    currentMinValueArena_.resize(config_.maxParameters * config_.numVoices);
    targetMinValueArena_.resize(config_.maxParameters * config_.numVoices);
    currentMaxValueArena_.resize(config_.maxParameters * config_.numVoices);
    targetMaxValueArena_.resize(config_.maxParameters * config_.numVoices);
    currentDepthArena_.resize(config_.maxParameters * config_.numVoices * config_.numModulators);
    targetDepthArena_.resize(config_.maxParameters * config_.numVoices * config_.numModulators);
    routeSourceIndexArena_.resize(config_.maxParameters * config_.numModulators);
    sourceRoutePositionArena_.resize(config_.maxParameters * config_.numModulators);
    currentKnobValueArena_.resize(config_.maxParameters * config_.numVoices);
    uiDisplayCenterArena_.resize(config_.maxParameters * config_.numVoices);
    uiDisplaySpreadEnergyArena_.resize(config_.maxParameters * config_.numVoices);
    modulationDepthArena_.resize(config_.maxParameters * config_.numModulators, nullptr);
    sceneCenterArena_.resize(config_.maxParameters * config_.numScenes);
    gestureValueArena_.resize(config_.maxParameters * config_.numScenes * gestureCount_);
    gestureActiveMaskArena_.resize(config_.maxParameters * config_.numScenes, 0);
}

ParameterGroup::~ParameterGroup() = default;

bool ParameterGroup::CanAllocate() const {
    return AvailableParameterSlots() > 0;
}

std::size_t ParameterGroup::AvailableParameterSlots() const {
    const std::size_t initialAllocated = std::min(parameterCount_, config_.maxParameters);
    std::size_t available = config_.maxParameters - initialAllocated;
    for (const auto& batch : extraStorageBatches_) {
        available += batch->Available();
    }
    return available + recycledLocalSlots_.size();
}

void ParameterGroup::AddParameterStorageBatch(std::unique_ptr<ParameterStorageBatch> batch) {
    if (batch == nullptr || !batch->Compatible(config_, gestureCount_)) {
        throw std::invalid_argument("parameter storage batch does not match group shape");
    }
    storageRequestPending_ = false;
    extraStorageBatches_.push_back(std::move(batch));
}

Parameter& ParameterGroup::CreateLocalParameter(ParameterConfig config, ParameterId id) {
    if (config.name.empty()) {
        throw std::logic_error("parameter name must not be empty");
    }
    if (!CanAllocate()) {
        throw std::length_error("parameter group capacity exhausted");
    }

    if (id == kLocalParameterId && !recycledLocalSlots_.empty()) {
        RecycledLocalSlot recycled = recycledLocalSlots_.back();
        recycledLocalSlots_.pop_back();
        if (recycled.parameter == nullptr || recycled.parameter->storageBatch_ != recycled.batch ||
            recycled.parameter->slotIx_ != recycled.slotIx ||
            recycled.parameter->storageLocalIx_ != recycled.storageLocalIx ||
            &ParameterByLocalIndex(recycled.storageLocalIx) != recycled.parameter ||
            (recycled.batch != nullptr && !recycled.batch->Compatible(config_, gestureCount_))) {
            throw std::logic_error("recycled local parameter slot identity is invalid");
        }
        recycled.parameter->ResetLocalForReuse(id, std::move(config));
        ++liveLocalParameterCount_;
        RequestParameterStorageBatchIfLow();
        return *recycled.parameter;
    }

    if (parameterCount_ < config_.maxParameters) {
        auto parameter = std::make_unique<Parameter>(id, *this, std::move(config), parameterCount_);
        Parameter& result = *parameter;
        parameters_.push_back(std::move(parameter));
        ++parameterCount_;
        if (id == kLocalParameterId) {
            ++liveLocalParameterCount_;
        }
        RequestParameterStorageBatchIfLow();
        return result;
    }

    for (const auto& batch : extraStorageBatches_) {
        if (batch->Available() == 0) {
            continue;
        }
        const std::size_t slotIx = batch->allocated++;
        auto parameter = std::make_unique<Parameter>(id, *this, std::move(config), *batch, slotIx);
        Parameter& result = *parameter;
        batch->parameters.push_back(std::move(parameter));
        ++parameterCount_;
        if (id == kLocalParameterId) {
            ++liveLocalParameterCount_;
        }
        RequestParameterStorageBatchIfLow();
        return result;
    }

    throw std::length_error("parameter group capacity exhausted");
}

void ParameterGroup::RecycleLocalParameter(Parameter& parameter) {
    if (parameter.id_ != kLocalParameterId || parameter.localViewPinCount_ != 0) {
        throw std::logic_error("only unpinned local parameters can be recycled");
    }
    if (liveLocalParameterCount_ == 0) {
        throw std::logic_error("local parameter accounting underflow");
    }
    if (parameter.storageLocalIx_ >= parameterCount_ ||
        &ParameterByLocalIndex(parameter.storageLocalIx_) != &parameter) {
        throw std::logic_error("local parameter storage identity is invalid");
    }
    recycledLocalSlots_.push_back({
        .parameter = &parameter,
        .batch = parameter.storageBatch_,
        .slotIx = parameter.slotIx_,
        .storageLocalIx = parameter.storageLocalIx_,
    });
    --liveLocalParameterCount_;
}

std::size_t ParameterGroup::CollectNeutralLocalParameters() {
    if (processingObserver_ != nullptr) {
        ++processingObserver_->neutralCollectionPasses;
    }
    std::size_t collected = 0;
    for (Parameter* root : topLevelParameters_) {
        collected += root->CollectNeutralChildren();
    }
    return collected;
}

void ParameterGroup::RegisterTopLevelParameter(Parameter& parameter) {
    topLevelParameters_.push_back(&parameter);
}

Parameter& ParameterGroup::ParameterByLocalIndex(std::size_t localIx) {
    if (localIx < parameters_.size()) {
        return *parameters_.at(localIx);
    }
    std::size_t remaining = localIx - parameters_.size();
    for (const auto& batch : extraStorageBatches_) {
        if (remaining < batch->parameters.size()) {
            return *batch->parameters.at(remaining);
        }
        remaining -= batch->parameters.size();
    }
    throw std::out_of_range("parameter local index out of range");
}

const Parameter& ParameterGroup::ParameterByLocalIndex(std::size_t localIx) const {
    if (localIx < parameters_.size()) {
        return *parameters_.at(localIx);
    }
    std::size_t remaining = localIx - parameters_.size();
    for (const auto& batch : extraStorageBatches_) {
        if (remaining < batch->parameters.size()) {
            return *batch->parameters.at(remaining);
        }
        remaining -= batch->parameters.size();
    }
    throw std::out_of_range("parameter local index out of range");
}

void ParameterGroup::RequestParameterStorageBatch(std::size_t minimumAdditionalParameters) {
    if (storageRequestPending_ || manager_ == nullptr || minimumAdditionalParameters == 0) {
        return;
    }
    if (manager_->RequestParameterStorageBatch(*this, minimumAdditionalParameters)) {
        storageRequestPending_ = true;
    }
}

void ParameterGroup::RequestParameterStorageBatchIfLow() {
    const std::size_t lowWatermark = config_.numModulators * 2;
    if (lowWatermark == 0) {
        return;
    }
    const std::size_t available = AvailableParameterSlots();
    if (available < lowWatermark) {
        RequestParameterStorageBatch(lowWatermark - available);
    }
}

void ParameterGroup::SetModulationSource(std::size_t modIx, std::span<float* const> sourcePointers,
                                         ModulatorMetadata metadata) {
    modulators_.SetModulationSource(modIx, sourcePointers, std::move(metadata));
}

void ParameterGroup::UpdateModValues() {
    modulators_.UpdateModValues();
}

void ParameterGroup::ConfigureProcessingTiming(const ParameterProcessingTiming& timing) {
    ValidateProcessingTiming(timing);
    config_.processLiteAlpha = timing.processLiteAlpha;
    config_.targetComputeIntervalSamples = timing.targetComputeIntervalSamples;
    config_.uiDisplayCenterAlpha = timing.uiDisplayCenterAlpha;
    config_.uiDisplaySpreadAlpha = timing.uiDisplaySpreadAlpha;
}

void ParameterGroup::ProcessSamplePhase1(std::uint64_t sampleIndex) {
    for (Parameter* parameter : topLevelParameters_) {
        parameter->ProcessSamplePhase1(sampleIndex);
        if (processingObserver_ != nullptr) {
            ++processingObserver_->topLevelProcessLiteCalls;
        }
    }
}

void ParameterGroup::ProcessSamplePhase2() {
    for (Parameter* parameter : topLevelParameters_) {
        parameter->ProcessSamplePhase2();
    }
}

void ParameterGroup::ProcessSample(std::uint64_t sampleIndex) {
    ProcessSamplePhase1(sampleIndex);
    ProcessSamplePhase2();
}

Parameter::Parameter(ParameterId id, ParameterGroup& group, ParameterConfig config, std::size_t slotIx)
    : id_(id),
      group_(group),
      config_(std::move(config)),
      slotIx_(slotIx),
      storageLocalIx_(group.parameterCount_),
      currentCenter_(ClampToRange(config_.defaultValue, config_.range)),
      targetCenter_(currentCenter_),
      currentCenterScales_(ArenaSlice(group_.currentCenterScaleArena_, slotIx_ * group_.Config().numVoices,
                                      group_.Config().numVoices)),
      targetCenterScales_(ArenaSlice(group_.targetCenterScaleArena_, slotIx_ * group_.Config().numVoices,
                                     group_.Config().numVoices)),
      currentNormalizationOffsets_(ArenaSlice(group_.currentNormalizationOffsetArena_,
                                             slotIx_ * group_.Config().numVoices,
                                             group_.Config().numVoices)),
      targetNormalizationOffsets_(ArenaSlice(group_.targetNormalizationOffsetArena_,
                                            slotIx_ * group_.Config().numVoices,
                                            group_.Config().numVoices)),
      currentMinValues_(ArenaSlice(group_.currentMinValueArena_,
                                   slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      targetMinValues_(ArenaSlice(group_.targetMinValueArena_,
                                  slotIx_ * group_.Config().numVoices,
                                  group_.Config().numVoices)),
      currentMaxValues_(ArenaSlice(group_.currentMaxValueArena_,
                                   slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      targetMaxValues_(ArenaSlice(group_.targetMaxValueArena_,
                                  slotIx_ * group_.Config().numVoices,
                                  group_.Config().numVoices)),
      currentDepths_(ArenaSlice(group_.currentDepthArena_,
                                slotIx_ * group_.Config().numVoices * group_.Config().numModulators,
                                group_.Config().numVoices * group_.Config().numModulators)),
      targetDepths_(ArenaSlice(group_.targetDepthArena_,
                               slotIx_ * group_.Config().numVoices * group_.Config().numModulators,
                               group_.Config().numVoices * group_.Config().numModulators)),
      routeSourceIndices_(ArenaSlice(group_.routeSourceIndexArena_,
                                     slotIx_ * group_.Config().numModulators,
                                     group_.Config().numModulators)),
      sourceRoutePositions_(ArenaSlice(group_.sourceRoutePositionArena_,
                                       slotIx_ * group_.Config().numModulators,
                                       group_.Config().numModulators)),
      currentKnobValues_(ArenaSlice(group_.currentKnobValueArena_, slotIx_ * group_.Config().numVoices,
                                    group_.Config().numVoices)),
      uiDisplayCenters_(ArenaSlice(group_.uiDisplayCenterArena_, slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      uiDisplaySpreadEnergies_(ArenaSlice(group_.uiDisplaySpreadEnergyArena_,
                                          slotIx_ * group_.Config().numVoices,
                                          group_.Config().numVoices)),
      modulationDepths_(ArenaSlice(group_.modulationDepthArena_, slotIx_ * group_.Config().numModulators,
                                   group_.Config().numModulators)),
      sceneCenters_(ArenaSlice(group_.sceneCenterArena_, slotIx_ * group_.Config().numScenes,
                               group_.Config().numScenes)),
      gestureValues_(ArenaSlice(group_.gestureValueArena_,
                                slotIx_ * group_.Config().numScenes * group_.GestureCount(),
                                group_.Config().numScenes * group_.GestureCount())),
      gestureActiveMasks_(ArenaSlice(group_.gestureActiveMaskArena_,
                                     slotIx_ * group_.Config().numScenes,
                                     group_.Config().numScenes)) {
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), currentCenter_);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), currentCenter_);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), currentCenter_);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), currentCenter_);
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    for (std::size_t sourceIx = 0; sourceIx < routeSourceIndices_.size(); ++sourceIx) {
        routeSourceIndices_[sourceIx] = sourceIx;
        sourceRoutePositions_[sourceIx] = sourceIx;
    }
    std::fill(modulationDepths_.begin(), modulationDepths_.end(), nullptr);
    std::fill(sceneCenters_.begin(), sceneCenters_.end(), currentCenter_);
    std::fill(gestureValues_.begin(), gestureValues_.end(), currentCenter_);
    std::fill(gestureActiveMasks_.begin(), gestureActiveMasks_.end(), 0);
    SeedCachedKnobAndUiDisplayState();
}

Parameter::Parameter(ParameterId id, ParameterGroup& group, ParameterConfig config,
                     ParameterStorageBatch& storageBatch, std::size_t slotIx)
    : id_(id),
      group_(group),
      config_(std::move(config)),
      storageBatch_(&storageBatch),
      slotIx_(slotIx),
      storageLocalIx_(group.parameterCount_),
      currentCenter_(ClampToRange(config_.defaultValue, config_.range)),
      targetCenter_(currentCenter_),
      currentCenterScales_(ArenaSlice(storageBatch.currentCenterScaleArena, slotIx_ * group_.Config().numVoices,
                                      group_.Config().numVoices)),
      targetCenterScales_(ArenaSlice(storageBatch.targetCenterScaleArena, slotIx_ * group_.Config().numVoices,
                                     group_.Config().numVoices)),
      currentNormalizationOffsets_(ArenaSlice(storageBatch.currentNormalizationOffsetArena,
                                             slotIx_ * group_.Config().numVoices,
                                             group_.Config().numVoices)),
      targetNormalizationOffsets_(ArenaSlice(storageBatch.targetNormalizationOffsetArena,
                                            slotIx_ * group_.Config().numVoices,
                                            group_.Config().numVoices)),
      currentMinValues_(ArenaSlice(storageBatch.currentMinValueArena,
                                   slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      targetMinValues_(ArenaSlice(storageBatch.targetMinValueArena,
                                  slotIx_ * group_.Config().numVoices,
                                  group_.Config().numVoices)),
      currentMaxValues_(ArenaSlice(storageBatch.currentMaxValueArena,
                                   slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      targetMaxValues_(ArenaSlice(storageBatch.targetMaxValueArena,
                                  slotIx_ * group_.Config().numVoices,
                                  group_.Config().numVoices)),
      currentDepths_(ArenaSlice(storageBatch.currentDepthArena,
                                slotIx_ * group_.Config().numVoices * group_.Config().numModulators,
                                group_.Config().numVoices * group_.Config().numModulators)),
      targetDepths_(ArenaSlice(storageBatch.targetDepthArena,
                               slotIx_ * group_.Config().numVoices * group_.Config().numModulators,
                               group_.Config().numVoices * group_.Config().numModulators)),
      routeSourceIndices_(ArenaSlice(storageBatch.routeSourceIndexArena,
                                     slotIx_ * group_.Config().numModulators,
                                     group_.Config().numModulators)),
      sourceRoutePositions_(ArenaSlice(storageBatch.sourceRoutePositionArena,
                                       slotIx_ * group_.Config().numModulators,
                                       group_.Config().numModulators)),
      currentKnobValues_(ArenaSlice(storageBatch.currentKnobValueArena, slotIx_ * group_.Config().numVoices,
                                    group_.Config().numVoices)),
      uiDisplayCenters_(ArenaSlice(storageBatch.uiDisplayCenterArena, slotIx_ * group_.Config().numVoices,
                                   group_.Config().numVoices)),
      uiDisplaySpreadEnergies_(ArenaSlice(storageBatch.uiDisplaySpreadEnergyArena,
                                          slotIx_ * group_.Config().numVoices,
                                          group_.Config().numVoices)),
      modulationDepths_(ArenaSlice(storageBatch.modulationDepthArena, slotIx_ * group_.Config().numModulators,
                                   group_.Config().numModulators)),
      sceneCenters_(ArenaSlice(storageBatch.sceneCenterArena, slotIx_ * group_.Config().numScenes,
                               group_.Config().numScenes)),
      gestureValues_(ArenaSlice(storageBatch.gestureValueArena,
                                slotIx_ * group_.Config().numScenes * group_.GestureCount(),
                                group_.Config().numScenes * group_.GestureCount())),
      gestureActiveMasks_(ArenaSlice(storageBatch.gestureActiveMaskArena,
                                     slotIx_ * group_.Config().numScenes,
                                     group_.Config().numScenes)) {
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), currentCenter_);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), currentCenter_);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), currentCenter_);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), currentCenter_);
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    for (std::size_t sourceIx = 0; sourceIx < routeSourceIndices_.size(); ++sourceIx) {
        routeSourceIndices_[sourceIx] = sourceIx;
        sourceRoutePositions_[sourceIx] = sourceIx;
    }
    std::fill(modulationDepths_.begin(), modulationDepths_.end(), nullptr);
    std::fill(sceneCenters_.begin(), sceneCenters_.end(), currentCenter_);
    std::fill(gestureValues_.begin(), gestureValues_.end(), currentCenter_);
    std::fill(gestureActiveMasks_.begin(), gestureActiveMasks_.end(), 0);
    SeedCachedKnobAndUiDisplayState();
}

ParameterStorageBatch::~ParameterStorageBatch() = default;

void Parameter::PinLocalForView() {
    if (id_ == kLocalParameterId) {
        ++localViewPinCount_;
    }
}

void Parameter::UnpinLocalForView() {
    if (id_ != kLocalParameterId) {
        return;
    }
    if (localViewPinCount_ == 0) {
        throw std::logic_error("local parameter view pin underflow");
    }
    --localViewPinCount_;
}

bool Parameter::CanRecycleLocal() const {
    if (id_ != kLocalParameterId || localViewPinCount_ != 0 || activeRouteCount_ != 0 ||
        HasNonDefaultState() || HasNonZeroState()) {
        return false;
    }
    if (std::any_of(modulationDepths_.begin(), modulationDepths_.end(),
                    [](const Parameter* child) { return child != nullptr; })) {
        return false;
    }

    const float defaultValue = ClampToRange(config_.defaultValue, config_.range);
    const auto allNear = [&](std::span<const float> values, float expected) {
        return std::all_of(values.begin(), values.end(), [&](float value) {
            return std::fabs(value - expected) <= kModulationNeutralTolerance;
        });
    };
    return allNear(currentMinValues_, defaultValue) && allNear(targetMinValues_, defaultValue) &&
           allNear(currentMaxValues_, defaultValue) && allNear(targetMaxValues_, defaultValue) &&
           allNear(currentKnobValues_, defaultValue) && allNear(uiDisplayCenters_, defaultValue) &&
           allNear(uiDisplaySpreadEnergies_, 0.0f);
}

std::size_t Parameter::CollectNeutralChildren() {
    std::size_t collected = 0;
    for (std::size_t sourceIx = 0; sourceIx < modulationDepths_.size(); ++sourceIx) {
        Parameter* child = modulationDepths_[sourceIx];
        if (child == nullptr) {
            continue;
        }
        collected += child->CollectNeutralChildren();
        if (!child->CanRecycleLocal()) {
            continue;
        }

        modulationDepths_[sourceIx] = nullptr;
        group_.RecycleLocalParameter(*child);
        ++collected;
    }
    return collected;
}

void Parameter::ResetLocalForReuse(ParameterId id, ParameterConfig config) {
    if (id != kLocalParameterId) {
        throw std::logic_error("recycled parameter slots are local-only");
    }
    id_ = id;
    config_ = std::move(config);
    recursionDepth_ = 0;
    localViewPinCount_ = 0;
    const float defaultValue = ClampToRange(config_.defaultValue, config_.range);
    currentCenter_ = defaultValue;
    targetCenter_ = defaultValue;
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), defaultValue);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), defaultValue);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), defaultValue);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), defaultValue);
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    for (std::size_t sourceIx = 0; sourceIx < routeSourceIndices_.size(); ++sourceIx) {
        routeSourceIndices_[sourceIx] = sourceIx;
        sourceRoutePositions_[sourceIx] = sourceIx;
    }
    activeRouteCount_ = 0;
    std::fill(currentKnobValues_.begin(), currentKnobValues_.end(), defaultValue);
    std::fill(uiDisplayCenters_.begin(), uiDisplayCenters_.end(), defaultValue);
    std::fill(uiDisplaySpreadEnergies_.begin(), uiDisplaySpreadEnergies_.end(), 0.0f);
    std::fill(modulationDepths_.begin(), modulationDepths_.end(), nullptr);
    std::fill(sceneCenters_.begin(), sceneCenters_.end(), defaultValue);
    std::fill(gestureValues_.begin(), gestureValues_.end(), defaultValue);
    std::fill(gestureActiveMasks_.begin(), gestureActiveMasks_.end(), 0);
    AssertRouteBijection();
}

void Parameter::UIState::Configure(std::size_t newVoiceCapacity, std::size_t newModulatorColorCapacity,
                                   std::size_t newGestureColorCapacity) {
    voiceCapacity = newVoiceCapacity;
    modulatorColorCapacity = newModulatorColorCapacity;
    gestureColorCapacity = newGestureColorCapacity;
    values = std::make_unique<std::atomic<float>[]>(voiceCapacity);
    spreadValues = std::make_unique<std::atomic<float>[]>(voiceCapacity);
    minValues = std::make_unique<std::atomic<float>[]>(voiceCapacity);
    maxValues = std::make_unique<std::atomic<float>[]>(voiceCapacity);
    switchValue = std::make_unique<std::atomic<std::size_t>[]>(voiceCapacity);
    indicatorColors = std::make_unique<AtomicColor[]>(voiceCapacity);
    modulatorSourceColors = std::make_unique<AtomicColor[]>(modulatorColorCapacity);
    gestureColors = std::make_unique<AtomicColor[]>(gestureColorCapacity);
    SetDisconnected();
}

void Parameter::UIState::SetDisconnected(std::uint64_t processedEpoch) {
    revision.fetch_add(1, std::memory_order_acq_rel);
    connected.store(false, std::memory_order_relaxed);
    bipolar.store(false, std::memory_order_relaxed);
    switchValues.store(0, std::memory_order_relaxed);
    modulatorsAffectingMask.store(0, std::memory_order_relaxed);
    gesturesAffectingMask.store(0, std::memory_order_relaxed);
    rawKnobValue.store(0.0f, std::memory_order_relaxed);
    processedAbsoluteEpoch.store(processedEpoch, std::memory_order_relaxed);
    baseColor.Store(Color::Off);
    visualizer.store(nullptr, std::memory_order_relaxed);
    shortName.store(nullptr, std::memory_order_relaxed);
    voiceCount.store(0, std::memory_order_relaxed);
    modulatorColorCount.store(0, std::memory_order_relaxed);
    gestureColorCount.store(0, std::memory_order_relaxed);
    for (std::size_t voiceIx = 0; voiceIx < voiceCapacity; ++voiceIx) {
        values[voiceIx].store(0.0f, std::memory_order_relaxed);
        spreadValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        minValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        maxValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        switchValue[voiceIx].store(0, std::memory_order_relaxed);
        indicatorColors[voiceIx].Store(Color::Off);
    }
    for (std::size_t modIx = 0; modIx < modulatorColorCapacity; ++modIx) {
        modulatorSourceColors[modIx].Store(Color::Off);
    }
    for (std::size_t gestureIx = 0; gestureIx < gestureColorCapacity; ++gestureIx) {
        gestureColors[gestureIx].Store(Color::Off);
    }
    revision.fetch_add(1, std::memory_order_release);
}

std::size_t Parameter::SwitchValues() const {
    return config_.switchValues;
}

bool Parameter::IsSwitch() const {
    return config_.switchValues > 1;
}

Color Parameter::IndicatorColor(std::size_t voiceIx) const {
    if (voiceIx >= config_.indicatorColors.size()) {
        throw std::out_of_range("parameter indicator color index out of range");
    }
    return config_.indicatorColors[voiceIx];
}

float Parameter::GetRaw(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return ClampToRange(currentCenter_ * currentCenterScales_[voiceIx] + currentNormalizationOffsets_[voiceIx] +
                            group_.GetModulators().ApplyActive(
                                voiceIx, CurrentDepthSlots(voiceIx).first(activeRouteCount_),
                                ActiveRouteSourceIndices()),
                        config_.range);
}

float Parameter::CachedKnobValue(std::size_t voiceIx) const {
    if (voiceIx >= currentKnobValues_.size()) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return currentKnobValues_[voiceIx];
}

float Parameter::UIDisplayCenter(std::size_t voiceIx) const {
    if (voiceIx >= uiDisplayCenters_.size()) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return uiDisplayCenters_[voiceIx];
}

float Parameter::UIDisplaySpread(std::size_t voiceIx) const {
    if (voiceIx >= uiDisplaySpreadEnergies_.size()) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return std::sqrt(std::max(0.0f, uiDisplaySpreadEnergies_[voiceIx]));
}

std::size_t Parameter::GetSwitchVal(std::size_t voiceIx) const {
    if (config_.switchValues <= 1) {
        if (voiceIx >= group_.Config().numVoices) {
            throw std::out_of_range("parameter voice index out of range");
        }
        return 0;
    }

    float normalized = TargetValue(voiceIx);
    normalized = std::clamp(normalized, 0.0f, 1.0f);
    const double maxBucket = static_cast<double>(config_.switchValues - 1);
    const double rounded = std::round(static_cast<double>(normalized) * maxBucket);
    return static_cast<std::size_t>(std::clamp(rounded, 0.0, maxBucket));
}

void Parameter::PopulateUIState(UIState& state) const {
    PopulateUIState(state, group_.Manager().Scene(), 0);
}

void Parameter::PopulateUIState(UIState& state, const SceneState& scene,
                                std::uint64_t processedAbsoluteEpoch) const {
    const std::size_t voices = std::min(state.voiceCapacity, group_.Config().numVoices);
    state.revision.fetch_add(1, std::memory_order_acq_rel);
    state.bipolar.store(config_.range == RangeKind::Bipolar, std::memory_order_relaxed);
    state.switchValues.store(config_.switchValues, std::memory_order_relaxed);
    state.modulatorsAffectingMask.store(ModulatorsAffectingMask(), std::memory_order_relaxed);
    state.gesturesAffectingMask.store(GesturesAffectingMask(), std::memory_order_relaxed);
    state.rawKnobValue.store(ClampToRange(ComputeRawCenter(scene), config_.range), std::memory_order_relaxed);
    state.processedAbsoluteEpoch.store(processedAbsoluteEpoch, std::memory_order_relaxed);
    state.baseColor.Store(config_.baseColor);
    state.visualizer.store(config_.visualizer, std::memory_order_relaxed);
    state.shortName.store(config_.shortName.c_str(), std::memory_order_relaxed);
    state.voiceCount.store(voices, std::memory_order_relaxed);
    for (std::size_t voiceIx = 0; voiceIx < voices; ++voiceIx) {
        state.values[voiceIx].store(ToUIPresentation(UIDisplayCenter(voiceIx), config_.range),
                                    std::memory_order_relaxed);
        state.spreadValues[voiceIx].store(
            IsSwitch() ? 0.0f : ToUISpreadPresentation(UIDisplaySpread(voiceIx), config_.range),
            std::memory_order_relaxed);
        state.minValues[voiceIx].store(ToUIPresentation(currentMinValues_[voiceIx], config_.range),
                                       std::memory_order_relaxed);
        state.maxValues[voiceIx].store(ToUIPresentation(currentMaxValues_[voiceIx], config_.range),
                                       std::memory_order_relaxed);
        state.switchValue[voiceIx].store(GetSwitchVal(voiceIx), std::memory_order_relaxed);
        state.indicatorColors[voiceIx].Store(config_.indicatorColors[voiceIx]);
    }
    const std::size_t modulatorColors = std::min(state.modulatorColorCapacity, group_.Config().numModulators);
    state.modulatorColorCount.store(modulatorColors, std::memory_order_relaxed);
    for (std::size_t modIx = 0; modIx < modulatorColors; ++modIx) {
        state.modulatorSourceColors[modIx].Store(group_.GetModulators().Metadata(modIx).sourceColor);
    }
    for (std::size_t modIx = modulatorColors; modIx < state.modulatorColorCapacity; ++modIx) {
        state.modulatorSourceColors[modIx].Store(Color::Off);
    }
    const std::size_t gestureColors = std::min(state.gestureColorCapacity, group_.Manager().GestureCount());
    state.gestureColorCount.store(gestureColors, std::memory_order_relaxed);
    for (std::size_t gestureIx = 0; gestureIx < gestureColors; ++gestureIx) {
        state.gestureColors[gestureIx].Store(group_.Manager().GestureMetadataAt(gestureIx).gestureColor);
    }
    for (std::size_t gestureIx = gestureColors; gestureIx < state.gestureColorCapacity; ++gestureIx) {
        state.gestureColors[gestureIx].Store(Color::Off);
    }
    for (std::size_t voiceIx = voices; voiceIx < state.voiceCapacity; ++voiceIx) {
        state.values[voiceIx].store(0.0f, std::memory_order_relaxed);
        state.spreadValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        state.minValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        state.maxValues[voiceIx].store(0.0f, std::memory_order_relaxed);
        state.switchValue[voiceIx].store(0, std::memory_order_relaxed);
        state.indicatorColors[voiceIx].Store(Color::Off);
    }
    state.connected.store(true, std::memory_order_relaxed);
    state.revision.fetch_add(1, std::memory_order_release);
}

void Parameter::Compute(const SceneState& scene) {
    ComputeAtDepth(scene, 0, true);
}

JSON Parameter::ToValueJSON(JsonArena& arena) const {
    JSON root = arena.Object();

    JSON sceneCenters = arena.Array();
    for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
        sceneCenters.AppendNew(arena.Real(SceneCenter(sceneIx)));
    }
    root.SetNew("sceneCenters", sceneCenters);

    JSON gestureValues = arena.Array();
    for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
        JSON row = arena.Array();
        for (std::size_t gestureIx = 0; gestureIx < group_.GestureCount(); ++gestureIx) {
            row.AppendNew(arena.Real(GestureValue(sceneIx, gestureIx)));
        }
        gestureValues.AppendNew(row);
    }
    root.SetNew("gestureValues", gestureValues);

    JSON gestureActive = arena.Array();
    for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
        JSON row = arena.Array();
        for (std::size_t gestureIx = 0; gestureIx < group_.GestureCount(); ++gestureIx) {
            row.AppendNew(arena.Boolean(GestureActive(sceneIx, gestureIx)));
        }
        gestureActive.AppendNew(row);
    }
    root.SetNew("gestureActive", gestureActive);

    JSON modDepths = arena.Object();
    for (std::size_t modIx = 0; modIx < modulationDepths_.size(); ++modIx) {
        const Parameter* depthParameter = modulationDepths_[modIx];
        if (depthParameter == nullptr || !depthParameter->HasNonDefaultState()) {
            continue;
        }
        const std::string key = std::to_string(modIx);
        modDepths.SetNew(key.c_str(), depthParameter->ToValueJSON(arena));
    }
    root.SetNew("modDepths", modDepths);

    return root;
}

bool Parameter::LoadValuesFromJSON(JSON json) {
    if (!IsJsonObject(json)) {
        return false;
    }

    JSON sceneCenters = json.Get("sceneCenters");
    if (!sceneCenters.IsNull() && IsJsonArray(sceneCenters) && sceneCenters.Size() == group_.Config().numScenes) {
        for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
            SceneCenter(sceneIx) = static_cast<float>(sceneCenters.GetAt(sceneIx).NumberValue());
        }
    }

    JSON gestureValues = json.Get("gestureValues");
    bool gestureValuesShapeMatches = !gestureValues.IsNull() && IsJsonArray(gestureValues) &&
                                     gestureValues.Size() == group_.Config().numScenes;
    if (gestureValuesShapeMatches) {
        for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
            JSON row = gestureValues.GetAt(sceneIx);
            if (!IsJsonArray(row) || row.Size() != group_.GestureCount()) {
                gestureValuesShapeMatches = false;
                break;
            }
        }
    }
    if (gestureValuesShapeMatches) {
        for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
            JSON row = gestureValues.GetAt(sceneIx);
            for (std::size_t gestureIx = 0; gestureIx < group_.GestureCount(); ++gestureIx) {
                GestureValue(sceneIx, gestureIx) = static_cast<float>(row.GetAt(gestureIx).NumberValue());
            }
        }
    }

    JSON gestureActive = json.Get("gestureActive");
    bool gestureActiveShapeMatches = !gestureActive.IsNull() && IsJsonArray(gestureActive) &&
                                     gestureActive.Size() == group_.Config().numScenes;
    if (gestureActiveShapeMatches) {
        for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
            JSON row = gestureActive.GetAt(sceneIx);
            if (!IsJsonArray(row) || row.Size() != group_.GestureCount()) {
                gestureActiveShapeMatches = false;
                break;
            }
        }
    }
    if (gestureActiveShapeMatches) {
        for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
            JSON row = gestureActive.GetAt(sceneIx);
            for (std::size_t gestureIx = 0; gestureIx < group_.GestureCount(); ++gestureIx) {
                SetGestureActive(sceneIx, gestureIx, row.GetAt(gestureIx).BooleanValue());
            }
        }
    }

    JSON modDepths = json.Get("modDepths");
    if (IsJsonObject(modDepths)) {
        const JsonMember* members = JsonObjectMembers(modDepths);
        for (std::size_t ix = 0; ix < modDepths.Size(); ++ix) {
            if (members[ix].m_key == nullptr) {
                continue;
            }
            std::size_t modIx = 0;
            if (!ParseDecimalIndex(members[ix].m_key, modIx) || modIx >= modulationDepths_.size()) {
                continue;
            }
            Parameter* depthParameter = modulationDepths_[modIx];
            if (depthParameter == nullptr) {
                depthParameter = EnsureModulationDepth(modIx);
            }
            if (depthParameter == nullptr) {
                continue;
            }
            depthParameter->LoadValuesFromJSON(JSON(members[ix].m_value));
        }
    }

    return true;
}

void Parameter::ProcessLitePhase1() {
    const float alpha = group_.Config().processLiteAlpha;
    currentCenter_ += alpha * (targetCenter_ - currentCenter_);
    for (std::size_t voiceIx = 0; voiceIx < currentCenterScales_.size(); ++voiceIx) {
        currentCenterScales_[voiceIx] +=
            alpha * (targetCenterScales_[voiceIx] - currentCenterScales_[voiceIx]);
        currentNormalizationOffsets_[voiceIx] +=
            alpha * (targetNormalizationOffsets_[voiceIx] - currentNormalizationOffsets_[voiceIx]);
        currentMinValues_[voiceIx] += alpha * (targetMinValues_[voiceIx] - currentMinValues_[voiceIx]);
        currentMaxValues_[voiceIx] += alpha * (targetMaxValues_[voiceIx] - currentMaxValues_[voiceIx]);
    }
    for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
        for (std::size_t routeSlot = 0; routeSlot < activeRouteCount_; ++routeSlot) {
            const std::size_t ix = VoiceRouteIndex(voiceIx, routeSlot);
            currentDepths_[ix] += alpha * (targetDepths_[ix] - currentDepths_[ix]);
            if (group_.processingObserver_ != nullptr) {
                ++group_.processingObserver_->activeRouteVisits;
            }
        }
    }
    for (std::size_t voiceIx = 0; voiceIx < currentKnobValues_.size(); ++voiceIx) {
        currentKnobValues_[voiceIx] = GetRaw(voiceIx);
    }
}

void Parameter::ReplaceCachedKnobValue(std::size_t voiceIx, float normalizedValue) {
    if (voiceIx >= currentKnobValues_.size()) {
        throw std::out_of_range("parameter cached knob voice index out of range");
    }
    currentKnobValues_[voiceIx] = std::clamp(normalizedValue, 0.0f, 1.0f);
}

void Parameter::ProcessLitePhase2() {
    for (std::size_t voiceIx = 0; voiceIx < currentKnobValues_.size(); ++voiceIx) {
        const float knob = currentKnobValues_[voiceIx];
        uiDisplayCenters_[voiceIx] += group_.Config().uiDisplayCenterAlpha * (knob - uiDisplayCenters_[voiceIx]);
        const float residual = knob - uiDisplayCenters_[voiceIx];
        uiDisplaySpreadEnergies_[voiceIx] +=
            group_.Config().uiDisplaySpreadAlpha * ((residual * residual) - uiDisplaySpreadEnergies_[voiceIx]);
    }
}

void Parameter::ProcessLite() {
    ProcessLitePhase1();
    ProcessLitePhase2();
}

void Parameter::ProcessSamplePhase1(std::uint64_t sampleIndex) {
    if (sampleIndex % group_.Config().targetComputeIntervalSamples == 0) {
        Compute(group_.Manager().Scene());
    }
    ProcessLitePhase1();
}

void Parameter::ProcessSamplePhase2() {
    ProcessLitePhase2();
}

void Parameter::ProcessSample(std::uint64_t sampleIndex) {
    ProcessSamplePhase1(sampleIndex);
    ProcessSamplePhase2();
}

void Parameter::HandleIncDec(const SceneState& scene, float delta) {
    ValidateSceneEndpoints(scene);
    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);

    auto armSelectedGesture = [&](std::size_t sceneIx, std::size_t gestureIx) {
        if (GestureActive(sceneIx, gestureIx)) {
            return false;
        }
        GestureValue(sceneIx, gestureIx) = SceneCenter(sceneIx);
        SetGestureActive(sceneIx, gestureIx, true);
        return true;
    };

    bool armedGesture = false;
    ForEachGestureBit(group_.Manager().SelectedGestureMask() & GestureCountMask(group_.GestureCount()),
                      [&](std::size_t gestureIx) {
        if (blend <= 0.0f) {
            armedGesture = armSelectedGesture(scene.leftScene, gestureIx) || armedGesture;
        } else if (blend >= 1.0f) {
            armedGesture = armSelectedGesture(scene.rightScene, gestureIx) || armedGesture;
        } else {
            armedGesture = armSelectedGesture(scene.leftScene, gestureIx) || armedGesture;
            if (scene.rightScene != scene.leftScene) {
                armedGesture = armSelectedGesture(scene.rightScene, gestureIx) || armedGesture;
            }
        }
    });

    if (armedGesture) {
        return;
    }

    float activeEffectiveWeightSum = 0.0f;
    float baseShareNumerator = 0.0f;
    const GestureMask activeGestures =
        (gestureActiveMasks_[scene.leftScene] | gestureActiveMasks_[scene.rightScene]) &
        GestureCountMask(group_.GestureCount());
    ForEachGestureBit(activeGestures, [&](std::size_t gestureIx) {
        const float effectiveWeight = EffectiveGestureWeight(scene, gestureIx, blend);
        if (effectiveWeight == 0.0f) {
            return;
        }
        activeEffectiveWeightSum += effectiveWeight;
        baseShareNumerator += effectiveWeight * (1.0f - effectiveWeight);
    });

    if (activeEffectiveWeightSum == 0.0f) {
        ApplySceneDistribution(SceneCenter(scene.leftScene), SceneCenter(scene.rightScene), blend, delta, config_.range);
        return;
    }

    ApplySceneDistribution(SceneCenter(scene.leftScene), SceneCenter(scene.rightScene), blend,
                           delta * (baseShareNumerator / activeEffectiveWeightSum), config_.range);

    ForEachGestureBit(activeGestures, [&](std::size_t gestureIx) {
        const float effectiveWeight = EffectiveGestureWeight(scene, gestureIx, blend);
        if (effectiveWeight == 0.0f) {
            return;
        }

        const float gestureDelta = delta * ((effectiveWeight * effectiveWeight) / activeEffectiveWeightSum);
        ApplySceneDistribution(GestureValue(scene.leftScene, gestureIx), GestureValue(scene.rightScene, gestureIx),
                               blend, gestureDelta, config_.range);
    });
}

void Parameter::HandleSetAbsolute(const SceneState& scene, float normalizedTarget) noexcept {
    constexpr std::size_t kMaxGestures = detail::kAbsoluteMaxGestureContributions;
    std::array<float, kMaxGestures> leftGestureSnapshot{};
    std::array<float, kMaxGestures> rightGestureSnapshot{};
    std::size_t gestureCount = 0;
    GestureMask leftMaskSnapshot = 0;
    GestureMask rightMaskSnapshot = 0;
    bool snapshotReady = false;
    bool committed = false;

    auto restoreSnapshot = [&]() noexcept {
        if (!snapshotReady || committed) {
            return;
        }
        for (std::size_t gestureIx = 0; gestureIx < gestureCount; ++gestureIx) {
            gestureValues_[scene.leftScene * gestureCount + gestureIx] =
                leftGestureSnapshot[gestureIx];
            gestureValues_[scene.rightScene * gestureCount + gestureIx] =
                rightGestureSnapshot[gestureIx];
        }
        gestureActiveMasks_[scene.leftScene] = leftMaskSnapshot;
        gestureActiveMasks_[scene.rightScene] = rightMaskSnapshot;
    };

    try {
        const std::size_t sceneCount = group_.Config().numScenes;
        gestureCount = group_.GestureCount();
        const bool storageSizeWouldOverflow =
            gestureCount != 0 &&
            sceneCount > std::numeric_limits<std::size_t>::max() / gestureCount;
        if (!std::isfinite(normalizedTarget) || !std::isfinite(scene.blend) ||
            scene.leftScene >= sceneCount || scene.rightScene >= sceneCount ||
            gestureCount > kMaxGestures || group_.Manager().GestureCount() != gestureCount ||
            storageSizeWouldOverflow ||
            sceneCenters_.size() < sceneCount || gestureActiveMasks_.size() < sceneCount ||
            gestureValues_.size() < sceneCount * gestureCount) {
            return;
        }

        const double minimum = static_cast<double>(RangeMin(config_.range));
        const double maximum = static_cast<double>(RangeMax(config_.range));
        auto storageIsValid = [minimum, maximum](float value) {
            return std::isfinite(value) && value >= minimum && value <= maximum;
        };
        if (!storageIsValid(sceneCenters_[scene.leftScene]) ||
            !storageIsValid(sceneCenters_[scene.rightScene])) {
            return;
        }

        const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
        const GestureMask validGestureMask = GestureCountMask(gestureCount);
        const GestureMask selectedGestures =
            group_.Manager().SelectedGestureMask() & validGestureMask;
        const GestureMask initiallyActiveGestures =
            (gestureActiveMasks_[scene.leftScene] | gestureActiveMasks_[scene.rightScene]) &
            validGestureMask;
        const GestureMask relevantGestures = selectedGestures | initiallyActiveGestures;
        std::array<double, kMaxGestures> gestureWeights{};
        bool relevantStateValid = true;
        ForEachGestureBit(relevantGestures, [&](std::size_t gestureIx) {
            const double weight =
                static_cast<double>(group_.Manager().GestureValue(gestureIx));
            if (!std::isfinite(weight) || weight < 0.0 || weight > 1.0) {
                relevantStateValid = false;
                return;
            }
            gestureWeights[gestureIx] = weight;
            if ((initiallyActiveGestures & (GestureMask{1} << gestureIx)) != 0 &&
                (!storageIsValid(gestureValues_[scene.leftScene * gestureCount + gestureIx]) ||
                 !storageIsValid(gestureValues_[scene.rightScene * gestureCount + gestureIx]))) {
                relevantStateValid = false;
            }
        });
        if (!relevantStateValid) {
            return;
        }

        leftMaskSnapshot = gestureActiveMasks_[scene.leftScene];
        rightMaskSnapshot = gestureActiveMasks_[scene.rightScene];
        for (std::size_t gestureIx = 0; gestureIx < gestureCount; ++gestureIx) {
            leftGestureSnapshot[gestureIx] =
                gestureValues_[scene.leftScene * gestureCount + gestureIx];
            rightGestureSnapshot[gestureIx] =
                gestureValues_[scene.rightScene * gestureCount + gestureIx];
        }
        snapshotReady = true;

        auto armSelectedGesture = [&](std::size_t sceneIx, std::size_t gestureIx) {
            const GestureMask bit = GestureMask{1} << gestureIx;
            if ((gestureActiveMasks_[sceneIx] & bit) != 0) {
                return;
            }
            gestureValues_[sceneIx * gestureCount + gestureIx] = sceneCenters_[sceneIx];
            gestureActiveMasks_[sceneIx] |= bit;
        };
        ForEachGestureBit(selectedGestures, [&](std::size_t gestureIx) {
            if (blend <= 0.0f) {
                armSelectedGesture(scene.leftScene, gestureIx);
            } else if (blend >= 1.0f) {
                armSelectedGesture(scene.rightScene, gestureIx);
            } else {
                armSelectedGesture(scene.leftScene, gestureIx);
                if (scene.rightScene != scene.leftScene) {
                    armSelectedGesture(scene.rightScene, gestureIx);
                }
            }
        });

        std::array<detail::AbsoluteGestureContribution, kMaxGestures> gestureContributions{};
        std::size_t contributionCount = 0;
        const GestureMask activeGestures =
            (gestureActiveMasks_[scene.leftScene] | gestureActiveMasks_[scene.rightScene]) &
            validGestureMask;
        ForEachGestureBit(activeGestures, [&](std::size_t gestureIx) {
            const double weight = gestureWeights[gestureIx];
            const double effectiveWeight =
                ((gestureActiveMasks_[scene.leftScene] & (GestureMask{1} << gestureIx)) != 0
                     ? weight * (1.0 - static_cast<double>(blend))
                     : 0.0) +
                ((gestureActiveMasks_[scene.rightScene] & (GestureMask{1} << gestureIx)) != 0
                     ? weight * static_cast<double>(blend)
                     : 0.0);
            if (effectiveWeight <= 0.0) {
                return;
            }
            gestureContributions[contributionCount++] = {
                &gestureValues_[scene.leftScene * gestureCount + gestureIx],
                &gestureValues_[scene.rightScene * gestureCount + gestureIx],
                effectiveWeight,
            };
        });

        detail::AbsoluteEditWorkspace workspace;
        if (!detail::TryBuildAbsoluteEditLocations(
                sceneCenters_[scene.leftScene], sceneCenters_[scene.rightScene],
                static_cast<double>(blend),
                std::span<const detail::AbsoluteGestureContribution>(
                    gestureContributions.data(), contributionCount),
                workspace)) {
            restoreSnapshot();
            return;
        }
        const double target = static_cast<double>(LinearMap(
            static_cast<float>(minimum), static_cast<float>(maximum),
            std::clamp(normalizedTarget, 0.0f, 1.0f)));
        if (!detail::ProjectAbsoluteTarget(workspace, minimum, maximum, target)) {
            restoreSnapshot();
            return;
        }
        committed = true;
    } catch (...) {
        restoreSnapshot();
    }
}

void Parameter::RandomizeVisibleValue(const SceneState& scene, float normalized) {
    ValidateSceneEndpoints(scene);
    const float target = LinearMap(RangeMin(config_.range), RangeMax(config_.range),
                                   std::clamp(normalized, 0.0f, 1.0f));
    Compute(scene);
    SnapCurrentToTarget();
    const float delta = target - TargetValue(0);
    HandleIncDec(scene, delta);
    Compute(scene);
    SnapCurrentToTarget();
}

void Parameter::RevertToDefault(const SceneState& scene) {
    ValidateSceneEndpoints(scene);
    for (Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr) {
            depthParameter->ResetModulationDepthToNeutral(scene);
        }
    }
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    activeRouteCount_ = 0;
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);

    const float defaultValue = ClampToRange(config_.defaultValue, config_.range);
    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
    if (blend <= 0.0f) {
        ResetSceneToDefault(scene.leftScene, defaultValue);
    } else if (blend >= 1.0f) {
        ResetSceneToDefault(scene.rightScene, defaultValue);
    } else {
        ResetSceneToDefault(scene.leftScene, defaultValue);
        if (scene.rightScene != scene.leftScene) {
            ResetSceneToDefault(scene.rightScene, defaultValue);
        }
    }

    currentCenter_ = defaultValue;
    targetCenter_ = defaultValue;
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), defaultValue);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), defaultValue);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), defaultValue);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), defaultValue);
    SeedCachedKnobAndUiDisplayState();
}

void Parameter::RevertAllToDefault() {
    for (Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr) {
            depthParameter->RevertAllToDefault();
        }
    }
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    activeRouteCount_ = 0;
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);

    const float defaultValue = ClampToRange(config_.defaultValue, config_.range);
    for (std::size_t sceneIx = 0; sceneIx < group_.Config().numScenes; ++sceneIx) {
        ResetSceneToDefault(sceneIx, defaultValue);
        for (std::size_t gestureIx = 0; gestureIx < group_.GestureCount(); ++gestureIx) {
            GestureValue(sceneIx, gestureIx) = defaultValue;
        }
    }

    currentCenter_ = defaultValue;
    targetCenter_ = defaultValue;
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), defaultValue);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), defaultValue);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), defaultValue);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), defaultValue);
    SeedCachedKnobAndUiDisplayState();
}

bool Parameter::AssignModulationDepth(std::size_t modIx, Parameter* parameter) {
    if (modIx >= modulationDepths_.size()) {
        throw std::out_of_range("modulation depth index out of range");
    }
    if (parameter != nullptr && &parameter->Group() != &group_) {
        return false;
    }
    if (parameter != nullptr && WouldCreateCycle(parameter)) {
        return false;
    }

    modulationDepths_[modIx] = parameter;
    return true;
}

Parameter* Parameter::EnsureModulationDepth(std::size_t modIx) {
    if (modIx >= modulationDepths_.size()) {
        throw std::out_of_range("modulation depth index out of range");
    }
    if (Parameter* existing = modulationDepths_[modIx]; existing != nullptr) {
        return existing;
    }
    if (!group_.CanAllocate()) {
        group_.RequestParameterStorageBatch(1);
        return nullptr;
    }
    return &EnsureModulationDepth(modIx, ModulationDepthConfig(modIx));
}

Parameter& Parameter::EnsureModulationDepth(std::size_t modIx, ParameterConfig config) {
    if (modIx >= modulationDepths_.size()) {
        throw std::out_of_range("modulation depth index out of range");
    }
    if (Parameter* existing = modulationDepths_[modIx]; existing != nullptr) {
        return *existing;
    }

    config = ResolveParameterAppearance(std::move(config), group_.Config().numVoices);
    Parameter& created = group_.CreateLocalParameter(std::move(config), kLocalParameterId);
    if (!AssignModulationDepth(modIx, &created)) {
        throw std::logic_error("created modulation depth could not be assigned");
    }
    return created;
}

void Parameter::ClearModulationDepths() {
    std::fill(modulationDepths_.begin(), modulationDepths_.end(), nullptr);
}

Parameter* Parameter::ModulationDepthParameter(std::size_t modIx) const {
    if (modIx >= modulationDepths_.size()) {
        throw std::out_of_range("modulation depth index out of range");
    }
    return modulationDepths_[modIx];
}

ParameterConfig Parameter::ModulationDepthConfig(std::size_t modIx) const {
    if (modIx >= modulationDepths_.size()) {
        throw std::out_of_range("modulation depth index out of range");
    }
    const ModulatorMetadata& modulator = group_.GetModulators().Metadata(modIx);
    return {
        .name = modulator.name.empty()
                    ? Name() + " Mod Depth " + std::to_string(modIx + 1)
                    : Name() + " " + modulator.name,
        .shortName = modulator.shortName.empty() ? ShortName() : modulator.shortName,
        .defaultValue = kNeutralModulationDepthCenter,
        .range = RangeKind::Bipolar,
        .baseColor = modulator.sourceColor,
        .visualizer = modulator.visualizer,
        .indicatorColors = config_.indicatorColors,
    };
}

float& Parameter::SceneCenter(std::size_t sceneIx) {
    if (sceneIx >= group_.Config().numScenes) {
        throw std::out_of_range("parameter scene index out of range");
    }
    return sceneCenters_[sceneIx];
}

float Parameter::SceneCenter(std::size_t sceneIx) const {
    if (sceneIx >= group_.Config().numScenes) {
        throw std::out_of_range("parameter scene index out of range");
    }
    return sceneCenters_[sceneIx];
}

float& Parameter::GestureValue(std::size_t sceneIx, std::size_t gestureIx) {
    return gestureValues_[SceneGestureIndex(sceneIx, gestureIx)];
}

float Parameter::GestureValue(std::size_t sceneIx, std::size_t gestureIx) const {
    return gestureValues_[SceneGestureIndex(sceneIx, gestureIx)];
}

void Parameter::SetGestureActive(std::size_t sceneIx, std::size_t gestureIx, bool active) {
    ValidateSceneGestureIndices(sceneIx, gestureIx);
    const GestureMask bit = GestureMask{1} << gestureIx;
    if (active) {
        gestureActiveMasks_[sceneIx] |= bit;
    } else {
        gestureActiveMasks_[sceneIx] &= ~bit;
    }
}

bool Parameter::GestureActive(std::size_t sceneIx, std::size_t gestureIx) const {
    ValidateSceneGestureIndices(sceneIx, gestureIx);
    return (gestureActiveMasks_[sceneIx] & (GestureMask{1} << gestureIx)) != 0;
}

std::span<float> Parameter::CurrentDepthSlots(std::size_t voiceIx) {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    if (group_.Config().numModulators == 0) {
        return {};
    }
    const std::size_t rowStart = voiceIx * group_.Config().numModulators;
    return std::span<float>(currentDepths_.data() + rowStart, group_.Config().numModulators);
}

std::span<const float> Parameter::CurrentDepthSlots(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    if (group_.Config().numModulators == 0) {
        return {};
    }
    const std::size_t rowStart = voiceIx * group_.Config().numModulators;
    return std::span<const float>(currentDepths_.data() + rowStart, group_.Config().numModulators);
}

std::span<float> Parameter::TargetDepthSlots(std::size_t voiceIx) {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    if (group_.Config().numModulators == 0) {
        return {};
    }
    const std::size_t rowStart = voiceIx * group_.Config().numModulators;
    return std::span<float>(targetDepths_.data() + rowStart, group_.Config().numModulators);
}

std::span<const float> Parameter::TargetDepthSlots(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    if (group_.Config().numModulators == 0) {
        return {};
    }
    const std::size_t rowStart = voiceIx * group_.Config().numModulators;
    return std::span<const float>(targetDepths_.data() + rowStart, group_.Config().numModulators);
}

float Parameter::CurrentDepthForSource(std::size_t voiceIx, std::size_t sourceIx) const {
    return currentDepths_[VoiceRouteIndex(voiceIx, RoutePositionForSource(sourceIx))];
}

float Parameter::TargetDepthForSource(std::size_t voiceIx, std::size_t sourceIx) const {
    return targetDepths_[VoiceRouteIndex(voiceIx, RoutePositionForSource(sourceIx))];
}

std::size_t Parameter::RouteSourceIndex(std::size_t slot) const {
    if (slot >= routeSourceIndices_.size()) {
        throw std::out_of_range("parameter route slot out of range");
    }
    return routeSourceIndices_[slot];
}

std::size_t Parameter::RoutePositionForSource(std::size_t sourceIx) const {
    if (sourceIx >= sourceRoutePositions_.size()) {
        throw std::out_of_range("parameter modulator index out of range");
    }
    return sourceRoutePositions_[sourceIx];
}

float Parameter::CurrentCenterScale(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return currentCenterScales_[voiceIx];
}

float Parameter::TargetCenterScale(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return targetCenterScales_[voiceIx];
}

float Parameter::CurrentNormalizationOffset(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return currentNormalizationOffsets_[voiceIx];
}

float Parameter::TargetNormalizationOffset(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return targetNormalizationOffsets_[voiceIx];
}

std::size_t Parameter::VoiceRouteIndex(std::size_t voiceIx, std::size_t routeSlot) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    if (routeSlot >= group_.Config().numModulators) {
        throw std::out_of_range("parameter route slot out of range");
    }
    return voiceIx * group_.Config().numModulators + routeSlot;
}

void Parameter::AssertRouteBijection() const {
#ifndef NDEBUG
    assert(activeRouteCount_ <= routeSourceIndices_.size());
    for (std::size_t slot = 0; slot < routeSourceIndices_.size(); ++slot) {
        assert(routeSourceIndices_[slot] < sourceRoutePositions_.size());
        assert(sourceRoutePositions_[routeSourceIndices_[slot]] == slot);
    }
#endif
}

void Parameter::EnsureRouteActive(std::size_t sourceIx) {
    if (sourceIx >= sourceRoutePositions_.size()) {
        throw std::out_of_range("parameter modulator index out of range");
    }
    const std::size_t routeSlot = sourceRoutePositions_[sourceIx];
    if (routeSlot < activeRouteCount_) {
        return;
    }

    const std::size_t destination = activeRouteCount_;
    if (routeSlot != destination) {
        for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
            std::swap(currentDepths_[VoiceRouteIndex(voiceIx, routeSlot)],
                      currentDepths_[VoiceRouteIndex(voiceIx, destination)]);
            std::swap(targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)],
                      targetDepths_[VoiceRouteIndex(voiceIx, destination)]);
        }
        const std::size_t displacedSource = routeSourceIndices_[destination];
        std::swap(routeSourceIndices_[routeSlot], routeSourceIndices_[destination]);
        sourceRoutePositions_[sourceIx] = destination;
        sourceRoutePositions_[displacedSource] = routeSlot;
    }
    ++activeRouteCount_;
    AssertRouteBijection();
}

void Parameter::RemoveActiveRoute(std::size_t routeSlot) {
    if (routeSlot >= activeRouteCount_) {
        throw std::out_of_range("active parameter route slot out of range");
    }
    const std::size_t lastActive = activeRouteCount_ - 1;
    if (routeSlot != lastActive) {
        for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
            std::swap(currentDepths_[VoiceRouteIndex(voiceIx, routeSlot)],
                      currentDepths_[VoiceRouteIndex(voiceIx, lastActive)]);
            std::swap(targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)],
                      targetDepths_[VoiceRouteIndex(voiceIx, lastActive)]);
        }
        const std::size_t removedSource = routeSourceIndices_[routeSlot];
        const std::size_t movedSource = routeSourceIndices_[lastActive];
        std::swap(routeSourceIndices_[routeSlot], routeSourceIndices_[lastActive]);
        sourceRoutePositions_[movedSource] = routeSlot;
        sourceRoutePositions_[removedSource] = lastActive;
    }
    --activeRouteCount_;
    AssertRouteBijection();
}

bool Parameter::RouteNeutralAcrossVoices(std::size_t routeSlot) const {
    for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
        if (std::fabs(currentDepths_[VoiceRouteIndex(voiceIx, routeSlot)]) > kModulationNeutralTolerance ||
            std::fabs(targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)]) > kModulationNeutralTolerance) {
            return false;
        }
    }
    return true;
}

void Parameter::PruneNeutralActiveRoutes() {
    for (std::size_t routeSlot = activeRouteCount_; routeSlot-- > 0;) {
        if (!RouteNeutralAcrossVoices(routeSlot)) {
            continue;
        }
        for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
            currentDepths_[VoiceRouteIndex(voiceIx, routeSlot)] = 0.0f;
            targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)] = 0.0f;
        }
        RemoveActiveRoute(routeSlot);
    }
}

std::size_t Parameter::SceneGestureIndex(std::size_t sceneIx, std::size_t gestureIx) const {
    ValidateSceneGestureIndices(sceneIx, gestureIx);
    return sceneIx * group_.GestureCount() + gestureIx;
}

void Parameter::ValidateSceneGestureIndices(std::size_t sceneIx, std::size_t gestureIx) const {
    if (sceneIx >= group_.Config().numScenes) {
        throw std::out_of_range("parameter scene index out of range");
    }
    if (gestureIx >= group_.GestureCount()) {
        throw std::out_of_range("parameter gesture index out of range");
    }
}

void Parameter::ValidateSceneEndpoints(const SceneState& scene) const {
    if (scene.leftScene >= group_.Config().numScenes || scene.rightScene >= group_.Config().numScenes) {
        throw std::out_of_range("parameter scene index out of range");
    }
}

float Parameter::EffectiveGestureWeight(const SceneState& scene, std::size_t gestureIx, float blend) const {
    const float clampedBlend = std::clamp(blend, 0.0f, 1.0f);
    const float groupWeight = group_.Manager().GestureValue(gestureIx);
    const float leftWeight = GestureActive(scene.leftScene, gestureIx) ? groupWeight * (1.0f - clampedBlend) : 0.0f;
    const float rightWeight = GestureActive(scene.rightScene, gestureIx) ? groupWeight * clampedBlend : 0.0f;
    return leftWeight + rightWeight;
}

void Parameter::ResetSceneToDefault(std::size_t sceneIx, float defaultValue) {
    SceneCenter(sceneIx) = defaultValue;
    gestureActiveMasks_[sceneIx] = 0;
}

void Parameter::ResetModulationDepthToNeutral(const SceneState& scene) {
    ValidateSceneEndpoints(scene);
    for (Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr) {
            depthParameter->ResetModulationDepthToNeutral(scene);
        }
    }

    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
    if (blend <= 0.0f) {
        ResetSceneToDefault(scene.leftScene, kNeutralModulationDepthCenter);
    } else if (blend >= 1.0f) {
        ResetSceneToDefault(scene.rightScene, kNeutralModulationDepthCenter);
    } else {
        ResetSceneToDefault(scene.leftScene, kNeutralModulationDepthCenter);
        if (scene.rightScene != scene.leftScene) {
            ResetSceneToDefault(scene.rightScene, kNeutralModulationDepthCenter);
        }
    }

    currentCenter_ = kNeutralModulationDepthCenter;
    targetCenter_ = kNeutralModulationDepthCenter;
    std::fill(currentCenterScales_.begin(), currentCenterScales_.end(), 1.0f);
    std::fill(targetCenterScales_.begin(), targetCenterScales_.end(), 1.0f);
    std::fill(currentNormalizationOffsets_.begin(), currentNormalizationOffsets_.end(), 0.0f);
    std::fill(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), 0.0f);
    std::fill(currentMinValues_.begin(), currentMinValues_.end(), kNeutralModulationDepthCenter);
    std::fill(targetMinValues_.begin(), targetMinValues_.end(), kNeutralModulationDepthCenter);
    std::fill(currentMaxValues_.begin(), currentMaxValues_.end(), kNeutralModulationDepthCenter);
    std::fill(targetMaxValues_.begin(), targetMaxValues_.end(), kNeutralModulationDepthCenter);
    std::fill(currentDepths_.begin(), currentDepths_.end(), 0.0f);
    std::fill(targetDepths_.begin(), targetDepths_.end(), 0.0f);
    activeRouteCount_ = 0;
    SeedCachedKnobAndUiDisplayState();
}

float Parameter::ComputeRawCenter(const SceneState& scene) const {
    ValidateSceneEndpoints(scene);
    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
    const float inverseBlend = 1.0f - blend;
    const float base = SceneCenter(scene.leftScene) * inverseBlend + SceneCenter(scene.rightScene) * blend;

    float weightedMixSum = 0.0f;
    float activeWeightSum = 0.0f;
    const GestureMask activeGestures =
        (gestureActiveMasks_[scene.leftScene] | gestureActiveMasks_[scene.rightScene]) &
        GestureCountMask(group_.GestureCount());
    ForEachGestureBit(activeGestures, [&](std::size_t gestureIx) {
        if (group_.processingObserver_ != nullptr) {
            ++group_.processingObserver_->activeGestureVisits;
        }
        const float effectiveWeight = EffectiveGestureWeight(scene, gestureIx, blend);
        if (effectiveWeight == 0.0f) {
            return;
        }

        const float gestureValue = GestureValue(scene.leftScene, gestureIx) * inverseBlend +
                                   GestureValue(scene.rightScene, gestureIx) * blend;
        const float mix = base * (1.0f - effectiveWeight) + gestureValue * effectiveWeight;
        weightedMixSum += effectiveWeight * mix;
        activeWeightSum += effectiveWeight;
    });

    if (activeWeightSum == 0.0f) {
        return base;
    }
    return weightedMixSum / activeWeightSum;
}

void Parameter::ComputeAtDepth(const SceneState& scene, std::size_t recursionDepth, bool smoothTargetCenter) {
    recursionDepth_ = recursionDepth;
    if (recursionDepth > 0 && group_.processingObserver_ != nullptr) {
        ++group_.processingObserver_->localRecursiveComputeCalls;
    }
    const float rawCenter = ClampToRange(ComputeRawCenter(scene), config_.range);
    if (smoothTargetCenter && recursionDepth == 0) {
        const float alpha = group_.Config().targetCenterAlpha;
        targetCenter_ += alpha * (rawCenter - targetCenter_);
        targetCenter_ = ClampToRange(targetCenter_, config_.range);
    } else {
        targetCenter_ = rawCenter;
    }

    for (Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr) {
            depthParameter->ComputeAtDepth(scene, recursionDepth_ + 1, smoothTargetCenter);
        }
    }

    for (std::size_t sourceIx = 0; sourceIx < group_.Config().numModulators; ++sourceIx) {
        const Parameter* depthParameter = modulationDepths_[sourceIx];
        bool targetNonNeutral = false;
        if (depthParameter != nullptr) {
            for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
                if (std::fabs(ModulationDepthTargetFromKnob(depthParameter->GetRaw(voiceIx))) >
                    kModulationNeutralTolerance) {
                    targetNonNeutral = true;
                    break;
                }
            }
        }

        const std::size_t oldRouteSlot = sourceRoutePositions_[sourceIx];
        bool currentNonNeutral = false;
        if (oldRouteSlot < activeRouteCount_) {
            for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
                if (std::fabs(currentDepths_[VoiceRouteIndex(voiceIx, oldRouteSlot)]) >
                    kModulationNeutralTolerance) {
                    currentNonNeutral = true;
                    break;
                }
            }
        }
        if (targetNonNeutral || currentNonNeutral) {
            EnsureRouteActive(sourceIx);
        }

        const std::size_t routeSlot = sourceRoutePositions_[sourceIx];
        for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
            targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)] =
                depthParameter == nullptr ? 0.0f
                                          : ModulationDepthTargetFromKnob(depthParameter->GetRaw(voiceIx));
        }
    }

    for (std::size_t voiceIx = 0; voiceIx < group_.Config().numVoices; ++voiceIx) {
        float weightSum = 0.0f;
        for (std::size_t routeSlot = 0; routeSlot < activeRouteCount_; ++routeSlot) {
            weightSum += std::fabs(targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)]);
        }

        if (weightSum < 1.0f) {
            targetCenterScales_[voiceIx] = 1.0f - weightSum;
        } else {
            targetCenterScales_[voiceIx] = 0.0f;
            for (std::size_t routeSlot = 0; routeSlot < activeRouteCount_; ++routeSlot) {
                targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)] /= weightSum;
            }
        }

        float normalizationOffset = 0.0f;
        for (std::size_t routeSlot = 0; routeSlot < activeRouteCount_; ++routeSlot) {
            normalizationOffset -= std::min(0.0f, targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)]);
        }
        targetNormalizationOffsets_[voiceIx] = normalizationOffset;

        if (weightSum > 1.0f) {
            targetMinValues_[voiceIx] = RangeMin(config_.range);
            targetMaxValues_[voiceIx] = RangeMax(config_.range);
        } else {
            float minContribution = 0.0f;
            float maxContribution = 0.0f;
            for (std::size_t routeSlot = 0; routeSlot < activeRouteCount_; ++routeSlot) {
                const float depth = targetDepths_[VoiceRouteIndex(voiceIx, routeSlot)];
                minContribution += std::min(0.0f, depth);
                maxContribution += std::max(0.0f, depth);
            }
            const float base = targetCenter_ * targetCenterScales_[voiceIx] + targetNormalizationOffsets_[voiceIx];
            targetMinValues_[voiceIx] = ClampToRange(base + minContribution, config_.range);
            targetMaxValues_[voiceIx] = ClampToRange(base + maxContribution, config_.range);
        }
    }

    if (recursionDepth_ > 0) {
        currentCenter_ = targetCenter_;
        std::copy(targetCenterScales_.begin(), targetCenterScales_.end(), currentCenterScales_.begin());
        std::copy(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(),
                  currentNormalizationOffsets_.begin());
        std::copy(targetMinValues_.begin(), targetMinValues_.end(), currentMinValues_.begin());
        std::copy(targetMaxValues_.begin(), targetMaxValues_.end(), currentMaxValues_.begin());
        std::copy(targetDepths_.begin(), targetDepths_.end(), currentDepths_.begin());
        SeedCachedKnobAndUiDisplayState();
    }
    PruneNeutralActiveRoutes();
}

void Parameter::SnapCurrentToTarget() {
    currentCenter_ = targetCenter_;
    std::copy(targetCenterScales_.begin(), targetCenterScales_.end(), currentCenterScales_.begin());
    std::copy(targetNormalizationOffsets_.begin(), targetNormalizationOffsets_.end(), currentNormalizationOffsets_.begin());
    std::copy(targetMinValues_.begin(), targetMinValues_.end(), currentMinValues_.begin());
    std::copy(targetMaxValues_.begin(), targetMaxValues_.end(), currentMaxValues_.begin());
    std::copy(targetDepths_.begin(), targetDepths_.end(), currentDepths_.begin());
    PruneNeutralActiveRoutes();
    SeedCachedKnobAndUiDisplayState();
    for (Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr) {
            depthParameter->SnapCurrentToTarget();
        }
    }
}

void Parameter::SeedCachedKnobAndUiDisplayState() {
    for (std::size_t voiceIx = 0; voiceIx < currentKnobValues_.size(); ++voiceIx) {
        currentKnobValues_[voiceIx] = GetRaw(voiceIx);
        uiDisplayCenters_[voiceIx] = currentKnobValues_[voiceIx];
        uiDisplaySpreadEnergies_[voiceIx] = 0.0f;
    }
}

bool Parameter::WouldCreateCycle(const Parameter* candidate) const {
    if (candidate == this) {
        return true;
    }

    for (const Parameter* route : candidate->modulationDepths_) {
        if (route != nullptr && WouldCreateCycle(route)) {
            return true;
        }
    }
    return false;
}

float Parameter::TargetValue(std::size_t voiceIx) const {
    if (voiceIx >= group_.Config().numVoices) {
        throw std::out_of_range("parameter voice index out of range");
    }
    return ClampToRange(targetCenter_ * targetCenterScales_[voiceIx] + targetNormalizationOffsets_[voiceIx] +
                            group_.GetModulators().ApplyActive(
                                voiceIx, TargetDepthSlots(voiceIx).first(activeRouteCount_),
                                ActiveRouteSourceIndices()),
                        config_.range);
}

std::uint32_t Parameter::ModulatorsAffectingMask() const {
    std::uint32_t mask = 0;
    const std::size_t count = std::min<std::size_t>(modulationDepths_.size(), 32);
    for (std::size_t modIx = 0; modIx < count; ++modIx) {
        if (modulationDepths_[modIx] != nullptr && modulationDepths_[modIx]->HasNonZeroState()) {
            mask |= (std::uint32_t{1} << modIx);
        }
    }
    return mask;
}

bool Parameter::HasNonZeroState() const {
    if (std::fabs(currentCenter_ - kNeutralModulationDepthCenter) > kModulationNeutralTolerance ||
        std::fabs(targetCenter_ - kNeutralModulationDepthCenter) > kModulationNeutralTolerance) {
        return true;
    }
    for (const float center : sceneCenters_) {
        if (std::fabs(center - kNeutralModulationDepthCenter) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const GestureMask activeMask : gestureActiveMasks_) {
        if (activeMask != 0) {
            return true;
        }
    }
    for (const float depth : currentDepths_) {
        if (std::fabs(depth) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float depth : targetDepths_) {
        if (std::fabs(depth) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float offset : currentNormalizationOffsets_) {
        if (std::fabs(offset) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float offset : targetNormalizationOffsets_) {
        if (std::fabs(offset) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr && depthParameter->HasNonZeroState()) {
            return true;
        }
    }
    return false;
}

bool Parameter::HasNonDefaultState() const {
    const float defaultValue = ClampToRange(config_.defaultValue, config_.range);

    if (std::fabs(currentCenter_ - defaultValue) > kModulationNeutralTolerance ||
        std::fabs(targetCenter_ - defaultValue) > kModulationNeutralTolerance) {
        return true;
    }
    for (const float center : sceneCenters_) {
        if (std::fabs(center - defaultValue) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float value : gestureValues_) {
        if (std::fabs(value - defaultValue) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const GestureMask activeMask : gestureActiveMasks_) {
        if (activeMask != 0) {
            return true;
        }
    }
    for (const float depth : currentDepths_) {
        if (std::fabs(depth) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float depth : targetDepths_) {
        if (std::fabs(depth) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float scale : currentCenterScales_) {
        if (std::fabs(scale - 1.0f) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float scale : targetCenterScales_) {
        if (std::fabs(scale - 1.0f) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float offset : currentNormalizationOffsets_) {
        if (std::fabs(offset) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const float offset : targetNormalizationOffsets_) {
        if (std::fabs(offset) > kModulationNeutralTolerance) {
            return true;
        }
    }
    for (const Parameter* depthParameter : modulationDepths_) {
        if (depthParameter != nullptr && depthParameter->HasNonDefaultState()) {
            return true;
        }
    }
    return false;
}

GestureMask Parameter::GesturesAffectingMask() const {
    const SceneState& scene = group_.Manager().Scene();
    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
    const bool leftSceneValid = scene.leftScene < group_.Config().numScenes;
    const bool rightSceneValid = scene.rightScene < group_.Config().numScenes;
    if (blend <= 0.0f) {
        return leftSceneValid ? gestureActiveMasks_[scene.leftScene] & GestureCountMask(group_.GestureCount()) : 0;
    }
    if (blend >= 1.0f) {
        return rightSceneValid ? gestureActiveMasks_[scene.rightScene] & GestureCountMask(group_.GestureCount()) : 0;
    }
    const GestureMask leftMask = leftSceneValid ? gestureActiveMasks_[scene.leftScene] : 0;
    const GestureMask rightMask = rightSceneValid ? gestureActiveMasks_[scene.rightScene] : 0;
    return (leftMask | rightMask) & GestureCountMask(group_.GestureCount());
}

void ParameterGroup::SelectGesture(std::size_t gestureIx) {
    manager_->SelectGesture(gestureIx);
}

void ParameterGroup::DeselectGesture(std::size_t gestureIx) {
    manager_->DeselectGesture(gestureIx);
}

bool ParameterGroup::GestureSelected(std::size_t gestureIx) const {
    return manager_->GestureSelected(gestureIx);
}

void ParameterGroup::SetGestureValue(std::size_t gestureIx, float value) {
    manager_->SetGestureValue(gestureIx, value);
}

float ParameterGroup::GestureValue(std::size_t gestureIx) const {
    return manager_->GestureValue(gestureIx);
}

void ParameterGroup::ClearGestureActiveFlagsForActiveSceneSelection(const SceneState& scene, std::size_t gestureIx) {
    if (gestureIx >= gestureCount_) {
        throw std::out_of_range("gesture index out of range");
    }
    if (scene.leftScene >= config_.numScenes || scene.rightScene >= config_.numScenes) {
        throw std::out_of_range("scene index out of range");
    }

    const float blend = std::clamp(scene.blend, 0.0f, 1.0f);
    auto clearParameter = [&](Parameter& parameter) {
        if (blend <= 0.0f) {
            parameter.SetGestureActive(scene.leftScene, gestureIx, false);
        } else if (blend >= 1.0f) {
            parameter.SetGestureActive(scene.rightScene, gestureIx, false);
        } else {
            parameter.SetGestureActive(scene.leftScene, gestureIx, false);
            if (scene.rightScene != scene.leftScene) {
                parameter.SetGestureActive(scene.rightScene, gestureIx, false);
            }
        }
    };

    for (const auto& parameter : parameters_) {
        clearParameter(*parameter);
    }
    for (const auto& batch : extraStorageBatches_) {
        for (const auto& parameter : batch->parameters) {
            clearParameter(*parameter);
        }
    }
}

Bank::Bank(ParameterManager* manager)
    : manager_(manager) {}

void Bank::AddMapping(PhysicalEncoderId encoderId, Parameter& parameter) {
    for (Cell& cell : topLevel_) {
        if (cell.encoderId == encoderId) {
            cell.parameter = &parameter;
            if (!ShowingModulation()) {
                visible_ = topLevel_;
            }
            return;
        }
    }

    topLevel_.push_back({.encoderId = encoderId, .parameter = &parameter});
    if (!ShowingModulation()) {
        visible_ = topLevel_;
    }
}

void Bank::RegisterParameters(std::span<Parameter* const> parameters, std::size_t offset) {
    if (slot_ == nullptr) {
        throw std::logic_error("bank registration requires an associated bank slot");
    }

    const std::span<const PhysicalEncoderId> layout = slot_->PhysicalEncoders();
    if (offset > layout.size() || parameters.size() > layout.size() - offset) {
        throw std::logic_error("bank registration exceeds slot capacity");
    }

    for (std::size_t parameterIx = 0; parameterIx < parameters.size(); ++parameterIx) {
        if (parameters[parameterIx] == nullptr) {
            throw std::logic_error("bank registration parameter must not be null");
        }
        for (std::size_t otherIx = parameterIx + 1; otherIx < parameters.size(); ++otherIx) {
            if (parameters[otherIx] == nullptr) {
                throw std::logic_error("bank registration parameter must not be null");
            }
            if (parameters[parameterIx]->Name() == parameters[otherIx]->Name()) {
                throw std::logic_error("duplicate visible parameter name in bank registration");
            }
        }
    }

    for (std::size_t slotIx = offset; slotIx < offset + parameters.size(); ++slotIx) {
        for (std::size_t otherIx = slotIx + 1; otherIx < offset + parameters.size(); ++otherIx) {
            if (layout[slotIx] == layout[otherIx]) {
                throw std::logic_error("duplicate physical slot in bank registration");
            }
        }
    }

    std::vector<Cell> next = topLevel_;
    for (std::size_t parameterIx = 0; parameterIx < parameters.size(); ++parameterIx) {
        const PhysicalEncoderId encoderId = layout[offset + parameterIx];
        bool replaced = false;
        for (Cell& cell : next) {
            if (cell.encoderId == encoderId) {
                cell.parameter = parameters[parameterIx];
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            next.push_back({.encoderId = encoderId, .parameter = parameters[parameterIx]});
        }
    }

    topLevel_ = std::move(next);
    if (!ShowingModulation()) {
        visible_ = topLevel_;
    }
}

std::size_t Bank::SlotCapacity() const {
    if (slot_ == nullptr) {
        throw std::logic_error("bank has no associated slot layout");
    }
    return slot_->PhysicalEncoders().size();
}

bool Bank::OwnsVisible(PhysicalEncoderId encoderId) const {
    return FindVisibleCell(encoderId) != nullptr;
}

void Bank::HandlePress(PhysicalEncoderId encoderId) {
    const std::vector<PhysicalEncoderId> layout = CompactPhysicalLayout();
    HandlePress(encoderId, layout);
}

void Bank::HandlePress(PhysicalEncoderId encoderId, std::span<const PhysicalEncoderId> physicalLayout) {
    Cell* cell = FindVisibleCell(encoderId);
    if (cell == nullptr) {
        return;
    }
    const Modifier modifier = manager_ == nullptr ? Modifier::None : manager_->GetCurrentModifier();
    if (modifier != Modifier::None) {
        if (cell->parameter != nullptr) {
            ApplyModifierToParameter(*cell->parameter, modifier, manager_->Scene());
            if (modifier == Modifier::Reset) {
                cell->parameter->Group().CollectNeutralLocalParameters();
            }
        }
        return;
    }
    if (ShowingModulation() && cell->parameter == selected_) {
        Deselect();
        return;
    }
    if (cell->parameter != nullptr) {
        OpenModulationView(*cell->parameter, physicalLayout);
    }
}

void Bank::HandleTick(PhysicalEncoderId encoderId, const SceneState& scene, float delta) {
    Cell* cell = FindVisibleCell(encoderId);
    if (cell == nullptr || cell->parameter == nullptr) {
        return;
    }
    cell->parameter->HandleIncDec(scene, delta);
}

void Bank::HandleSetAbsolute(PhysicalEncoderId encoderId, const SceneState& scene, float normalizedTarget) {
    Cell* cell = FindVisibleCell(encoderId);
    if (cell == nullptr || cell->parameter == nullptr) {
        return;
    }
    cell->parameter->HandleSetAbsolute(scene, normalizedTarget);
}

// Same shape as HandleSetAbsolute above, but resolves against topLevel_
// instead of visible_, so the write reaches the real parameter even when
// this bank's visible_ has been swapped out for a modulation-depth view.
void Bank::HandleSetAbsoluteOnTopLevel(PhysicalEncoderId encoderId, const SceneState& scene, float normalizedTarget) {
    Cell* cell = FindTopLevelCell(encoderId);
    if (cell == nullptr || cell->parameter == nullptr) {
        return;
    }
    cell->parameter->HandleSetAbsolute(scene, normalizedTarget);
}

void Bank::ApplyModifierToTopLevel(Modifier modifier, const SceneState& scene) {
    std::vector<Parameter*> visited;
    visited.reserve(topLevel_.size());
    std::vector<ParameterGroup*> affectedGroups;
    affectedGroups.reserve(topLevel_.size());
    for (Cell& cell : topLevel_) {
        if (cell.parameter == nullptr) {
            continue;
        }
        if (std::find(visited.begin(), visited.end(), cell.parameter) != visited.end()) {
            continue;
        }
        visited.push_back(cell.parameter);
        ApplyModifierToParameter(*cell.parameter, modifier, scene);
        if (modifier == Modifier::Reset &&
            std::find(affectedGroups.begin(), affectedGroups.end(), &cell.parameter->Group()) ==
                affectedGroups.end()) {
            affectedGroups.push_back(&cell.parameter->Group());
        }
    }
    for (ParameterGroup* group : affectedGroups) {
        group->CollectNeutralLocalParameters();
    }
}

void Bank::Deselect() {
    ParameterGroup* affectedGroup = selected_ == nullptr ? nullptr : &selected_->Group();
    if (selected_ != nullptr) {
        for (const Cell& cell : visible_) {
            if (cell.parameter != nullptr && cell.parameter != selected_) {
                cell.parameter->UnpinLocalForView();
            }
        }
        selected_->UnpinLocalForView();
    }
    visible_ = topLevel_;
    selected_ = nullptr;
    if (affectedGroup != nullptr) {
        affectedGroup->CollectNeutralLocalParameters();
    }
}

bool Bank::ShowingModulation() const {
    return selected_ != nullptr;
}

std::size_t Bank::VisibleMappingCount() const {
    return visible_.size();
}

Parameter* Bank::VisibleParameter(PhysicalEncoderId encoderId) const {
    const Cell* cell = FindVisibleCell(encoderId);
    return cell == nullptr ? nullptr : cell->parameter;
}

Bank::VisibleCell Bank::VisibleCellFor(PhysicalEncoderId encoderId) const {
    const Cell* cell = FindVisibleCell(encoderId);
    if (cell == nullptr) {
        return {};
    }
    return {
        .parameter = cell->parameter,
    };
}

Parameter* Bank::TargetParameter() const {
    return ShowingModulation() ? selected_ : nullptr;
}

GestureMask Bank::GesturesAffectingMask() const {
    GestureMask mask = 0;
    for (const Cell& cell : topLevel_) {
        if (cell.parameter != nullptr) {
            mask |= cell.parameter->GesturesAffectingMask();
        }
    }
    return mask;
}

void BankSlot::UIState::Configure(std::size_t newCellCapacity, std::size_t voiceCapacity,
                                  std::size_t modulatorColorCapacity, std::size_t gestureColorCapacity) {
    cellCapacity = newCellCapacity;
    cells = std::make_unique<Parameter::UIState[]>(cellCapacity);
    for (std::size_t cellIx = 0; cellIx < cellCapacity; ++cellIx) {
        cells[cellIx].Configure(voiceCapacity, modulatorColorCapacity, gestureColorCapacity);
    }
    connected.store(false, std::memory_order_relaxed);
    showingModulationView.store(false, std::memory_order_relaxed);
}

Bank::Cell* Bank::FindVisibleCell(PhysicalEncoderId encoderId) {
    for (Cell& cell : visible_) {
        if (cell.encoderId == encoderId) {
            return &cell;
        }
    }
    return nullptr;
}

const Bank::Cell* Bank::FindVisibleCell(PhysicalEncoderId encoderId) const {
    for (const Cell& cell : visible_) {
        if (cell.encoderId == encoderId) {
            return &cell;
        }
    }
    return nullptr;
}

Bank::Cell* Bank::FindTopLevelCell(PhysicalEncoderId encoderId) {
    for (Cell& cell : topLevel_) {
        if (cell.encoderId == encoderId) {
            return &cell;
        }
    }
    return nullptr;
}

void Bank::AssociateSlot(BankSlot& slot) {
    if (slot_ != nullptr && slot_ != &slot) {
        throw std::logic_error("bank is already associated with a different slot");
    }
    slot_ = &slot;
}

Parameter* Bank::EnsureModulationDepthParameter(Parameter& parameter, std::size_t modIx) {
    if (!parameter.Group().GetModulators().Metadata(modIx).connected) {
        return nullptr;
    }
    Parameter* depthParameter = parameter.ModulationDepthParameter(modIx);
    if (depthParameter != nullptr || manager_ == nullptr) {
        return depthParameter;
    }
    if (!parameter.Group().CanAllocate()) {
        return nullptr;
    }

    return parameter.EnsureModulationDepth(modIx);
}

bool Bank::CanOpenModulationView(const Parameter& parameter) const {
    return parameter.Group().AvailableParameterSlots() >= MissingModulationDepthCount(parameter);
}

std::size_t Bank::MissingModulationDepthCount(const Parameter& parameter) const {
    std::size_t missing = 0;
    for (std::size_t modIx = 0; modIx < parameter.Group().Config().numModulators; ++modIx) {
        if (parameter.Group().GetModulators().Metadata(modIx).connected &&
            parameter.ModulationDepthParameter(modIx) == nullptr) {
            ++missing;
        }
    }
    return missing;
}

void Bank::OpenModulationView(Parameter& parameter, std::span<const PhysicalEncoderId> physicalLayout) {
    if (physicalLayout.empty()) {
        throw std::logic_error("modulation view requires at least one physical position");
    }

    const std::size_t modulatorCount = parameter.Group().Config().numModulators;
    if (modulatorCount > physicalLayout.size() - 1) {
        throw std::logic_error("modulation view has more modulators than slot depth positions");
    }

    const std::size_t missing = MissingModulationDepthCount(parameter);
    const std::size_t available = parameter.Group().AvailableParameterSlots();
    if (available < missing) {
        parameter.Group().RequestParameterStorageBatch(missing - available);
        return;
    }

    if (selected_ != nullptr) {
        for (const Cell& cell : visible_) {
            if (cell.parameter != nullptr && cell.parameter != selected_) {
                cell.parameter->UnpinLocalForView();
            }
        }
        selected_->UnpinLocalForView();
    }

    selected_ = &parameter;
    selected_->PinLocalForView();
    visible_.clear();

    for (std::size_t cellIx = 0; cellIx < modulatorCount; ++cellIx) {
        Parameter* depthParameter = EnsureModulationDepthParameter(parameter, cellIx);
        if (depthParameter != nullptr) {
            depthParameter->PinLocalForView();
        }
        visible_.push_back({
            .encoderId = physicalLayout[cellIx],
            .parameter = depthParameter,
        });
    }

    visible_.push_back({
        .encoderId = physicalLayout.back(),
        .parameter = &parameter,
    });
    parameter.Group().RequestParameterStorageBatchIfLow();
}

void Bank::ApplyModifierToParameter(Parameter& parameter, Modifier modifier, const SceneState& scene) {
    if (manager_ == nullptr) {
        return;
    }

    switch (modifier) {
    case Modifier::None:
        break;
    case Modifier::Reset:
        parameter.RevertToDefault(scene);
        break;
    case Modifier::Random:
        parameter.RandomizeVisibleValue(scene, manager_->NextRandomValue());
        break;
    case Modifier::RandomMod:
        RandomizeModulationDepths(parameter, scene);
        break;
    }
}

void Bank::RandomizeModulationDepths(Parameter& parameter, const SceneState& scene) {
    if (manager_ == nullptr) {
        return;
    }

    const auto metadata = parameter.Group().GetModulators().Metadata();
    const std::size_t connectedCount = static_cast<std::size_t>(
        std::count_if(metadata.begin(), metadata.end(),
                      [](const ModulatorMetadata& source) { return source.connected; }));
    if (connectedCount == 0) {
        return;
    }

    while (manager_->NextRandomCoin() < 0.5f) {
        std::size_t ordinal = manager_->NextRandomIndex(connectedCount);
        std::size_t modIx = 0;
        for (; modIx < metadata.size(); ++modIx) {
            if (!metadata[modIx].connected) {
                continue;
            }
            if (ordinal == 0) {
                break;
            }
            --ordinal;
        }
        Parameter* depthParameter = EnsureModulationDepthParameter(parameter, modIx);
        if (depthParameter == nullptr) {
            return;
        }

        depthParameter->RandomizeVisibleValue(scene, manager_->NextRandomValue());
    }
}

std::vector<PhysicalEncoderId> Bank::CompactPhysicalLayout() const {
    std::vector<PhysicalEncoderId> layout;
    layout.reserve(topLevel_.size());
    for (const Cell& cell : topLevel_) {
        layout.push_back(cell.encoderId);
    }
    return layout;
}

void BankSlot::SelectBank(Bank* bank) {
    if (bank != nullptr) {
        bank->AssociateSlot(*this);
    }
    if (selectedBank_ != nullptr && selectedBank_ != bank) {
        selectedBank_->Deselect();
    }
    selectedBank_ = bank;
}

bool BankSlot::Owns(PhysicalEncoderId encoderId) const {
    return selectedBank_ != nullptr && OwnsPhysicalEncoder(encoderId) && selectedBank_->OwnsVisible(encoderId);
}

void BankSlot::AddPhysicalEncoder(PhysicalEncoderId encoderId) {
    if (OwnsPhysicalEncoder(encoderId)) {
        throw std::logic_error("duplicate physical encoder in bank slot");
    }
    processedAbsoluteEpochs_.push_back(0);
    try {
        physicalEncoders_.push_back(encoderId);
    } catch (...) {
        processedAbsoluteEpochs_.pop_back();
        throw;
    }
}

void BankSlot::HandlePress(PhysicalEncoderId encoderId) {
    if (Owns(encoderId)) {
        selectedBank_->HandlePress(encoderId, physicalEncoders_);
    }
}

void BankSlot::HandleTick(PhysicalEncoderId encoderId, const SceneState& scene, float delta) {
    if (Owns(encoderId)) {
        selectedBank_->HandleTick(encoderId, scene, delta);
    }
}

void BankSlot::HandleSetAbsolute(PhysicalEncoderId encoderId, const SceneState& scene, float normalizedTarget) {
    if (Owns(encoderId)) {
        selectedBank_->HandleSetAbsolute(encoderId, scene, normalizedTarget);
    }
}

bool BankSlot::ResolvePosition(std::size_t position, PhysicalEncoderId& encoderId) const {
    if (position >= physicalEncoders_.size()) {
        return false;
    }
    encoderId = physicalEncoders_[position];
    return true;
}

void BankSlot::RecordProcessedAbsoluteEpoch(std::size_t position, std::uint64_t epoch) noexcept {
    if (epoch == 0 || position >= processedAbsoluteEpochs_.size()) {
        return;
    }
    processedAbsoluteEpochs_[position] = std::max(processedAbsoluteEpochs_[position], epoch);
}

void BankSlot::PopulateUIState(UIState& state, const SceneState& scene) const {
    state.connected.store(selectedBank_ != nullptr, std::memory_order_relaxed);
    state.showingModulationView.store(selectedBank_ != nullptr && selectedBank_->ShowingModulation(),
                                      std::memory_order_relaxed);
    for (std::size_t cellIx = 0; cellIx < state.cellCapacity; ++cellIx) {
        const std::uint64_t processedEpoch =
            cellIx < processedAbsoluteEpochs_.size() ? processedAbsoluteEpochs_[cellIx] : 0;
        if (cellIx >= physicalEncoders_.size() || selectedBank_ == nullptr) {
            state.cells[cellIx].SetDisconnected(processedEpoch);
            continue;
        }
        const Bank::VisibleCell cell = selectedBank_->VisibleCellFor(physicalEncoders_[cellIx]);
        if (cell.parameter == nullptr) {
            state.cells[cellIx].SetDisconnected(processedEpoch);
            continue;
        }
        cell.parameter->PopulateUIState(state.cells[cellIx], scene, processedEpoch);
    }
}

bool BankSlot::OwnsPhysicalEncoder(PhysicalEncoderId encoderId) const {
    return std::find(physicalEncoders_.begin(), physicalEncoders_.end(), encoderId) != physicalEncoders_.end();
}

ParameterMessageOut ParameterMessageOut::ParameterStorageBatchNeeded(ParameterGroup& group,
                                                                     std::size_t minimumAdditionalParameters,
                                                                     std::size_t requestedParameters) {
    ParameterMessageOut message;
    message.type = Type::ParameterStorageBatchNeeded;
    message.group = &group;
    message.minimumAdditionalParameters = minimumAdditionalParameters;
    message.requestedParameters = requestedParameters;
    return message;
}

ParameterMessageOut ParameterMessageOut::AppAction(std::size_t appActionIx, float value) {
    ParameterMessageOut message;
    message.type = Type::AppAction;
    message.appActionIx = appActionIx;
    message.value = value;
    return message;
}

ParameterMessageOut ParameterMessageOut::AppEncoderPress(std::size_t slotIx, std::size_t position) {
    ParameterMessageOut message;
    message.type = Type::AppEncoderPress;
    message.slotIx = slotIx;
    message.position = position;
    return message;
}

ParameterMessageOutBus::ParameterMessageOutBus(std::size_t capacity)
    : queue_(capacity == 0 ? 1 : capacity) {}

bool ParameterMessageOutBus::Push(const ParameterMessageOut& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size >= queue_.size()) {
        return false;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    queue_[tail] = message;
    tail_.store((tail + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_add(1, std::memory_order_release);
    return true;
}

bool ParameterMessageOutBus::Pop(ParameterMessageOut& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size == 0) {
        return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    message = queue_[head];
    head_.store((head + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_sub(1, std::memory_order_release);
    return true;
}

bool ParameterManager::SetGestureCount(std::size_t count) {
    if (count > std::numeric_limits<GestureMask>::digits || !groups_.empty()) {
        return false;
    }
    gestures_ = Gestures(count);
    return true;
}

ParameterGroup& ParameterManager::CreateGroup(ParameterGroupConfig config) {
    auto group = std::make_unique<ParameterGroup>(std::move(config), *this, gestures_.NumGestures());
    ParameterGroup& result = *group;
    groups_.push_back(std::move(group));
    return result;
}

ParameterId ParameterManager::RegisterParameter(ParameterGroup& group, ParameterConfig config) {
    if (!OwnsGroup(group)) {
        throw std::logic_error("parameter group is not owned by this manager");
    }
    if (config.name.empty()) {
        throw std::logic_error("parameter name must not be empty");
    }
    config = ResolveParameterAppearance(std::move(config), group.Config().numVoices);
    if (std::find(parameterNames_.begin(), parameterNames_.end(), config.name) != parameterNames_.end()) {
        throw std::logic_error("duplicate parameter name");
    }
    if (!group.CanAllocate()) {
        throw std::length_error("parameter group capacity exhausted");
    }
    if (parameters_.size() >= static_cast<std::size_t>(kLocalParameterId)) {
        throw std::overflow_error("parameter ID space exhausted");
    }

    const ParameterId id = static_cast<ParameterId>(parameters_.size());
    const std::string name = config.name;
    Parameter& created = group.CreateLocalParameter(std::move(config), id);
    Parameter* result = &created;
    parameters_.push_back(result);
    parameterNames_.push_back(name);
    group.RegisterTopLevelParameter(created);
    return id;
}

Parameter& ParameterManager::CreateParameter(ParameterGroup& group, ParameterConfig config) {
    return ParameterById(RegisterParameter(group, std::move(config)));
}

Parameter& ParameterManager::ParameterById(ParameterId id) {
    return *parameters_.at(static_cast<std::size_t>(id));
}

const Parameter& ParameterManager::ParameterById(ParameterId id) const {
    return *parameters_.at(static_cast<std::size_t>(id));
}

Parameter* ParameterManager::FindParameterByName(std::string_view name) {
    for (Parameter* parameter : parameters_) {
        if (parameter != nullptr && parameter->Name() == name) {
            return parameter;
        }
    }
    return nullptr;
}

const Parameter* ParameterManager::FindParameterByName(std::string_view name) const {
    for (const Parameter* parameter : parameters_) {
        if (parameter != nullptr && parameter->Name() == name) {
            return parameter;
        }
    }
    return nullptr;
}

JSON ParameterManager::ParameterValuesToJSON(JsonArena& arena) const {
    JSON root = arena.Object();
    for (const Parameter* parameter : parameters_) {
        if (parameter == nullptr) {
            continue;
        }
        root.SetNew(parameter->Name().c_str(), parameter->ToValueJSON(arena));
    }
    return root;
}

bool ParameterManager::LoadParameterValuesFromJSON(JSON json) {
    if (!IsJsonObject(json)) {
        return false;
    }

    for (Parameter* parameter : parameters_) {
        if (parameter != nullptr) {
            parameter->RevertAllToDefault();
        }
    }

    const JsonMember* members = JsonObjectMembers(json);
    for (std::size_t ix = 0; ix < json.Size(); ++ix) {
        if (members[ix].m_key == nullptr) {
            continue;
        }
        Parameter* parameter = FindParameterByName(members[ix].m_key);
        if (parameter == nullptr) {
            continue;
        }
        parameter->LoadValuesFromJSON(JSON(members[ix].m_value));
    }

    ComputeAllParameters();
    return true;
}

std::size_t ParameterManager::CollectNeutralLocalParameters() {
    std::size_t collected = 0;
    for (const auto& group : groups_) {
        collected += group->CollectNeutralLocalParameters();
    }
    return collected;
}

void ParameterManager::ComputeAllParameters() {
    for (Parameter* parameter : parameters_) {
        if (parameter == nullptr) {
            continue;
        }
        parameter->ComputeAtDepth(scene_, 0, false);
        parameter->SnapCurrentToTarget();
    }
}

void ParameterManager::ComputeAllTargets() {
    for (Parameter* parameter : parameters_) {
        if (parameter == nullptr) {
            continue;
        }
        parameter->Compute(scene_);
    }
}

void ParameterManager::CaptureDefaultControlState() {
    defaultControlState_.scene = scene_;
    defaultControlState_.resetHeld = resetHeld_;
    defaultControlState_.randomHeld = randomHeld_;
    defaultControlState_.randomModHeld = randomModHeld_;
    defaultControlState_.activePageOrdinal = activePageOrdinal_;
    defaultControlState_.gestureValues.clear();
    defaultControlState_.gestureSelected.clear();
    defaultControlState_.gestureValues.reserve(GestureCount());
    defaultControlState_.gestureSelected.reserve(GestureCount());
    for (std::size_t gestureIx = 0; gestureIx < GestureCount(); ++gestureIx) {
        defaultControlState_.gestureValues.push_back(GestureValue(gestureIx));
        defaultControlState_.gestureSelected.push_back(GestureSelected(gestureIx));
    }
}

void ParameterManager::RevertAllToDefaults() {
    for (Parameter* parameter : parameters_) {
        if (parameter != nullptr) {
            parameter->RevertAllToDefault();
        }
    }

    if (SceneEndpointsValid(defaultControlState_.scene.leftScene, defaultControlState_.scene.rightScene)) {
        scene_ = defaultControlState_.scene;
    } else {
        scene_ = {};
    }
    scene_.blend = std::clamp(scene_.blend, 0.0f, 1.0f);
    resetHeld_ = defaultControlState_.resetHeld;
    randomHeld_ = defaultControlState_.randomHeld;
    randomModHeld_ = defaultControlState_.randomModHeld;
    activePageOrdinal_ = defaultControlState_.activePageOrdinal;
    if (activePageOrdinal_.has_value() && FindPage(*activePageOrdinal_) == nullptr) {
        activePageOrdinal_.reset();
    }

    for (std::size_t gestureIx = 0; gestureIx < GestureCount(); ++gestureIx) {
        const float value = gestureIx < defaultControlState_.gestureValues.size()
                                ? defaultControlState_.gestureValues[gestureIx]
                                : 0.0f;
        SetGestureValue(gestureIx, value);
        const bool selected = gestureIx < defaultControlState_.gestureSelected.size() &&
                              defaultControlState_.gestureSelected[gestureIx];
        if (selected) {
            SelectGesture(gestureIx);
        } else {
            DeselectGesture(gestureIx);
        }
    }

    ComputeAllParameters();
    CollectNeutralLocalParameters();
}

float ParameterManager::GetLinear(float minValue, float maxValue, std::size_t voiceIx, ParameterId id) const {
    const float normalized = std::clamp(ParameterById(id).CachedKnobValue(voiceIx), 0.0f, 1.0f);
    return LinearMap(minValue, maxValue, normalized);
}

float ParameterManager::GetExponential(float minValue, float maxValue, std::size_t voiceIx, ParameterId id) const {
    const float normalized = std::clamp(ParameterById(id).CachedKnobValue(voiceIx), 0.0f, 1.0f);
    return ExponentialMap(minValue, maxValue, normalized);
}

float ParameterManager::GetZeroBasedExponential(float maxValue, float midpointValue, std::size_t voiceIx,
                                                ParameterId id) const {
    const float normalized = std::clamp(ParameterById(id).CachedKnobValue(voiceIx), 0.0f, 1.0f);
    return ZeroBasedExponentialMapFromMidpoint(maxValue, midpointValue, normalized);
}

float ParameterManager::GetBipolarLinear(float maxAbsValue, std::size_t voiceIx, ParameterId id) const {
    if (maxAbsValue < 0.0f) {
        throw std::invalid_argument("bipolar linear maximum must be non-negative");
    }
    const Parameter& parameter = ParameterById(id);
    if (parameter.Range() != RangeKind::Bipolar) {
        throw std::invalid_argument("bipolar mapping requires a bipolar parameter");
    }
    const float bipolar = ToBipolarPresentation(parameter.CachedKnobValue(voiceIx));
    return bipolar * maxAbsValue;
}

float ParameterManager::GetBipolarExponential(float leftValue, float centerValue, float rightValue,
                                              std::size_t voiceIx, ParameterId id) const {
    if (!(leftValue > 0.0f) || !(centerValue > 0.0f) || !(rightValue > 0.0f)) {
        throw std::invalid_argument("centered bipolar exponential values must be positive");
    }
    const Parameter& parameter = ParameterById(id);
    if (parameter.Range() != RangeKind::Bipolar) {
        throw std::invalid_argument("bipolar mapping requires a bipolar parameter");
    }
    const float knob = ToBipolarPresentation(parameter.CachedKnobValue(voiceIx));
    if (knob < 0.0f) {
        return centerValue * std::pow(leftValue / centerValue, -knob);
    }
    return centerValue * std::pow(rightValue / centerValue, knob);
}

float ParameterManager::GetBipolarZeroBasedExponential(float maxAbsValue, float midpointAbsValue,
                                                       std::size_t voiceIx, ParameterId id) const {
    if (!(maxAbsValue > 0.0f) || !(midpointAbsValue > 0.0f) || midpointAbsValue >= maxAbsValue) {
        throw std::invalid_argument("zero-based exponential mapping requires 0 < midpoint < max");
    }
    const Parameter& parameter = ParameterById(id);
    if (parameter.Range() != RangeKind::Bipolar) {
        throw std::invalid_argument("bipolar mapping requires a bipolar parameter");
    }
    const float bipolar = ToBipolarPresentation(parameter.CachedKnobValue(voiceIx));
    if (bipolar == 0.0f) {
        return 0.0f;
    }
    const float magnitude = ZeroBasedExponentialMapFromMidpoint(maxAbsValue, midpointAbsValue, std::fabs(bipolar));
    return std::copysign(magnitude, bipolar);
}

void ParameterManager::UpdateModValues(ParameterGroup& group) {
    if (!OwnsGroup(group)) {
        throw std::logic_error("parameter group is not owned by this manager");
    }
    group.UpdateModValues();
}

void ParameterManager::UpdateModValues() {
    for (const std::unique_ptr<ParameterGroup>& group : groups_) {
        group->UpdateModValues();
    }
}

bool ParameterManager::SceneEndpointsValid(std::size_t leftScene, std::size_t rightScene) const {
    for (const auto& group : groups_) {
        if (leftScene >= group->Config().numScenes || rightScene >= group->Config().numScenes) {
            return false;
        }
    }
    return true;
}

bool ParameterManager::OwnsGroup(const ParameterGroup& group) const {
    return std::any_of(groups_.begin(), groups_.end(),
                       [&group](const std::unique_ptr<ParameterGroup>& owned) { return owned.get() == &group; });
}

bool ParameterManager::SetSceneEndpoints(std::size_t leftScene, std::size_t rightScene) {
    if (!SceneEndpointsValid(leftScene, rightScene)) {
        return false;
    }
    scene_.leftScene = leftScene;
    scene_.rightScene = rightScene;
    return true;
}

bool ParameterManager::SetLessSelectedScene(std::size_t sceneIx) {
    const float blend = std::clamp(scene_.blend, 0.0f, 1.0f);
    if (blend <= 0.5f) {
        return SetSceneEndpoints(scene_.leftScene, sceneIx);
    }
    return SetSceneEndpoints(sceneIx, scene_.rightScene);
}

void ParameterManager::SetSceneBlend(float blend) {
    scene_.blend = std::clamp(blend, 0.0f, 1.0f);
}

Page& ParameterManager::CreatePage(std::string name) {
    auto page = std::make_unique<Page>();
    page->ordinal = static_cast<PageOrdinal>(pages_.size());
    page->name = std::move(name);
    Page& result = *page;
    pages_.push_back(std::move(page));
    if (!activePageOrdinal_.has_value()) {
        activePageOrdinal_ = result.ordinal;
    }
    return result;
}

bool ParameterManager::AssignParameterToPage(PageOrdinal ordinal, Parameter& parameter) {
    Page* page = FindPage(ordinal);
    if (page == nullptr) {
        return false;
    }
    if (std::find(page->parameters.begin(), page->parameters.end(), &parameter) == page->parameters.end()) {
        page->parameters.push_back(&parameter);
    }
    return true;
}

bool ParameterManager::SelectActivePage(PageOrdinal ordinal) {
    if (FindPage(ordinal) == nullptr) {
        return false;
    }
    activePageOrdinal_ = ordinal;
    return true;
}

void ParameterManager::SetActivePage(PageOrdinal ordinal) {
    if (!SelectActivePage(ordinal)) {
        throw std::out_of_range("page ordinal out of range");
    }
}

Page* ParameterManager::ActivePage() {
    if (!activePageOrdinal_.has_value()) {
        return nullptr;
    }
    return FindPage(*activePageOrdinal_);
}

const Page* ParameterManager::ActivePage() const {
    if (!activePageOrdinal_.has_value()) {
        return nullptr;
    }
    return FindPage(*activePageOrdinal_);
}

std::optional<PageOrdinal> ParameterManager::ActivePageOrdinal() const {
    return activePageOrdinal_;
}

Bank& ParameterManager::CreateBank() {
    auto bank = std::make_unique<Bank>(this);
    Bank& result = *bank;
    banks_.push_back(std::move(bank));
    return result;
}

Bank* ParameterManager::BankAt(std::size_t bankIx) {
    return bankIx >= banks_.size() ? nullptr : banks_[bankIx].get();
}

const Bank* ParameterManager::BankAt(std::size_t bankIx) const {
    return bankIx >= banks_.size() ? nullptr : banks_[bankIx].get();
}

BankSlot& ParameterManager::CreateBankSlot() {
    auto slot = std::make_unique<BankSlot>();
    BankSlot& result = *slot;
    slots_.push_back(std::move(slot));
    return result;
}

BankSlot* ParameterManager::BankSlotAt(std::size_t slotIx) {
    return slotIx >= slots_.size() ? nullptr : slots_[slotIx].get();
}

const BankSlot* ParameterManager::BankSlotAt(std::size_t slotIx) const {
    return slotIx >= slots_.size() ? nullptr : slots_[slotIx].get();
}

void ParameterManager::HandlePress(PhysicalEncoderId encoderId) {
    for (const auto& slot : slots_) {
        if (slot->Owns(encoderId)) {
            slot->HandlePress(encoderId);
            return;
        }
    }
}

void ParameterManager::HandleTick(PhysicalEncoderId encoderId, float delta) {
    for (const auto& slot : slots_) {
        if (slot->Owns(encoderId)) {
            slot->HandleTick(encoderId, scene_, delta);
            return;
        }
    }
}

// Legacy physical-ID callers have no controller route/slot-position epoch.
// They remain intentionally untracked, equivalent to absolute epoch zero.
void ParameterManager::HandleSetAbsolute(PhysicalEncoderId encoderId, float normalizedTarget) {
    for (const auto& slot : slots_) {
        if (slot->Owns(encoderId)) {
            slot->HandleSetAbsolute(encoderId, scene_, normalizedTarget);
            return;
        }
    }
}

void ParameterManager::HandlePress(std::size_t slotIx, std::size_t position) {
    BankSlot* slot = BankSlotAt(slotIx);
    PhysicalEncoderId encoderId = 0;
    if (slot != nullptr && slot->ResolvePosition(position, encoderId)) {
        slot->HandlePress(encoderId);
    }
}

void ParameterManager::HandleTick(std::size_t slotIx, std::size_t position, float delta) {
    BankSlot* slot = BankSlotAt(slotIx);
    PhysicalEncoderId encoderId = 0;
    if (slot != nullptr && slot->ResolvePosition(position, encoderId)) {
        slot->HandleTick(encoderId, scene_, delta);
    }
}

void ParameterManager::HandleSetAbsolute(std::size_t slotIx, std::size_t position, float normalizedTarget,
                                         std::uint64_t absoluteEpoch) {
    BankSlot* slot = BankSlotAt(slotIx);
    PhysicalEncoderId encoderId = 0;
    if (slot != nullptr && slot->ResolvePosition(position, encoderId)) {
        if (GetCurrentModifier() == Modifier::None) {
            slot->HandleSetAbsolute(encoderId, scene_, normalizedTarget);
        }
        slot->RecordProcessedAbsoluteEpoch(position, absoluteEpoch);
    }
}

// Writes to bankIx directly instead of through slotIx's own bank selection:
// no modifier gate (this write did not come from an operator's encoder, so
// a held modifier must not silently swallow it, unlike HandleSetAbsolute
// above), and no call into BankSlot::SelectBank/Owns/selectedBank_, so the
// slot's current selection is left exactly as it was. slotIx is consulted
// only for its encoder layout (to resolve position) and its epoch
// bookkeeping, recorded the same way the slot-addressed path records it.
void ParameterManager::HandleSetAbsoluteOnBank(std::size_t bankIx, std::size_t slotIx, std::size_t position,
                                               float normalizedTarget, std::uint64_t absoluteEpoch) {
    BankSlot* slot = BankSlotAt(slotIx);
    Bank* bank = BankAt(bankIx);
    if (slot == nullptr || bank == nullptr) {
        return;
    }
    PhysicalEncoderId encoderId = 0;
    if (slot->ResolvePosition(position, encoderId)) {
        bank->HandleSetAbsoluteOnTopLevel(encoderId, scene_, normalizedTarget);
        slot->RecordProcessedAbsoluteEpoch(position, absoluteEpoch);
    }
}

bool ParameterManager::SelectBankForSlot(std::size_t slotIx, std::size_t bankIx) {
    BankSlot* slot = BankSlotAt(slotIx);
    Bank* bank = BankAt(bankIx);
    if (slot == nullptr || bank == nullptr) {
        return false;
    }
    const Modifier modifier = GetCurrentModifier();
    if (modifier != Modifier::None) {
        bank->ApplyModifierToTopLevel(modifier, scene_);
        return true;
    }
    slot->SelectBank(bank);
    return true;
}

bool ParameterManager::NavigateBankForSlot(std::size_t slotIx, BankDirection direction) {
    BankSlot* slot = BankSlotAt(slotIx);
    if (slot == nullptr || banks_.empty() || slot->SelectedBank() == nullptr) {
        return false;
    }

    const auto current = std::find_if(
        banks_.begin(), banks_.end(),
        [selected = slot->SelectedBank()](const std::unique_ptr<Bank>& bank) {
            return bank.get() == selected;
        });
    if (current == banks_.end()) {
        return false;
    }

    const Modifier modifier = GetCurrentModifier();
    if (modifier != Modifier::None) {
        (*current)->ApplyModifierToTopLevel(modifier, scene_);
        return true;
    }

    const std::size_t currentIx = static_cast<std::size_t>(std::distance(banks_.begin(), current));
    const std::size_t nextIx = direction == BankDirection::Next
        ? (currentIx + 1 == banks_.size() ? 0 : currentIx + 1)
        : (currentIx == 0 ? banks_.size() - 1 : currentIx - 1);
    slot->SelectBank(banks_[nextIx].get());
    return true;
}

Modifier ParameterManager::GetCurrentModifier() const {
    if (randomModHeld_) {
        return Modifier::RandomMod;
    }
    if (randomHeld_) {
        return Modifier::Random;
    }
    if (resetHeld_) {
        return Modifier::Reset;
    }
    return Modifier::None;
}

void ParameterManager::SetRandomSource(ParameterRandomFloat valueSource, ParameterRandomFloat coinSource,
                                       ParameterRandomIndex indexSource) {
    randomValueSource_ = std::move(valueSource);
    randomCoinSource_ = std::move(coinSource);
    randomIndexSource_ = std::move(indexSource);
}

float ParameterManager::NextRandomValue() {
    if (randomValueSource_) {
        return std::clamp(randomValueSource_(), 0.0f, 1.0f);
    }
    return std::generate_canonical<float, 24>(randomEngine_);
}

float ParameterManager::NextRandomCoin() {
    if (randomCoinSource_) {
        return randomCoinSource_();
    }
    return std::generate_canonical<float, 24>(randomEngine_);
}

std::size_t ParameterManager::NextRandomIndex(std::size_t exclusiveMax) {
    if (exclusiveMax == 0) {
        return 0;
    }
    if (randomIndexSource_) {
        return randomIndexSource_(exclusiveMax) % exclusiveMax;
    }
    std::uniform_int_distribution<std::size_t> dist(0, exclusiveMax - 1);
    return dist(randomEngine_);
}

void ParameterManager::SelectGesture(std::size_t gestureIx) {
    gestures_.Select(gestureIx, true);
}

void ParameterManager::DeselectGesture(std::size_t gestureIx) {
    gestures_.Select(gestureIx, false);
}

void ParameterManager::ToggleGestureSelected(std::size_t gestureIx) {
    gestures_.Select(gestureIx, !gestures_.Selected(gestureIx));
}

bool ParameterManager::GestureSelected(std::size_t gestureIx) const {
    return gestures_.Selected(gestureIx);
}

void ParameterManager::SetGestureValue(std::size_t gestureIx, float value) {
    gestures_.Value(gestureIx) = std::clamp(value, 0.0f, 1.0f);
}

float ParameterManager::GestureValue(std::size_t gestureIx) const {
    return gestures_.Value(gestureIx);
}

GestureMetadata& ParameterManager::GestureMetadataAt(std::size_t gestureIx) {
    return gestures_.Metadata(gestureIx);
}

const GestureMetadata& ParameterManager::GestureMetadataAt(std::size_t gestureIx) const {
    return gestures_.Metadata(gestureIx);
}

void ParameterManager::ClearGestureActiveFlagsForActiveSceneSelection(std::size_t gestureIx) {
    for (const auto& group : groups_) {
        group->ClearGestureActiveFlagsForActiveSceneSelection(scene_, gestureIx);
    }
}

std::size_t ParameterManager::MaxVoiceCount() const {
    std::size_t result = 0;
    for (const auto& group : groups_) {
        result = std::max(result, group->Config().numVoices);
    }
    return result;
}

std::size_t ParameterManager::MaxModulatorCount() const {
    std::size_t result = 0;
    for (const auto& group : groups_) {
        result = std::max(result, group->Config().numModulators);
    }
    return result;
}

std::size_t ParameterManager::SceneCapacity() const {
    if (groups_.empty()) {
        return 0;
    }
    std::size_t result = groups_.front()->Config().numScenes;
    for (const auto& group : groups_) {
        result = std::min(result, group->Config().numScenes);
    }
    return result;
}

std::size_t ParameterManager::MaxSlotCellCount() const {
    std::size_t result = 0;
    for (const auto& slot : slots_) {
        result = std::max(result, slot->PhysicalEncoders().size());
    }
    return result;
}

void ParameterManager::GestureManagerUIState::Configure(std::size_t newGestureCapacity) {
    gestureCapacity = newGestureCapacity;
    values = std::make_unique<std::atomic<float>[]>(gestureCapacity);
    selected = std::make_unique<std::atomic<bool>[]>(gestureCapacity);
    colors = std::make_unique<AtomicColor[]>(gestureCapacity);
    connected = std::make_unique<std::atomic<bool>[]>(gestureCapacity);
    bankAffectingMask = std::make_unique<std::atomic<std::uint32_t>[]>(gestureCapacity);
    bankAffectingCount = std::make_unique<std::atomic<std::size_t>[]>(gestureCapacity);
    for (std::size_t gestureIx = 0; gestureIx < gestureCapacity; ++gestureIx) {
        values[gestureIx].store(0.0f, std::memory_order_relaxed);
        selected[gestureIx].store(false, std::memory_order_relaxed);
        colors[gestureIx].Store(Color::Off);
        connected[gestureIx].store(false, std::memory_order_relaxed);
        bankAffectingMask[gestureIx].store(0, std::memory_order_relaxed);
        bankAffectingCount[gestureIx].store(0, std::memory_order_relaxed);
    }
}

void ParameterManager::UIState::Configure(std::size_t newSlotCapacity, std::size_t cellCapacity,
                                          std::size_t voiceCapacity, std::size_t modulatorColorCapacity,
                                          std::size_t gestureCapacity,
                                          std::size_t newBankCapacity) {
    slotCapacity = newSlotCapacity;
    slots = std::make_unique<BankSlot::UIState[]>(slotCapacity);
    for (std::size_t slotIx = 0; slotIx < slotCapacity; ++slotIx) {
        slots[slotIx].Configure(cellCapacity, voiceCapacity, modulatorColorCapacity, gestureCapacity);
    }
    bankCapacity = newBankCapacity;
    banks = std::make_unique<BankUIState[]>(bankCapacity);
    for (std::size_t bankIx = 0; bankIx < bankCapacity; ++bankIx) {
        banks[bankIx].connected.store(false, std::memory_order_relaxed);
        banks[bankIx].selected.store(false, std::memory_order_relaxed);
        banks[bankIx].bankColor.Store(Color::Off);
    }
    gestures.Configure(gestureCapacity);
}

std::unique_ptr<ParameterManager::UIState> ParameterManager::CreateUIState() const {
    auto state = std::make_unique<UIState>();
    state->Configure(slots_.size(), MaxSlotCellCount(), MaxVoiceCount(), MaxModulatorCount(),
                     gestures_.NumGestures(), banks_.size());
    state->sceneCapacity = SceneCapacity();
    return state;
}

void ParameterManager::PopulateUIState(UIState& state) const {
    state.leftScene.store(scene_.leftScene, std::memory_order_relaxed);
    state.rightScene.store(scene_.rightScene, std::memory_order_relaxed);
    state.sceneBlend.store(scene_.blend, std::memory_order_relaxed);
    state.resetHeld.store(resetHeld_, std::memory_order_relaxed);
    state.randomHeld.store(randomHeld_, std::memory_order_relaxed);
    state.randomModHeld.store(randomModHeld_, std::memory_order_relaxed);
    state.sceneCapacity = SceneCapacity();
    for (std::size_t slotIx = 0; slotIx < state.slotCapacity; ++slotIx) {
        if (slotIx < slots_.size()) {
            slots_[slotIx]->PopulateUIState(state.slots[slotIx], scene_);
        } else {
            state.slots[slotIx].connected.store(false, std::memory_order_relaxed);
            state.slots[slotIx].showingModulationView.store(false, std::memory_order_relaxed);
            for (std::size_t cellIx = 0; cellIx < state.slots[slotIx].cellCapacity; ++cellIx) {
                state.slots[slotIx].cells[cellIx].SetDisconnected();
            }
        }
    }
    for (std::size_t bankIx = 0; bankIx < state.bankCapacity; ++bankIx) {
        const bool connected = bankIx < banks_.size();
        state.banks[bankIx].connected.store(connected, std::memory_order_relaxed);
        state.banks[bankIx].selected.store(false, std::memory_order_relaxed);
        state.banks[bankIx].bankColor.Store(connected ? banks_[bankIx]->BankColor() : Color::Off);
    }
    for (const auto& slot : slots_) {
        Bank* selectedBank = slot->SelectedBank();
        if (selectedBank == nullptr) {
            continue;
        }
        for (std::size_t bankIx = 0; bankIx < std::min(state.bankCapacity, banks_.size()); ++bankIx) {
            if (banks_[bankIx].get() == selectedBank) {
                state.banks[bankIx].selected.store(true, std::memory_order_relaxed);
                break;
            }
        }
    }
    for (std::size_t gestureIx = 0; gestureIx < state.gestures.gestureCapacity; ++gestureIx) {
        const bool connected = gestureIx < gestures_.NumGestures();
        state.gestures.connected[gestureIx].store(connected, std::memory_order_relaxed);
        state.gestures.bankAffectingMask[gestureIx].store(0, std::memory_order_relaxed);
        state.gestures.bankAffectingCount[gestureIx].store(0, std::memory_order_relaxed);
        if (!connected) {
            state.gestures.values[gestureIx].store(0.0f, std::memory_order_relaxed);
            state.gestures.selected[gestureIx].store(false, std::memory_order_relaxed);
            state.gestures.colors[gestureIx].Store(Color::Off);
            continue;
        }
        state.gestures.values[gestureIx].store(gestures_.Value(gestureIx), std::memory_order_relaxed);
        state.gestures.selected[gestureIx].store(gestures_.Selected(gestureIx), std::memory_order_relaxed);
        state.gestures.colors[gestureIx].Store(gestures_.Metadata(gestureIx).gestureColor);
    }
    const std::size_t compactBankCount = std::min<std::size_t>({state.bankCapacity, banks_.size(), 32});
    for (std::size_t bankIx = 0; bankIx < compactBankCount; ++bankIx) {
        const GestureMask affecting = banks_[bankIx]->GesturesAffectingMask();
        for (std::size_t gestureIx = 0;
             gestureIx < std::min<std::size_t>(state.gestures.gestureCapacity, 64);
             ++gestureIx) {
            if ((affecting & (GestureMask{1} << gestureIx)) == 0) {
                continue;
            }
            std::uint32_t mask = state.gestures.bankAffectingMask[gestureIx].load(std::memory_order_relaxed);
            mask |= (std::uint32_t{1} << bankIx);
            state.gestures.bankAffectingMask[gestureIx].store(mask, std::memory_order_relaxed);
            state.gestures.bankAffectingCount[gestureIx].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

bool ParameterManager::RequestParameterStorageBatch(ParameterGroup& group, std::size_t minimumAdditionalParameters) {
    if (!OwnsGroup(group) || parameterMessageOutBus_ == nullptr || minimumAdditionalParameters == 0) {
        return false;
    }
    const std::size_t lowWatermark = group.Config().numModulators * 2;
    const std::size_t requested = std::max(minimumAdditionalParameters, lowWatermark);
    return parameterMessageOutBus_->Push(
        ParameterMessageOut::ParameterStorageBatchNeeded(group, minimumAdditionalParameters, requested));
}

MessageIn MessageIn::ParamIncDec(std::uint64_t timestamp, std::size_t slotIx, std::size_t position, float delta) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ParamIncDec;
    message.slotIx = slotIx;
    message.position = position;
    message.delta = delta;
    return message;
}

MessageIn MessageIn::ParamSetAbsolute(std::uint64_t timestamp, std::size_t slotIx, std::size_t position,
                                      float normalizedValue, std::uint64_t absoluteEpoch) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ParamSetAbsolute;
    message.slotIx = slotIx;
    message.position = position;
    message.value = normalizedValue;
    message.absoluteEpoch = absoluteEpoch;
    return message;
}

MessageIn MessageIn::ParamSetAbsoluteOnBank(std::uint64_t timestamp, std::size_t bankIx, std::size_t slotIx,
                                            std::size_t position, float normalizedValue,
                                            std::uint64_t absoluteEpoch) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ParamSetAbsoluteOnBank;
    message.bankIx = bankIx;
    message.slotIx = slotIx;
    message.position = position;
    message.value = normalizedValue;
    message.absoluteEpoch = absoluteEpoch;
    return message;
}

MessageIn MessageIn::ParamPush(std::uint64_t timestamp, std::size_t slotIx, std::size_t position) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ParamPush;
    message.slotIx = slotIx;
    message.position = position;
    return message;
}

MessageIn MessageIn::ToggleReset(std::uint64_t timestamp) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ToggleReset;
    return message;
}

MessageIn MessageIn::SetReset(std::uint64_t timestamp, bool held) {
    MessageIn message = ToggleReset(timestamp);
    message.boolValue = held;
    message.hasBoolValue = true;
    return message;
}

MessageIn MessageIn::ToggleRandom(std::uint64_t timestamp) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ToggleRandom;
    return message;
}

MessageIn MessageIn::SetRandom(std::uint64_t timestamp, bool held) {
    MessageIn message = ToggleRandom(timestamp);
    message.boolValue = held;
    message.hasBoolValue = true;
    return message;
}

MessageIn MessageIn::ToggleRandomMod(std::uint64_t timestamp) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ToggleRandomMod;
    return message;
}

MessageIn MessageIn::SetRandomMod(std::uint64_t timestamp, bool held) {
    MessageIn message = ToggleRandomMod(timestamp);
    message.boolValue = held;
    message.hasBoolValue = true;
    return message;
}

MessageIn MessageIn::ToggleGestureSelect(std::uint64_t timestamp, std::size_t gestureIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::ToggleGestureSelect;
    message.gestureIx = gestureIx;
    return message;
}

MessageIn MessageIn::SetGestureSelect(std::uint64_t timestamp, std::size_t gestureIx, bool selected) {
    MessageIn message = ToggleGestureSelect(timestamp, gestureIx);
    message.type = Type::SetGestureSelect;
    message.boolValue = selected;
    message.hasBoolValue = true;
    return message;
}

MessageIn MessageIn::SelectParamBank(std::uint64_t timestamp, std::size_t slotIx, std::size_t bankIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::SelectParamBank;
    message.slotIx = slotIx;
    message.bankIx = bankIx;
    return message;
}

MessageIn MessageIn::NextParamBank(std::uint64_t timestamp, std::size_t slotIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::NextParamBank;
    message.slotIx = slotIx;
    return message;
}

MessageIn MessageIn::PrevParamBank(std::uint64_t timestamp, std::size_t slotIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::PrevParamBank;
    message.slotIx = slotIx;
    return message;
}

MessageIn MessageIn::Start(std::uint64_t timestamp, Origin origin, std::size_t externalControllerSlot) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::Start;
    message.origin = origin;
    message.externalControllerSlot = externalControllerSlot;
    return message;
}

MessageIn MessageIn::Continue(std::uint64_t timestamp, Origin origin, std::size_t externalControllerSlot) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::Continue;
    message.origin = origin;
    message.externalControllerSlot = externalControllerSlot;
    return message;
}

MessageIn MessageIn::Stop(std::uint64_t timestamp, Origin origin, std::size_t externalControllerSlot) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::Stop;
    message.origin = origin;
    message.externalControllerSlot = externalControllerSlot;
    return message;
}

MessageIn MessageIn::Clock(std::uint64_t timestamp, Origin origin, std::size_t externalControllerSlot) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::Clock;
    message.origin = origin;
    message.externalControllerSlot = externalControllerSlot;
    return message;
}

MessageIn MessageIn::SetGestureValue(std::uint64_t timestamp, std::size_t gestureIx, float value) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::SetGestureValue;
    message.gestureIx = gestureIx;
    message.value = value;
    return message;
}

MessageIn MessageIn::SceneSelect(std::uint64_t timestamp, std::size_t sceneIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::SceneSelect;
    message.sceneIx = sceneIx;
    return message;
}

MessageIn MessageIn::SetSceneBlend(std::uint64_t timestamp, float blend) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::SetSceneBlend;
    message.value = blend;
    return message;
}

MessageIn MessageIn::GridPress(std::uint64_t timestamp, std::size_t gridSlotIx,
                               int gridX, int gridY, std::uint8_t velocity) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::GridPress;
    message.gridSlotIx = gridSlotIx;
    message.gridX = gridX;
    message.gridY = gridY;
    message.velocity = velocity;
    return message;
}

MessageIn MessageIn::GridRelease(std::uint64_t timestamp, std::size_t gridSlotIx,
                                 int gridX, int gridY) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::GridRelease;
    message.gridSlotIx = gridSlotIx;
    message.gridX = gridX;
    message.gridY = gridY;
    return message;
}

MessageIn MessageIn::GridPressureChange(std::uint64_t timestamp, std::size_t gridSlotIx,
                                        int gridX, int gridY, std::uint8_t pressure) {
    MessageIn message = GridPress(timestamp, gridSlotIx, gridX, gridY, pressure);
    message.type = Type::GridPressureChange;
    return message;
}

MessageIn MessageIn::SelectGrid(std::uint64_t timestamp, std::size_t gridSlotIx,
                                std::size_t gridIx) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::SelectGrid;
    message.gridSlotIx = gridSlotIx;
    message.gridIx = gridIx;
    return message;
}

MessageIn MessageIn::AppAction(std::uint64_t timestamp, std::size_t appActionIx, float value) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::AppAction;
    message.appActionIx = appActionIx;
    message.value = value;
    return message;
}

MessageIn MessageIn::HoldDrill(std::uint64_t timestamp, bool held) {
    MessageIn message;
    message.timestamp = timestamp;
    message.type = Type::HoldDrill;
    message.boolValue = held;
    message.hasBoolValue = true;
    return message;
}

MessageInBus::MessageInBus(ParameterManager* manager, std::size_t capacity)
    : manager_(manager),
      queue_(capacity == 0 ? 1 : capacity) {}

bool MessageInBus::Push(const MessageIn& message) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size >= queue_.size()) {
        return false;
    }
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    queue_[tail] = message;
    tail_.store((tail + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_add(1, std::memory_order_release);
    return true;
}

bool MessageInBus::Pop(MessageIn& message, std::uint64_t timestamp) {
    const std::size_t size = size_.load(std::memory_order_acquire);
    if (size == 0) {
        return false;
    }
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (queue_[head].timestamp > timestamp) {
        return false;
    }
    message = queue_[head];
    head_.store((head + 1) % queue_.size(), std::memory_order_release);
    size_.fetch_sub(1, std::memory_order_release);
    return true;
}

void MessageInBus::Apply(const MessageIn& message) {
    switch (message.type) {
    case MessageIn::Type::ParamIncDec:
        if (manager_ != nullptr && manager_->GetCurrentModifier() == Modifier::None) {
            manager_->HandleTick(message.slotIx, message.position, message.delta);
        }
        break;
    case MessageIn::Type::ParamSetAbsolute:
        // Modifier gating and processed-epoch acknowledgement deliberately
        // occur downstream in the slot-position handler, after the final
        // apply-or-reject decision for this absolute event.
        manager_->HandleSetAbsolute(message.slotIx, message.position, message.value, message.absoluteEpoch);
        break;
    case MessageIn::Type::ParamSetAbsoluteOnBank:
        if (manager_ != nullptr) {
            manager_->HandleSetAbsoluteOnBank(message.bankIx, message.slotIx, message.position, message.value,
                                              message.absoluteEpoch);
        }
        break;
    case MessageIn::Type::ParamPush:
        if (appActionOut_ != nullptr && forwardEncoderPress_) {
            appActionOut_->Push(ParameterMessageOut::AppEncoderPress(message.slotIx, message.position));
        } else if (manager_ != nullptr) {
            manager_->HandlePress(message.slotIx, message.position);
        }
        break;
    case MessageIn::Type::ToggleReset:
        if (manager_ == nullptr) {
            break;
        } else if (message.hasBoolValue) {
            manager_->SetResetHeld(message.boolValue);
        } else {
            manager_->ToggleResetHeld();
        }
        break;
    case MessageIn::Type::ToggleRandom:
        if (manager_ == nullptr) {
            break;
        } else if (message.hasBoolValue) {
            manager_->SetRandomHeld(message.boolValue);
        } else {
            manager_->ToggleRandomHeld();
        }
        break;
    case MessageIn::Type::ToggleRandomMod:
        if (manager_ == nullptr) {
            break;
        } else if (message.hasBoolValue) {
            manager_->SetRandomModHeld(message.boolValue);
        } else {
            manager_->ToggleRandomModHeld();
        }
        break;
    case MessageIn::Type::ToggleGestureSelect:
        if (manager_ != nullptr && message.gestureIx < manager_->GestureCount()) {
            manager_->ToggleGestureSelected(message.gestureIx);
        }
        break;
    case MessageIn::Type::SetGestureSelect:
        if (manager_ != nullptr && message.gestureIx < manager_->GestureCount()) {
            if (message.boolValue) {
                manager_->SelectGesture(message.gestureIx);
            } else {
                manager_->DeselectGesture(message.gestureIx);
            }
        }
        break;
    case MessageIn::Type::SelectParamBank:
        if (manager_ != nullptr) {
            manager_->SelectBankForSlot(message.slotIx, message.bankIx);
        }
        break;
    case MessageIn::Type::NextParamBank:
        if (manager_ != nullptr) {
            manager_->NavigateBankForSlot(message.slotIx, BankDirection::Next);
        }
        break;
    case MessageIn::Type::PrevParamBank:
        if (manager_ != nullptr) {
            manager_->NavigateBankForSlot(message.slotIx, BankDirection::Previous);
        }
        break;
    case MessageIn::Type::SetGestureValue:
        if (manager_ != nullptr && message.gestureIx < manager_->GestureCount()) {
            manager_->SetGestureValue(message.gestureIx, message.value);
        }
        break;
    case MessageIn::Type::SceneSelect:
        if (manager_ != nullptr) {
            manager_->SetLessSelectedScene(message.sceneIx);
        }
        break;
    case MessageIn::Type::SetSceneBlend:
        if (manager_ != nullptr) {
            manager_->SetSceneBlend(message.value);
        }
        break;
    case MessageIn::Type::Start:
    case MessageIn::Type::Continue:
    case MessageIn::Type::Stop:
    case MessageIn::Type::Clock:
        break;
    case MessageIn::Type::GridPress:
        if (gridManager_ != nullptr) {
            gridManager_->HandlePress(message.gridSlotIx, message.gridX, message.gridY, message.velocity);
        }
        break;
    case MessageIn::Type::GridRelease:
        if (gridManager_ != nullptr) {
            gridManager_->HandleRelease(message.gridSlotIx, message.gridX, message.gridY);
        }
        break;
    case MessageIn::Type::GridPressureChange:
        if (gridManager_ != nullptr) {
            gridManager_->HandlePressureChange(message.gridSlotIx, message.gridX, message.gridY,
                                               message.velocity);
        }
        break;
    case MessageIn::Type::SelectGrid:
        if (gridManager_ != nullptr) {
            gridManager_->SelectGridForSlot(message.gridSlotIx, message.gridIx);
        }
        break;
    case MessageIn::Type::AppAction:
        if (appActionOut_ != nullptr) {
            appActionOut_->Push(ParameterMessageOut::AppAction(message.appActionIx, message.value));
        }
        break;
    case MessageIn::Type::HoldDrill:
        // Consumed by the MIDI processors before it reaches this bus.
        break;
    }
}

void MessageInBus::Process(std::uint64_t timestamp) {
    MessageIn message;
    while (Pop(message, timestamp)) {
        Apply(message);
    }
}

Page* ParameterManager::FindPage(PageOrdinal ordinal) {
    for (const auto& page : pages_) {
        if (page->ordinal == ordinal) {
            return page.get();
        }
    }
    return nullptr;
}

const Page* ParameterManager::FindPage(PageOrdinal ordinal) const {
    for (const auto& page : pages_) {
        if (page->ordinal == ordinal) {
            return page.get();
        }
    }
    return nullptr;
}

} // namespace synth
