#include "modules/combat/triggerbot/critical_logic.h"
#include <limits>
#include <iostream>

using vanta::modules::triggerbot::critical::can_attack;

int main() {
    if (!can_attack(false, false, false, false, false, false, -0.001, 0.001)) return 1;
    if (can_attack(true, false, false, false, false, false, -0.2, 0.3)) return 2;
    if (can_attack(false, true, false, false, false, false, -0.2, 0.3)) return 3;
    if (can_attack(false, false, true, false, false, false, -0.2, 0.3)) return 4;
    if (can_attack(false, false, false, true, false, false, -0.2, 0.3)) return 5;
    if (can_attack(false, false, false, false, true, false, -0.2, 0.3)) return 6;
    if (can_attack(false, false, false, false, false, true, -0.2, 0.3)) return 7;
    if (can_attack(false, false, false, false, false, false, 0.2, 0.3)) return 8;
    if (can_attack(false, false, false, false, false, false, 0, 0.3)) return 9;
    if (can_attack(false, false, false, false, false, false, -0.2, 0)) return 10;
    if (can_attack(false, false, false, false, false, false, std::numeric_limits<double>::quiet_NaN(), 0.3)) return 11;
    if (can_attack(false, false, false, false, false, false, -0.2, std::numeric_limits<double>::infinity())) return 12;
    std::cout << "Strict critical eligibility: OK\n";
}
