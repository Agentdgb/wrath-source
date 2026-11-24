#pragma once
#include <cmath>
namespace vanta {
inline int numericDecimalPlaces(double step) {
    if (!std::isfinite(step) || step<=0) return 1;
    int places = 0;
    while (places<6 && std::abs(std::round(step)-step)>1e-6) { step*=10; ++places; }
    return places;
}
}
