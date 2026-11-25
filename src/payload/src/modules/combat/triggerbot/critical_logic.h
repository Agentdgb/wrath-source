#pragma once
#include <cmath>

namespace dt::modules::triggerbot::critical {
inline constexpr float charge_floor(bool on_ground) noexcept {
    return on_ground ? 0.94f : 0.91f;
}

inline bool can_attack(bool on_ground, bool sprinting, bool in_water, bool climbing,
                       bool passenger, bool blind, double velocity, double fall_distance) noexcept {
    return !on_ground && !sprinting && !in_water && !climbing && !passenger && !blind &&
           std::isfinite(velocity) && velocity < 0 &&
           std::isfinite(fall_distance) && fall_distance > 0;
}
}
