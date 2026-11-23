package dev.readableaim;

/** Gives deliberate mouse movement priority over assisted rotation. */
public final class ManualAimOverride {
  private boolean released;

  public void reset() {
    released = false;
  }

  public boolean shouldSuspend(
      double yawError,
      double pitchError,
      double manualYaw,
      double manualPitch,
      boolean verticalEnabled,
      double elapsedSeconds) {
    double deliberateMovement = Math.max(0.8, elapsedSeconds * 60.0);
    boolean horizontalAway =
        Math.abs(yawError) > 12
            && manualYaw * yawError < 0
            && Math.abs(manualYaw) > deliberateMovement;
    boolean verticalAway =
        verticalEnabled
            && Math.abs(pitchError) > 12
            && manualPitch * pitchError < 0
            && Math.abs(manualPitch) > deliberateMovement;
    if (horizontalAway || verticalAway) {
      released = true;
      return true;
    }
    boolean nearTarget = Math.hypot(yawError, verticalEnabled ? pitchError : 0) < 8;
    boolean movingToward =
        manualYaw * yawError > 0.1 || (verticalEnabled && manualPitch * pitchError > 0.1);
    if (released && (nearTarget || movingToward)) released = false;
    return released;
  }
}
