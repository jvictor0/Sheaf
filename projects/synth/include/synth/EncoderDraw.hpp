#pragma once

// JUCE-free portable encoder draw state, geometry, fourteen-segment label,
// and DrawCommand builder. Backends render the returned synth::ui commands.

#include "synth/ParameterModulation.hpp"
#include "synth/PortableUI.hpp"
#include "synth/PortableUIBuilders.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace synth::ui {

inline constexpr float x_Pi = 3.14159265358979323846f;

inline float Clamp(float value, float minValue, float maxValue)
{
    return std::max(minValue, std::min(maxValue, value));
}

namespace EncoderGeometry {

inline float ValueToArcAngle(float value)
{
    return x_Pi * 1.25f + value * x_Pi * 1.5f;
}

inline float ValueToIndicatorAngle(float value)
{
    return x_Pi * 0.75f + value * x_Pi * 1.5f;
}

inline Point IndicatorPoint(float centerX, float centerY, float radius, float value)
{
    const float angle = ValueToIndicatorAngle(value);
    return {
        centerX + radius * std::cos(angle),
        centerY + radius * std::sin(angle),
    };
}

inline float NormalizeForDisplay(float value, bool bipolar)
{
    const float normalized = bipolar ? (value + 1.0f) * 0.5f : value;
    return Clamp(normalized, 0.0f, 1.0f);
}

inline float IndicatorDotRadius(float radius)
{
    return Clamp(radius * 0.11f, 3.0f, 8.0f);
}

inline float MotionBlurAmount(float displaySpread)
{
    constexpr float x_FullBlurSpread = 0.20f;
    return Clamp(std::max(0.0f, displaySpread) / x_FullBlurSpread, 0.0f, 1.0f);
}

inline float MotionBlurArcHalfValue(float radius, float displaySpread)
{
    constexpr float x_FullArcRadians = x_Pi * 1.5f;
    constexpr float x_ProbableSigma = 2.0f;
    constexpr float x_CollapsedHalfWidth = 0.35f;
    const float collapsedHalfValue = x_CollapsedHalfWidth / (radius * x_FullArcRadians);
    return collapsedHalfValue + std::max(0.0f, displaySpread) * x_ProbableSigma;
}

inline float MotionBlurOutlineAlpha(float motionAmount)
{
    const float motion = Clamp(motionAmount, 0.0f, 1.0f);
    return 0.55f * (1.0f - 0.95f * motion * motion);
}

struct MotionIndicatorGeometry
{
    float centerAngle = 0.0f;
    float arcHalfValue = 0.0f;
    float startValue = 0.0f;
    float endValue = 0.0f;
    float outlineAlpha = 0.0f;
    float outlineStrokeWidth = 0.0f;
    float outerStrokeWidth = 0.0f;
    float outerAlpha = 0.0f;
    float midStrokeWidth = 0.0f;
    float midAlpha = 0.0f;
    float coreStrokeWidth = 0.0f;
    float coreAlpha = 0.0f;
};

inline MotionIndicatorGeometry MotionIndicatorGeometryFor(float radius, float value, float displaySpread)
{
    const float normalizedValue = Clamp(value, 0.0f, 1.0f);
    const float dotRadius = IndicatorDotRadius(radius);
    const float clampedDisplaySpread = std::max(0.0f, displaySpread);
    const float motion = MotionBlurAmount(clampedDisplaySpread);
    const float radialMotion = motion * motion;
    const float halfValue = MotionBlurArcHalfValue(radius, clampedDisplaySpread);

    MotionIndicatorGeometry geometry;
    geometry.centerAngle = ValueToArcAngle(normalizedValue);
    geometry.arcHalfValue = halfValue;
    geometry.startValue = Clamp(normalizedValue - halfValue, 0.0f, 1.0f);
    geometry.endValue = Clamp(normalizedValue + halfValue, 0.0f, 1.0f);
    geometry.outlineAlpha = MotionBlurOutlineAlpha(motion);
    geometry.outerStrokeWidth = dotRadius * 2.0f + radialMotion * radius * 0.30f;
    geometry.midStrokeWidth = dotRadius * 1.35f + radialMotion * radius * 0.18f;
    geometry.coreStrokeWidth =
        std::max(dotRadius * 0.95f, dotRadius * (2.0f - motion * 0.85f) + radialMotion * radius * 0.05f);
    geometry.outlineStrokeWidth =
        geometry.coreStrokeWidth + 1.0f + motion * std::max(0.0f, geometry.outerStrokeWidth - geometry.coreStrokeWidth);
    geometry.outerAlpha = motion * (0.10f + motion * 0.08f);
    geometry.midAlpha = motion * (0.24f + motion * 0.16f);
    geometry.coreAlpha = 0.96f - motion * 0.08f;
    return geometry;
}

inline void GetSwitchValueRange(std::size_t switchVal, std::size_t switchValues, float& startValue, float& endValue)
{
    if (switchValues <= 1)
    {
        startValue = 0.0f;
        endValue = 1.0f;
        return;
    }

    const float denominator = static_cast<float>(switchValues - 1);
    startValue = switchVal == 0 ? 0.0f : (static_cast<float>(switchVal) - 0.5f) / denominator;
    endValue = switchVal == switchValues - 1 ? 1.0f : (static_cast<float>(switchVal) + 0.5f) / denominator;
}

inline Bounds ArcBoundsFor(float centerX, float centerY, float radius)
{
    return {centerX - radius, centerY - radius, radius * 2.0f, radius * 2.0f};
}

inline void AppendArc(std::vector<DrawCommand>& commands,
                      float centerX,
                      float centerY,
                      float radius,
                      float startValue,
                      float endValue,
                      Color color,
                      float strokeWidth)
{
    commands.push_back(DrawCommand::Arc(
        ArcBoundsFor(centerX, centerY, radius),
        ValueToArcAngle(startValue),
        ValueToArcAngle(endValue),
        color,
        strokeWidth));
}

inline void AppendArcWithSwitchGaps(std::vector<DrawCommand>& commands,
                                    float centerX,
                                    float centerY,
                                    float radius,
                                    float startValue,
                                    float endValue,
                                    std::size_t switchValues,
                                    Color color,
                                    float strokeWidth)
{
    startValue = Clamp(startValue, 0.0f, 1.0f);
    endValue = Clamp(endValue, 0.0f, 1.0f);
    if (endValue < startValue)
    {
        std::swap(startValue, endValue);
    }

    if (switchValues <= 1)
    {
        AppendArc(commands, centerX, centerY, radius, startValue, endValue, color, strokeWidth);
        return;
    }

    constexpr float x_SwitchGapRadians = x_Pi / 90.0f;
    for (std::size_t switchVal = 0; switchVal < switchValues; ++switchVal)
    {
        float switchStart = 0.0f;
        float switchEnd = 1.0f;
        GetSwitchValueRange(switchVal, switchValues, switchStart, switchEnd);

        const float segmentStart = std::max(startValue, switchStart);
        const float segmentEnd = std::min(endValue, switchEnd);
        if (segmentEnd <= segmentStart)
        {
            continue;
        }

        float startAngle = ValueToArcAngle(segmentStart);
        float endAngle = ValueToArcAngle(segmentEnd);
        if (switchVal > 0 && segmentStart <= switchStart)
        {
            startAngle += x_SwitchGapRadians;
        }
        if (switchVal + 1 < switchValues && segmentEnd >= switchEnd)
        {
            endAngle -= x_SwitchGapRadians;
        }
        if (endAngle <= startAngle)
        {
            continue;
        }

        commands.push_back(DrawCommand::Arc(
            ArcBoundsFor(centerX, centerY, radius), startAngle, endAngle, color, strokeWidth));
    }
}

inline std::size_t CountMaskBits(std::uint64_t mask)
{
    std::size_t count = 0;
    while (mask != 0)
    {
        count += mask & 1u;
        mask >>= 1u;
    }
    return count;
}

inline std::string BadgeText(bool modulator, std::size_t index)
{
    if (modulator)
    {
        return "M" + std::to_string(index + 1);
    }
    if (index < 8)
    {
        return std::to_string(index + 1);
    }
    static constexpr const char* x_Symbols[] = {"U", "R", "D", "L", "UU", "RR", "DD", "LL"};
    if (index < 16)
    {
        return x_Symbols[index - 8];
    }
    return std::to_string(index + 1);
}

inline void GetBadgePosition(float centerX,
                             float centerY,
                             float radius,
                             std::size_t ix,
                             std::size_t total,
                             bool upper,
                             float& badgeX,
                             float& badgeY,
                             float& badgeLength)
{
    if (total <= 8)
    {
        badgeLength = 1.0f / std::sqrt(1.0f + static_cast<float>(total * total) / 4.0f);
        badgeX = -badgeLength * static_cast<float>(total) / 2.0f + static_cast<float>(ix) * badgeLength;
        badgeY = badgeLength;
    }
    else
    {
        badgeLength = 1.0f / (2.0f * std::sqrt(5.0f));
        badgeX = -4.0f / (2.0f * std::sqrt(5.0f)) + static_cast<float>(ix % 8) * badgeLength;
        badgeY = 2.0f / (2.0f * std::sqrt(5.0f)) - static_cast<float>(ix / 8) * badgeLength;
    }

    badgeX = centerX + radius * badgeX;
    badgeLength = radius * badgeLength;
    badgeY = upper ? centerY - radius * badgeY : centerY + radius * badgeY - badgeLength;
}

}  // namespace EncoderGeometry

struct EncoderVoiceDrawState
{
    float value = 0.0f;
    float spreadValue = 0.0f;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    std::size_t switchValue = 0;
    synth::Color indicatorColor = synth::Color::Grey;
};

struct EncoderDrawState
{
    bool connected = false;
    bool hasVisualizerUnderlay = false;
    bool wantsFrame = true;
    bool bipolar = false;
    std::size_t switchValues = 0;
    std::uint32_t modulatorsAffectingMask = 0;
    synth::GestureMask gesturesAffectingMask = 0;
    synth::Color baseColor = synth::Color::Off;
    std::string shortLabel;
    std::size_t voiceCount = 0;
    std::vector<EncoderVoiceDrawState> voices;
    std::vector<synth::Color> modulatorColors;
    std::vector<synth::Color> gestureColors;
};

inline EncoderDrawState EncoderDrawStateFromParameter(const synth::Parameter::UIState& state)
{
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const std::uint32_t startRevision = state.revision.load(std::memory_order_acquire);
        if ((startRevision & 1u) != 0)
        {
            continue;
        }

        EncoderDrawState candidate;
        candidate.connected = state.connected.load(std::memory_order_relaxed);
        candidate.bipolar = state.bipolar.load(std::memory_order_relaxed);
        candidate.switchValues = state.switchValues.load(std::memory_order_relaxed);
        candidate.modulatorsAffectingMask = state.modulatorsAffectingMask.load(std::memory_order_relaxed);
        candidate.gesturesAffectingMask = state.gesturesAffectingMask.load(std::memory_order_relaxed);
        candidate.baseColor = state.baseColor.Load(std::memory_order_relaxed);
        const char* shortName = state.shortName.load(std::memory_order_relaxed);
        if (shortName != nullptr)
        {
            candidate.shortLabel = shortName;
        }
        candidate.voiceCount = std::min(state.voiceCount.load(std::memory_order_relaxed), state.voiceCapacity);

        candidate.voices.resize(candidate.voiceCount);
        for (std::size_t voiceIx = 0; voiceIx < candidate.voiceCount; ++voiceIx)
        {
            EncoderVoiceDrawState& voice = candidate.voices[voiceIx];
            voice.value = state.values[voiceIx].load(std::memory_order_relaxed);
            voice.spreadValue = state.spreadValues[voiceIx].load(std::memory_order_relaxed);
            voice.minValue = state.minValues[voiceIx].load(std::memory_order_relaxed);
            voice.maxValue = state.maxValues[voiceIx].load(std::memory_order_relaxed);
            voice.switchValue = state.switchValue[voiceIx].load(std::memory_order_relaxed);
            voice.indicatorColor = state.indicatorColors[voiceIx].Load(std::memory_order_relaxed);
        }
        const std::size_t modulatorColorCount = std::min(
            state.modulatorColorCount.load(std::memory_order_relaxed), state.modulatorColorCapacity);
        candidate.modulatorColors.resize(modulatorColorCount);
        for (std::size_t modIx = 0; modIx < modulatorColorCount; ++modIx)
        {
            candidate.modulatorColors[modIx] = state.modulatorSourceColors[modIx].Load(std::memory_order_relaxed);
        }
        const std::size_t gestureColorCount = std::min(
            state.gestureColorCount.load(std::memory_order_relaxed), state.gestureColorCapacity);
        candidate.gestureColors.resize(gestureColorCount);
        for (std::size_t gestureIx = 0; gestureIx < gestureColorCount; ++gestureIx)
        {
            candidate.gestureColors[gestureIx] = state.gestureColors[gestureIx].Load(std::memory_order_relaxed);
        }

        const std::uint32_t endRevision = state.revision.load(std::memory_order_acquire);
        if (startRevision == endRevision && (endRevision & 1u) == 0)
        {
            return candidate;
        }
    }

    return {};
}

namespace FourteenSegment {

enum class Segment {
    A = 0,
    B = 1,
    C = 2,
    D = 3,
    E = 4,
    F = 5,
    G1 = 6,
    G2 = 7,
    H = 8,
    J = 9,
    K = 10,
    L = 11,
    M = 12,
    N = 13,
    DP = 14,
};

inline bool IsSegmentOn(std::uint16_t mask, Segment segment)
{
    return (mask & (1u << static_cast<int>(segment))) != 0;
}

inline std::uint16_t GetSegmentMask(char c)
{
    static constexpr std::uint16_t x_AsciiTable[96] = {
        0x0000, 0x4006, 0x0202, 0x12CE, 0x12ED, 0x3FE4, 0x2359, 0x0200, 0x2400, 0x0900, 0x3FC0, 0x12C0,
        0x0800, 0x00C0, 0x4000, 0x0C00, 0x0C3F, 0x0406, 0x00DB, 0x008F, 0x00E6, 0x2069, 0x00FD, 0x0007,
        0x00FF, 0x00EF, 0x1200, 0x0A00, 0x2440, 0x00C8, 0x0980, 0x5083, 0x02BB, 0x00F7, 0x128F, 0x0039,
        0x120F, 0x0079, 0x0071, 0x00BD, 0x00F6, 0x1209, 0x001E, 0x2470, 0x0038, 0x0536, 0x2136, 0x003F,
        0x00F3, 0x203F, 0x20F3, 0x00ED, 0x1201, 0x003E, 0x0C30, 0x2836, 0x2D00, 0x00EE, 0x0C09, 0x0039,
        0x2100, 0x000F, 0x2800, 0x0008, 0x0100, 0x1058, 0x2078, 0x00D8, 0x088E, 0x0858, 0x14C0, 0x048E,
        0x1070, 0x1000, 0x0A10, 0x3600, 0x0030, 0x10D4, 0x1050, 0x00DC, 0x0170, 0x0486, 0x0050, 0x2088,
        0x0078, 0x001C, 0x0810, 0x2814, 0x2D00, 0x028E, 0x0848, 0x0949, 0x1200, 0x2489, 0x0CC0, 0x0000,
    };

    if (c < 32 || c > 127)
    {
        return 0x0000;
    }
    return x_AsciiTable[static_cast<std::size_t>(c - 32)];
}

inline std::vector<Point> HorizontalSegmentPolygon(float x, float y, float width, float height)
{
    const float halfH = height / 2.0f;
    return {
        {x + halfH, y},
        {x + width - halfH, y},
        {x + width, y + halfH},
        {x + width - halfH, y + height},
        {x + halfH, y + height},
        {x, y + halfH},
    };
}

inline std::vector<Point> VerticalSegmentPolygon(float x, float y, float width, float height)
{
    const float halfW = width / 2.0f;
    return {
        {x + halfW, y},
        {x + width, y + halfW},
        {x + width, y + height - halfW},
        {x + halfW, y + height},
        {x, y + height - halfW},
        {x, y + halfW},
    };
}

inline std::vector<Point> DiagonalSegmentPolygon(float x1, float y1, float x2, float y2, float thickness)
{
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.001f)
    {
        return {};
    }

    const float nx = -dy / len * thickness / 2.0f;
    const float ny = dx / len * thickness / 2.0f;
    return {
        {x1 + nx, y1 + ny},
        {x2 + nx, y2 + ny},
        {x2 - nx, y2 - ny},
        {x1 - nx, y1 - ny},
    };
}

inline void AppendCharacter(std::vector<DrawCommand>& commands,
                            Bounds bounds,
                            std::uint16_t mask,
                            Color onColor,
                            Color offColor,
                            float segmentThickness,
                            float segmentGap)
{
    const float padding = bounds.width * 0.05f;
    bounds.x += padding;
    bounds.y += padding;
    bounds.width -= padding * 2.0f;
    bounds.height -= padding * 2.0f;

    const float w = bounds.width;
    const float h = bounds.height;
    const float x = bounds.x;
    const float y = bounds.y;
    const float thickness = w * segmentThickness;
    const float gap = w * segmentGap;
    const float halfH = h / 2.0f;

    const float horzLeft = x + thickness + gap;
    const float horzRight = x + w - thickness - gap;
    const float horzMid = x + w / 2.0f;
    const float horzLen = (horzRight - horzLeft - gap) / 2.0f;

    const float vertTop = y + thickness + gap;
    const float vertMid = y + halfH;
    const float vertBottom = y + h - thickness - gap;

    const auto appendSegment = [&](const std::vector<Point>& polygon, bool on) {
        if (!polygon.empty())
        {
            commands.push_back(DrawCommand::FillPolygon(polygon, on ? onColor : offColor));
        }
    };

    appendSegment(HorizontalSegmentPolygon(horzLeft, y, horzRight - horzLeft, thickness), IsSegmentOn(mask, Segment::A));
    appendSegment(HorizontalSegmentPolygon(horzLeft, y + h - thickness, horzRight - horzLeft, thickness),
                  IsSegmentOn(mask, Segment::D));
    appendSegment(HorizontalSegmentPolygon(horzLeft, vertMid - thickness / 2.0f, horzLen, thickness),
                  IsSegmentOn(mask, Segment::G1));
    appendSegment(HorizontalSegmentPolygon(horzMid + gap / 2.0f, vertMid - thickness / 2.0f, horzLen, thickness),
                  IsSegmentOn(mask, Segment::G2));

    appendSegment(VerticalSegmentPolygon(x, vertTop, thickness, vertMid - vertTop - gap), IsSegmentOn(mask, Segment::F));
    appendSegment(VerticalSegmentPolygon(x, vertMid + gap, thickness, vertBottom - vertMid - gap),
                  IsSegmentOn(mask, Segment::E));
    appendSegment(VerticalSegmentPolygon(x + w - thickness, vertTop, thickness, vertMid - vertTop - gap),
                  IsSegmentOn(mask, Segment::B));
    appendSegment(VerticalSegmentPolygon(x + w - thickness, vertMid + gap, thickness, vertBottom - vertMid - gap),
                  IsSegmentOn(mask, Segment::C));
    appendSegment(VerticalSegmentPolygon(horzMid - thickness / 2.0f, vertTop, thickness, vertMid - vertTop - gap),
                  IsSegmentOn(mask, Segment::J));
    appendSegment(VerticalSegmentPolygon(horzMid - thickness / 2.0f, vertMid + gap, thickness, vertBottom - vertMid - gap),
                  IsSegmentOn(mask, Segment::M));

    const float diagInnerX = horzMid - thickness / 2.0f;
    const float diagOuterLeft = x + thickness + gap;
    const float diagOuterRight = x + w - thickness - gap;

    appendSegment(DiagonalSegmentPolygon(diagOuterLeft, vertTop, diagInnerX - gap, vertMid - gap, thickness),
                  IsSegmentOn(mask, Segment::H));
    appendSegment(DiagonalSegmentPolygon(diagOuterRight, vertTop, diagInnerX + thickness + gap, vertMid - gap, thickness),
                  IsSegmentOn(mask, Segment::K));
    appendSegment(DiagonalSegmentPolygon(diagInnerX - gap, vertMid + gap, diagOuterLeft, vertBottom, thickness),
                  IsSegmentOn(mask, Segment::L));
    appendSegment(DiagonalSegmentPolygon(diagInnerX + thickness + gap, vertMid + gap, diagOuterRight, vertBottom, thickness),
                  IsSegmentOn(mask, Segment::N));

    const float dpSize = thickness * 1.2f;
    commands.push_back(DrawCommand::FillEllipse(
        {x + w + gap, y + h - dpSize, dpSize, dpSize},
        IsSegmentOn(mask, Segment::DP) ? onColor : offColor));
}

}  // namespace FourteenSegment

inline std::vector<DrawCommand> BuildFourteenSegmentCommands(std::string_view text,
                                                                        Bounds bounds,
                                                                        Color onColor,
                                                                        Color offColor,
                                                                        int numChars = 4,
                                                                        float segmentThickness = 0.085f,
                                                                        float segmentGap = 0.02f)
{
    std::vector<DrawCommand> commands;
    if (numChars <= 0)
    {
        numChars = static_cast<int>(text.size());
    }
    if (numChars == 0)
    {
        numChars = 1;
    }

    const float charWidth = bounds.width / static_cast<float>(numChars);
    for (int i = 0; i < numChars; ++i)
    {
        const char c = i < static_cast<int>(text.size()) ? static_cast<char>(text[i]) : ' ';
        const std::uint16_t mask = FourteenSegment::GetSegmentMask(c);
        FourteenSegment::AppendCharacter(
            commands,
            {bounds.x + static_cast<float>(i) * charWidth, bounds.y, charWidth, bounds.height},
            mask,
            onColor,
            offColor,
            segmentThickness,
            segmentGap);
    }
    return commands;
}

inline std::string UpperShortLabel(std::string_view label, std::size_t maxChars = 4)
{
    std::string upper;
    upper.reserve(maxChars);
    for (char c : label)
    {
        if (upper.size() >= maxChars)
        {
            break;
        }
        upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return upper;
}

inline void AppendBadge(std::vector<DrawCommand>& commands,
                        float x,
                        float y,
                        float length,
                        Color color,
                        std::string_view text)
{
    const Bounds rect{x, y, length, length};
    const float corner = length * 0.1f;
    commands.push_back(DrawCommand::FillRoundedRect(rect, corner, Color::Rgb(32, 34, 36)));
    commands.push_back(
        DrawCommand::FillRoundedRect({x + length * 0.07f, y + length * 0.07f, length * 0.86f, length * 0.86f},
                                                corner,
                                                Color::Rgba(color.r, color.g, color.b,
                                                                       static_cast<std::uint8_t>(color.a * 0.9f))));
    commands.push_back(
        DrawCommand::StrokeRoundedRect(rect, corner, Color::Rgba(0, 0, 0, 140), 1.0f));
    TextStyle style;
    style.size = length * 0.36f;
    style.color = Color::Rgb(255, 255, 255);
    style.align = TextAlign::Center;
    commands.push_back(DrawCommand::Text(rect, std::string(text), style));
}

inline void AppendMotionIndicator(std::vector<DrawCommand>& commands,
                                  float centerX,
                                  float centerY,
                                  float radius,
                                  float value,
                                  float displaySpread,
                                  Color indicatorColor)
{
    const EncoderGeometry::MotionIndicatorGeometry geometry =
        EncoderGeometry::MotionIndicatorGeometryFor(radius, value, displaySpread);

    const auto appendLayer = [&](float strokeWidth, Color color) {
        commands.push_back(DrawCommand::Arc(
            EncoderGeometry::ArcBoundsFor(centerX, centerY, radius),
            EncoderGeometry::ValueToArcAngle(geometry.startValue),
            EncoderGeometry::ValueToArcAngle(geometry.endValue),
            color,
            strokeWidth));
    };

    appendLayer(geometry.outlineStrokeWidth,
                Color::Rgba(0, 0, 0, static_cast<std::uint8_t>(geometry.outlineAlpha * 255.0f)));
    appendLayer(geometry.outerStrokeWidth,
                Color::Rgba(indicatorColor.r,
                                       indicatorColor.g,
                                       indicatorColor.b,
                                       static_cast<std::uint8_t>(geometry.outerAlpha * 255.0f)));
    appendLayer(geometry.midStrokeWidth,
                Color::Rgba(indicatorColor.r,
                                       indicatorColor.g,
                                       indicatorColor.b,
                                       static_cast<std::uint8_t>(geometry.midAlpha * 255.0f)));
    appendLayer(geometry.coreStrokeWidth,
                Color::Rgba(indicatorColor.r,
                                       indicatorColor.g,
                                       indicatorColor.b,
                                       static_cast<std::uint8_t>(geometry.coreAlpha * 255.0f)));
}

inline std::vector<DrawCommand> BuildEncoderDrawCommands(const EncoderDrawState& state,
                                                                    Bounds nodeExtent)
{
    std::vector<DrawCommand> commands;
    if (!state.connected)
    {
        return commands;
    }

    constexpr float x_Inset = 4.0f;
    Bounds bounds{
        x_Inset,
        x_Inset,
        std::max(0.0f, nodeExtent.width - x_Inset * 2.0f),
        std::max(0.0f, nodeExtent.height - x_Inset * 2.0f),
    };

    const float centerX = bounds.x + bounds.width * 0.5f;
    const float displayCenterY = bounds.y + bounds.height * 0.5f;
    const float centerY = displayCenterY - bounds.height * 0.03f;
    const float baseRadius = std::min(bounds.width, bounds.height) * 0.43f;
    const Color cellColor = state.baseColor;

    const Bounds body{
        centerX - baseRadius * 1.08f,
        centerY - baseRadius * 1.08f,
        baseRadius * 2.16f,
        baseRadius * 2.16f,
    };
    commands.push_back(DrawCommand::FillEllipse(
        body,
        state.hasVisualizerUnderlay
            ? Color::Rgba(synth::kSurfaceBackground.r, synth::kSurfaceBackground.g,
                          synth::kSurfaceBackground.b, 150)
            : synth::kSurfaceBackground));
    {
        const float inset = baseRadius * 0.07f;
        commands.push_back(DrawCommand::FillEllipse(
            {body.x + inset, body.y + inset, body.width - inset * 2.0f, body.height - inset * 2.0f},
            synth::ScaleAlpha(state.baseColor, state.hasVisualizerUnderlay ? 0.14f : 0.28f)));
    }
    commands.push_back(DrawCommand::StrokeEllipse(body, synth::ScaleAlpha(state.baseColor, 0.9f), 1.5f));
    if (state.wantsFrame)
    {
        commands.push_back(DrawCommand::StrokeRoundedRect(
            {bounds.x + 1.0f, bounds.y + 1.0f, bounds.width - 2.0f, bounds.height - 2.0f},
            6.0f,
            Color::Rgb(8, 9, 10),
            1.0f));
    }

    const auto drawBadges = [&](std::uint64_t mask, bool upper, bool modulator) {
        const std::vector<synth::Color>& colors = modulator ? state.modulatorColors : state.gestureColors;
        const std::uint64_t validMask = colors.size() >= 64
                                            ? std::numeric_limits<std::uint64_t>::max()
                                            : (colors.empty() ? 0u : (std::uint64_t{1} << colors.size()) - 1u);
        assert((mask & ~validMask) == 0u && "badge mask index exceeds published color count");
        mask &= validMask;
        const std::size_t total = EncoderGeometry::CountMaskBits(mask);
        std::size_t badgeIndex = 0;
        for (std::size_t bit = 0; bit < colors.size() && badgeIndex < total; ++bit)
        {
            if ((mask & (std::uint64_t{1} << bit)) == 0)
            {
                continue;
            }

            float x = 0.0f;
            float y = 0.0f;
            float length = 0.0f;
            EncoderGeometry::GetBadgePosition(
                centerX, centerY, baseRadius * 0.72f, badgeIndex, total, upper, x, y, length);
            AppendBadge(commands, x, y, length, colors[bit], EncoderGeometry::BadgeText(modulator, bit));
            ++badgeIndex;
        }
    };

    drawBadges(state.modulatorsAffectingMask, true, true);
    drawBadges(state.gesturesAffectingMask, false, false);

    for (std::size_t voiceIx = 0; voiceIx < state.voiceCount; ++voiceIx)
    {
        const float radius = baseRadius - static_cast<float>(voiceIx) * baseRadius * 0.12f;
        EncoderGeometry::AppendArcWithSwitchGaps(
            commands, centerX, centerY, radius, 0.0f, 1.0f, state.switchValues, Color::Rgba(255, 255, 255, 61),
            2.0f);
    }

    for (std::size_t voiceIx = 0; voiceIx < state.voiceCount; ++voiceIx)
    {
        const EncoderVoiceDrawState& voice = state.voices[voiceIx];
        const float radius = baseRadius - static_cast<float>(voiceIx) * baseRadius * 0.12f;
        const float minValue = EncoderGeometry::NormalizeForDisplay(voice.minValue, state.bipolar);
        const float maxValue = EncoderGeometry::NormalizeForDisplay(voice.maxValue, state.bipolar);
        EncoderGeometry::AppendArcWithSwitchGaps(commands,
                                                 centerX,
                                                 centerY,
                                                 radius,
                                                 minValue,
                                                 maxValue,
                                                 state.switchValues,
                                                 synth::ScaleAlpha(voice.indicatorColor, 0.74f),
                                                 3.2f);
    }

    for (std::size_t voiceIx = 0; voiceIx < state.voiceCount; ++voiceIx)
    {
        const EncoderVoiceDrawState& voice = state.voices[voiceIx];
        const float radius = baseRadius - static_cast<float>(voiceIx) * baseRadius * 0.12f;
        const float value = EncoderGeometry::NormalizeForDisplay(voice.value, state.bipolar);
        const Color indicatorColor = voice.indicatorColor;

        if (state.switchValues > 1)
        {
            float startValue = 0.0f;
            float endValue = 1.0f;
            EncoderGeometry::GetSwitchValueRange(
                std::min(voice.switchValue, state.switchValues - 1), state.switchValues, startValue, endValue);
            EncoderGeometry::AppendArcWithSwitchGaps(commands,
                                                     centerX,
                                                     centerY,
                                                     radius,
                                                     startValue,
                                                     endValue,
                                                     state.switchValues,
                                                     synth::Brighten(indicatorColor, 0.35f),
                                                     4.4f);
        }
        else
        {
            const float displaySpread =
                state.bipolar ? voice.spreadValue * 0.5f : voice.spreadValue;
            AppendMotionIndicator(commands, centerX, centerY, radius, value, displaySpread, indicatorColor);
        }
    }

    const float displayHeight = Clamp(baseRadius * 0.34f, 14.0f, 24.0f);
    const float displayWidth = displayHeight * 3.3f;
    const Bounds displayBounds{
        centerX - displayWidth / 2.0f,
        displayCenterY + baseRadius * 0.54f,
        displayWidth,
        displayHeight,
    };
    const std::string label = UpperShortLabel(state.shortLabel);
    const Color onColor = synth::Brighten(cellColor, 0.45f);
    const Color offColor = Color::Rgb(36, 40, 42);
    auto segmentCommands = BuildFourteenSegmentCommands(label, displayBounds, onColor, offColor);
    commands.insert(commands.end(), segmentCommands.begin(), segmentCommands.end());

    return commands;
}

}  // namespace synth::ui
