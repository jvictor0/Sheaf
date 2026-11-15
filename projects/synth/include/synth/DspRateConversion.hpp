#pragma once

#include "synth/DspFilters.hpp"
#include <array>
#include <span>
#include <stdexcept>
#include <vector>

namespace synth {

inline float RateConversionSectionStep(float input, float b0, float b1, float b2,
                                       float a1, float a2, float& x1, float& x2,
                                       float& y1, float& y2) {
    const float output = b0 * input + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    return output;
}

// Fixed engine/matrix converter. Each instance owns independent histories.
class RateConversionLowPass {
public:
    struct SectionCoefficients { float b0, b1, b2, a1, a2; };
    inline static constexpr std::array<SectionCoefficients, 6> kCoefficients{{
        {0x1.5b07be0000000p-13f, 0x1.d6b7500000000p-16f, 0x1.5b07be0000000p-13f, -0x1.96835e0000000p-1f, 0x0.0p+0f},
        {0x1.0000000000000p+0f, 0x1.0000000000000p+0f, 0x0.0p+0f, -0x1.94b2420000000p+0f, 0x1.58c5c00000000p-1f},
        {0x1.0000000000000p+0f, -0x1.d759fa0000000p-1f, 0x1.0000000000000p+0f, -0x1.90db1c0000000p+0f, 0x1.89be5e0000000p-1f},
        {0x1.0000000000000p+0f, -0x1.451b1c0000000p+0f, 0x1.0000000000000p+0f, -0x1.8dc5c60000000p+0f, 0x1.b9ce620000000p-1f},
        {0x1.0000000000000p+0f, -0x1.65de0c0000000p+0f, 0x1.0000000000000p+0f, -0x1.8d26600000000p+0f, 0x1.dcfad00000000p-1f},
        {0x1.0000000000000p+0f, -0x1.716c7c0000000p+0f, 0x1.0000000000000p+0f, -0x1.9013820000000p+0f, 0x1.f581980000000p-1f},
    }};

    explicit RateConversionLowPass(float initial = 0) { Reset(initial); }

    void Reset(float initial) {
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            const auto& c = kCoefficients[i];
            auto& section = sections_[i];
            section.m_b0 = c.b0; section.m_b1 = c.b1; section.m_b2 = c.b2;
            section.m_a1 = c.a1; section.m_a2 = c.a2;
            const double gain = (double(c.b0) + c.b1 + c.b2) / (1.0 + c.a1 + c.a2);
            const float output = static_cast<float>(double(initial) * gain);
            section.m_x1 = section.m_x2 = initial;
            section.m_y1 = section.m_y2 = output;
            initial = output;
        }
    }

    float Process(float input) {
        for (auto& s : sections_)
            input = RateConversionSectionStep(input, s.m_b0, s.m_b1, s.m_b2,
                s.m_a1, s.m_a2, s.m_x1, s.m_x2, s.m_y1, s.m_y2);
        return input;
    }

    const std::array<BiquadSection, 6>& Sections() const { return sections_; }

private:
    std::array<BiquadSection, 6> sections_;
};

// Section-major independent histories; Configure is an off-audio operation.
class RateConversionLowPassBatch {
public:
    void Configure(std::span<const float> initials) {
        size_ = initials.size();
        for (auto& h : histories_) {
            h.x1.resize(size_); h.x2.resize(size_);
            h.y1.resize(size_); h.y2.resize(size_);
        }
        for (std::size_t lane = 0; lane < size_; ++lane) {
            float initial = initials[lane];
            for (std::size_t section = 0; section < histories_.size(); ++section) {
                const auto& c = RateConversionLowPass::kCoefficients[section];
                auto& h = histories_[section];
                const double gain = (double(c.b0) + c.b1 + c.b2) / (1.0 + c.a1 + c.a2);
                const float output = static_cast<float>(double(initial) * gain);
                h.x1[lane] = h.x2[lane] = initial;
                h.y1[lane] = h.y2[lane] = output;
                initial = output;
            }
        }
    }
    std::size_t Size() const { return size_; }
    void Process(std::span<float> inOut) {
        if (inOut.size() != size_) throw std::invalid_argument("converter lane count mismatch");
        for (std::size_t section = 0; section < histories_.size(); ++section) {
            const auto& c = RateConversionLowPass::kCoefficients[section];
            auto& h = histories_[section];
            for (std::size_t lane = 0; lane < size_; ++lane)
                inOut[lane] = RateConversionSectionStep(inOut[lane], c.b0, c.b1, c.b2,
                    c.a1, c.a2, h.x1[lane], h.x2[lane], h.y1[lane], h.y2[lane]);
        }
    }
private:
    struct Histories { std::vector<float> x1, x2, y1, y2; };
    std::array<Histories, 6> histories_;
    std::size_t size_ = 0;
};

} // namespace synth
