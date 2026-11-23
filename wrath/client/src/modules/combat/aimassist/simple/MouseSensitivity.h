#pragma once
#include "AimModel.h"
namespace readableaim {
inline double mouseAngularStep(double sensitivity) {
    const double base = std::clamp(sensitivity, 0.0, 1.0) * .6 + .2;
    return base * base * base * 8 * .15;
}
// Keep fractional counts between frames instead of snapping tiny corrections back and forth.
class MouseSensitivityOutput {
    double yawRemainder = 0, pitchRemainder = 0, previousStep = 0;
    static double axis(double correction, double error, double step, double &remainder) {
        if (std::abs(error) <= step || correction * error <= 0) { remainder = 0; return 0; }
        if (remainder * error < 0) remainder = 0;
        const double total = correction + remainder;
        const double maximum = std::floor(std::abs(error) / step);
        const double counts = std::copysign(std::min(std::abs(std::trunc(total / step)), maximum), error);
        const double applied = counts * step;
        remainder = std::clamp(total-applied, -step, step);
        return applied;
    }
  public:
    void reset() { yawRemainder = pitchRemainder = previousStep = 0; }
    Angles apply(Angles current, Angles requested, Angles desired, bool vertical, double sensitivity) {
        const double step = mouseAngularStep(sensitivity);
        if (previousStep != step) { reset(); previousStep = step; }
        const double yaw = axis(wrapDegrees(requested.yaw-current.yaw), wrapDegrees(desired.yaw-current.yaw), step, yawRemainder);
        const double pitch = vertical ? axis(requested.pitch-current.pitch, desired.pitch-current.pitch, step, pitchRemainder) : 0;
        if (!vertical) pitchRemainder = 0;
        return {static_cast<float>(current.yaw+yaw),
                vertical ? std::clamp(static_cast<float>(current.pitch+pitch), -90.f, 90.f) : current.pitch};
    }
};
} // namespace readableaim
