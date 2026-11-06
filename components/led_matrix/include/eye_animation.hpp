#pragma once

#include <algorithm>
#include <cmath>

// Pure math port of app/eyes/animations.py; functions do not sleep or access hardware.
namespace eye_animation {
constexpr float kPi = 3.14159265358979323846F;
inline float clamp01(float t) { return std::clamp(t, 0.0F, 1.0F); }
inline float lerp(float a, float b, float t) { return a + (b - a) * clamp01(t); }
inline float ease_in_out(float t) { t = clamp01(t); return t * t * (3.0F - 2.0F * t); }
inline float ease_out_elastic(float t) {
    t = clamp01(t);
    if (t == 0.0F || t == 1.0F) { return t; }
    constexpr float p = 0.3F;
    return std::pow(2.0F, -10.0F * t) * std::sin((t - p / 4.0F) * (2.0F * kPi) / p) + 1.0F;
}
inline float calculate_blink_progress(float elapsed, float duration) {
    if (duration <= 0.0F) { return 1.0F; }
    const float t = clamp01(elapsed / duration);
    return t < 0.5F ? 1.0F - ease_in_out(t * 2.0F) : ease_in_out((t - 0.5F) * 2.0F);
}
inline float smoothstep(float edge0, float edge1, float x) {
    return edge1 == edge0 ? 0.0F : ease_in_out((x - edge0) / (edge1 - edge0));
}
inline float ease_out_cubic(float t) { t = 1.0F - clamp01(t); return 1.0F - t * t * t; }
inline float ease_in_cubic(float t) { t = clamp01(t); return t * t * t; }
} // namespace eye_animation
