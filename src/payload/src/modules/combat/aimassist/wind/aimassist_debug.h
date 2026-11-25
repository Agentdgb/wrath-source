#pragma once

namespace dt::modules::aimassist::debug {

void setEnabled(bool on);
bool active();
// Literal stage names only. The render heartbeat reports a stalled worker too.
void workerStage(const char* stage);
void renderStage(const char* stage);
void frameStatus(const char* stage, bool workerRunning, bool moduleEnabled);

void sessionOpen(int mode, float hSpeed, float vSpeed, bool aimVertically,
                 bool requireMouseDown, float sensitivity);
void sessionClose();
void resolveLine(const char* text);

void workerTick(double realIntervalMs);
void opts(int mode, float hSpeed, float vSpeed, bool aimVertically,
          bool requireMouseDown);
void optsTick();
void setQuantum(float quantum);

void emit(float yawSteps, float pitchSteps, float pendYaw, float pendPitch,
          float liveYaw, float livePitch, float gapYaw, float gapPitch);

struct AdaptiveSnap {
    float dt, aimStrength;
    float targetYaw, targetPitch, yawError, pitchError;
    float yawRate, pitchRate;
    float pendingYaw, pendingPitch;
    float yawBias, pitchBias, yawVelocity, pitchVelocity;
    float leadY, verticalLead, adjustedAimY, playerEyeY, aimY;
    float aimEyeY;
    float driftPos, driftVelocity;
    float targetDistance;
    bool  snapYaw, snapPitch;

    float cmdRateYaw, cmdRatePitch;
    float playerYawRate, playerPitchRate;
};
void ctrlAdaptive(const AdaptiveSnap& s);

struct SimpleSnap {
    float angDiffH, vertAngDiff;
    float accH, accV;
    float velH, velV, bufH, bufV;
    float forceH, forceV;
    double targetMotionX, targetMotionZ;
    float strafe;
    double distance;
};

}
