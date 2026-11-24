#pragma once
#include <algorithm>
namespace vanta {
struct player_list_size { float width, height; };
inline player_list_size resize_player_list(float width, float height, float available_w, float available_h) {
    const float max_w = std::clamp(available_w, 1.f, 1400.f);
    const float max_h = std::clamp(available_h, 1.f, 1000.f);
    return {std::clamp(width, std::min(640.f,max_w),max_w), std::clamp(height,std::min(418.f,max_h),max_h)};
}
}
