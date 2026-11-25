#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace dt::modules::throwpot {
struct Potion { int slot = -1; int healing = 0; };
struct Plan { std::array<Potion, 2> potions{}; unsigned count = 0; };
inline constexpr unsigned maxThrows = 2;

// Keep the same healing budget, but use an already-held planned potion first.
// This avoids a slot packet and its required selection tick.
inline Plan preferHeld(Plan plan, int heldSlot) noexcept {
    if (plan.count == 2 && plan.potions[1].slot == heldSlot)
        std::swap(plan.potions[0], plan.potions[1]);
    return plan;
}

inline bool needsHealing(float health, float maximum) noexcept {
    return std::isfinite(health) && std::isfinite(maximum) && health > 0.f
        && maximum > health;
}

// Plan once per press, using at most two distinct hotbar slots. Never expand
// the plan because health updates arrive late or damage arrives mid-action.
inline Plan randomPlan(std::span<const Potion> candidates, float health,
                       float maximum, std::uint32_t random) noexcept {
    Plan single, pair;
    if (!needsHealing(health, maximum)) return {};
    int singleHealing = 0, pairHealing = 0;
    const auto count = (std::min)(candidates.size(), std::size_t{9});
    for (std::size_t i = 0; i < count; ++i) {
        const auto first = candidates[i];
        if (first.slot < 0 || first.slot > 8 || first.healing <= 0 || first.healing > 8) continue;
        if (first.healing > singleHealing || (first.healing == singleHealing && ((random >> i) & 1u))) {
            single = {{first, {}}, 1}; singleHealing = first.healing;
        }
        for (std::size_t j = i + 1; j < count; ++j) {
            const auto second = candidates[j];
            if (second.slot < 0 || second.slot > 8 || second.slot == first.slot
                || second.healing <= 0 || second.healing > 8 || first.healing > 8) continue;
            const int healing = first.healing + second.healing;
            if (healing > pairHealing || (healing == pairHealing && ((random >> j) & 1u))) {
                pair = {{first, second}, 2}; pairHealing = healing;
            }
        }
    }
    // Do not spend a second potion when one already completes the estimated heal.
    // Otherwise use the best pair, even when the final potion tops health off.
    if (health + singleHealing >= maximum) return single;
    return pair.count ? pair : single;
}

// Double Pot uses the strongest available pair without randomized tie-breaking.
// The same healing budget avoids a second throw when one already fills health.
inline Plan doublePlan(std::span<const Potion> candidates, float health,
                       float maximum) noexcept {
    return randomPlan(candidates, health, maximum, 0);
}
}
