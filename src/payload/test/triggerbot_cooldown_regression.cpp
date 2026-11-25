#include "modules/combat/triggerbot/cooldown_logic.h"
#include <limits>
#include <iostream>
int main() {
 using namespace dt::modules::triggerbot::cooldown;
 if (threshold(100,true,-6)!=1.0f || threshold(97,true,-6)<0.97f || threshold(97,true,2)<0.99f) return 1;
 if (ready(0.97f,1.0f) || !ready(1.0f,1.0f) || !ready(0.97f,0.97f)) return 2;
 if (ready(std::numeric_limits<float>::quiet_NaN(),1.0f)) return 3;
 for (int percent = 1; percent <= 100; ++percent) {
   const float selected = threshold(static_cast<float>(percent), false, 0.0);
   if (selected != static_cast<float>(percent / 100.0)) return 4;
   if (!ready(selected, selected) || ready(std::nextafter(selected, 0.0f), selected)) return 5;
 }
 std::cout << "Cooldown thresholds and revalidation: OK\n";
}
