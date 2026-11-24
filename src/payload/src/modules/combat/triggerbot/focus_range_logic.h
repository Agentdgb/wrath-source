#pragma once
#include <cmath>

namespace vanta::modules::triggerbot::focus_range {
inline bool contains(double distance, float range) noexcept {
    return std::isfinite(distance) && std::isfinite(range) &&
           distance >= 0.0 && range >= 1.0f && distance <= static_cast<double>(range);
}
}
