#pragma once

#ifndef SNAP_DRAWING_TGFX_EXPERIMENT
#error "TgfxBridge.hpp is only available to the explicit TGFX experimental target"
#endif

#include "tgfx/core/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace snap::drawing {

/**
 * Owns a real TGFX value. The accessor spelling keeps the selected SnapDrawing
 * callers usable, but does not promise any Skia type or memory layout.
 */
template<typename T>
class WrappedTgfxValue {
public:
    template<typename... Args>
    explicit WrappedTgfxValue(Args&&... args) : _value(std::forward<Args>(args)...) {}

    constexpr T& getSkValue() {
        return _value;
    }

    constexpr const T& getSkValue() const {
        return _value;
    }

private:
    T _value;
};

inline tgfx::Color tgfxColorFromARGB(uint32_t value) {
    constexpr float scale = 1.0f / 255.0f;
    return {static_cast<float>((value >> 16) & 0xFF) * scale,
            static_cast<float>((value >> 8) & 0xFF) * scale,
            static_cast<float>(value & 0xFF) * scale,
            static_cast<float>((value >> 24) & 0xFF) * scale};
}

inline uint32_t argbFromTgfxColor(const tgfx::Color& color) {
    const auto component = [](float value) -> uint32_t {
        // Map NaN to zero as well, so the integer conversion is always defined.
        if (std::isnan(value)) {
            return 0;
        }
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return (component(color.alpha) << 24) | (component(color.red) << 16) |
           (component(color.green) << 8) | component(color.blue);
}

} // namespace snap::drawing
