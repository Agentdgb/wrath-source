

#include "aimassist.h"
#include "aimassist_debug.h"

#include "jvm/JniRef.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include "vgui/bridge.h"

#include <chrono>
#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace vanta::modules::aimassist::regular {

namespace {

constexpr float SNAP_ENTER_THRESHOLD = 5.0f;
constexpr float SNAP_EXIT_THRESHOLD  = 7.0f;
constexpr float SNAP_MAX_ANGLE       = 20.0f;

float lerp(float factor, float start, float end) { return start + factor * (end - start); }

float smoothStep(float edgeStart, float edgeEnd, float value)
{
    const float normalized = std::max(0.0f, std::min(1.0f,
        (value - edgeStart) / (edgeEnd - edgeStart)));
    return normalized * normalized * (3.0f - 2.0f * normalized);
}

float javaSignum(float v) { return v > 0.f ? 1.f : (v < 0.f ? -1.f : v); }
int   javaRound(float v)  { return static_cast<int>(std::floor(v + 0.5f)); }

bool shouldSnap(bool enabled, bool currentlySnapped, float angleDifference)
{
    if (!enabled) return false;
    return currentlySnapped ? angleDifference <= SNAP_EXIT_THRESHOLD
                            : angleDifference <= SNAP_ENTER_THRESHOLD;
}

struct AdaptiveState {
    JavaRandom random;

    float pitchVelocity = 0.f;
    float lastYawDiff = 0.f;
    double predictedZ = 0.0;
    bool aimPointInitialized = false;
    float airFactor = 0.f;
    bool predictionInitialized = false;
    long long lastFrameNanos = 0;
    double lastGroundEyeY = 0.0;
    float lastPlayerPitch = 0.f;
    float pitchBias = 0.f;
    float overshoot = 0.f;
    float driftPos = 0.f;
    double leadY = 0.0, leadX = 0.0;
    bool yawSnapped = false;
    long long driftNextNanos = 0;
    double leadZ = 0.0;
    float pitchFlickTicks = 0.f;
    float lastPitchSign = 0.f;
    float aimStrength = 0.f;
    double aimX = 0.0;
    float driftVelocity = 0.f;
    float yawAccel = 0.f;
    double predictedY = 0.0;
    float verticalVelocity = 0.f;
    float yawBias = 0.f;
    long long noiseStartNanos = 0;
    float lastTargetPitch = 0.f;
    double lastEyeY = 0.0;
    float driftTarget = 0.f;
    bool pitchSnapped = false;
    float lastTargetYaw = 0.f;
    float yawVelocity = 0.f;
    float lastYawSign = 0.f;
    float driftNoise = 0.f;
    double predictedX = 0.0;
    float lastPlayerYaw = 0.f;
    float yawFlickTicks = 0.f;
    int targetSwitchTicks = 0;
    float pitchAccel = 0.f;
    bool initialized = false;
    double aimY = 0.0;
    float lastPitchDiff = 0.f;
    double aimZ = 0.0;

    float dtRealAvg = 0.002f;

    bool   drInit = false;
    double drX = 0.0, drY = 0.0, drZ = 0.0;
    double drVX = 0.0, drVY = 0.0, drVZ = 0.0;
    long long drStampNs = 0;
} g;

void setTarget(SharedState& sh, JNIEnv* env, jobject newTarget)
{
    std::lock_guard<std::mutex> lk(sh.mx);
    if (sh.targetRef) env->DeleteGlobalRef(sh.targetRef);
    sh.targetRef = newTarget ? env->NewGlobalRef(newTarget) : nullptr;
}
jobject borrowTarget(SharedState& sh, JNIEnv* env)
{
    std::lock_guard<std::mutex> lk(sh.mx);
    return sh.targetRef ? env->NewLocalRef(sh.targetRef) : nullptr;
}

float pendingGet(SharedState& sh, bool yaw)
{
    std::lock_guard<std::mutex> lk(sh.mx);
    return yaw ? sh.pendingYaw : sh.pendingPitch;
}
void pendingSet(SharedState& sh, bool yaw, float v)
{
    std::lock_guard<std::mutex> lk(sh.mx);
    if (yaw) sh.pendingYaw = v; else sh.pendingPitch = v;
}

void reset(JNIEnv* env, SharedState& sh)
{
    (void)env;
    g.yawSnapped = false;
    g.pitchSnapped = false;
    g.yawBias = 0.f;
    g.pitchBias = 0.f;
    g.yawVelocity = 0.f;
    g.pitchVelocity = 0.f;
    g.lastYawDiff = 0.f;
    g.lastPitchDiff = 0.f;
    g.lastTargetYaw = 0.f;
    g.lastTargetPitch = 0.f;
    g.yawAccel = 0.f;
    g.pitchAccel = 0.f;
    g.lastPlayerYaw = 0.f;
    g.lastPlayerPitch = 0.f;
    g.initialized = false;
    g.lastFrameNanos = 0;
    g.aimPointInitialized = false;
    g.aimX = 0.0; g.aimY = 0.0; g.aimZ = 0.0;
    g.leadX = 0.0; g.leadY = 0.0; g.leadZ = 0.0;
    g.aimStrength = 0.f;
    g.yawFlickTicks = 0.f;
    g.pitchFlickTicks = 0.f;
    g.lastYawSign = 0.f;
    g.lastPitchSign = 0.f;
    g.overshoot = 0.f;
    g.lastEyeY = 0.0;
    g.lastGroundEyeY = 0.0;
    g.verticalVelocity = 0.f;
    g.airFactor = 0.f;
    g.predictionInitialized = false;
    g.predictedX = 0.0; g.predictedY = 0.0; g.predictedZ = 0.0;
    g.noiseStartNanos = nowNanos();
    g.driftPos = 0.f;
    g.driftVelocity = 0.f;
    g.driftTarget = 0.f;
    g.driftNoise = 0.f;
    g.driftNextNanos = 0;

    g.drInit = false;
    g.drVX = g.drVY = g.drVZ = 0.0;
    g.drStampNs = 0;
    std::lock_guard<std::mutex> lk(sh.mx);
    sh.pendingYaw = 0.f;
    sh.pendingPitch = 0.f;
    sh.yawSnapped = false;
    sh.pitchSnapped = false;
    sh.smoothPY = 0.f;
    sh.smoothPP = 0.f;
    sh.cmdRateYaw = sh.cmdRatePitch = 0.f;
    sh.cmdStampNs = 0;
    sh.rateFiltYaw = sh.rateFiltPitch = 0.f;
    sh.carryYaw = sh.carryPitch = 0.f;
    sh.emitRateYaw = sh.emitRatePitch = 0.f;
}

double areaTargetY(int area, double targetMinY, double targetMaxY,
                   double airborneCenterBlend)
{
    const double h = targetMaxY - targetMinY;

    double frac;
    switch (area) {
        case 0:  frac = 0.90; break;
        case 2:  frac = 0.20; break;
        case 1:
        default: frac = 0.65; break;
    }

    const double desired = targetMinY + h * frac;
    const double center  = targetMinY + h * 0.65;
    const double blended = desired + (center - desired) * airborneCenterBlend;
    return std::max(targetMinY + 0.01, std::min(targetMaxY - 0.01, blended));
}

void resolveSnapPoint(JNIEnv* env, jobject player, jobject target,
                       const SettingsView& sv, double out[3])
{
    const EntPos pp = entityPos(env, player);
    const float playerEyeY = static_cast<float>(pp.y) + entityEyeHeight(env, player);
    const Box6 box = entityBox(env, target);
    const EntPos tpos = entityPos(env, target);
    double targetMinY = tpos.y, targetMaxY = tpos.y + 1.8;
    if (box.valid) { targetMinY = box.minY; targetMaxY = box.maxY; }

    const double airborneOffset = std::min(0.85, static_cast<double>(g.airFactor));
    const double centerBlend = std::max(0.0, std::min(1.0, airborneOffset / 0.55));

    const double ty = areaTargetY(sv.targetArea, targetMinY, targetMaxY, centerBlend);
    const EntPos tp = entityPos(env, target);
    out[0] = tp.x; out[1] = ty; out[2] = tp.z;
}

void resolveAimTarget(JNIEnv* env, jobject player, jobject target,
                      const SettingsView& sv, double out[3])
{
    const EntPos pp = entityPos(env, player);
    const float playerEyeY = static_cast<float>(pp.y) + entityEyeHeight(env, player);
    const Box6 box = entityBox(env, target);
    const EntPos tpos = entityPos(env, target);
    double targetMinY = tpos.y, targetMaxY = tpos.y + 1.8;
    if (box.valid) { targetMinY = box.minY; targetMaxY = box.maxY; }

    const double airborneOffset = std::min(0.85, static_cast<double>(g.airFactor));
    const double centerBlend = std::max(0.0, std::min(1.0, airborneOffset / 0.55));

    const double ty = areaTargetY(sv.targetArea, targetMinY, targetMaxY, centerBlend);
    const EntPos tp = entityPos(env, target);
    out[0] = tp.x; out[1] = ty; out[2] = tp.z;

    g.predictedX = tp.x; g.predictedY = ty; g.predictedZ = tp.z;
    g.predictionInitialized = false;
}

float computePitchDrift(float playerPitch, float deltaTime, long long nowN)
{
    const float strength = 0.65f + 0.35f * g.aimStrength;
    if (g.driftNextNanos == 0 || nowN >= g.driftNextNanos) {
        const float randomAmplitude = lerp(strength, 0.05f, 0.15f);
        const float pitchCorrection = playerPitch > 22.f ? -0.18f
                                    : (playerPitch < -22.f ? 0.18f : 0.f);
        g.driftTarget = (g.random.nextFloat() * 2.f - 1.f) * randomAmplitude + pitchCorrection;
        g.driftTarget = std::max(-0.5f, std::min(0.5f, g.driftTarget));
        const long long intervalMillis = 300LL + g.random.nextInt(420);
        g.driftNextNanos = nowN + intervalMillis * 1000000LL;
    }
    const float springStrength = lerp(strength, 2.0f, 5.0f);
    const float damping = static_cast<float>(std::pow(0.04f, deltaTime));
    g.driftVelocity += (g.driftTarget - g.driftPos) * springStrength * deltaTime;
    g.driftVelocity *= damping;
    g.driftPos += g.driftVelocity * deltaTime;
    const float positionLimit = lerp(strength, 0.45f, 0.8f);
    if (g.driftPos > positionLimit) {
        g.driftPos = positionLimit;
        g.driftVelocity = std::min(0.f, g.driftVelocity);
    } else if (g.driftPos < -positionLimit) {
        g.driftPos = -positionLimit;
        g.driftVelocity = std::max(0.f, g.driftVelocity);
    }
    g.driftNoise += (g.random.nextFloat() * 2.f - 1.f - g.driftNoise)
                    * std::max(0.02f, std::min(0.18f, deltaTime * 5.f));
    const float elapsedSeconds =
        static_cast<float>(nowN - g.noiseStartNanos) / 1.0E9f;
    const float noise = static_cast<float>(
        std::sin(elapsedSeconds * 8.7 + 0.4) * 0.35
        + std::sin(elapsedSeconds * 13.1 + 2.2) * 0.22
        + std::sin(elapsedSeconds * 19.6 + 1.1) * 0.12
        + g.driftNoise * 0.3f);
    const float restoringForce = -g.driftPos * lerp(strength, 1.4f, 3.0f);
    const float output = g.driftVelocity * 0.25f + restoringForce
                       + noise * lerp(strength, 0.35f, 0.9f);
    const float pitchLimitScale =
        1.f - 0.85f * smoothStep(72.f, 88.f, std::fabs(playerPitch));
    const float outputLimit = lerp(strength, 0.25f, 0.55f) * pitchLimitScale;
    return std::max(-outputLimit, std::min(outputLimit, output));
}

void updateAim(JNIEnv* env, jobject mc, jobject player, jobject target,
               SharedState& sh, const SettingsView& sv)
{
    const JniCache& c = cache();

    const bool yawSnapEnabled = false;
    const bool pitchSnapEnabled = false;
    (void)sv;
    bool snapYaw = yawSnapEnabled && g.yawSnapped;
    bool snapPitch = pitchSnapEnabled && g.pitchSnapped;

    const long long nowN = nowNanos();
    if (g.noiseStartNanos == 0) g.noiseStartNanos = nowN;
    float dtReal = 0.001f;
    float deltaTime;
    if (!g.initialized || g.lastFrameNanos == 0) {
        deltaTime = 0.016666668f;
    } else {
        deltaTime = static_cast<float>(nowN - g.lastFrameNanos) / 1.0E9f;
        dtReal = std::max(0.0002f, std::min(0.02f, deltaTime));
        deltaTime = std::max(0.008333334f, std::min(0.12f, deltaTime));
    }
    g.lastFrameNanos = nowN;

    if (g.initialized) g.dtRealAvg += (dtReal - g.dtRealAvg) * 0.002f;

    double resolvedAimPoint[3];
    resolveAimTarget(env, player, target, sv, resolvedAimPoint);

    const float renderTd = renderTickDelta(env);
    {
        const EntPos tTick = entityPos(env, target);
        const EntPos tRender = entityRenderPos(env, target, renderTd);
        resolvedAimPoint[0] += tRender.x - tTick.x;
        resolvedAimPoint[1] += tRender.y - tTick.y;
        resolvedAimPoint[2] += tRender.z - tTick.z;
    }
    const double resolvedAimX = resolvedAimPoint[0];
    const double resolvedAimY = resolvedAimPoint[1];
    const double resolvedAimZ = resolvedAimPoint[2];

    const EntVel tv = entityVelocity(env, target);
    const EntVel pv = entityVelocity(env, player);
    const double targetMotionX = tv.x, targetMotionY = tv.y, targetMotionZ = tv.z;
    const double relativeMotionX = targetMotionX - pv.x;
    const double relativeMotionY = targetMotionY - pv.y;
    const double relativeMotionZ = targetMotionZ - pv.z;
    const double targetLeadX = targetMotionX + relativeMotionX * 0.08;
    const double targetLeadY = targetMotionY + relativeMotionY * 0.1;
    const double targetLeadZ = targetMotionZ + relativeMotionZ * 0.08;
    if (!g.aimPointInitialized) {
        g.aimX = resolvedAimX; g.aimY = resolvedAimY; g.aimZ = resolvedAimZ;
        g.leadX = targetLeadX; g.leadY = targetLeadY; g.leadZ = targetLeadZ;
        g.aimPointInitialized = true;
    }

    const double targetHorizontalSpeed =
        std::sqrt(targetMotionX * targetMotionX + targetMotionZ * targetMotionZ);
    const double relativeHorizontalSpeed =
        std::sqrt(relativeMotionX * relativeMotionX + relativeMotionZ * relativeMotionZ);
    const double targetDistance = distanceToEntity(env, player, target);

    (void)sv;
    const double baseAimSmoothing = 0.28;
    const double aimSmoothing = std::max(0.08, std::min(0.75,
        baseAimSmoothing + std::min(0.35, relativeHorizontalSpeed * 0.5)));
    const double leadMotionScale = targetHorizontalSpeed + relativeHorizontalSpeed * 0.25;
    const double leadSmoothing = std::max(0.12, std::min(0.68,
        0.18 + std::min(0.42, leadMotionScale * 0.72)));
    const double frameScale = std::max(0.45, std::min(2.4, deltaTime * 60.0));
    const double aimBlend = 1.0 - std::pow(1.0 - aimSmoothing, frameScale);
    const double leadBlend = 1.0 - std::pow(1.0 - leadSmoothing, frameScale);
    g.aimX += (resolvedAimX - g.aimX) * aimBlend;
    g.aimY += (resolvedAimY - g.aimY) * aimBlend;
    g.aimZ += (resolvedAimZ - g.aimZ) * aimBlend;
    g.leadX += (targetLeadX - g.leadX) * leadBlend;
    g.leadY += (targetLeadY - g.leadY) * leadBlend;
    g.leadZ += (targetLeadZ - g.leadZ) * leadBlend;

    const double aimLagX = resolvedAimX - g.aimX;
    const double aimLagZ = resolvedAimZ - g.aimZ;
    const double horizontalAimLag = std::sqrt(aimLagX * aimLagX + aimLagZ * aimLagZ);
    const double maximumAimLag =
        0.3 + targetHorizontalSpeed * 1.3 + relativeHorizontalSpeed * 0.4;
    if (horizontalAimLag > maximumAimLag) {
        g.aimX = resolvedAimX; g.aimY = resolvedAimY; g.aimZ = resolvedAimZ;
        g.leadX = targetLeadX; g.leadY = targetLeadY; g.leadZ = targetLeadZ;
    }

    double leadAmount = 0.35 + targetDistance * 0.045 + targetHorizontalSpeed * 0.95
                      + std::min(0.18, relativeHorizontalSpeed * 0.12);
    leadAmount = std::max(0.1, std::min(1.05, leadAmount));
    const double distanceLeadScale =
        std::max(0.0, std::min(1.0, (targetDistance - 0.8) / 2.5));
    leadAmount *= 0.3 + 0.7 * distanceLeadScale;
    leadAmount = std::max(0.05, leadAmount);

    const EntPos pp = entityRenderPos(env, player, renderTd);
    const double playerX = pp.x, playerZ = pp.z;
    const float playerEyeY = static_cast<float>(pp.y) + entityEyeHeight(env, player);

    float measuredVerticalVelocity = 0.f;
    if (g.initialized && g.lastEyeY != 0.0)
        measuredVerticalVelocity =
            static_cast<float>((playerEyeY - g.lastEyeY) / deltaTime);
    g.verticalVelocity += (measuredVerticalVelocity - g.verticalVelocity) * 0.65f;
    g.lastEyeY = playerEyeY;

    const bool onGround = c.ent_on_ground_mid
        ? env->CallBooleanMethod(player, c.ent_on_ground_mid) == JNI_TRUE : true;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (onGround) {
        g.lastGroundEyeY = playerEyeY;
        g.airFactor *= 0.35f;
    } else {
        if (g.lastGroundEyeY == 0.0) g.lastGroundEyeY = playerEyeY;
        const double airborneHeight = std::max(0.0, playerEyeY - g.lastGroundEyeY);
        const float velocityAirFactor = static_cast<float>(std::min(0.7,
            std::max(0.0, static_cast<double>(g.verticalVelocity)) * 0.34));
        const float heightAirFactor =
            static_cast<float>(std::min(0.82, airborneHeight * 0.58));
        g.airFactor = std::max(g.airFactor * 0.92f,
                               std::max(velocityAirFactor, heightAirFactor));
    }

    float aimEyeY = playerEyeY;
    if (sv.jumpCompensate && g.lastGroundEyeY > 0.0) {
        const float baseline = static_cast<float>(g.lastGroundEyeY);
        if (aimEyeY > baseline + 0.05f) aimEyeY = baseline + 0.05f;
    }

    double adjustedAimX = g.aimX;
    double adjustedAimZ = g.aimZ;
    const double verticalAimDelta = g.aimY - aimEyeY;
    const double verticalOffsetScale =
        std::max(0.0, std::min(1.0, std::fabs(verticalAimDelta) / 0.7));
    const double verticalLeadAmount =
        std::min(leadAmount, 0.7 + targetDistance * 0.04);
    const double verticalLeadScale = 0.28 + 0.52 * verticalOffsetScale;
    double verticalLead = g.leadY * verticalLeadAmount * verticalLeadScale;
    const double verticalLeadLimit = 0.16 + targetDistance * 0.055;
    verticalLead = std::max(-verticalLeadLimit, std::min(verticalLeadLimit, verticalLead));
    double adjustedAimY = g.aimY + verticalLead;
    if (snapYaw) { adjustedAimX = resolvedAimX; adjustedAimZ = resolvedAimZ; }
    if (snapPitch) adjustedAimY = resolvedAimY;

    const double aimDeltaX = adjustedAimX - playerX;
    const double aimDeltaZ = adjustedAimZ - playerZ;
    const double aimDeltaY = adjustedAimY - aimEyeY;
    const double horizontalAimDistance =
        std::sqrt(aimDeltaX * aimDeltaX + aimDeltaZ * aimDeltaZ);
    float targetYaw = static_cast<float>(
        std::atan2(aimDeltaZ, aimDeltaX) * 57.29577951308232 - 90.0);
    float targetPitch = static_cast<float>(
        -std::atan2(aimDeltaY, std::max(horizontalAimDistance, 1.0E-4)) * 57.29577951308232);

    const float playerYaw = env->GetFloatField(player, c.ent_yaw_fid);
    const float playerPitch = env->GetFloatField(player, c.ent_pitch_fid);
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (g.initialized) {
        const float yawTargetChange =
            std::fabs(wrapAngleTo180_f(targetYaw - g.lastTargetYaw));
        const float pitchTargetChange =
            std::fabs(wrapAngleTo180_f(targetPitch - g.lastTargetPitch));
        const float abruptChangeThreshold = 12.f;
        const float maximumTargetChange = std::max(yawTargetChange, pitchTargetChange);
        const float abruptChangeFactor = std::max(0.f, std::min(1.f,
            (maximumTargetChange - abruptChangeThreshold) / 50.f));
        g.overshoot = std::max(g.overshoot, abruptChangeFactor);
        if (abruptChangeFactor > 0.3f) {
            g.aimStrength *= 0.3f;
            g.aimX = resolvedAimX; g.aimY = resolvedAimY; g.aimZ = resolvedAimZ;
        }
        const float yawRateScale = 1.f + std::min(2.f, yawTargetChange / 60.f);
        const float pitchRateScale = 1.f + std::min(2.f, pitchTargetChange / 60.f);
        float maximumYawRate =
            (120.f + static_cast<float>(relativeHorizontalSpeed * 700.0)) * yawRateScale;
        float maximumPitchRate =
            (95.f + static_cast<float>(std::fabs(g.leadY)) * 550.f) * pitchRateScale;
        const float viewYawDifference =
            std::fabs(wrapAngleTo180_f(targetYaw - playerYaw));
        const float wideTurnScale = smoothStep(10.f, 35.f, viewYawDifference);
        maximumYawRate *= 1.f + wideTurnScale * 1.5f;
        maximumYawRate = std::max(90.f, std::min(1080.f, maximumYawRate));
        maximumPitchRate = std::max(70.f, std::min(500.f, maximumPitchRate));
        float yawStep = wrapAngleTo180_f(targetYaw - g.lastTargetYaw);
        float pitchStep = wrapAngleTo180_f(targetPitch - g.lastTargetPitch);
        if (!snapYaw)
            yawStep = std::max(-maximumYawRate * deltaTime,
                               std::min(maximumYawRate * deltaTime, yawStep));
        if (!snapPitch)
            pitchStep = std::max(-maximumPitchRate * deltaTime,
                                 std::min(maximumPitchRate * deltaTime, pitchStep));
        targetYaw = g.lastTargetYaw + yawStep;
        targetPitch = g.lastTargetPitch + pitchStep;
    }

    const float overshootDecay = static_cast<float>(std::pow(0.02, deltaTime));
    g.overshoot *= overshootDecay;
    if (g.overshoot < 0.01f) g.overshoot = 0.f;
    const float overshootMultiplier = 1.f + 3.f * g.overshoot;

    float yawError = wrapAngleTo180_f(targetYaw - playerYaw);
    float pitchError = wrapAngleTo180_f(targetPitch - playerPitch);
    if (!sv.aimVertically) pitchError = 0.f;
    const float absoluteYawError = std::fabs(yawError);
    const float absolutePitchError = std::fabs(pitchError);
    const float combinedAngleError = std::sqrt(absoluteYawError * absoluteYawError
                                             + absolutePitchError * absolutePitchError);
    g.yawSnapped = shouldSnap(yawSnapEnabled, g.yawSnapped, absoluteYawError);
    g.pitchSnapped = shouldSnap(pitchSnapEnabled, g.pitchSnapped, absolutePitchError);
    snapYaw = yawSnapEnabled && g.yawSnapped;
    snapPitch = pitchSnapEnabled && g.pitchSnapped;

    float desiredAimStrength = std::max(0.35f, 1.f - smoothStep(1.5f, 8.f, combinedAngleError));
    if (g.initialized) {
        const float yawClosingSpeed =
            -(absoluteYawError - std::fabs(g.lastYawDiff)) / deltaTime;
        const float closingAdjustment =
            std::max(-0.3f, std::min(0.3f, yawClosingSpeed / 20.f));
        desiredAimStrength += closingAdjustment;
        desiredAimStrength = std::max(0.f, std::min(1.f, desiredAimStrength));
    }
    const float aimStrengthBlend = desiredAimStrength > g.aimStrength
        ? std::max(0.01f, std::min(0.25f, deltaTime * 3.f))
        : std::max(0.05f, std::min(0.8f, deltaTime * 20.f));
    g.aimStrength += (desiredAimStrength - g.aimStrength) * aimStrengthBlend;
    const float currentAimStrength = g.aimStrength;

    float smoothedTargetYawRate = 0.f;
    float smoothedTargetPitchRate = 0.f;
    float targetYawRateRaw = 0.f;
    float targetPitchRateRaw = 0.f;
    float playerYawRate = 0.f;
    float playerPitchRate = 0.f;
    if (g.initialized) {
        targetYawRateRaw =
            wrapAngleTo180_f(targetYaw - g.lastTargetYaw) / deltaTime;
        targetPitchRateRaw =
            wrapAngleTo180_f(targetPitch - g.lastTargetPitch) / deltaTime;
        playerYawRate =
            wrapAngleTo180_f(playerYaw - g.lastPlayerYaw) / deltaTime;
        playerPitchRate =
            wrapAngleTo180_f(playerPitch - g.lastPlayerPitch) / deltaTime;
        const float accelerationBlend =
            std::max(0.05f, std::min(0.45f, deltaTime * 12.f));
        g.yawAccel += (targetYawRateRaw - g.yawAccel) * accelerationBlend;
        g.pitchAccel += (targetPitchRateRaw - g.pitchAccel) * accelerationBlend;
        smoothedTargetYawRate = g.yawAccel;
        smoothedTargetPitchRate = g.pitchAccel;
    }

    const float horizontalSpeed = sv.horizontalSpeed * 3.5f;
    const float verticalSpeed   = sv.verticalSpeed   * 3.5f;
    const float horizontalSpeedFactor =
        std::max(0.f, std::min(1.f, (horizontalSpeed - 10.f) / 90.f));
    const float verticalSpeedFactor =
        std::max(0.f, std::min(1.f, (verticalSpeed - 10.f) / 90.f));

    if (javaSignum(yawError) != javaSignum(g.lastYawDiff)
        && std::fabs(yawError) > 0.1f && std::fabs(g.lastYawDiff) > 0.1f)
        g.yawBias *= 0.3f;
    if (javaSignum(pitchError) != javaSignum(g.lastPitchDiff)
        && std::fabs(pitchError) > 0.1f && std::fabs(g.lastPitchDiff) > 0.1f)
        g.pitchBias *= 0.3f;

    const float squaredAimStrength = currentAimStrength * currentAimStrength;
    g.yawBias += yawError * deltaTime * squaredAimStrength * (1.f - horizontalSpeedFactor);
    g.pitchBias += pitchError * deltaTime * squaredAimStrength * (1.f - verticalSpeedFactor);
    const float biasRetention = 1.f
        - (1.f - currentAimStrength) * std::max(0.f, std::min(0.5f, deltaTime * 5.f));
    g.yawBias *= biasRetention;
    g.pitchBias *= biasRetention;
    const float yawBiasLimit = 15.f * (1.f - horizontalSpeedFactor * 0.9f);
    const float pitchBiasLimit = 10.f * (1.f - verticalSpeedFactor * 0.9f);
    g.yawBias = std::max(-yawBiasLimit, std::min(yawBiasLimit, g.yawBias));
    g.pitchBias = std::max(-pitchBiasLimit, std::min(pitchBiasLimit, g.pitchBias));

    const float yawErrorVelocity =
        g.initialized ? (yawError - g.lastYawDiff) / deltaTime : 0.f;
    const float pitchErrorVelocity =
        g.initialized ? (pitchError - g.lastPitchDiff) / deltaTime : 0.f;
    const float velocityBlend = 0.15f;
    g.yawVelocity = g.yawVelocity * (1.f - velocityBlend) + yawErrorVelocity * velocityBlend;
    g.pitchVelocity = g.pitchVelocity * (1.f - velocityBlend) + pitchErrorVelocity * velocityBlend;

    const float flickThreshold = 0.5f;
    const float yawSign = javaSignum(yawError);
    g.yawFlickTicks = yawSign != g.lastYawSign && std::fabs(yawError) > flickThreshold
        ? std::min(g.yawFlickTicks + 1.f, 8.f)
        : std::max(0.f, g.yawFlickTicks - deltaTime * 3.f);
    g.lastYawSign = yawSign;
    const float pitchSign = javaSignum(pitchError);
    g.pitchFlickTicks = pitchSign != g.lastPitchSign && std::fabs(pitchError) > flickThreshold
        ? std::min(g.pitchFlickTicks + 1.f, 8.f)
        : std::max(0.f, g.pitchFlickTicks - deltaTime * 3.f);
    g.lastPitchSign = pitchSign;
    const float yawFlickFactor =
        std::max(0.f, std::min(1.f, g.yawFlickTicks / 5.f));
    const float pitchFlickFactor =
        std::max(0.f, std::min(1.f, g.pitchFlickTicks / 5.f));

    const float limitedHorizontalSpeed = std::min(horizontalSpeed, 45.f);
    const float limitedVerticalSpeed = std::min(verticalSpeed, 45.f);

    float horizontalGainInput = (limitedHorizontalSpeed - 1.f) / 44.f;
    float verticalGainInput = (limitedVerticalSpeed - 1.f) / 44.f;
    horizontalGainInput *= horizontalGainInput;
    verticalGainInput *= verticalGainInput;
    const float horizontalGainScale =
        0.15f + 0.85f * std::max(0.f, std::min(1.f, horizontalGainInput));
    const float verticalGainScale =
        0.15f + 0.85f * std::max(0.f, std::min(1.f, verticalGainInput));

    float yawErrorGain = lerp(currentAimStrength, 8.f,
        2.5f + limitedHorizontalSpeed * 0.15f) * horizontalGainScale;
    float pitchErrorGain = lerp(currentAimStrength, 7.f,
        2.2f + limitedVerticalSpeed * 0.13f) * verticalGainScale;
    const float yawBiasGain = lerp(currentAimStrength, 0.15f,
        0.8f + limitedHorizontalSpeed * 0.04f) * horizontalGainScale;
    const float pitchBiasGain = lerp(currentAimStrength, 0.12f,
        0.65f + limitedVerticalSpeed * 0.035f) * verticalGainScale;
    float yawVelocityGain = lerp(currentAimStrength, 0.08f, 0.25f) * horizontalGainScale;
    float pitchVelocityGain = lerp(currentAimStrength, 0.06f, 0.2f) * verticalGainScale;
    const float targetYawRateGain =
        (0.85f + limitedHorizontalSpeed * 0.015f) * horizontalGainScale;
    const float targetPitchRateGain =
        (0.82f + limitedVerticalSpeed * 0.013f) * verticalGainScale;
    const float playerYawCompensation = lerp(currentAimStrength, 0.1f, 0.3f);
    const float playerPitchCompensation = lerp(currentAimStrength, 0.08f, 0.25f);
    yawErrorGain *= 1.f - 0.6f * yawFlickFactor;
    yawVelocityGain *= 1.f + 2.f * yawFlickFactor;
    pitchErrorGain *= 1.f - 0.6f * pitchFlickFactor;
    pitchVelocityGain *= 1.f + 2.f * pitchFlickFactor;

    float yawRate = yawErrorGain * yawError + yawBiasGain * g.yawBias
                  + yawVelocityGain * g.yawVelocity
                  + targetYawRateGain * smoothedTargetYawRate
                  - playerYawCompensation * playerYawRate;
    float pitchRate = 0.f;
    if (sv.aimVertically) {
        pitchRate = pitchErrorGain * pitchError + pitchBiasGain * g.pitchBias
                  + pitchVelocityGain * g.pitchVelocity
                  + targetPitchRateGain * smoothedTargetPitchRate
                  - playerPitchCompensation * playerPitchRate;
    } else {
        g.pitchBias = 0.f;
        g.pitchVelocity = 0.f;
    }

    if (sv.humanize && !snapPitch)
        pitchRate += computePitchDrift(playerPitch, deltaTime, nowN);

    (void)mc;

    const float wideYawScale = smoothStep(8.f, 40.f, absoluteYawError);
    const float wideTurnMultiplier = 1.f + wideYawScale * 2.5f;
    const float yawBaseLimit = (22.f + limitedHorizontalSpeed * 15.f)
                             * horizontalGainScale * overshootMultiplier * wideTurnMultiplier;
    const float pitchBaseLimit = (18.f + limitedVerticalSpeed * 13.f)
                               * verticalGainScale * overshootMultiplier;
    const float yawMotionLimit = (std::fabs(smoothedTargetYawRate) * 0.4f + 18.f)
        * (0.35f + horizontalGainScale * 0.65f) * wideTurnMultiplier;
    const float pitchMotionLimit = (std::fabs(smoothedTargetPitchRate) * 0.38f + 14.f)
        * (0.35f + verticalGainScale * 0.65f);
    const float maximumYawOutput = std::min(400.f * overshootMultiplier * wideTurnMultiplier,
        std::max(yawBaseLimit, yawMotionLimit));
    const float maximumPitchOutput = std::min(300.f * overshootMultiplier,
        std::max(pitchBaseLimit, pitchMotionLimit));
    yawRate = std::max(-maximumYawOutput, std::min(maximumYawOutput, yawRate));
    pitchRate = std::max(-maximumPitchOutput, std::min(maximumPitchOutput, pitchRate));

    const float distanceScale = smoothStep(0.5f, 3.f, static_cast<float>(targetDistance));
    const float wideTurnBaseScale = lerp(wideYawScale, 0.15f, 0.65f);
    const float yawDistanceMultiplier = lerp(currentAimStrength,
        wideTurnBaseScale + (1.f - wideTurnBaseScale) * distanceScale,
        0.4f + 0.6f * distanceScale);
    const float pitchDistanceMultiplier = lerp(currentAimStrength,
        0.2f + 0.8f * distanceScale, 0.45f + 0.55f * distanceScale);
    yawRate *= yawDistanceMultiplier;
    pitchRate *= pitchDistanceMultiplier;

    const float noiseElapsedSeconds =
        static_cast<float>(nowN - g.noiseStartNanos) / 1.0E9f;
    const float highFrequencyYawNoise = static_cast<float>(
        std::sin(noiseElapsedSeconds * 62.83) * 0.4
        + std::sin(noiseElapsedSeconds * 47.12) * 0.25
        + std::sin(noiseElapsedSeconds * 78.54) * 0.15);
    const float highFrequencyPitchNoise = static_cast<float>(
        std::sin(noiseElapsedSeconds * 56.55 + 1.3) * 0.35
        + std::sin(noiseElapsedSeconds * 43.98 + 0.7) * 0.2
        + std::sin(noiseElapsedSeconds * 72.26 + 2.1) * 0.12);
    const float mediumFrequencyYawNoise = static_cast<float>(
        std::sin(noiseElapsedSeconds * 12.57) * 0.8
        + std::sin(noiseElapsedSeconds * 7.85) * 0.5);
    const float mediumFrequencyPitchNoise = static_cast<float>(
        std::sin(noiseElapsedSeconds * 10.47 + 0.9) * 0.6
        + std::sin(noiseElapsedSeconds * 5.65 + 1.8) * 0.4);
    const float lowFrequencyYawNoise =
        static_cast<float>(std::sin(noiseElapsedSeconds * 1.26) * 0.3);
    const float lowFrequencyPitchNoise =
        static_cast<float>(std::sin(noiseElapsedSeconds * 0.94 + 0.5) * 0.2);
    const float noiseStrength = 0.15f + 0.85f * currentAimStrength;
    const float speedNoiseScale = 0.5f + 0.5f * (1.f - horizontalGainInput);
    const float yawNoise = (highFrequencyYawNoise + mediumFrequencyYawNoise
                          + lowFrequencyYawNoise) * noiseStrength * speedNoiseScale * 1.5f;
    const float pitchNoise = (highFrequencyPitchNoise + mediumFrequencyPitchNoise
                            + lowFrequencyPitchNoise) * noiseStrength * speedNoiseScale;

    if (sv.humanize) {
        if (!snapYaw) yawRate += yawNoise;
        if (sv.aimVertically && !snapPitch) pitchRate += pitchNoise;
    }

    float cmdYaw = 0.f, cmdPitch = 0.f;
    {
        const float dilation = std::max(1.f, std::min(41.f,
            deltaTime / std::max(0.0002f, g.dtRealAvg)));
        cmdYaw   = snapYaw ? 0.f : yawRate * dilation;
        cmdPitch = (sv.aimVertically && !snapPitch) ? pitchRate * dilation : 0.f;
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.cmdRateYaw   = cmdYaw;
        sh.cmdRatePitch = cmdPitch;
        sh.cmdStampNs   = nowN;
        sh.pendingYaw = 0.f;
        sh.pendingPitch = 0.f;
    }
    if (!g.initialized) g.initialized = true;
    g.lastYawDiff = yawError;
    g.lastPitchDiff = pitchError;
    g.lastTargetYaw = targetYaw;
    g.lastTargetPitch = targetPitch;
    g.lastPlayerYaw = playerYaw;
    g.lastPlayerPitch = playerPitch;

    {
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.yawSnapped = g.yawSnapped;
        sh.pitchSnapped = g.pitchSnapped;
    }

    debug::AdaptiveSnap sn;
    sn.dt = deltaTime; sn.aimStrength = currentAimStrength;
    sn.targetYaw = targetYaw; sn.targetPitch = targetPitch;
    sn.yawError = yawError; sn.pitchError = pitchError;
    sn.yawRate = yawRate; sn.pitchRate = pitchRate;
    sn.pendingYaw = pendingGet(sh, true); sn.pendingPitch = pendingGet(sh, false);
    sn.yawBias = g.yawBias; sn.pitchBias = g.pitchBias;
    sn.yawVelocity = g.yawVelocity; sn.pitchVelocity = g.pitchVelocity;
    sn.leadY = static_cast<float>(g.leadY);
    sn.aimEyeY = aimEyeY;
    sn.verticalLead = static_cast<float>(verticalLead);
    sn.adjustedAimY = static_cast<float>(adjustedAimY);
    sn.playerEyeY = playerEyeY; sn.aimY = static_cast<float>(g.aimY);
    sn.driftPos = g.driftPos; sn.driftVelocity = g.driftVelocity;
    sn.targetDistance = static_cast<float>(targetDistance);
    sn.snapYaw = snapYaw; sn.snapPitch = snapPitch;
    sn.cmdRateYaw = cmdYaw; sn.cmdRatePitch = cmdPitch;
    sn.playerYawRate = playerYawRate; sn.playerPitchRate = playerPitchRate;
    debug::ctrlAdaptive(sn);
}

}

void workerTick(JNIEnv* env, SharedState& sh, const SettingsView& sv,
                int& blockBreakCooldown)
{
    debug::workerStage("resolving JNI cache");
    if (!ensure_cache(env)) { debug::workerStage("JNI cache unavailable"); return; }
    const JniCache& c = cache();
    if (!c.mc_instance_fid) return;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;
    jobject player = c.mc_player_fid ? env->GetObjectField(mc, c.mc_player_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); player = nullptr; }
    jobject world = c.mc_world_fid ? env->GetObjectField(mc, c.mc_world_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); world = nullptr; }
    if (!world || !player) {
        debug::workerStage("no world/player");
        if (player) env->DeleteLocalRef(player);
        env->DeleteLocalRef(mc);
        return;
    }

    if (!canAim(env, sv, blockBreakCooldown)) {
        reset(env, sh);
        setTarget(sh, env, nullptr);
        env->DeleteLocalRef(player);
        env->DeleteLocalRef(world);
        env->DeleteLocalRef(mc);
        return;
    }

    jobject target = borrowTarget(sh, env);
    if (target && isDeadOrDying(env, target)) {
        env->DeleteLocalRef(target);
        target = nullptr;
        setTarget(sh, env, nullptr);
    }

    const bool attackDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    if (target && isDeadOrDying(env, target)) {
        reset(env, sh);
        setTarget(sh, env, nullptr);
        env->DeleteLocalRef(target);
        target = nullptr;
    }

    if (target) {
        const double dropLimit = sv.focusMode && sv.focusRange > 0.f
            ? static_cast<double>(sv.focusRange)
            : static_cast<double>(sv.distance);
        if (distanceToEntity(env, player, target) > dropLimit) {
            reset(env, sh);
            setTarget(sh, env, nullptr);
            env->DeleteLocalRef(target);
            target = nullptr;
        }
    }

    if (sv.requireMouseDown && !attackDown) {
        debug::workerStage("waiting for mouse click");
        if (!(sv.focusMode && target)) {
            setTarget(sh, env, nullptr);
            reset(env, sh);
        }
        if (target) env->DeleteLocalRef(target);
        env->DeleteLocalRef(player);
        env->DeleteLocalRef(world);
        env->DeleteLocalRef(mc);
        return;
    }

    const bool focusLocked = sv.focusMode && target != nullptr;

    if (!focusLocked
        && ((sv.requireMouseDown && attackDown && !target) || !sv.requireMouseDown)) {
        debug::workerStage("searching entities");
        jobject candidate = findBestTarget(env, sv);
        debug::workerStage(candidate ? "candidate selected" : "no valid candidate");

        {
            static jobject pendingCandidate = nullptr;
            static int      pendingCount = 0;
            bool same = target && candidate
                     && env->IsSameObject(target, candidate) == JNI_TRUE;
            bool adopt = same || !target || !candidate;
            if (!adopt) {
                bool isPending = pendingCandidate
                              && env->IsSameObject(pendingCandidate, candidate) == JNI_TRUE;
                if (!isPending) {
                    if (pendingCandidate) env->DeleteGlobalRef(pendingCandidate);
                    pendingCandidate = env->NewGlobalRef(candidate);
                    pendingCount = 0;
                }
                if (++pendingCount >= 3) adopt = true;
            }
            if (adopt) {
                if (pendingCandidate) {
                    env->DeleteGlobalRef(pendingCandidate);
                    pendingCandidate = nullptr;
                }
                pendingCount = 0;
            } else {

                if (candidate) { env->DeleteLocalRef(candidate); candidate = nullptr; }
                candidate = target ? env->NewLocalRef(target) : nullptr;
                if (candidate) same = true;
            }
        }
        if (!sv.requireMouseDown) {
            ++g.targetSwitchTicks;
            if (g.targetSwitchTicks > 700 || !target) {
                bool same = target && candidate
                         && env->IsSameObject(target, candidate) == JNI_TRUE;
                if (!same) reset(env, sh);
                setTarget(sh, env, candidate);
                g.targetSwitchTicks = 0;
                if (target) { env->DeleteLocalRef(target); target = nullptr; }
                if (candidate) { target = candidate; candidate = nullptr; }
            }
        } else {
            bool same = target && candidate
                     && env->IsSameObject(target, candidate) == JNI_TRUE;
            if (!same) reset(env, sh);
            setTarget(sh, env, candidate);
            if (target) env->DeleteLocalRef(target);
            target = candidate;
            candidate = nullptr;
        }
        if (candidate) env->DeleteLocalRef(candidate);
    }

    jobject screen = c.mc_screen_fid ? env->GetObjectField(mc, c.mc_screen_fid) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    const bool screenOpen = screen != nullptr
                         || ::vanta::vgui::bridge::isOverlayVisible()
                         || ::vanta::vgui::bridge::isEnabled("Freecam");   // rotation is frozen there
    if (screen) env->DeleteLocalRef(screen);

    const bool inAimRange = target
        && distanceToEntity(env, player, target) <= static_cast<double>(sv.distance);

    if (inAimRange && !screenOpen) {
        debug::workerStage("calculating aim");
        updateAim(env, mc, player, target, sh, sv);
        debug::workerStage("aim command queued");
    } else {
        debug::workerStage(screenOpen ? "screen/panel/Freecam open"
                                     : target ? "target outside aim range" : "no target");
        if (!(sv.focusMode && target)) {
            setTarget(sh, env, nullptr);
        }
        reset(env, sh);
    }

    if (target) env->DeleteLocalRef(target);
    env->DeleteLocalRef(player);
    env->DeleteLocalRef(world);
    env->DeleteLocalRef(mc);
}

void onDisable(JNIEnv* env, SharedState& sh)
{
    setTarget(sh, env, nullptr);
    reset(env, sh);
}

void renderApply(JNIEnv* env, jobject player, SharedState& sh, const SettingsView& sv)
{
    const JniCache& c = cache();
    if (::vanta::vgui::bridge::isOverlayVisible()) {
        debug::renderStage("panel visible"); return;
    }

    static std::chrono::steady_clock::time_point lastFrame = {};
    const auto nowT = std::chrono::steady_clock::now();
    float dtSec = 0.004f;
    if (lastFrame.time_since_epoch().count() > 0)
        dtSec = std::chrono::duration<float>(nowT - lastFrame).count();
    lastFrame = nowT;
    if (dtSec < 0.0005f) dtSec = 0.0005f;
    if (dtSec > 0.05f)   dtSec = 0.05f;

    const float sensitivity = playerSensitivity(env);
    const float sensitivityBase = sensitivity * 0.6f + 0.2f;
    const float sensitivityScale = sensitivityBase * sensitivityBase * sensitivityBase * 8.f;
    const float mouseAngleUnit = sensitivityScale * 0.15f;
    if (mouseAngleUnit <= 1.0E-5f) return;

    const float tau = sv.emissionSmooth * 0.0025f;
    const float a = tau > 1e-5f ? 1.f - std::exp(-dtSec / tau) : 1.f;

    int yawSteps = 0, pitchSteps = 0;
    float carryYaw = 0.f, carryPitch = 0.f;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        if (!sh.targetRef) {
            debug::renderStage("no target reference");
            sh.rateFiltYaw = sh.rateFiltPitch = 0.f;
            sh.carryYaw = sh.carryPitch = 0.f;
            sh.emitRateYaw = sh.emitRatePitch = 0.f;
            return;
        }
        float cmdYaw = 0.f, cmdPitch = 0.f;
        if (sh.cmdStampNs != 0 && nowNanos() - sh.cmdStampNs < 60'000'000LL) {
            cmdYaw = sh.cmdRateYaw;
            cmdPitch = sh.cmdRatePitch;
        }
        sh.rateFiltYaw   += (cmdYaw   - sh.rateFiltYaw)   * a;
        sh.rateFiltPitch += (cmdPitch - sh.rateFiltPitch) * a;

        auto quantize = [&](float rateDegPerSec, float& carry) -> int {
            float units = rateDegPerSec * dtSec / mouseAngleUnit + carry;
            if (units >  250.f) units =  250.f;
            if (units < -250.f) units = -250.f;
            const int steps = javaRound(units);
            carry = units - static_cast<float>(steps);
            return steps;
        };
        yawSteps   = quantize(sh.rateFiltYaw,   sh.carryYaw);
        pitchSteps = quantize(sh.rateFiltPitch, sh.carryPitch);
        carryYaw = sh.carryYaw;
        carryPitch = sh.carryPitch;
        sh.emitRateYaw   = sh.rateFiltYaw;
        sh.emitRatePitch = sh.rateFiltPitch;
    }

    const float remainingYaw = carryYaw;
    const float remainingPitch = carryPitch;
    const float mouseDeltaX =  static_cast<float>(yawSteps)   * sensitivityScale;
    const float mouseDeltaY = -static_cast<float>(pitchSteps) * sensitivityScale;

    applyTrackedMouseDelta(env, player, mouseDeltaX, mouseDeltaY);
    debug::renderStage(yawSteps || pitchSteps ? "rotation steps applied" : "zero rotation steps");

    if (debug::active() && c.ent_yaw_fid && c.ent_pitch_fid) {
        const float ly = env->GetFloatField(player, c.ent_yaw_fid);
        const float lp = env->GetFloatField(player, c.ent_pitch_fid);
        float gy = 0.f, gp = 0.f;
        if (c.ent_prev_yaw_fid) {
            gy = wrapAngleTo180_f(ly - env->GetFloatField(player, c.ent_prev_yaw_fid));
            gp = lp - env->GetFloatField(player, c.ent_prev_pitch_fid);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        const float sens = playerSensitivity(env);
        const float sb = sens * 0.6f + 0.2f;
        const float q = sb * sb * sb * 8.f * 0.15f;
        debug::setQuantum(q);
        debug::emit(static_cast<float>(yawSteps), static_cast<float>(pitchSteps),
                    remainingYaw, remainingPitch, ly, lp, gy, gp);
        (void)q; (void)sb; (void)sens;
    }

    std::lock_guard<std::mutex> lk(sh.mx);
    sh.pendingYaw = remainingYaw;
    sh.pendingPitch = remainingPitch;
}

}
