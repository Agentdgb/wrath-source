

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

namespace vanta::modules::aimassist::lockon {

namespace {

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

void resolveAimTarget(JNIEnv* env, jobject player, jobject target,
                      const SettingsView& sv, double out[3])
{
    const EntPos pp = entityPos(env, player);
    const float playerEyeY = static_cast<float>(pp.y) + entityEyeHeight(env, player);
    const Box6 box = entityBox(env, target);
    const EntPos tpos = entityPos(env, target);
    double targetMinY = tpos.y, targetMaxY = tpos.y + 1.8;
    if (box.valid) { targetMinY = box.minY; targetMaxY = box.maxY; }

    if (sv.targetArea == 3  && box.valid) {
        const Vec3 closest = stableClosestPointOnBox(pp.x, playerEyeY, pp.z, box);
        const double closestY = std::max(targetMinY + 0.01,
            std::min(targetMaxY - 0.01, closest.y));
        out[0] = closest.x; out[1] = closestY; out[2] = closest.z;
        return;
    }
    const double h = targetMaxY - targetMinY;
    double frac;
    switch (sv.targetArea) {
        case 0:  frac = 0.90; break;
        case 2:  frac = 0.20; break;
        case 1:
        default: frac = 0.65; break;
    }
    const double ty = std::max(targetMinY + 0.01,
        std::min(targetMaxY - 0.01, targetMinY + h * frac));
    out[0] = tpos.x; out[1] = ty; out[2] = tpos.z;
}

int g_targetSwitchTicks = 0;

constexpr float kStrengths[] = { 0.62f, 0.41f, 0.83f, 0.55f, 0.72f, 0.47f, 0.68f };
int   g_seqIdx = 0;
float nextStrength() {
    const float s = kStrengths[g_seqIdx];
    g_seqIdx = (g_seqIdx + 1) % (sizeof(kStrengths) / sizeof(kStrengths[0]));
    return s;
}

uint32_t g_weyl = 0x9E3779B1u;
int weylJitter() {
    g_weyl += 0x9E3779B1u;
    const int step = (g_weyl >> 30) & 0x3;
    return step - 1;
}

}

void workerTick(JNIEnv* env, SharedState& sh, const SettingsView& sv,
                int& blockBreakCooldown)
{
    if (!ensure_cache(env)) return;
    const JniCache& c = cache();
    if (!c.mc_instance_fid) return;

    jobject mc = env->GetStaticObjectField(c.minecraft_class, c.mc_instance_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;
    vanta::jvm::LocalRef<jobject> mcRef{env, mc};
    jobject player = c.mc_player_fid ? env->GetObjectField(mc, c.mc_player_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); player = nullptr; }
    jobject world = c.mc_world_fid ? env->GetObjectField(mc, c.mc_world_fid) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); world = nullptr; }
    if (!world || !player) {
        if (player) env->DeleteLocalRef(player);
        return;
    }
    vanta::jvm::LocalRef<jobject> playerRef{env, player};
    vanta::jvm::LocalRef<jobject> worldRef{env, world};

    if (!canAim(env, sv, blockBreakCooldown)) {
        setTarget(sh, env, nullptr);
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
        setTarget(sh, env, nullptr);
        env->DeleteLocalRef(target);
        target = nullptr;
    }

    if (target) {
        const double dropLimit = sv.focusMode && sv.focusRange > 0.f
            ? static_cast<double>(sv.focusRange)
            : static_cast<double>(sv.distance);
        if (distanceToEntity(env, player, target) > dropLimit) {
            setTarget(sh, env, nullptr);
            env->DeleteLocalRef(target);
            target = nullptr;
        }
    }

    if (sv.requireMouseDown && !attackDown) {
        if (!(sv.focusMode && target)) {
            setTarget(sh, env, nullptr);
        }
        if (target) env->DeleteLocalRef(target);
        return;
    }

    const bool focusLocked = sv.focusMode && target != nullptr;

    if (!focusLocked
        && ((sv.requireMouseDown && attackDown && !target) || !sv.requireMouseDown)) {
        jobject candidate = findBestTarget(env, sv);
        if (!sv.requireMouseDown) {
            ++g_targetSwitchTicks;
            if (g_targetSwitchTicks > 700 || !target) {
                setTarget(sh, env, candidate);
                g_targetSwitchTicks = 0;
                if (target) { env->DeleteLocalRef(target); target = nullptr; }
                if (candidate) { target = candidate; candidate = nullptr; }
            }
        } else {
            setTarget(sh, env, candidate);
            if (target) env->DeleteLocalRef(target);
            target = candidate;
            candidate = nullptr;
        }
        if (candidate) env->DeleteLocalRef(candidate);
    }

    if (target) env->DeleteLocalRef(target);
}

void renderApply(JNIEnv* env, jobject player, SharedState& sh, const SettingsView& sv)
{
    const JniCache& c = cache();
    if (::vanta::vgui::bridge::isOverlayVisible()) return;
    if (::vanta::vgui::bridge::isEnabled("Freecam")) return;   // rotation is frozen there
    if (!c.ent_yaw_fid || !c.ent_pitch_fid) return;

    jobject target = nullptr;
    {
        std::lock_guard<std::mutex> lk(sh.mx);
        if (!sh.targetRef) {
            sh.lockonCarryYaw = sh.lockonCarryPitch = 0.f;
            return;
        }
        target = env->NewLocalRef(sh.targetRef);
    }
    if (!target) return;
    vanta::jvm::LocalRef<jobject> targetRef{env, target};

    if (distanceToEntity(env, player, target) > static_cast<double>(sv.distance)) {
        std::lock_guard<std::mutex> lk(sh.mx);
        sh.lockonCarryYaw = sh.lockonCarryPitch = 0.f;
        return;
    }

    const float td = renderTickDelta(env);
    const EntPos pp = entityRenderPos(env, player, td);
    const float playerEyeY = static_cast<float>(pp.y) + entityEyeHeight(env, player);

    double aimPoint[3];
    resolveAimTarget(env, player, target, sv, aimPoint);
    {
        const EntPos tTick = entityPos(env, target);
        const EntPos tRender = entityRenderPos(env, target, td);
        aimPoint[0] += tRender.x - tTick.x;
        aimPoint[1] += tRender.y - tTick.y;
        aimPoint[2] += tRender.z - tTick.z;
    }

    const double dx = aimPoint[0] - pp.x;
    const double dy = aimPoint[1] - playerEyeY;
    const double dz = aimPoint[2] - pp.z;
    const double horiz = std::sqrt(dx * dx + dz * dz);
    const float targetYaw = static_cast<float>(
        std::atan2(dz, dx) * 57.29577951308232 - 90.0);
    float targetPitch = static_cast<float>(
        -std::atan2(dy, std::max(horiz, 1.0E-4)) * 57.29577951308232);
    if (!sv.aimVertically) targetPitch = env->GetFloatField(player, c.ent_pitch_fid);

    const float curYaw   = env->GetFloatField(player, c.ent_yaw_fid);
    const float curPitch = env->GetFloatField(player, c.ent_pitch_fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }

    const float fullDYaw   = wrapAngleTo180_f(targetYaw   - curYaw);
    const float fullDPitch = wrapAngleTo180_f(targetPitch - curPitch);

    const float sensitivity   = playerSensitivity(env);
    const float sensitivityBase = sensitivity * 0.6f + 0.2f;
    const float sensitivityScale =
        sensitivityBase * sensitivityBase * sensitivityBase * 8.f;
    const float unitDeg = sensitivityScale * 0.15f;
    if (unitDeg <= 1e-5f) return;

    const float fullUnitsY = fullDYaw   / unitDeg;
    const float fullUnitsP = fullDPitch / unitDeg;

    const float k = nextStrength();

    float wantY = fullUnitsY * k;
    float wantP = fullUnitsP * k;

    wantY += static_cast<float>(weylJitter());
    wantP += static_cast<float>(weylJitter());

    const float degY = std::fabs(wantY * unitDeg);
    const float degP = std::fabs(wantP * unitDeg);
    if (degY > 1.25f && degP > 1.25f) {
        if (degY >= degP) {
            const float cap = 1.24f / unitDeg;
            wantP = wantP >= 0.f ? std::min(wantP, cap) : std::max(wantP, -cap);
        } else {
            const float cap = 1.24f / unitDeg;
            wantY = wantY >= 0.f ? std::min(wantY, cap) : std::max(wantY, -cap);
        }
    }

    if (std::fabs(fullUnitsY) < 3.f) wantY = fullUnitsY;
    if (std::fabs(fullUnitsP) < 3.f) wantP = fullUnitsP;

    const int yawSteps   = static_cast<int>(std::floor(wantY + 0.5f));
    const int pitchSteps = static_cast<int>(std::floor(wantP + 0.5f));

    const float mouseDeltaX =  static_cast<float>(yawSteps)   * sensitivityScale;
    const float mouseDeltaY = -static_cast<float>(pitchSteps) * sensitivityScale;

    applyTrackedMouseDelta(env, player, mouseDeltaX, mouseDeltaY);

    if (debug::active()) {
        const float ly = env->GetFloatField(player, c.ent_yaw_fid);
        const float lp = env->GetFloatField(player, c.ent_pitch_fid);
        if (env->ExceptionCheck()) env->ExceptionClear();
        debug::setQuantum(unitDeg);
        debug::emit(static_cast<float>(yawSteps), static_cast<float>(pitchSteps),
                    0.f, 0.f, ly, lp, 0.f, 0.f);
    }
}

void onDisable(JNIEnv* env, SharedState& sh)
{
    setTarget(sh, env, nullptr);
    g_targetSwitchTicks = 0;
    std::lock_guard<std::mutex> lk(sh.mx);
    sh.lockonCarryYaw = sh.lockonCarryPitch = 0.f;
}

}
