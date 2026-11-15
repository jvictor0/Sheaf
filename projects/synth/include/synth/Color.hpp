#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace synth {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    bool operator==(const Color& other) const = default;

    static constexpr Color Rgb(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
        return {red, green, blue, 255};
    }
    static constexpr Color Rgba(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                                std::uint8_t alpha) {
        return {red, green, blue, alpha};
    }

    constexpr std::uint32_t Packed() const {
        return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
               (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(a) << 24);
    }
    static constexpr Color FromPacked(std::uint32_t packed) {
        return {
            static_cast<std::uint8_t>(packed & 0xff),
            static_cast<std::uint8_t>((packed >> 8) & 0xff),
            static_cast<std::uint8_t>((packed >> 16) & 0xff),
            static_cast<std::uint8_t>((packed >> 24) & 0xff),
        };
    }

    Color AdjustBrightness(float scale) const;
    static Color FromHsvTurns(float hueTurns, float saturation, float value);
    static Color FromHsvDegrees(float hueDegrees, float saturation, float value);

    static const Color Off;
    static const Color White;
    static const Color Red;
    static const Color Orange;
    static const Color Yellow;
    static const Color Green;
    static const Color Cyan;
    static const Color Blue;
    static const Color Indigo;
    static const Color Grey;
};

// The app-wide surface background painted by the window root fill and the
// encoder cell fill. The browser site's CSS carries the same colour as
// `#121416` in browser/public/synth-browser.css (pinned by
// browser/tests/static-site.spec.ts), which cannot read this constant --
// change both together.
inline constexpr Color kSurfaceBackground = Color::Rgb(18, 20, 22);

struct HsvColor {
    float hueTurns = 0.0f;
    float saturation = 0.0f;
    float value = 0.0f;
};

inline Color ScaleAlpha(Color color, float scale) {
    if (!std::isfinite(scale)) {
        throw std::invalid_argument("alpha scale must be finite");
    }
    const float alpha = std::clamp(static_cast<float>(color.a) * scale, 0.0f, 255.0f);
    color.a = static_cast<std::uint8_t>(alpha);
    return color;
}

inline Color Darken(Color color, float scale) {
    if (!std::isfinite(scale)) {
        throw std::invalid_argument("darkening scale must be finite");
    }
    const auto channel = [scale](std::uint8_t value) {
        return static_cast<std::uint8_t>(
            std::clamp(std::round(static_cast<float>(value) * scale), 0.0f, 255.0f));
    };
    return Color::Rgba(channel(color.r), channel(color.g), channel(color.b), color.a);
}

inline Color Brighten(Color color, float amount) {
    if (!std::isfinite(amount)) {
        throw std::invalid_argument("brightening amount must be finite");
    }
    const float clampedAmount = std::max(0.0f, amount);
    const float factor = clampedAmount / (1.0f + clampedAmount);
    const auto channel = [factor](std::uint8_t value) {
        return static_cast<std::uint8_t>(std::clamp(
            static_cast<float>(value) + (255.0f - static_cast<float>(value)) * factor, 0.0f, 255.0f));
    };
    return Color::Rgba(channel(color.r), channel(color.g), channel(color.b), color.a);
}

inline Color Color::AdjustBrightness(float scale) const {
    return Darken(*this, scale);
}

inline Color Color::FromHsvTurns(float hueTurns, float saturation, float value) {
    if (!std::isfinite(hueTurns) || hueTurns < 0.0f || hueTurns >= 1.0f) {
        throw std::invalid_argument("HSV hue turns must be finite and in [0, 1)");
    }
    if (!std::isfinite(saturation) || !std::isfinite(value)) {
        throw std::invalid_argument("HSV saturation and value must be finite");
    }
    saturation = std::clamp(saturation, 0.0f, 1.0f);
    value = std::clamp(value, 0.0f, 1.0f);
    const float chroma = value * saturation;
    const float hueSector = hueTurns * 6.0f;
    const float x = chroma * (1.0f - std::fabs(std::fmod(hueSector, 2.0f) - 1.0f));
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    if (hueSector < 1.0f) {
        red = chroma;
        green = x;
    } else if (hueSector < 2.0f) {
        red = x;
        green = chroma;
    } else if (hueSector < 3.0f) {
        green = chroma;
        blue = x;
    } else if (hueSector < 4.0f) {
        green = x;
        blue = chroma;
    } else if (hueSector < 5.0f) {
        red = x;
        blue = chroma;
    } else {
        red = chroma;
        blue = x;
    }
    const float minimum = value - chroma;
    const auto byte = [minimum](float channel) {
        return static_cast<std::uint8_t>(
            std::clamp(std::round((channel + minimum) * 255.0f), 0.0f, 255.0f));
    };
    return Rgb(byte(red), byte(green), byte(blue));
}

inline Color Color::FromHsvDegrees(float hueDegrees, float saturation, float value) {
    if (!std::isfinite(hueDegrees)) {
        throw std::invalid_argument("HSV hue degrees must be finite");
    }
    float wrappedDegrees = std::fmod(hueDegrees, 360.0f);
    if (wrappedDegrees < 0.0f) {
        wrappedDegrees += 360.0f;
    }
    return FromHsvTurns(wrappedDegrees / 360.0f, saturation, value);
}

inline HsvColor ToHsv(Color color) {
    const float red = static_cast<float>(color.r) / 255.0f;
    const float green = static_cast<float>(color.g) / 255.0f;
    const float blue = static_cast<float>(color.b) / 255.0f;
    const float maximum = std::max({red, green, blue});
    const float minimum = std::min({red, green, blue});
    const float delta = maximum - minimum;
    float hueTurns = 0.0f;
    if (delta != 0.0f) {
        if (maximum == red) {
            hueTurns = std::fmod((green - blue) / delta, 6.0f);
        } else if (maximum == green) {
            hueTurns = ((blue - red) / delta) + 2.0f;
        } else {
            hueTurns = ((red - green) / delta) + 4.0f;
        }
        hueTurns /= 6.0f;
        if (hueTurns < 0.0f) {
            hueTurns += 1.0f;
        }
    }
    return {hueTurns, maximum == 0.0f ? 0.0f : delta / maximum, maximum};
}

inline const Color Color::Off = Color::Rgb(0, 0, 0);
inline const Color Color::White = Color::Rgb(255, 255, 255);
inline const Color Color::Red = Color::Rgb(255, 0, 0);
inline const Color Color::Orange = Color::Rgb(255, 128, 0);
inline const Color Color::Yellow = Color::Rgb(255, 220, 0);
inline const Color Color::Green = Color::Rgb(0, 200, 80);
inline const Color Color::Cyan = Color::Rgb(0, 255, 255);
inline const Color Color::Blue = Color::Rgb(0, 80, 255);
inline const Color Color::Indigo = Color::Rgb(75, 0, 130);
inline const Color Color::Grey = Color::Rgb(128, 128, 128);

static_assert(sizeof(Color) == sizeof(std::uint32_t));
static_assert(std::is_trivially_copyable_v<Color>);

}  // namespace synth
