#include "modules/combat/triggerbot/focus_range_logic.h"
#include <limits>
#include <iostream>
int main() {
    using vanta::modules::triggerbot::focus_range::contains;
    if (!contains(1.0, 1.f) || contains(1.01, 1.f)) return 1;
    if (!contains(30.0, 30.f) || contains(30.01, 30.f)) return 2;
    if (!contains(10.0, 30.f) || contains(10.0, 3.f)) return 3;
    if (contains(-1.0, 30.f) || contains(std::numeric_limits<double>::quiet_NaN(), 30.f)) return 4;
    std::cout << "Focus range boundaries and invalid distances: OK\n";
    return 0;
}
