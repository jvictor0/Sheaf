#pragma once

#include "synth/DspFilters.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace synth {

namespace detail {

inline constexpr double kFirDesignPi = 3.141592653589793238462643383279502884;

constexpr double FirAbs(double value) {
    return value < 0.0 ? -value : value;
}

constexpr double FirSqrt(double value) {
    if (value <= 0.0) {
        return 0.0;
    }

    double estimate = value >= 1.0 ? value : 1.0;
    for (int i = 0; i < 48; ++i) {
        estimate = 0.5 * (estimate + value / estimate);
    }
    return estimate;
}

constexpr double FirSin(double value) {
    constexpr double twoPi = 2.0 * kFirDesignPi;
    while (value > kFirDesignPi) {
        value -= twoPi;
    }
    while (value < -kFirDesignPi) {
        value += twoPi;
    }

    double term = value;
    double sum = value;
    for (int i = 1; i < 20; ++i) {
        const double a = static_cast<double>(2 * i);
        const double b = static_cast<double>(2 * i + 1);
        term *= -(value * value) / (a * b);
        sum += term;
    }
    return sum;
}

constexpr double FirBesselI0(double value) {
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k <= 50; ++k) {
        const double denominator = 4.0 * static_cast<double>(k) * static_cast<double>(k);
        term *= (value * value) / denominator;
        sum += term;
    }
    return sum;
}

template<std::size_t Taps>
constexpr std::array<double, Taps> MakeKaiserLowpass(double cutoff, double beta) {
    static_assert(Taps > 0);

    std::array<double, Taps> coefficients{};
    const double center = static_cast<double>(Taps - 1) * 0.5;
    const double besselDenominator = FirBesselI0(beta);

    for (std::size_t i = 0; i < Taps; ++i) {
        const double distanceFromCenter = FirAbs(static_cast<double>(i) - center);
        const double ideal = distanceFromCenter == 0.0
            ? 2.0 * cutoff
            : FirSin(2.0 * kFirDesignPi * cutoff * distanceFromCenter)
                / (kFirDesignPi * distanceFromCenter);
        const double normalizedDistance = center == 0.0 ? 0.0 : distanceFromCenter / center;
        const double window = FirBesselI0(beta * FirSqrt(1.0 - normalizedDistance * normalizedDistance))
            / besselDenominator;
        coefficients[i] = ideal * window;
    }

    double sum = 0.0;
    for (const double coefficient : coefficients) {
        sum += coefficient;
    }
    for (double& coefficient : coefficients) {
        coefficient /= sum;
    }
    return coefficients;
}

} // namespace detail

inline constexpr std::size_t kFourToOneDecimatorTaps = 287;
inline constexpr std::array<double, kFourToOneDecimatorTaps> kFourToOneDecimatorCoefficients =
    detail::MakeKaiserLowpass<kFourToOneDecimatorTaps>(11.0 / 96.0, 9.0);

constexpr std::span<const double, kFourToOneDecimatorTaps> FourToOneDecimatorCoefficients() {
    return std::span<const double, kFourToOneDecimatorTaps>{kFourToOneDecimatorCoefficients};
}

template<std::size_t Factor, std::size_t Channels, std::size_t Taps>
class FirDecimator {
    static_assert(Factor > 0);
    static_assert(Channels > 0);
    static_assert(Taps > 0);

public:
    static constexpr std::size_t kFactor = Factor;
    static constexpr std::size_t kChannels = Channels;
    static constexpr std::size_t kTaps = Taps;

    constexpr explicit FirDecimator(std::span<const double, Taps> coefficients) {
        for (std::size_t i = 0; i < Taps; ++i) {
            coefficients_[i] = static_cast<float>(coefficients[i]);
        }
    }

    constexpr explicit FirDecimator(const std::array<double, Taps>& coefficients)
        : FirDecimator(std::span<const double, Taps>{coefficients}) {}

    void Reset() {
        for (auto& channelHistory : history_) {
            channelHistory.fill(0.0f);
        }
        writeIndex_ = 0;
        phase_ = 0;
    }

    bool ProcessFrame(std::span<const float, Channels> input, std::span<float, Channels> output) {
        for (std::size_t channel = 0; channel < Channels; ++channel) {
            history_[channel][writeIndex_] = input[channel];
        }

        writeIndex_ = (writeIndex_ + 1) % Taps;
        phase_ = (phase_ + 1) % Factor;
        if (phase_ != 0) {
            return false;
        }

        for (std::size_t channel = 0; channel < Channels; ++channel) {
            double sum = 0.0;
            std::size_t historyIndex = writeIndex_;
            for (std::size_t tap = 0; tap < Taps; ++tap) {
                if (historyIndex == 0) {
                    historyIndex = Taps;
                }
                --historyIndex;
                sum += static_cast<double>(coefficients_[tap] * history_[channel][historyIndex]);
            }
            output[channel] = static_cast<float>(sum);
        }
        return true;
    }

private:
    std::array<float, Taps> coefficients_{};
    std::array<std::array<float, Taps>, Channels> history_{};
    std::size_t writeIndex_ = 0;
    std::size_t phase_ = 0;
};

template<std::size_t Factor, std::size_t Channels, typename Decimator>
class OversampledOutputStage {
    static_assert(Factor > 0);
    static_assert(Channels > 0);
    static_assert(Decimator::kFactor == Factor, "OversampledOutputStage factor must match Decimator::kFactor");
    static_assert(Decimator::kChannels == Channels, "OversampledOutputStage channels must match Decimator::kChannels");

public:
    constexpr explicit OversampledOutputStage(Decimator decimator)
        : decimator_(std::move(decimator)) {}

    template<typename Generator>
    std::array<float, Channels> ProcessHostFrame(std::uint64_t hostSampleIndex, Generator&& generator) {
        std::array<float, Channels> output{};
        for (std::size_t subframe = 0; subframe < Factor; ++subframe) {
            const std::uint64_t internalIndex = hostSampleIndex * Factor + subframe;
            const std::array<float, Channels> input = generator(internalIndex);
            decimator_.ProcessFrame(input, output);
        }
        return output;
    }

    void Reset() {
        decimator_.Reset();
    }

private:
    Decimator decimator_;
};

} // namespace synth
