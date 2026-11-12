// Emit the exact binary32 coefficients used by the modulation-source bridge.
#include "synth/RateSeparatedParameters.hpp"
#include "synth/DspBuffers.hpp"
#include <bit>
#include <cstdint>
#include <iostream>

int main(int argc, char**) {
    if (argc > 1) {
        std::cout << "[";
        bool first = true;
        for (double coefficient : synth::FourToOneDecimatorCoefficients()) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << std::bit_cast<std::uint32_t>(static_cast<float>(coefficient));
        }
        std::cout << "]\n";
        return 0;
    }
    synth::RateSeparatedParameters::SourceFilter filter;
    std::cout << "[";
    bool first = true;
    for (const auto& section : filter.Sections()) {
        if (!first) std::cout << ",";
        first = false;
        std::cout << "[";
        bool firstCoefficient = true;
        for (float coefficient : {section.m_b0, section.m_b1, section.m_b2,
                                  section.m_a1, section.m_a2}) {
            if (!firstCoefficient) std::cout << ",";
            firstCoefficient = false;
            std::cout << std::bit_cast<std::uint32_t>(coefficient);
        }
        std::cout << "]";
    }
    std::cout << "]\n";
}
