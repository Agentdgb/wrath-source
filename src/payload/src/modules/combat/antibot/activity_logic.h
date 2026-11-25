#pragma once
#include <cstdint>
namespace dt::modules::antibot {
// Input time and held movement keys only: entity motion and damage never arm it.
struct Activity {
    std::uint64_t last_ms = 0;
    std::uintptr_t window = 0;
    unsigned held_keys = 0;
    void reset() noexcept { last_ms = 0; window = 0; held_keys = 0; }
    void note(std::uint64_t now, std::uintptr_t source_window, bool hardware) noexcept {
        if (!hardware) return;
        if (window != source_window) held_keys = 0;
        window = source_window;
        last_ms = now;
    }
    void key(std::uint64_t now, std::uintptr_t source_window, unsigned mask,
             bool down, bool hardware) noexcept {
        if (!hardware || !mask) return;
        note(now, source_window, true);
        if (down) held_keys |= mask; else held_keys &= ~mask;
    }
    bool active(std::uint64_t now, std::uintptr_t foreground, std::uint64_t timeout) const noexcept {
        return window && window == foreground && last_ms && now >= last_ms &&
               (held_keys || now - last_ms < timeout);
    }
};
}
