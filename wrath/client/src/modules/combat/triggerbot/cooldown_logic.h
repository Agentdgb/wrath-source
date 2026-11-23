#pragma once
#include <algorithm>
#include <cmath>
namespace vanta::modules::triggerbot::cooldown {
inline float threshold(float percent, bool randomized, double offset) noexcept {
    if (!std::isfinite(percent)) return 1.0f;
    const double base = std::clamp(static_cast<double>(percent), 1.0, 100.0);
    const double extra = randomized && std::isfinite(offset) ? std::max(0.0, offset) : 0.0;
    return static_cast<float>(std::min(100.0, base + extra) / 100.0);
}
inline bool ready(float progress, float target) noexcept {
    return std::isfinite(progress) && std::isfinite(target) && target >= 0.01f && target <= 1.0f && progress >= target;
}
}
