// Emit the exact binary32 coefficients used by the modulation-source bridge.
#include "synth/RateSeparatedParameters.hpp"
#include <bit>
#include <cstdint>
#include <iostream>

int main() {
    synth::RateSeparatedParameters::SourceFilter filter;
    filter.SetCutoff(synth::RateSeparatedParameters::kSourceCutoff);
    std::cout << "[";
    bool first = true;
    for (const auto* section : {&filter.m_biquad1, &filter.m_biquad2,
                                &filter.m_biquad3, &filter.m_biquad4}) {
        if (!first) std::cout << ",";
        first = false;
        std::cout << "[";
        bool firstCoefficient = true;
        for (float coefficient : {section->m_b0, section->m_b1, section->m_b2,
                                  section->m_a1, section->m_a2}) {
            if (!firstCoefficient) std::cout << ",";
            firstCoefficient = false;
            std::cout << std::bit_cast<std::uint32_t>(coefficient);
        }
        std::cout << "]";
    }
    std::cout << "]\n";
}
