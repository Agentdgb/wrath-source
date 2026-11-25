#pragma once
#include <string_view>
#include <cmath>

namespace dt::modules::shieldbreaker {
inline bool shield_faces_attacker(float yaw, double dx, double dz) noexcept {
    if (!std::isfinite(yaw) || !std::isfinite(dx) || !std::isfinite(dz)) return false;
    const double radians = static_cast<double>(yaw) * 0.017453292519943295;
    return -std::sin(radians) * dx + std::cos(radians) * dz > 1.e-8;
}
inline int axe_rank(std::string_view key) noexcept {
    constexpr std::string_view keys[] = {
        "item.minecraft.wooden_axe", "item.minecraft.golden_axe",
        "item.minecraft.stone_axe", "item.minecraft.copper_axe",
        "item.minecraft.iron_axe", "item.minecraft.diamond_axe",
        "item.minecraft.netherite_axe"
    };
    for (int i = 0; i < 7; ++i)
        if (key == keys[i]) return i;
    return -1;
}
inline bool is_sword(std::string_view key) noexcept {
    return key.starts_with("item.minecraft.") && key.ends_with("_sword");
}
inline bool should_restore(int current, int axe, int original) noexcept {
    return original >= 0 && original < 9 && axe >= 0 && axe < 9
        && original != axe && current == axe;
}
}
