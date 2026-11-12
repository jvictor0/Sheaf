#pragma once

#include "synth/DspMath.hpp"
#include "synth/DspTransferFunction.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <numbers>

namespace synth {

struct OnePoleLowPass {
    static constexpr float kMaxCutoff = 0.499f;

    struct Input {
        float value = 0.0f;
        float cutoff = 0.0f;
    };

    struct UIState : TransferFunction {
        std::atomic<float> alpha{0.0f};

        float FrequencyResponse(float normalizedFrequency) const override {
            return std::abs(OnePoleLowPass::TransferFunctionValue(alpha.load(), normalizedFrequency));
        }

        std::complex<float> TransferFunctionValue(float normalizedFrequency) const override {
            return OnePoleLowPass::TransferFunctionValue(alpha.load(), normalizedFrequency);
        }
    };

    float m_alpha = 0.0f;
    float m_output = 0.0f;

    static float AlphaFromNatFreq(float cyclesPerSample) {
        cyclesPerSample = std::clamp(cyclesPerSample, 0.0f, kMaxCutoff);
        if (cyclesPerSample <= 0.0f) {
            return 0.0f;
        }
        return 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * cyclesPerSample);
    }

    static std::complex<float> TransferFunctionValue(float alpha, float normalizedFrequency) {
        const float cosOmega = DefaultDspMath::Cos2Pi(normalizedFrequency);
        const float sinOmega = DefaultDspMath::Sin2Pi(normalizedFrequency);
        const float a1 = 1.0f - alpha;
        const std::complex<float> numerator(alpha, 0.0f);
        const std::complex<float> denominator(1.0f - a1 * cosOmega, a1 * sinOmega);
        return numerator / denominator;
    }

    void SetAlphaFromNatFreq(float cyclesPerSample) {
        m_alpha = AlphaFromNatFreq(cyclesPerSample);
    }

    void Reset(float output = 0.0f) {
        m_output = output;
    }

    float ProcessWithAlpha(float value, float alpha) {
        m_alpha = std::clamp(alpha, 0.0f, 1.0f);
        m_output += m_alpha * (value - m_output);
        return m_output;
    }

    float Process(const Input& input) {
        return ProcessWithAlpha(input.value, AlphaFromNatFreq(input.cutoff));
    }

    void PopulateUIState(UIState& state) const {
        state.alpha.store(m_alpha);
    }
};

struct OnePoleHighPass {
    static constexpr float kMaxCutoff = 0.499f;

    struct Input {
        float value = 0.0f;
        float cutoff = 0.0f;
    };

    struct UIState : TransferFunction {
        std::atomic<float> alpha{0.0f};

        float FrequencyResponse(float normalizedFrequency) const override {
            return std::abs(OnePoleHighPass::TransferFunctionValue(alpha.load(), normalizedFrequency));
        }

        std::complex<float> TransferFunctionValue(float normalizedFrequency) const override {
            return OnePoleHighPass::TransferFunctionValue(alpha.load(), normalizedFrequency);
        }
    };

    float m_alpha = 0.0f;
    float m_output = 0.0f;
    float m_prevInput = 0.0f;

    static float AlphaFromNatFreq(float cyclesPerSample) {
        cyclesPerSample = std::clamp(cyclesPerSample, 0.0f, kMaxCutoff);
        if (cyclesPerSample <= 0.0f) {
            return 1.0f;
        }
        return std::exp(-2.0f * std::numbers::pi_v<float> * cyclesPerSample);
    }

    static std::complex<float> TransferFunctionValue(float alpha, float normalizedFrequency) {
        const float cosOmega = DefaultDspMath::Cos2Pi(normalizedFrequency);
        const float sinOmega = DefaultDspMath::Sin2Pi(normalizedFrequency);
        const std::complex<float> numerator(alpha * (1.0f - cosOmega), alpha * sinOmega);
        const std::complex<float> denominator(1.0f - alpha * cosOmega, alpha * sinOmega);
        return numerator / denominator;
    }

    void SetAlphaFromNatFreq(float cyclesPerSample) {
        m_alpha = AlphaFromNatFreq(cyclesPerSample);
    }

    float Process(const Input& input) {
        SetAlphaFromNatFreq(input.cutoff);
        m_output = m_alpha * (m_output + input.value - m_prevInput);
        m_prevInput = input.value;
        return m_output;
    }

    void PopulateUIState(UIState& state) const {
        state.alpha.store(m_alpha);
    }
};

struct ClassicStateVariableFilter {
    static constexpr float kMaxCutoff = 0.499f;

    struct Input {
        float value = 0.0f;
        float cutoff = 0.0f;
        float resonance = 1.0f;
        float blend = 0.0f;
    };

    struct UIState : TransferFunction {
        std::atomic<float> cutoff{0.0f};
        std::atomic<float> resonance{1.0f};
        std::atomic<float> blend{0.0f};

        float FrequencyResponse(float normalizedFrequency) const override {
            return std::abs(ClassicStateVariableFilter::TransferFunctionValue(
                cutoff.load(), resonance.load(), blend.load(), normalizedFrequency));
        }

        std::complex<float> TransferFunctionValue(float normalizedFrequency) const override {
            return ClassicStateVariableFilter::TransferFunctionValue(
                cutoff.load(), resonance.load(), blend.load(), normalizedFrequency);
        }
    };

    float m_low = 0.0f;
    float m_band = 0.0f;
    float m_high = 0.0f;
    float m_output = 0.0f;
    float m_cutoff = 0.0f;
    float m_resonance = 1.0f;
    float m_blend = 0.0f;

    static float ClampCutoff(float cutoff) {
        if (!std::isfinite(cutoff)) {
            return 0.0f;
        }
        return std::clamp(cutoff, 0.0f, kMaxCutoff);
    }

    static float ClampResonance(float resonance) {
        static constexpr float kMinResonance = 1.0e-4f;
        if (!std::isfinite(resonance)) {
            return 1.0f;
        }
        return std::max(resonance, kMinResonance);
    }

    static float ClampBlend(float blend) {
        if (!std::isfinite(blend)) {
            return 0.0f;
        }
        return std::clamp(blend, -1.0f, 1.0f);
    }

    static float LowBlendAmount(float blend) {
        return std::max(-ClampBlend(blend), 0.0f);
    }

    static float HighBlendAmount(float blend) {
        return std::max(ClampBlend(blend), 0.0f);
    }

    static float BandBlendAmount(float blend) {
        blend = ClampBlend(blend);
        return std::sqrt(std::max(0.0f, 1.0f - blend * blend));
    }

    static std::complex<float> TransferFunctionValue(
        float cutoff, float resonance, float blend, float normalizedFrequency) {
        cutoff = ClampCutoff(cutoff);
        resonance = ClampResonance(resonance);
        blend = ClampBlend(blend);
        normalizedFrequency = ClampCutoff(normalizedFrequency);

        const float lowAmount = LowBlendAmount(blend);
        const float highAmount = HighBlendAmount(blend);
        const float bandAmount = BandBlendAmount(blend);
        if (cutoff <= 0.0f) {
            return {highAmount, 0.0f};
        }
        if (normalizedFrequency <= 0.0f) {
            return {lowAmount, 0.0f};
        }

        const float damping = 1.0f / resonance;
        const float g = std::tan(std::numbers::pi_v<float> * cutoff);
        const float cosOmega = DefaultDspMath::Cos2Pi(normalizedFrequency);
        const float sinOmega = DefaultDspMath::Sin2Pi(normalizedFrequency);
        const std::complex<float> zInv(cosOmega, -sinOmega);
        const std::complex<float> integrator = g * (1.0f + zInv) / (1.0f - zInv);
        const std::complex<float> denominator = 1.0f + damping * integrator + integrator * integrator;
        const std::complex<float> low = integrator * integrator / denominator;
        const std::complex<float> band = integrator / denominator;
        const std::complex<float> high = 1.0f / denominator;
        return low * lowAmount + band * bandAmount + high * highAmount;
    }

    float Process(const Input& input) {
        m_cutoff = ClampCutoff(input.cutoff);
        m_resonance = ClampResonance(input.resonance);
        m_blend = ClampBlend(input.blend);

        const float g = std::tan(std::numbers::pi_v<float> * m_cutoff);
        const float damping = 1.0f / m_resonance;
        const float a1 = 1.0f / (1.0f + g * (g + damping));
        const float a2 = g * a1;
        const float a3 = g * a2;
        const float v3 = input.value - m_ic2eq;
        const float v1 = a1 * m_ic1eq + a2 * v3;
        const float v2 = m_ic2eq + a2 * m_ic1eq + a3 * v3;

        m_ic1eq = 2.0f * v1 - m_ic1eq;
        m_ic2eq = 2.0f * v2 - m_ic2eq;

        m_low = v2;
        m_band = v1;
        m_high = input.value - damping * v1 - v2;
        m_output = m_low * LowBlendAmount(m_blend)
            + m_band * BandBlendAmount(m_blend)
            + m_high * HighBlendAmount(m_blend);
        return m_output;
    }

    void PopulateUIState(UIState& state) const {
        state.cutoff.store(m_cutoff);
        state.resonance.store(m_resonance);
        state.blend.store(m_blend);
    }

private:
    float m_ic1eq = 0.0f;
    float m_ic2eq = 0.0f;
};

struct BiquadSection {
    static constexpr float kMinCutoff = 0.0001f;
    static constexpr float kMaxCutoff = 0.499f;
    static constexpr float kMinQ = 1.0e-6f;

    struct Input {
        float value = 0.0f;
    };

    float m_b0 = 1.0f;
    float m_b1 = 0.0f;
    float m_b2 = 0.0f;
    float m_a1 = 0.0f;
    float m_a2 = 0.0f;
    float m_x1 = 0.0f;
    float m_x2 = 0.0f;
    float m_y1 = 0.0f;
    float m_y2 = 0.0f;

    static float ClampCutoff(float cyclesPerSample) {
        if (!std::isfinite(cyclesPerSample)) {
            return kMinCutoff;
        }
        return std::clamp(cyclesPerSample, kMinCutoff, kMaxCutoff);
    }

    static float ClampQ(float q) {
        if (!std::isfinite(q)) {
            return 1.0f;
        }
        return std::max(q, kMinQ);
    }

    static float ClampFrequency(float cyclesPerSample) {
        if (!std::isfinite(cyclesPerSample)) {
            return 0.0f;
        }
        return std::clamp(cyclesPerSample, 0.0f, kMaxCutoff);
    }

    float Process(float input) {
        const float output = m_b0 * input + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;

        m_x2 = m_x1;
        m_x1 = input;
        m_y2 = m_y1;
        m_y1 = output;

        return output;
    }

    float Process(const Input& input) {
        return Process(input.value);
    }

    void Reset() {
        m_x1 = 0.0f;
        m_x2 = 0.0f;
        m_y1 = 0.0f;
        m_y2 = 0.0f;
    }

    void SetLowPassCoefficients(float cyclesPerSample, float q) {
        SetCoefficients(ClampCutoff(cyclesPerSample), ClampQ(q), false);
    }

    void SetHighPassCoefficients(float cyclesPerSample, float q) {
        SetCoefficients(ClampCutoff(cyclesPerSample), ClampQ(q), true);
    }

    static std::complex<float> TransferFunction(
        float cyclesPerSample,
        float q,
        float frequency,
        bool isHighPass = false) {
        BiquadSection section;
        if (isHighPass) {
            section.SetHighPassCoefficients(cyclesPerSample, q);
        } else {
            section.SetLowPassCoefficients(cyclesPerSample, q);
        }
        return section.TransferFunctionAt(frequency);
    }

private:
    void SetCoefficients(float cyclesPerSample, float q, bool isHighPass) {
        const float cosw = DefaultDspMath::Cos2Pi(cyclesPerSample);
        const float sinw = DefaultDspMath::Sin2Pi(cyclesPerSample);
        const float alpha = sinw / (2.0f * q);
        const float a0 = 1.0f + alpha;
        const float a2 = 1.0f - alpha;

        float b0 = 0.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        if (isHighPass) {
            b0 = (1.0f + cosw) * 0.5f;
            b1 = -(1.0f + cosw);
            b2 = (1.0f + cosw) * 0.5f;
        } else {
            b0 = (1.0f - cosw) * 0.5f;
            b1 = 1.0f - cosw;
            b2 = (1.0f - cosw) * 0.5f;
        }

        m_b0 = b0 / a0;
        m_b1 = b1 / a0;
        m_b2 = b2 / a0;
        m_a1 = -2.0f * cosw / a0;
        m_a2 = a2 / a0;
    }

    std::complex<float> TransferFunctionAt(float frequency) const {
        frequency = ClampFrequency(frequency);
        const float cosw = DefaultDspMath::Cos2Pi(frequency);
        const float sinw = DefaultDspMath::Sin2Pi(frequency);
        const float cos2w = DefaultDspMath::Cos2Pi(2.0f * frequency);
        const float sin2w = DefaultDspMath::Sin2Pi(2.0f * frequency);

        const std::complex<float> numerator(
            m_b0 + m_b1 * cosw + m_b2 * cos2w,
            -m_b1 * sinw - m_b2 * sin2w);
        const std::complex<float> denominator(
            1.0f + m_a1 * cosw + m_a2 * cos2w,
            -m_a1 * sinw - m_a2 * sin2w);
        return numerator / denominator;
    }
};

struct ButterworthFilter {
    static constexpr float kDefaultCutoff = 0.1f;

    struct Input {
        float value = 0.0f;
        float cutoff = kDefaultCutoff;
    };

    BiquadSection m_biquad1;
    BiquadSection m_biquad2;
    BiquadSection m_biquad3;
    BiquadSection m_biquad4;
    float m_cutoff = kDefaultCutoff;

    ButterworthFilter() {
        SetCutoff(m_cutoff);
    }

    float Process(const Input& input) {
        SetCutoff(input.cutoff);
        return Process(input.value);
    }

    // Fixed-coefficient processing for rate converters; configure once.
    float Process(float value) {
        const float stage1 = m_biquad1.Process(value);
        const float stage2 = m_biquad2.Process(stage1);
        const float stage3 = m_biquad3.Process(stage2);
        return m_biquad4.Process(stage3);
    }

    void SetCutoff(float cyclesPerSample) {
        m_cutoff = BiquadSection::ClampCutoff(cyclesPerSample);
        m_biquad1.SetLowPassCoefficients(m_cutoff, ButterworthQ(1.0f / 32.0f));
        m_biquad2.SetLowPassCoefficients(m_cutoff, ButterworthQ(3.0f / 32.0f));
        m_biquad3.SetLowPassCoefficients(m_cutoff, ButterworthQ(5.0f / 32.0f));
        m_biquad4.SetLowPassCoefficients(m_cutoff, ButterworthQ(7.0f / 32.0f));
    }

    void Reset() {
        m_biquad1.Reset();
        m_biquad2.Reset();
        m_biquad3.Reset();
        m_biquad4.Reset();
    }

private:
    static float ButterworthQ(float phase) {
        return 1.0f / (2.0f * DefaultDspMath::Cos2Pi(phase));
    }
};

struct LinkwitzRileyCrossover {
    static constexpr float kDefaultCutoff = 0.1f;

    struct Input {
        float value = 0.0f;
        float cutoff = kDefaultCutoff;
    };

    struct Output {
        float lowPass = 0.0f;
        float highPass = 0.0f;
    };

    struct ComplexOutput {
        std::complex<float> lowPass = {0.0f, 0.0f};
        std::complex<float> highPass = {0.0f, 0.0f};
    };

    BiquadSection m_lowBiquad1;
    BiquadSection m_lowBiquad2;
    BiquadSection m_highBiquad1;
    BiquadSection m_highBiquad2;
    float m_cutoff = kDefaultCutoff;

    LinkwitzRileyCrossover() {
        SetCutoff(m_cutoff);
    }

    Output Process(const Input& input) {
        SetCutoff(input.cutoff);

        const float lowStage1 = m_lowBiquad1.Process(input.value);
        const float lowPass = m_lowBiquad2.Process(lowStage1);
        const float highStage1 = m_highBiquad1.Process(input.value);
        const float highPass = m_highBiquad2.Process(highStage1);
        return {.lowPass = lowPass, .highPass = highPass};
    }

    void SetCutoff(float cyclesPerSample) {
        m_cutoff = BiquadSection::ClampCutoff(cyclesPerSample);
        const float q = LinkwitzRileyQ();

        m_lowBiquad1.SetLowPassCoefficients(m_cutoff, q);
        m_lowBiquad2.SetLowPassCoefficients(m_cutoff, q);
        m_highBiquad1.SetHighPassCoefficients(m_cutoff, q);
        m_highBiquad2.SetHighPassCoefficients(m_cutoff, q);
    }

    void Reset() {
        m_lowBiquad1.Reset();
        m_lowBiquad2.Reset();
        m_highBiquad1.Reset();
        m_highBiquad2.Reset();
    }

    static ComplexOutput TransferFunction(float cutoff, float frequency) {
        cutoff = BiquadSection::ClampCutoff(cutoff);
        const float q = LinkwitzRileyQ();

        const auto lowPass1 = BiquadSection::TransferFunction(cutoff, q, frequency);
        const auto lowPass2 = BiquadSection::TransferFunction(cutoff, q, frequency);
        const auto highPass1 = BiquadSection::TransferFunction(cutoff, q, frequency, true);
        const auto highPass2 = BiquadSection::TransferFunction(cutoff, q, frequency, true);
        return {.lowPass = lowPass1 * lowPass2, .highPass = highPass1 * highPass2};
    }

private:
    static float LinkwitzRileyQ() {
        return 1.0f / std::sqrt(2.0f);
    }
};

template<bool Normalize = false>
struct TanhSaturator {
    struct Input {
        float value = 0.0f;
        float gain = 1.0f;
    };

    float m_inputGain = 1.0f;
    float m_tanhGain = RawApprox(1.0f);
    float m_output = 0.0f;

    static float RawApprox(float x) {
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    static float Tanh(float x) {
        return std::clamp(RawApprox(x), -1.0f, 1.0f);
    }

    void SetInputGain(float gain) {
        m_inputGain = gain;
        m_tanhGain = Tanh(gain);
    }

    float Process(const Input& input) {
        SetInputGain(input.gain);
        const float output = Tanh(input.value * m_inputGain);
        m_output = Normalize && std::abs(m_tanhGain) > 1.0e-6f ? output / m_tanhGain : output;
        return m_output;
    }
};

} // namespace synth
