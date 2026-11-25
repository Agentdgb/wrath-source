#pragma once

#include <cstdint>

namespace dt::modules::triggerbot::pending_click {

// The caller serializes access. A generation lets a failed SendInput cancel
// only its own reservation, even if the game has already consumed it.
struct Click {
    std::uint64_t id = 0;
    std::uint64_t sent_ms = 0;
    float cooldown_target = 1.0f;
    bool miss = false;
};

class State {
public:
    bool pending(std::uint64_t now, std::uint64_t window) noexcept {
        if (click_.id && now >= click_.sent_ms && now - click_.sent_ms > window)
            click_ = {};
        return click_.id != 0 || attack_depth_ != 0;
    }

    std::uint64_t reserve(std::uint64_t now, float cooldown_target, bool miss) noexcept {
        if (click_.id || attack_depth_) return 0;
        if (++next_id_ == 0) ++next_id_;
        click_ = {next_id_, now, cooldown_target, miss};
        return click_.id;
    }

    Click consume() noexcept {
        const Click result = click_;
        click_ = {};
        return result;
    }

    void cancel(std::uint64_t id) noexcept {
        if (id && click_.id == id) click_ = {};
    }

    // While vanilla is executing an attack, its cooldown may still be charged.
    // Keep producers out until the original call has reset that cooldown.
    void begin_attack() noexcept { ++attack_depth_; }
    void end_attack() noexcept { if (attack_depth_) --attack_depth_; }

    void clear() noexcept { click_ = {}; }

private:
    unsigned int attack_depth_ = 0;
    Click click_{};
    std::uint64_t next_id_ = 0;
};

}
