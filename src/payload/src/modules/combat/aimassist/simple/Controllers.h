#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <random>
#include <utility>

// Platform-independent port of Readable Aim Fabric 1.1.0, not the retired native engine.
namespace readableaim {
inline double randomRange(double minimum, double maximum) {
    static std::mt19937_64 generator{std::random_device{}()};
    const double fraction = std::generate_canonical<double, 53>(generator);
    const double value = minimum + (maximum - minimum) * fraction;
    return value < maximum ? value : std::nextafter(maximum, minimum);
}
inline std::int64_t javaRound(double value) {
    // Math.round(double): floor(x + .5) alone is wrong just below half boundaries.
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const int shift = 1074 - static_cast<int>((bits >> 52) & 2047);
    if (shift >= 0 && shift < 64) {
        std::int64_t significand = static_cast<std::int64_t>((bits & 0x000fffffffffffffull) | 0x0010000000000000ull);
        if ((bits >> 63) != 0)
            significand = -significand;
        return ((significand >> shift) + 1) >> 1;
    }
    if (std::isnan(value))
        return 0;
    if (value >= 0x1p63)
        return std::numeric_limits<std::int64_t>::max();
    if (value <= -0x1p63)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}
inline std::int32_t javaInteger(std::int64_t value) {
    return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}
struct Rotation {
    std::int32_t horizontalUnits = 0, verticalUnits = 0;
};
class SimpleAimController {
  public:
    using RandomRange = std::function<double(double, double)>;
    explicit SimpleAimController(RandomRange sampler = randomRange) : random(std::move(sampler)) {}
    void updateRotation(double currentHorizontalUnits, double currentVerticalUnits, double targetHorizontalUnits,
                        double targetVerticalUnits, double speedParameter, double responseParameter) {
        const double currentHorizontal = currentHorizontalUnits * 0.15;
        const double currentVertical = currentVerticalUnits * 0.15;
        const double horizontalError = currentHorizontal - targetHorizontalUnits * 0.15;
        const double verticalError = currentVertical - targetVerticalUnits * 0.15;
        if (std::abs(horizontalError) < 1.0 && std::abs(verticalError) < 1.0)
            return;
        const double horizontalSpeedTerm =
            horizontalError * random(speedParameter - 1.47328, speedParameter + 2.48293) / 100.0;
        const double verticalSpeedTerm =
            verticalError * random(speedParameter - 1.47328, speedParameter + 2.48293) / 100.0;
        const double horizontalCorrection = static_cast<float>(
            -(horizontalSpeedTerm +
              horizontalError / (101.0 - static_cast<float>(random(responseParameter - 4.723847, responseParameter)))));
        const double verticalCorrection = static_cast<float>(
            -(verticalSpeedTerm +
              verticalError / (101.0 - static_cast<float>(random(responseParameter - 4.723847, responseParameter)))));
        pendingRotation = Rotation{javaInteger(javaRound((currentHorizontal + horizontalCorrection) / 0.15)),
                                   javaInteger(javaRound((currentVertical + verticalCorrection) / 0.15))};
    }
    bool hasPendingRotation() const { return pendingRotation.has_value(); }
    std::optional<Rotation> consumeRotation() {
        auto result = pendingRotation;
        pendingRotation.reset();
        return result;
    }

  private:
    RandomRange random;
    std::optional<Rotation> pendingRotation;
};
struct Correction {
    double horizontal = 0, vertical = 0;
};
inline double limitedCorrection(double value, double error) {
    return value * error <= 0 ? 0 : std::copysign(std::min(std::abs(value), std::abs(error)), error);
}
class ManualAimOverride {
  public:
    void reset() { released = false; }
    bool shouldSuspend(double yawError, double pitchError, double manualYaw, double manualPitch, bool verticalEnabled,
                       double elapsedSeconds) {
        const double deliberateMovement = std::max(0.8, elapsedSeconds * 60.0);
        const bool horizontalAway =
            std::abs(yawError) > 12 && manualYaw * yawError < 0 && std::abs(manualYaw) > deliberateMovement;
        const bool verticalAway = verticalEnabled && std::abs(pitchError) > 12 && manualPitch * pitchError < 0 &&
                                  std::abs(manualPitch) > deliberateMovement;
        if (horizontalAway || verticalAway) {
            released = true;
            return true;
        }
        const bool nearTarget = std::hypot(yawError, verticalEnabled ? pitchError : 0) < 8;
        const bool movingToward = manualYaw * yawError > 0.1 || (verticalEnabled && manualPitch * pitchError > 0.1);
        if (released && (nearTarget || movingToward))
            released = false;
        return released;
    }

  private:
    bool released = false;
};
} // namespace readableaim
