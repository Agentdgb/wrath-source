#pragma once
#include "Controllers.h"
#include <array>

namespace readableaim {
struct Point {
    double x = 0, y = 0, z = 0;
};
struct Bounds {
    Point minimum, maximum;
};
struct Angles {
    float yaw = 0, pitch = 0;
};
enum class AimMode { Wind, Simple };
enum class AimPosition { Center, Random };
enum class Priority { Mouse, Distance };
enum class FocusMode { Auto, Keybind, Off };
enum class TrackingStyle { Soft, Balanced, Strong };
struct Settings {
    AimMode mode = AimMode::Simple;
    AimPosition position = AimPosition::Center;
    Priority priority = Priority::Mouse;
    FocusMode focusMode = FocusMode::Auto;
    TrackingStyle trackingStyle = TrackingStyle::Balanced;
    double smoothing = 50;
    double speed1 = 20, speed2 = 30;
    double prediction = 0.091, distance = 5, fov = 100, maxFocusDistance = 100;
    bool players = true, mobs = false, animals = false, crystals = false, villagers = false;
    bool onlyWeapon = false, vertical = false, pauseOnEat = true, teams = false;
    bool stopOnHit = false, onlyOnHit = false;
    bool clampDistance = false;
    double stopOnHitMs = 400.0;
    static double finite(double value, double low, double high, double fallback) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    }
    void sanitize() {
        smoothing = finite(smoothing, 0, 100, 50);
        if (trackingStyle != TrackingStyle::Soft && trackingStyle != TrackingStyle::Balanced &&
            trackingStyle != TrackingStyle::Strong)
            trackingStyle = TrackingStyle::Balanced;
        speed1 = finite(speed1, 1, 100, 20);
        speed2 = finite(speed2, 1, 100, 30);
        prediction = finite(prediction, 0, 0.5, 0.091);
        distance = clampDistance ? finite(distance, 1, 10, 3.5) : finite(distance, 1, 1000, 3.5);
        fov = finite(fov, 10, 360, 100);
        maxFocusDistance = clampDistance ? finite(maxFocusDistance, 1, 100, distance) : finite(maxFocusDistance, 1, 1000, 100);
        stopOnHitMs = finite(stopOnHitMs, 0, 5000, 400);
        if (mode != AimMode::Wind && mode != AimMode::Simple)
            mode = AimMode::Simple;
        if (position != AimPosition::Center && position != AimPosition::Random)
            position = AimPosition::Center;
        if (priority != Priority::Mouse && priority != Priority::Distance)
            priority = Priority::Mouse;
        if (focusMode != FocusMode::Auto && focusMode != FocusMode::Keybind && focusMode != FocusMode::Off)
            focusMode = FocusMode::Auto;
    }
};
inline float wrapDegrees(float value) {
    value = std::fmod(value, 360.0f);
    if (value >= 180)
        value -= 360;
    if (value < -180)
        value += 360;
    return value;
}
inline double distanceSquared(Point a, Point b) {
    return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
}
inline Angles anglesTo(Point eye, Point point) {
    constexpr double degrees = 57.29577951308232;
    const double x = point.x - eye.x, y = point.y - eye.y, z = point.z - eye.z;
    return {static_cast<float>(std::atan2(z, x) * degrees - 90),
            static_cast<float>(-std::atan2(y, std::hypot(x, z)) * degrees)};
}
inline double angleError(Angles camera, Angles desired, bool vertical) {
    return std::hypot(static_cast<double>(wrapDegrees(desired.yaw - camera.yaw)),
                      vertical ? static_cast<double>(desired.pitch - camera.pitch) : 0.0);
}
inline Bounds predict(Bounds box, Point velocity, double seconds) {
    const Point displacement{velocity.x * seconds * 20, velocity.y * seconds * 20, velocity.z * seconds * 20};
    box.minimum.x += displacement.x;
    box.maximum.x += displacement.x;
    box.minimum.y += displacement.y;
    box.maximum.y += displacement.y;
    box.minimum.z += displacement.z;
    box.maximum.z += displacement.z;
    return box;
}
inline Point aimPoint(Bounds box, Point eye, Angles camera, const Settings &settings, Point random) {
    const Point center{(box.minimum.x + box.maximum.x) * 0.5, (box.minimum.y + box.maximum.y) * 0.5,
                       (box.minimum.z + box.maximum.z) * 0.5};
    const auto lerp = [](double fraction, double low, double high) { return low + fraction * (high - low); };
    if (settings.position == AimPosition::Random) {
        return {lerp(random.x, box.minimum.x, box.maximum.x), lerp(random.y, box.minimum.y, box.maximum.y),
                lerp(random.z, box.minimum.z, box.maximum.z)};
    }
    constexpr double radians = 0.017453292519943295;
    const double cosine = std::cos(camera.pitch * radians);
    const Point direction{-std::sin(camera.yaw * radians) * cosine, -std::sin(camera.pitch * radians),
                          std::cos(camera.yaw * radians) * cosine};
    const double projection = std::max(0.0, (center.x - eye.x) * direction.x + (center.y - eye.y) * direction.y +
                                                (center.z - eye.z) * direction.z);
    const Point nearest{
        std::clamp(eye.x + direction.x * projection, box.minimum.x + 0.03, box.maximum.x - 0.03),
        std::clamp(eye.y + direction.y * projection, box.minimum.y + 0.03, box.maximum.y - 0.03),
        std::clamp(eye.z + direction.z * projection, box.minimum.z + 0.03, box.maximum.z - 0.03)};
    // Dynamic pursuit pull toward the center tracks the rival's moving core while preserving continuous micro-feel.
    const double x = std::lerp(nearest.x, center.x, 0.72);
    const double z = std::lerp(nearest.z, center.z, 0.72);
    const double originalDistance = std::hypot(nearest.x - eye.x, nearest.z - eye.z);
    const double centeredDistance = std::hypot(x - eye.x, z - eye.z);
    const double y = originalDistance > 1.0e-6
        ? eye.y + (nearest.y - eye.y) * centeredDistance / originalDistance : nearest.y;
    return {x, std::clamp(y, box.minimum.y + 0.03, box.maximum.y - 0.03), z};
}
// Predict a point, not a separate target box. Keep it inside the current hitbox.
inline Point constrainedAimPoint(Bounds box, Point velocity, Point eye, Angles camera, const Settings &settings,
                                 Point random) {
    Point point = aimPoint(box, eye, camera, settings, random);
    const auto clampAxis = [](double value, double low, double high) {
        const double margin = std::min(0.03, (high - low) * .25);
        return std::clamp(value, low + margin, high - margin);
    };
    point.x = clampAxis(point.x + velocity.x * settings.prediction * 20, box.minimum.x, box.maximum.x);
    point.y = clampAxis(point.y + velocity.y * settings.prediction * 20, box.minimum.y, box.maximum.y);
    point.z = clampAxis(point.z + velocity.z * settings.prediction * 20, box.minimum.z, box.maximum.z);
    return point;
}
// Native mouse updates can arrive in isolated batches between presented frames.
// Confirm small away movements before handing them to the Java-compatible latch.
class ManualReleaseFilter {
  public:
    void reset() {
        horizontal = {};
        vertical = {};
    }
    Correction update(double yawError, double pitchError, double manualYaw, double manualPitch, bool verticalEnabled,
                      double seconds) {
        return {filter(horizontal, yawError, manualYaw, seconds),
                verticalEnabled ? filter(vertical, pitchError, manualPitch, seconds) : 0};
    }

  private:
    struct Axis {
        double movement = 0, duration = 0, quiet = 0;
    };
    Axis horizontal, vertical;
    static double filter(Axis &axis, double error, double movement, double seconds) {
        const bool away = std::abs(error) > 12 && error * movement < 0;
        if (!away && error * movement > 0) {
            axis = {};
            return movement;
        }
        if (std::abs(error) <= 12) {
            axis = {};
            return movement;
        }
        if (axis.movement > 0)
            axis.duration += seconds;
        if (away && std::abs(movement) > 0.01) {
            if (axis.movement == 0)
                axis.duration = seconds;
            axis.movement += std::abs(movement);
            axis.quiet = 0;
            if (std::abs(movement) >= 8 || (axis.movement >= 3 && axis.duration >= 0.04)) {
                axis = {};
                // The latch's per-frame threshold must not undo accumulated evidence.
                return std::copysign(std::max(std::abs(movement), std::max(0.8, seconds * 60) + 0.01), movement);
            }
        } else {
            axis.quiet += seconds;
            if (axis.quiet > 0.08)
                axis = {};
        }
        return 0;
    }
};
struct FrameResult {
    Angles camera;
    bool manuallyReleased = false, suspended = false;
};
class AimEngine {
  public:
    void reset() {
        manual.reset();
        releaseFilter.reset();
        yawRate = pitchRate = 0;
        observed = false;
    }
    void recordApplied(Angles value) {
        lastApplied = value;
        observed = true;
    }
    FrameResult update(Angles camera, Angles desired, const Settings &settings, double seconds,
                       bool outsideAssistance) {
        FrameResult result{camera};
        if (!std::isfinite(seconds) || seconds <= 0)
            return result;
        if (lastMode != settings.mode || lastVertical != settings.vertical) {
            reset();
            lastMode = settings.mode;
            lastVertical = settings.vertical;
        }
        const float yawError = wrapDegrees(desired.yaw - camera.yaw), pitchError = desired.pitch - camera.pitch;
        const float manualYaw = observed ? wrapDegrees(camera.yaw - lastApplied.yaw) : 0;
        const float manualPitch = observed ? camera.pitch - lastApplied.pitch : 0;
        const auto intentional =
            releaseFilter.update(yawError, pitchError, manualYaw, manualPitch, settings.vertical, seconds);
        result.manuallyReleased = manual.shouldSuspend(yawError, pitchError, intentional.horizontal,
                                                       intentional.vertical, settings.vertical, seconds);
        if (result.manuallyReleased || outsideAssistance) {
            yawRate = pitchRate = 0;
            recordApplied(camera);
            result.suspended = true;
            return result;
        }
        // Proportional gain with natural damping near target rather than aggressive magnetic snap
        const double error = angleError(camera, desired, settings.vertical);
        const double nearTarget = 1.0 - std::min(error / 10.0, 1.0);
        const double gripScale = 1.0 + nearTarget * 0.08;
        const double fineScale = (1.0 + nearTarget * (settings.speed2 - 30.0) / 100.0) * gripScale;
        const bool soft = settings.trackingStyle == TrackingStyle::Soft;
        const bool strong = settings.trackingStyle == TrackingStyle::Strong;
        const double strengthScale = soft ? 0.7 : (strong ? 1.35 : 1.0);
        const double gain = std::min(0.95, (settings.speed1 / 100.0 + 0.040) * fineScale * strengthScale);
        const double dt = std::min(seconds, 0.05);
        const double response = 1.0 - std::pow(1.0 - gain, dt * 60.0);
        const double smoothingScale = soft ? 1.5 : (strong ? 0.65 : 1.0);
        const double smoothingTime = settings.smoothing * 0.00075 * smoothingScale;
        double effectiveResponse = response;
        if (observed) {
            if (manualYaw * yawError < -0.04f) {
                const double awaySpeed = std::min(static_cast<double>(std::abs(manualYaw)) / 2.5, 0.50);
                effectiveResponse *= (1.0 - awaySpeed);
            } else if (manualYaw * yawError > 0.04f) {
                // When player moves mouse towards the target, ease assist to prevent compounding overshoot
                const double towardSpeed = std::min(static_cast<double>(std::abs(manualYaw)) / 3.5, 0.40);
                effectiveResponse *= (1.0 - towardSpeed);
            }
        }
        const double yawDelta = smoothStep(yawError, effectiveResponse, dt, smoothingTime, yawRate);
        const double verticalResponse = response * 0.45;
        const double verticalSmoothing = smoothingTime * 1.4;
        double effectiveVerticalResponse = verticalResponse;
        if (observed) {
            if (manualPitch * pitchError < -0.04f) {
                const double awaySpeedV = std::min(static_cast<double>(std::abs(manualPitch)) / 2.5, 0.50);
                effectiveVerticalResponse *= (1.0 - awaySpeedV);
            } else if (manualPitch * pitchError > 0.04f) {
                const double towardSpeedV = std::min(static_cast<double>(std::abs(manualPitch)) / 3.5, 0.40);
                effectiveVerticalResponse *= (1.0 - towardSpeedV);
            }
        }
        const double pitchDelta = smoothStep(settings.vertical ? pitchError : 0, effectiveVerticalResponse, dt, verticalSmoothing, pitchRate);
        result.camera.yaw = static_cast<float>(camera.yaw + limitedCorrection(yawDelta, yawError));
        if (settings.vertical)
            result.camera.pitch =
                std::clamp(static_cast<float>(camera.pitch + limitedCorrection(pitchDelta, pitchError)), -90.0f, 90.0f);
        recordApplied(result.camera);
        return result;
    }

  private:
    static double smoothStep(double error, double response, double dt, double smoothingTime, double &rate) {
        if (std::abs(error) <= 0.01) {
            rate = 0;
            return 0;
        }
        const double requested = error * response / dt;
        const double aligned = rate * requested > 0 ? rate : 0;
        const double blend = smoothingTime > 0 ? -std::expm1(-dt / smoothingTime) : 1.0;
        // Integrate the changing rate over the frame instead of applying its final value for the whole frame.
        const double delta = requested * dt + (aligned - requested) * smoothingTime * blend;
        rate = limitedCorrection(std::lerp(aligned, requested, blend), error / dt);
        return limitedCorrection(delta, error);
    }

    double yawRate = 0, pitchRate = 0;
    ManualAimOverride manual;
    ManualReleaseFilter releaseFilter;
    Angles lastApplied;
    bool observed = false, lastVertical = false;
    AimMode lastMode = AimMode::Simple;
};
} // namespace readableaim
