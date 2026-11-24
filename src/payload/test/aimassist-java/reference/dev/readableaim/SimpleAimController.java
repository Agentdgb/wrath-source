package dev.readableaim;

import java.util.concurrent.ThreadLocalRandom;

/**
 * Readable reconstruction of the captured direct rotation controller. Input/output angles use the
 * controller's internal units, not screen pixels. This is the rotation core only: target selection
 * and activation stay outside.
 */
public final class SimpleAimController {
  private static final double ANGLE_SCALE = 0.15;
  private static final double DEAD_ZONE = 1.0;
  private static final double SPEED_VARIATION_BELOW = 1.47328;
  private static final double SPEED_VARIATION_ABOVE = 2.48293;
  private static final double RESPONSE_VARIATION = 4.723847;

  public record Rotation(int horizontalUnits, int verticalUnits) {}

  @FunctionalInterface
  public interface RandomRange {
    double between(double minimum, double maximum);
  }

  private final RandomRange random;
  private Rotation pendingRotation;

  public SimpleAimController() {
    this((minimum, maximum) -> ThreadLocalRandom.current().nextDouble(minimum, maximum));
  }

  public SimpleAimController(RandomRange random) {
    this.random = java.util.Objects.requireNonNull(random);
  }

  public void updateRotation(
      double currentHorizontalUnits,
      double currentVerticalUnits,
      double targetHorizontalUnits,
      double targetVerticalUnits,
      double speedParameter,
      double responseParameter) {
    double currentHorizontal = currentHorizontalUnits * ANGLE_SCALE;
    double currentVertical = currentVerticalUnits * ANGLE_SCALE;
    double horizontalError = currentHorizontal - targetHorizontalUnits * ANGLE_SCALE;
    double verticalError = currentVertical - targetVerticalUnits * ANGLE_SCALE;

    // The original retains any pending result when both errors are small.
    if (Math.abs(horizontalError) < DEAD_ZONE && Math.abs(verticalError) < DEAD_ZONE) {
      return;
    }

    double horizontalSpeedTerm =
        horizontalError
            * random.between(
                speedParameter - SPEED_VARIATION_BELOW, speedParameter + SPEED_VARIATION_ABOVE)
            / 100.0;
    double verticalSpeedTerm =
        verticalError
            * random.between(
                speedParameter - SPEED_VARIATION_BELOW, speedParameter + SPEED_VARIATION_ABOVE)
            / 100.0;

    // Keep the original float casts: removing them changes rounding at boundaries.
    double horizontalCorrection =
        (float)
            -(horizontalSpeedTerm
                + horizontalError
                    / (101.0
                        - (float)
                            random.between(
                                responseParameter - RESPONSE_VARIATION, responseParameter)));
    double verticalCorrection =
        (float)
            -(verticalSpeedTerm
                + verticalError
                    / (101.0
                        - (float)
                            random.between(
                                responseParameter - RESPONSE_VARIATION, responseParameter)));

    pendingRotation =
        new Rotation(
            (int) Math.round((currentHorizontal + horizontalCorrection) / ANGLE_SCALE),
            (int) Math.round((currentVertical + verticalCorrection) / ANGLE_SCALE));
  }

  public boolean hasPendingRotation() {
    return pendingRotation != null;
  }

  public Rotation consumeRotation() {
    Rotation result = pendingRotation;
    pendingRotation = null;
    return result;
  }
}
