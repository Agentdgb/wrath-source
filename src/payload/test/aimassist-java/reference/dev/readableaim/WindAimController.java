package dev.readableaim;

/** Original damped controller for this mod; not a recovered Doomsday Wind implementation. */
public final class WindAimController {
  public record Correction(double horizontal, double vertical) {}

  private double horizontalVelocity, verticalVelocity;

  public void reset() {
    horizontalVelocity = verticalVelocity = 0;
  }

  public Correction update(
      double horizontalError,
      double verticalError,
      double speed,
      double sensitivity,
      double seconds) {
    if (!Double.isFinite(seconds) || seconds <= 0 || seconds > 0.1) {
      reset();
      return new Correction(0, 0);
    }
    double gain = 1 + speed * 0.5, limit = 20 + speed * 15;
    double response = 1 - Math.exp(-seconds / (0.015 + (101 - sensitivity) * 0.001));
    horizontalVelocity +=
        (Math.clamp(horizontalError * gain, -limit, limit) - horizontalVelocity) * response;
    verticalVelocity +=
        (Math.clamp(verticalError * gain, -limit, limit) - verticalVelocity) * response;
    if (Math.abs(horizontalError) < 0.05 || horizontalVelocity * horizontalError <= 0)
      horizontalVelocity = 0;
    if (Math.abs(verticalError) < 0.05 || verticalVelocity * verticalError <= 0)
      verticalVelocity = 0;
    return new Correction(
        limited(horizontalVelocity * seconds, horizontalError),
        limited(verticalVelocity * seconds, verticalError));
  }

  private static double limited(double value, double error) {
    return value * error <= 0
        ? 0
        : Math.copySign(Math.min(Math.abs(value), Math.abs(error)), error);
  }
}
