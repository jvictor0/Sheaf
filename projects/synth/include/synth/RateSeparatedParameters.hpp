#pragma once

#include "synth/DspRateConversion.hpp"
#include "synth/ParameterModulation.hpp"
#include <vector>

namespace synth {

// Editable trees remain authoritative. Evaluate them once per control frame,
// including the application's phase-1/phase-2 seam. Root targets pass through
// four-phase FOH interpolation, the six-section low-pass, then normalized DSP
// publication. Configure off the audio thread after creating roots.
// Sources run at engine rate; causal low-pass decimation feeds matrix-frame evaluation.
class RateSeparatedParameters {
public:
    static constexpr std::size_t kFactor = 4;
    static constexpr std::size_t kRootSettleSamples = 4096;
    static constexpr float kRootSettleTolerance = 2e-5f;
    using SourceFilter = RateConversionLowPass;

    void Configure(ParameterGroup& group) {
        roots_.clear();
        rootValues_.clear();
        sources_.clear();
        const auto voices = group.Config().numVoices;
        for (auto* parameter : group.TopLevelParameters()) {
            for (std::size_t voice = 0; voice < voices; ++voice) {
                const float value = parameter->CachedKnobValue(voice);
                roots_.push_back({parameter, voice, value, value});
                rootValues_.push_back(value);
            }
        }
        rootFilters_.Configure(rootValues_);
        auto& modulators = group.GetModulators();
        group.UpdateModValues();
        for (std::size_t voice = 0; voice < voices; ++voice) {
            for (std::size_t mod = 0; mod < modulators.NumModulators(); ++mod) {
                sources_.emplace_back(voice, mod, modulators.Value(voice, mod));
            }
        }
        matrixFrames_ = 0;
        engineSamples_ = 0;
    }

    template<class Seam>
    void ProcessSample(ParameterGroup& group, std::uint64_t controlSample,
                       std::size_t phase, Seam&& seam, bool advanceSources = true) {
        auto& modulators = group.GetModulators();
        if (advanceSources) group.UpdateModValues();
        for (auto& source : sources_) {
            if (advanceSources)
                source.filtered = source.filter.Process(modulators.Value(source.voice, source.mod));
            modulators.Value(source.voice, source.mod) = source.filtered;
        }
        if (phase == 0) {
            group.ProcessSamplePhase1(controlSample);
            seam();
            group.ProcessSamplePhase2();
            for (auto& root : roots_) {
                root.previous = root.target;
                root.target = root.parameter->CachedKnobValue(root.voice);
                if (root.target != root.previous) root.unchangedTargetAge = 0;
            }
            ++matrixFrames_;
        }
        const float weight = static_cast<float>(phase + 1) / kFactor;
        // Full configured lane span, in parameter-major/voice-minor order.
        for (std::size_t i = 0; i < roots_.size(); ++i) {
            const auto& root = roots_[i];
            rootValues_[i] = root.previous + (root.target - root.previous) * weight;
        }
        rootFilters_.Process(rootValues_);
        for (std::size_t i = 0; i < roots_.size(); ++i) {
            auto& root = roots_[i];
            if (root.unchangedTargetAge < kRootSettleSamples) ++root.unchangedTargetAge;
            const float published = root.unchangedTargetAge >= kRootSettleSamples &&
                std::abs(double(rootValues_[i]) - double(root.target)) <= double(kRootSettleTolerance)
                ? root.target : rootValues_[i];
            root.parameter->ReplaceCachedKnobValue(root.voice, published);
        }
        ++engineSamples_;
    }

    std::uint64_t MatrixFrames() const { return matrixFrames_; }
    std::uint64_t EngineSamples() const { return engineSamples_; }

private:
    struct Root {
        Parameter* parameter;
        std::size_t voice;
        float previous;
        float target;
        std::size_t unchangedTargetAge = 0;
    };
    struct Source {
        std::size_t voice, mod;
        float filtered;
        SourceFilter filter;
        Source(std::size_t v, std::size_t m, float initial)
            : voice(v), mod(m), filtered(initial), filter(initial) {
        }
    };
    std::vector<Root> roots_;
    std::vector<float> rootValues_;
    RateConversionLowPassBatch rootFilters_;
    std::vector<Source> sources_;
    std::uint64_t matrixFrames_ = 0, engineSamples_ = 0;
};

} // namespace synth
