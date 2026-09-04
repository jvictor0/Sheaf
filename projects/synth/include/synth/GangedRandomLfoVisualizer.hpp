#pragma once

#include "synth/DspRandomLfo.hpp"
#include "synth/PortableUI.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace synth::ui {

namespace GangedRandomLfoGeometry {

inline constexpr std::size_t kPathSegments = 64;
inline constexpr float kInset = 4.0f;
inline constexpr float kDotRadius = 3.0f;

template<std::size_t VoiceCount>
constexpr std::size_t MaximumCommandCount()
{
    constexpr std::size_t futureDashCommands = (kPathSegments + 1) / 2;
    constexpr std::size_t commandsPerVoice = 1 + futureDashCommands + 1;
    return 2 + VoiceCount * commandsPerVoice;
}

} // namespace GangedRandomLfoGeometry

namespace ganged_random_lfo_detail {

struct VoiceTiming {
    double waitingSamples = 0.0;
    double movingSamples = 0.0;
    double totalSamples = 0.0;
};

inline bool FiniteBounds(Bounds bounds)
{
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) &&
           std::isfinite(bounds.width) && std::isfinite(bounds.height) &&
           bounds.width >= 0.0f && bounds.height >= 0.0f;
}

inline Bounds PlotBounds(Bounds bounds)
{
    const float xInset = std::min(GangedRandomLfoGeometry::kInset, bounds.width * 0.5f);
    const float yInset = std::min(GangedRandomLfoGeometry::kInset, bounds.height * 0.5f);
    return {
        xInset,
        yInset,
        std::max(0.0f, bounds.width - 2.0f * xInset),
        std::max(0.0f, bounds.height - 2.0f * yInset),
    };
}

inline void AppendBackgroundAndAxis(Bounds bounds, std::vector<DrawCommand>& commands)
{
    if (!FiniteBounds(bounds))
    {
        return;
    }
    commands.push_back(DrawCommand::Fill({0.0f, 0.0f, bounds.width, bounds.height},
                                         Color::Rgb(12, 14, 16)));
    const Bounds plot = PlotBounds(bounds);
    commands.push_back(DrawCommand::Line(
        {plot.x, plot.y + plot.height * 0.5f},
        {plot.x + plot.width, plot.y + plot.height * 0.5f},
        Color::Rgb(42, 46, 48),
        1.0f));
}

inline bool ValidVoiceValue(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

inline bool ComputeTiming(const GangedRandomLfoVoiceSnapshot& voice, VoiceTiming& timing)
{
    if (!std::isfinite(voice.waitingIncrement) || voice.waitingIncrement <= 0.0 ||
        !std::isfinite(voice.movingIncrement) || voice.movingIncrement <= 0.0 ||
        !ValidVoiceValue(voice.source) || !ValidVoiceValue(voice.target) ||
        !ValidVoiceValue(voice.output) || !ValidVoiceValue(voice.shape) ||
        !std::isfinite(voice.currentStateProgress))
    {
        return false;
    }

    timing.waitingSamples = std::ceil(1.0 / voice.waitingIncrement);
    timing.movingSamples = std::ceil(1.0 / voice.movingIncrement);
    timing.totalSamples = timing.waitingSamples + timing.movingSamples;
    return std::isfinite(timing.waitingSamples) && timing.waitingSamples > 0.0 &&
           std::isfinite(timing.movingSamples) && timing.movingSamples > 0.0 &&
           std::isfinite(timing.totalSamples) && timing.totalSamples > 0.0;
}

inline float ValueAtSample(
    const GangedRandomLfoVoiceSnapshot& voice,
    const VoiceTiming& timing,
    double sample)
{
    if (sample <= timing.waitingSamples)
    {
        return voice.source;
    }
    if (sample >= timing.totalSamples)
    {
        return voice.target;
    }
    const double movingProgress = (sample - timing.waitingSamples) * voice.movingIncrement;
    return ShapedInterpolate(voice.source, voice.target, voice.shape, movingProgress);
}

inline Point PointAtSample(
    const GangedRandomLfoVoiceSnapshot& voice,
    const VoiceTiming& timing,
    double sample,
    double sharedDurationSamples,
    Bounds plot)
{
    const double normalizedX = std::clamp(sample / sharedDurationSamples, 0.0, 1.0);
    const float value = std::clamp(ValueAtSample(voice, timing, sample), 0.0f, 1.0f);
    return {
        std::clamp(
            plot.x + plot.width * static_cast<float>(normalizedX),
            plot.x,
            plot.x + plot.width),
        std::clamp(
            plot.y + plot.height * (1.0f - value),
            plot.y,
            plot.y + plot.height),
    };
}

inline Bounds ClippedDotBounds(Point center, Bounds plot, Bounds outer)
{
    const float radius = std::min({
        GangedRandomLfoGeometry::kDotRadius,
        plot.width * 0.5f,
        plot.height * 0.5f,
    });
    const float clampedX = std::clamp(center.x, radius, outer.width - radius);
    const float clampedY = std::clamp(center.y, radius, outer.height - radius);
    return {clampedX - radius, clampedY - radius, radius * 2.0f, radius * 2.0f};
}

} // namespace ganged_random_lfo_detail

template<std::size_t VoiceCount>
void BuildGangedRandomLfoCommands(
    const GangedRandomLfoSnapshot<VoiceCount>& snapshot,
    Bounds bounds,
    std::vector<DrawCommand>& commandBuffer,
    bool drawBackground = true)
{
    namespace detail = ganged_random_lfo_detail;

    if (!detail::FiniteBounds(bounds))
    {
        return;
    }
    const Bounds nodeExtent{0.0f, 0.0f, bounds.width, bounds.height};
    if (drawBackground)
    {
        detail::AppendBackgroundAndAxis(nodeExtent, commandBuffer);
    }
    if (nodeExtent.width <= 0.0f || nodeExtent.height <= 0.0f ||
        !std::isfinite(snapshot.sampleRate) || snapshot.sampleRate <= 0.0 ||
        !std::isfinite(snapshot.roundElapsedSamples) || snapshot.roundElapsedSamples < 0.0)
    {
        return;
    }

    std::array<detail::VoiceTiming, VoiceCount> timings{};
    double sharedDurationSeconds = 0.0;
    for (std::size_t voiceIndex = 0; voiceIndex < VoiceCount; ++voiceIndex)
    {
        if (!detail::ComputeTiming(snapshot.voices[voiceIndex], timings[voiceIndex]))
        {
            return;
        }
        const double durationSeconds = timings[voiceIndex].totalSamples / snapshot.sampleRate;
        if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0)
        {
            return;
        }
        sharedDurationSeconds = std::max(sharedDurationSeconds, durationSeconds);
    }
    if (!std::isfinite(sharedDurationSeconds) || sharedDurationSeconds <= 0.0)
    {
        return;
    }

    const double sharedDurationSamples = sharedDurationSeconds * snapshot.sampleRate;
    if (!std::isfinite(sharedDurationSamples) || sharedDurationSamples <= 0.0)
    {
        return;
    }
    const double presentSeconds = std::clamp(
        snapshot.roundElapsedSamples / snapshot.sampleRate,
        0.0,
        sharedDurationSeconds);
    const double presentSamples = presentSeconds * snapshot.sampleRate;
    const Bounds plot = detail::PlotBounds(nodeExtent);

    for (std::size_t voiceIndex = 0; voiceIndex < VoiceCount; ++voiceIndex)
    {
        const auto& voice = snapshot.voices[voiceIndex];
        const auto& timing = timings[voiceIndex];

        if (presentSamples > 0.0)
        {
            std::vector<Point> past;
            past.reserve(GangedRandomLfoGeometry::kPathSegments + 1);
            for (std::size_t segment = 0; segment <= GangedRandomLfoGeometry::kPathSegments; ++segment)
            {
                const double sample = presentSamples * static_cast<double>(segment) /
                                      static_cast<double>(GangedRandomLfoGeometry::kPathSegments);
                past.push_back(detail::PointAtSample(
                    voice, timing, sample, sharedDurationSamples, plot));
            }
            commandBuffer.push_back(DrawCommand::Polyline(std::move(past), voice.color, 1.4f));
        }

        if (presentSamples < sharedDurationSamples)
        {
            for (std::size_t segment = 0; segment < GangedRandomLfoGeometry::kPathSegments; segment += 2)
            {
                const double start = presentSamples +
                                     (sharedDurationSamples - presentSamples) *
                                         static_cast<double>(segment) /
                                         static_cast<double>(GangedRandomLfoGeometry::kPathSegments);
                const double end = presentSamples +
                                   (sharedDurationSamples - presentSamples) *
                                       static_cast<double>(segment + 1) /
                                       static_cast<double>(GangedRandomLfoGeometry::kPathSegments);
                std::vector<Point> dash;
                dash.reserve(2);
                dash.push_back(detail::PointAtSample(
                    voice, timing, start, sharedDurationSamples, plot));
                dash.push_back(detail::PointAtSample(
                    voice, timing, end, sharedDurationSamples, plot));
                commandBuffer.push_back(DrawCommand::Polyline(std::move(dash), voice.color, 1.4f));
            }
        }

        const Point dot = detail::PointAtSample(
            voice, timing, presentSamples, sharedDurationSamples, plot);
        const Bounds dotBounds = detail::ClippedDotBounds(dot, plot, nodeExtent);
        if (dotBounds.width > 0.0f && dotBounds.height > 0.0f)
        {
            commandBuffer.push_back(DrawCommand::FillEllipse(dotBounds, voice.color));
        }
    }
}

template<std::size_t VoiceCount>
class GangedRandomLfoVisualizer final : public Visualizer {
public:
    explicit GangedRandomLfoVisualizer(const GangedRandomLfoUiState<VoiceCount>& uiState, bool drawBackground = true)
        : m_uiState(uiState), m_drawBackground(drawBackground)
    {}

protected:
    std::vector<DrawCommand> DrawVisible() const override
    {
        GangedRandomLfoSnapshot<VoiceCount> snapshot;
        if (!m_uiState.ReadSnapshot(snapshot))
        {
            snapshot.sampleRate = 0.0;
        }
        std::vector<DrawCommand> commands;
        commands.reserve(GangedRandomLfoGeometry::MaximumCommandCount<VoiceCount>());
        const Bounds bounds = GetBounds();
        BuildGangedRandomLfoCommands(snapshot, bounds, commands, m_drawBackground);
        return commands;
    }

private:
    const GangedRandomLfoUiState<VoiceCount>& m_uiState;
    bool m_drawBackground;
};

} // namespace synth::ui
