#pragma once
#include <cstdint>

namespace vanta::modules::throwpot {

// Higher speed means less added time between throws. At 10 the tick ordering,
// rather than an artificial timer, determines when the next throw can happen.
inline constexpr int throwDelayMs(int speed, bool randomized,
                                  std::uint32_t random = 0) noexcept {
    const int bounded = speed < 1 ? 1 : (speed > 10 ? 10 : speed);
    const int base = (10 - bounded) * 25;
    if (!randomized || base == 0) return base;
    return base + static_cast<int>(random % static_cast<std::uint32_t>(base / 4 + 1));
}

// Selecting an already-held slot emits no slot change, so no tick is needed
// for that operation. A real slot change always precedes use by a tick.
inline constexpr bool canLaunch(unsigned tick, unsigned selectedTick,
                                bool slotChanged) noexcept {
    return !slotChanged || tick != selectedTick;
}

// Restore on the first subsequent tick, never alongside selection or use.
// Equality checks also work across the unsigned tick counter's wraparound.
inline constexpr bool canRestore(unsigned tick, unsigned selectedTick,
                                 unsigned lastUseTick) noexcept {
    return tick != selectedTick && tick != lastUseTick;
}

}
