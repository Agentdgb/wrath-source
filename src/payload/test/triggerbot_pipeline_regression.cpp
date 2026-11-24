#include "modules/combat/triggerbot/recharge_logic.h"
#include "modules/combat/triggerbot/pending_click_logic.h"
#include "modules/combat/triggerbot/critical_logic.h"
#include "modules/combat/triggerbot/cooldown_logic.h"
#include "modules/combat/triggerbot/weapon_logic.h"
#include <cstdio>
#include <limits>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"failed line %d: %s\n",__LINE__,#x); return 1; } } while(false)
int main() {
    using namespace vanta::modules::triggerbot;
    RechargeConfirmation recharge;
    recharge.after_attack(0.94f, 0.f, 100);
    CHECK(!recharge.blocked(0.f, 100)); // real reset adds no fixed delay
    CHECK(!cooldown::ready(0.93f, 0.94f));
    CHECK(cooldown::ready(0.94f, 0.94f));
    recharge.after_attack(1.f, 1.f, 200); // stale read during an unregistered attack
    CHECK(recharge.blocked(1.f, 201));
    CHECK(recharge.blocked(1.f, 249));
    CHECK(!recharge.blocked(0.08f, 249)); // observed reset releases immediately
    recharge.after_attack(1.f, 1.f, 700);
    CHECK(!recharge.blocked(1.f, 750)); // lost attack must not stick forever
    recharge.after_attack(0.94f, 0.f, 751);
    CHECK(!recharge.blocked(0.f, 751)); // successful retry adds no extra delay
    recharge.after_attack(0.f, 0.f, 1300);
    CHECK(!recharge.blocked(0.f, 1300)); // reset already at zero
    recharge.after_attack(1.f, std::numeric_limits<float>::quiet_NaN(), 1400);
    CHECK(recharge.blocked(std::numeric_limits<float>::quiet_NaN(), 1401));
    recharge.clear();
    CHECK(!recharge.blocked(1.f, 1402));
    CHECK(is_weapon_key("item.minecraft.netherite_sword") == true);
    CHECK(is_weapon_key("item.minecraft.diamond_axe") == true);
    CHECK(is_weapon_key("item.minecraft.diamond_pickaxe") == false);
    CHECK(is_weapon_key("item.minecraft.mace") == true);
    CHECK(is_weapon_key("item.minecraft.trident") == true);
    CHECK(is_weapon_key("item.minecraft.bread") == false);
    for (int percent = 1; percent <= 100; ++percent) {
        const float chosen = cooldown::threshold(static_cast<float>(percent), false, 0);
        CHECK(cooldown::ready(chosen, chosen));
        CHECK(!critical::can_attack(false, false, false, false, false, false, 0.2, 0)); // rising
        CHECK(critical::can_attack(false, false, false, false, false, false, -0.001, 0.001));
        CHECK(!critical::can_attack(true, false, false, false, false, false, 0, 0)); // landed
    }
    CHECK(cooldown::ready(0.91f, critical::charge_floor(false)));
    CHECK(!cooldown::ready(0.91f, critical::charge_floor(true)));
    CHECK(cooldown::ready(0.94f, critical::charge_floor(true)));
    pending_click::State pending;
    const auto hit = pending.reserve(2000, 0.94f, false);
    pending.begin_attack();
    CHECK(pending.consume().id == hit);
    recharge.after_attack(0.94f, 0.f, 2000);
    CHECK(pending.reserve(2001, 0.94f, false) == 0);
    pending.end_attack();
    CHECK(!recharge.blocked(0.08f, 2050));
    CHECK(!cooldown::ready(0.08f, 0.94f)); // no second hit right after a critical
    pending.clear(); // target lost: no queued target survives
    CHECK(!pending.pending(2050,500));
    std::puts("Trigger pipeline: reset acknowledgement, critical/landing, weapon classification and duplicate dispatch: OK");
}
