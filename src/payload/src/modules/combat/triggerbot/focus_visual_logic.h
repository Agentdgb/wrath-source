#pragma once
#include <algorithm>
#include <cstdint>

namespace vanta::modules::triggerbot::focus_visual {
inline bool expired(std::uint64_t now, std::uint64_t started, float seconds) {
    return seconds > 0.f && now >= started && now - started >= static_cast<std::uint64_t>(seconds * 1000.f);
}
inline float arrow_size(double distance) {
    const float factor = std::clamp((static_cast<float>(distance) - 1.f) / 9.f, 0.f, 1.f);
    return 12.f + 22.f * factor;
}
}
