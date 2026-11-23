#pragma once
#include <cstdint>

namespace vanta::modules::throwpot {

// Keep a floor between consecutive uses and before restoring after a use.
// Selecting a slot BEFORE using its potion is valid in the same input tick.
inline constexpr int slotPauseMs = 50;

// Cooldown 1 is fastest and always retains a nonzero interval.
inline constexpr int throwDelayMs(int cooldown, bool randomized, std::uint32_t random = 0) noexcept {
    const int bounded = cooldown < 1 ? 1 : (cooldown > 10 ? 10 : cooldown);
    const int base = bounded * 50;
    // At the fastest setting, jitter would miss the next 50 ms tick and
    // effectively double the cooldown. Keep that setting deterministic.
    return base + (randomized && bounded > 1
        ? static_cast<int>(random % static_cast<std::uint32_t>(base / 4 + 1)) : 0);
}

} // namespace vanta::modules::throwpot
