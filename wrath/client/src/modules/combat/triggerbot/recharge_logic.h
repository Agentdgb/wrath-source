#pragma once
#include <cmath>
#include <cstdint>
namespace vanta::modules::triggerbot {
struct RechargeConfirmation {
    bool waiting = false;
    float before = 0.f;
    std::uint64_t retry_at = 0;
    void after_attack(float prior, float current, std::uint64_t now) noexcept {
        before = prior;
        waiting = !std::isfinite(current) || !std::isfinite(prior) || (current >= prior && current > 0.01f);
        retry_at = now + 50; // retry after one tick if Minecraft never registered the click
    }
    bool blocked(float current, std::uint64_t now) noexcept {
        if (waiting && ((std::isfinite(current) && current < before) || now >= retry_at)) waiting = false;
        return waiting;
    }
    void clear() noexcept { waiting = false; }
};
}
