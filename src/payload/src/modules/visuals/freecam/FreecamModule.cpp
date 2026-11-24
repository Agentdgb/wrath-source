// FreecamModule.cpp — a detached, spectator-like camera.
//
// WHAT THIS IS
// ------------
// The world is rendered from whatever entity MinecraftClient calls its camera
// entity. Freecam builds an OtherClientPlayerEntity (class_745) that is never
// added to the world, points the camera at it, and flies it around with the
// keyboard while the real player stands exactly where — and looks exactly
// where — it did when the camera left. Nothing about the player reaches the
// server that would not have reached it anyway.
//
// WHY THE ROTATION IS A DETOUR AND NOT A WRITE
// --------------------------------------------
// Camera.update (class_4184.method_19321, javap on 1.21 and 1.21.11) begins
// with setRotation(entity.getYaw(td), entity.getPitch(td)), and for a
// LivingEntity getYaw(td) is NOT the yaw field: class_1309 overrides it to
// lerp(lastHeadYaw, headYaw). The class_745 is never ticked, so lastHeadYaw
// stays 0 and headYaw stays at the random yaw the constructor picked; the old
// build wrote the mouse yaw through refreshPositionAndAngles, which never
// touches headYaw, and the result was a 20 Hz sawtooth between 0 and a random
// few degrees while the horizontal mouse did nothing at all (pitch worked:
// getPitch(td) is not overridden). And even a correct field write from the
// swap pump lands one frame late.
//
// So the mouse enters the camera where the game itself puts it: inside
// spectate's production-verified Camera.setRotation detour, through its
// rotation-override callback. Mouse.tick has already applied this frame's
// deltas to the REAL player by then (MinecraftClient.render calls it between
// tick() and gameRenderer.render()). The callback harvests them as
// (live - frozen), accumulates them into the freecam look, and writes the
// frozen yaw/pitch/lastYaw/lastPitch straight back onto the player.
// changeLookDirection is the only inter-tick writer of those four fields, and
// baseTick / tickMovement derive lastYaw, headYaw and bodyYaw from them at
// the next tick, so the body never turns and the movement packet built in
// sendMovementPackets carries the frozen rotation. The camera entity gets the
// freecam look in every yaw field too (yaw, lastYaw, headYaw, lastHeadYaw,
// bodyYaw, lastBodyYaw), so updateCrosshairTarget — which raycasts from
// getRotationVec(td) of the camera entity — agrees with what is drawn.
//
// One consequence, accepted: Mouse.updateMouse clamps the player's pitch to
// +-90, so with the body frozen near a pole the delta that can be harvested
// per frame in that direction is (90 - |frozen|); looking further that way is
// slower, never blocked.
//
// THE ANTICHEAT SEAM
// ------------------
// sendMovementPackets starts with `if (!isCamera()) return;` (1.21.11 offsets
// 5-8, 1.21 53-56), so with the camera in the class_745 the client would go
// silent and Grim's BadPacketsR would fire in about two seconds. Spectate
// already solved this twice over (an isCamera detour, plus a camera loan
// around silent aim's packet build, whose original runs out of a JNIHook copy
// class where the detour is invisible); Freecam plugs into both through
// spectate's external camera hold. No new JNIHook attach anywhere.
//
// MOVEMENT
// --------
// Spectator-like: WASD in the camera's yaw basis, Space up, Shift down, Ctrl
// boost, the wheel as a speed dial. The velocity approaches its target
// exponentially (a short time constant when accelerating, a longer one when
// stopping) and is integrated with the pump's QueryPerformanceCounter delta.
// Position is written at the swap pump — one frame behind the rotation, which
// reads as nothing at all once the rotation itself is live.
//
// Intermediary names, grepped from tools/genmaps/.cache/<ver>/yarn_merged.tiny
// on 1.21 and 1.21.11 (identical on both):
//     class_310  method_1551 getInstance    method_1504 setCameraEntity
//                method_1560 getCameraEntity method_18854 isOnThread
//                field_1687 world  field_1724 player  field_1755 currentScreen
//                field_1730 chunkCullingEnabled Z (public)
//     class_1297 field_6031 yaw  field_5965 pitch  field_5982 lastYaw
//                field_6004 lastPitch  method_5808 refreshPositionAndAngles
//                method_5805 isAlive  method_36454 getYaw  method_36455 getPitch
//     class_1309 field_6241 headYaw  field_6259 lastHeadYaw
//                field_6283 bodyYaw  field_6220 lastBodyYaw
//     class_1657 method_7334 getGameProfile     class_304 method_1437 unpressAll
//     class_745  <init>(class_638, GameProfile)  -- runtime-proven on 1.21.11
//
// FIRST-PERSON HANDS
// ------------------
// Hidden while the camera is ours, without a new hook: the world is drawn from
// the class_745 but the hand pass still draws the REAL player's arms and held
// items at the camera, which is exactly what the user saw. 1.21.11 has no
// renderingPanorama boolean (method_35765 tests a class_12335 field;
// class_757.field_4001 Z exists only on 1.21) and hudHidden / SPECTATOR cost
// more than they hide, but the ItemShader feature already owns a JNIHook
// detour on HeldItemRenderer.renderFirstPersonItem (class_759.method_3228),
// and renderItem (method_22976) reaches both hands, item and empty arm alike,
// only through that method (javap, 1.21.4 and 1.21.11). So attach installs
// that detour if nobody has yet and raises itemshader::set_hands_hidden, which
// makes it return without the original; every exit path lowers it. The
// ItemShader module keeps its own wanted/effect state: installing the hook for
// it does not switch the glow on, and lowering the switch does not turn the
// glow off.

#include <windows.h>
#include "core/module/Module.h"
#include "modules/shared/NativeAccess.h"
#include "modules/misc/spectate/spectate.h"
#include "modules/combat/silentaim/silentaim.h"   // silent_rotation_available: packet cover check
#include "modules/render/chams/item_shader_hook.h" // install_hook / set_hands_hidden: no first-person hands
#include "vgui/bridge.h"
#include "util/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

namespace vanta::modules {

namespace {

/* Shared between the module and the rotation override, which is a plain
   function pointer called out of spectate's detour and so cannot be a
   member. Everything runs on the game thread (the detour inside
   Camera.update, the module from the swap pump); the atomic is hygiene. */
struct FreecamState {
    jclass    mc_cls        = nullptr;   // class_310, global (owned by native::Access)
    jmethodID m_getInstance = nullptr;
    jfieldID  f_player      = nullptr;   // class_310.field_1724
    jfieldID  f_cull        = nullptr;   // class_310.field_1730
    jfieldID  f_yaw = nullptr, f_pitch = nullptr, f_lastYaw = nullptr, f_lastPitch = nullptr;  // class_1297
    jfieldID  f_headYaw = nullptr, f_lastHeadYaw = nullptr;                                   // class_1309
    jfieldID  f_bodyYaw = nullptr, f_lastBodyYaw = nullptr;                                   // class_1309
    jfieldID  f_sideways = nullptr, f_forward = nullptr, f_jumping = nullptr;                 // class_1309 input snapshot

    jobject camera = nullptr;            // global ref, the class_745

    std::atomic<bool> live{ false };     // the override may act
    float frozen_yaw = 0.f, frozen_pitch = 0.f;   // what the real player keeps
    float fc_yaw     = 0.f, fc_pitch     = 0.f;   // where the camera looks
};
FreecamState g_fc;

/* Runs inside Camera.update, after Mouse.tick applied this frame's deltas and
   before anything of the world is drawn. Raw JNI only; never throws. A false
   return leaves the game's own values in place. */
bool override_cb(JNIEnv* env, jobject /*camera*/, float& yaw, float& pitch)
{
    if (!g_fc.live.load(std::memory_order_acquire)) return false;
    if (!g_fc.mc_cls || !g_fc.m_getInstance || !g_fc.f_player || !g_fc.f_yaw) return false;

    jobject mc = env->CallStaticObjectMethod(g_fc.mc_cls, g_fc.m_getInstance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!mc) return false;
    jobject player = env->GetObjectField(mc, g_fc.f_player);
    env->DeleteLocalRef(mc);
    if (!player) return false;

    const float ly = env->GetFloatField(player, g_fc.f_yaw);
    const float lp = env->GetFloatField(player, g_fc.f_pitch);
    const bool ok = !env->ExceptionCheck() && std::isfinite(ly) && std::isfinite(lp);
    if (ok) {
        /* This frame's mouse movement is whatever moved the player away from
           the frozen rotation since the last call. */
        g_fc.fc_yaw  += ly - g_fc.frozen_yaw;
        g_fc.fc_pitch = std::clamp(g_fc.fc_pitch + (lp - g_fc.frozen_pitch), -90.f, 90.f);

        /* The body goes back where it was, current AND last, so neither the
           render lerp nor the next tick's copy sees a turn. */
        env->SetFloatField(player, g_fc.f_yaw,       g_fc.frozen_yaw);
        env->SetFloatField(player, g_fc.f_pitch,     g_fc.frozen_pitch);
        env->SetFloatField(player, g_fc.f_lastYaw,   g_fc.frozen_yaw);
        env->SetFloatField(player, g_fc.f_lastPitch, g_fc.frozen_pitch);

        /* The camera entity carries the view in every yaw field, for the
           crosshair raycast that follows updateCamera. */
        if (g_fc.camera) {
            env->SetFloatField(g_fc.camera, g_fc.f_yaw,         g_fc.fc_yaw);
            env->SetFloatField(g_fc.camera, g_fc.f_lastYaw,     g_fc.fc_yaw);
            env->SetFloatField(g_fc.camera, g_fc.f_headYaw,     g_fc.fc_yaw);
            env->SetFloatField(g_fc.camera, g_fc.f_lastHeadYaw, g_fc.fc_yaw);
            env->SetFloatField(g_fc.camera, g_fc.f_pitch,       g_fc.fc_pitch);
            env->SetFloatField(g_fc.camera, g_fc.f_lastPitch,   g_fc.fc_pitch);
        }
        yaw   = g_fc.fc_yaw;
        pitch = g_fc.fc_pitch;
    }
    env->DeleteLocalRef(player);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return ok;
}

constexpr double kPi = 3.141592653589793;

} // namespace

class Freecam : public Module {
    native::Access a;
    jobject previous = nullptr, worldRef = nullptr;   // globals
    native::Vec pos, vel;                              // feet position, blocks and blocks/s

    bool ids_ready  = false;
    bool attached   = false;      // the camera is ours; false while waiting for spectate to let go
    bool free_look  = false;      // rotation override live in spectate's detour
    bool cam_lie    = false;      // spectate's isCamera detour landed
    bool saved_cull = true, have_saved_cull = false;
    bool capture_on = false;      // last value handed to the window procedure, for the transition log
    bool hands_hidden = false;    // itemshader's switch is ours to lower
    unsigned wait_frames = 0;     // swaps spent waiting for spectate to let go

    FloatSetting speed{this, "Speed", 5.f, 0.5f, 60.f, 0.5f};
    FloatSetting boost{this, "Sprint Multiplier", 6.f, 1.f, 20.f, 0.5f};

    void resolveIds() {
        if (ids_ready) return;
        jclass mc  = a.cls("net/minecraft/class_310");
        jclass ent = a.cls("net/minecraft/class_1297");
        jclass liv = a.cls("net/minecraft/class_1309");
        g_fc.mc_cls        = mc;
        g_fc.m_getInstance = a.method(mc, "method_1551", "()Lnet/minecraft/class_310;", true);
        g_fc.f_player      = a.field(mc, "field_1724", "Lnet/minecraft/class_746;");
        g_fc.f_cull        = a.field(mc, "field_1730", "Z");
        g_fc.f_yaw         = a.field(ent, "field_6031", "F");
        g_fc.f_pitch       = a.field(ent, "field_5965", "F");
        g_fc.f_lastYaw     = a.field(ent, "field_5982", "F");
        g_fc.f_lastPitch   = a.field(ent, "field_6004", "F");
        g_fc.f_headYaw     = a.field(liv, "field_6241", "F");
        g_fc.f_lastHeadYaw = a.field(liv, "field_6259", "F");
        g_fc.f_bodyYaw     = a.field(liv, "field_6283", "F");
        g_fc.f_lastBodyYaw = a.field(liv, "field_6220", "F");
        g_fc.f_sideways    = a.field(liv, "field_6212", "F");
        g_fc.f_forward     = a.field(liv, "field_6250", "F");
        g_fc.f_jumping     = a.field(liv, "field_6282", "Z");
        ids_ready = true;
        // a.field/a.method throw on a miss, so every id below is 1 by the time
        // this prints; the line exists so the resolved set is one grep away.
        VLOG_INFO("freecam", "cache: getInstance={} player={} cull={} yaw={} pitch={} lastYaw={} lastPitch={} "
                             "headYaw={} lastHeadYaw={} bodyYaw={} lastBodyYaw={} sideways={} forward={} jumping={} -> ready",
                  g_fc.m_getInstance ? 1 : 0, g_fc.f_player ? 1 : 0, g_fc.f_cull ? 1 : 0,
                  g_fc.f_yaw ? 1 : 0, g_fc.f_pitch ? 1 : 0, g_fc.f_lastYaw ? 1 : 0, g_fc.f_lastPitch ? 1 : 0,
                  g_fc.f_headYaw ? 1 : 0, g_fc.f_lastHeadYaw ? 1 : 0, g_fc.f_bodyYaw ? 1 : 0, g_fc.f_lastBodyYaw ? 1 : 0,
                  g_fc.f_sideways ? 1 : 0, g_fc.f_forward ? 1 : 0, g_fc.f_jumping ? 1 : 0);
    }

    /* Everything the unticked class_745 needs rewritten every frame: position
       and last position, yaw/pitch and last (refreshPositionAndAngles), and the
       four head/body yaw fields it never touches — the ones the camera and the
       crosshair actually read. */
    void writeCamera() {
        a.call<void>(g_fc.camera, "net/minecraft/class_1297", "method_5808", "(DDDFF)V",
                     pos.x, pos.y, pos.z, g_fc.fc_yaw, g_fc.fc_pitch);
        for (jfieldID f : { g_fc.f_headYaw, g_fc.f_lastHeadYaw, g_fc.f_bodyYaw, g_fc.f_lastBodyYaw })
            a.e->SetFloatField(g_fc.camera, f, g_fc.fc_yaw);
        a.check();
    }

    void attach(jobject mc, jobject world, jobject player, bool capture) {
        resolveIds();
        const spectate::ArmState st = spectate::arm(a.e);

        /* Refuse rather than fly silent: with the camera in the class_745,
           sendMovementPackets returns at its isCamera() check unless either
           the isCamera lie or silent aim's camera loan covers it. No cover
           means zero position packets while transactions flow -> Grim
           BadPacketsR within seconds. tick()'s catch turns the module off. */
        if (!st.cache) {
            VLOG_WARN("freecam", "spectate cache unavailable -- no movement-packet cover, refusing to attach");
            throw native::Failure("no movement-packet cover (spectate cache)");
        }
        if (!st.cam_lie && !silentaim::silent_rotation_available()) {
            VLOG_WARN("freecam", "isCamera cover absent and silent aim's packet hook is not live -- refusing to attach");
            throw native::Failure("no movement-packet cover (isCamera + sendMovementPackets)");
        }

        auto profile = a.call<>(player, "net/minecraft/class_1657", "method_7334", "()Lcom/mojang/authlib/GameProfile;");
        auto entity  = a.make("net/minecraft/class_745", "(Lnet/minecraft/class_638;Lcom/mojang/authlib/GameProfile;)V", world, profile);
        auto old     = a.call<>(mc, "net/minecraft/class_310", "method_1560", "()Lnet/minecraft/class_1297;");
        g_fc.camera = a.e->NewGlobalRef(entity);
        previous    = a.e->NewGlobalRef(old ? old : player);
        worldRef    = a.e->NewGlobalRef(world);
        a.check();
        if (!g_fc.camera || !previous || !worldRef) throw native::Failure("Camera reference allocation failed");

        /* Snapshot: the body keeps this rotation for the whole ride. */
        float fy = a.e->GetFloatField(player, g_fc.f_yaw);
        float fp = a.e->GetFloatField(player, g_fc.f_pitch);
        if (!std::isfinite(fy)) fy = 0.f;
        if (!std::isfinite(fp)) fp = 0.f;
        g_fc.frozen_yaw   = fy;
        g_fc.frozen_pitch = std::clamp(fp, -90.f, 90.f);
        g_fc.fc_yaw       = g_fc.frozen_yaw;
        g_fc.fc_pitch     = g_fc.frozen_pitch;
        pos = native::position(a, player);   // feet; the camera adds its own eye height
        vel = {};

        /* Whatever was held when the camera left must not keep walking the
           body; from here the fly keys never reach a KeyBinding at all. */
        jclass keys = a.cls("net/minecraft/class_304");
        a.e->CallStaticVoidMethod(keys, a.method(keys, "method_1437", "()V", true));
        a.check();
        /* unpressAll only clears the KeyBindings; the input snapshot already
           copied into the body is not rewritten while the camera is elsewhere
           (tickMovementInput takes the super path, which merely decays the
           speeds by 2% a tick and never touches jumping). Zero it here or a
           held W / Space keeps the body walking and bunny-hopping. */
        a.e->SetFloatField(player, g_fc.f_sideways, 0.f);
        a.e->SetFloatField(player, g_fc.f_forward, 0.f);
        a.e->SetBooleanField(player, g_fc.f_jumping, JNI_FALSE);
        a.call<void>(player, "net/minecraft/class_1297", "method_5728", "(Z)V", false);
        a.check();

        /* Chunk culling is computed from the camera entity's position by the
           WorldRenderer, but read through this public flag (class_761's
           updateCamera on both versions; Sodium honours the same field as
           smartCull). Off, or flying through a wall renders nothing behind it. */
        saved_cull = a.e->GetBooleanField(mc, g_fc.f_cull) == JNI_TRUE;
        have_saved_cull = true;
        a.e->SetBooleanField(mc, g_fc.f_cull, JNI_FALSE);
        a.check();

        writeCamera();
        a.call<void>(mc, "net/minecraft/class_310", "method_1504", "(Lnet/minecraft/class_1297;)V", g_fc.camera);
        attached = true;

        /* Capture goes on right here, not on the next pump, so a keybind
           toggle swallows the very next W press and the armed line below
           reports the flag as it actually stands. With the ClickGui open it
           stays off and the tick raises it once the panel closes. */
        vgui::bridge::setFreecamCapture(capture);
        capture_on = capture;

        /* No hands: itemshader's detour on renderFirstPersonItem, installed
           here if the ItemShader module never did (it is one attach per
           process and says nothing about the glow). A failed install is a
           visible hand, never a refused ride. */
        hands_hidden = itemshader::install_hook();
        itemshader::set_hands_hidden(hands_hidden);
        if (!hands_hidden)
            VLOG_WARN("freecam", "first-person hand hook unavailable -- hands stay visible from the free camera");

        /* The anticheat cover, then the look. Order matters only in that the
           override must not run before the camera entity carries a sane
           rotation, which writeCamera just gave it. */
        spectate::hold_camera(a.e, g_fc.camera);
        free_look = st.free_look;
        cam_lie   = st.cam_lie;
        if (free_look) {
            g_fc.live.store(true, std::memory_order_release);
            spectate::set_rotation_override(&override_cb);
        } else {
            VLOG_WARN("freecam", "free look unavailable (Camera.setRotation detour absent) -- rotation follows "
                                 "the real player: the body turns with the mouse and packets carry it");
        }
        if (!cam_lie)
            VLOG_WARN("freecam", "isCamera cover unavailable -- movement packets rely on the camera loan around "
                                 "silent aim's packet build alone; without that hook the client goes silent, "
                                 "disable Freecam on Grim servers");
        VLOG_INFO("freecam", "armed: rotation override={} camera hold={} capture={} hands hidden={} (cache={})",
                  free_look ? 1 : 0, spectate::lying() ? 1 : 0, capture_on ? 1 : 0, hands_hidden ? 1 : 0,
                  st.cache ? 1 : 0);
    }

    /* Every self-disable, the panel toggle, the panic key and the destructor
       come through here. Order: the override is gone before the ref it reads,
       the capture before the keys are handed back, the camera before the
       culling flag. Tolerates spectate already shut down (all null-checked). */
    void clear() {
        const bool had_free_look = free_look;
        const bool was = attached || g_fc.camera != nullptr;
        g_fc.live.store(false, std::memory_order_release);
        spectate::set_rotation_override(nullptr);
        vgui::bridge::setFreecamCapture(false);
        capture_on = false;
        /* Hands back before anything else: unconditional, so a ride that
           failed half-way through attach cannot leave the switch raised. */
        itemshader::set_hands_hidden(false);
        hands_hidden = false;
        attached = false; free_look = false; cam_lie = false;
        wait_frames = 0;
        vel = {};

        jvm::JniScope scope;
        if (!scope) {
            /* No VM to talk to: nothing can be restored or freed. */
            g_fc.camera = nullptr; previous = nullptr; worldRef = nullptr;
            have_saved_cull = false;
            return;
        }
        a.e = scope.get();
        spectate::release_camera(a.e);
        try {
            native::Frame frame(a.e);
            auto mc = a.minecraft();
            if (have_saved_cull && g_fc.f_cull) {
                a.e->SetBooleanField(mc, g_fc.f_cull, saved_cull ? JNI_TRUE : JNI_FALSE);
                a.check();
            }
            have_saved_cull = false;

            if (g_fc.camera) {
                auto current = a.call<>(mc, "net/minecraft/class_310", "method_1560", "()Lnet/minecraft/class_1297;");
                auto world   = a.get(mc, "net/minecraft/class_310", "field_1687", "Lnet/minecraft/class_638;");
                auto player  = a.get(mc, "net/minecraft/class_310", "field_1724", "Lnet/minecraft/class_746;");
                /* Only if the camera is still ours: on respawn, join or a
                   dimension change vanilla has already repointed it. */
                if (current && a.e->IsSameObject(current, g_fc.camera)) {
                    jobject restore = (world && worldRef && a.e->IsSameObject(world, worldRef) && previous) ? previous : player;
                    if (restore) a.call<void>(mc, "net/minecraft/class_310", "method_1504", "(Lnet/minecraft/class_1297;)V", restore);
                }
                /* The mouse may have moved since the detour last put the body
                   back; the body resumes from the frozen rotation, not from
                   wherever the last few deltas left it. */
                if (had_free_look && player && g_fc.f_yaw) {
                    a.e->SetFloatField(player, g_fc.f_yaw,       g_fc.frozen_yaw);
                    a.e->SetFloatField(player, g_fc.f_pitch,     g_fc.frozen_pitch);
                    a.e->SetFloatField(player, g_fc.f_lastYaw,   g_fc.frozen_yaw);
                    a.e->SetFloatField(player, g_fc.f_lastPitch, g_fc.frozen_pitch);
                    a.check();
                }
            }
        } catch (const native::Failure& ex) { VLOG_WARN("freecam", "restore: {}", ex.what()); }

        for (jobject* ref : { &g_fc.camera, &previous, &worldRef }) {
            if (*ref) a.e->DeleteGlobalRef(*ref);
            *ref = nullptr;
        }
        if (was) VLOG_INFO("freecam", "disarmed -- camera, rotation and chunk culling restored");
    }

    void tick(float delta) {
        jvm::JniScope scope; if (!scope) return;
        a.e = scope.get(); native::Frame frame(a.e);
        auto mc = a.minecraft();
        if (!a.call<jboolean>(mc, "net/minecraft/class_310", "method_18854", "()Z")) return;

        auto world  = a.get(mc, "net/minecraft/class_310", "field_1687", "Lnet/minecraft/class_638;");
        auto player = a.get(mc, "net/minecraft/class_310", "field_1724", "Lnet/minecraft/class_746;");
        if (!world || !player || (worldRef && !a.e->IsSameObject(world, worldRef))) { setEnabled(false); return; }
        if (!a.call<jboolean>(player, "net/minecraft/class_1297", "method_5805", "()Z")) { setEnabled(false); return; }

        /* Spectate wins: a watch requested while the camera is ours ends the
           ride, and a request standing while we wait ends it before it starts. */
        if (spectate::active()) { setEnabled(false); return; }

        auto screen = a.get(mc, "net/minecraft/class_310", "field_1755", "Lnet/minecraft/class_437;");
        const bool capture = !screen && !vgui::bridge::isOverlayVisible() && native::focused();

        if (!attached) {
            /* onEnable asked spectate to stop; it lets go at the end of this
               swap (its tick runs after the pump), and the camera is taken on
               the next one. */
            if (spectate::holds_camera()) {
                /* Spectate's tick runs after the pump and releases at the
                   end of this swap; anything longer than a second means it
                   is not letting go and the silence deserves a line. */
                if (++wait_frames == 120)
                    VLOG_WARN("freecam", "still waiting for spectate to release the camera after {} frames", wait_frames);
                return;
            }
            wait_frames = 0;
            attach(mc, world, player, capture);
        } else {
            auto current = a.call<>(mc, "net/minecraft/class_310", "method_1560", "()Lnet/minecraft/class_1297;");
            if (!current || !a.e->IsSameObject(current, g_fc.camera)) {
                VLOG_INFO("freecam", "the game repointed the camera -- standing down");
                setEnabled(false);
                return;
            }
        }

        if (capture != capture_on) {
            /* Follows the panel and the window focus; one line per edge so
               the log shows when the fly keys were actually ours. */
            capture_on = capture;
            vgui::bridge::setFreecamCapture(capture);
            VLOG_INFO("freecam", "capture={}", capture ? 1 : 0);
        }

        /* Wheel: a geometric speed dial, so a notch means the same thing at
           1 block/s as at 20. The setting itself moves, so the panel shows it. */
        if (const int notches = vgui::bridge::takeFreecamWheelNotches()) {
            const float next = speed.value() * std::pow(1.15f, static_cast<float>(notches));
            speed.set(std::clamp(next, speed.min(), speed.max()));
        }

        /* The 1.21.7+ JNIHook trap can take the detour away mid-session
           (spectate detaches it at the end of the frame it is caught in). */
        if (free_look && !spectate::free_look_available()) {
            free_look = false;
            g_fc.live.store(false, std::memory_order_release);
            spectate::set_rotation_override(nullptr);
            VLOG_WARN("freecam", "free look lost (Camera.setRotation detour detached) -- rotation now follows the real player");
        }
        if (!free_look) {
            /* Degraded: the body turns with the mouse and the camera follows it. */
            g_fc.fc_yaw   = a.call<jfloat>(player, "net/minecraft/class_1297", "method_36454", "()F");
            g_fc.fc_pitch = a.call<jfloat>(player, "net/minecraft/class_1297", "method_36455", "()F");
        }

        /* Target velocity from the keys, in the camera's yaw basis. The Ctrl
           boost multiplies the whole vector, vertical included, so Space and
           Shift climb and dive as fast as the horizontal keys run. */
        const float dt = std::clamp(delta, 0.f, 100.f);
        double tx = 0.0, ty = 0.0, tz = 0.0;
        bool boosting = false;
        if (capture) {
            auto key = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) ? 1.0 : 0.0; };
            const double f = key('W') - key('S'), side = key('D') - key('A'), up = key(VK_SPACE) - key(VK_SHIFT);
            const double len = std::sqrt(f * f + side * side + up * up);
            boosting = key(VK_CONTROL) > 0.0;
            if (len > 0.0) {
                const double v = static_cast<double>(speed.value()) * (boosting ? boost.value() : 1.f) / len;
                const double y = static_cast<double>(g_fc.fc_yaw) * kPi / 180.0;
                tx = (-std::sin(y) * f + std::cos(y) * side) * v;
                tz = ( std::cos(y) * f + std::sin(y) * side) * v;
                ty = up * v;
            }
        }

        /* Exponential approach: quick to get going, a touch longer to settle,
           which is what makes it feel like the spectator camera rather than a
           switch. Frame-rate independent by construction. Boosting shortens
           the accelerate constant to a few frames, so Ctrl reads as a kick
           instead of a ramp toward a target six times further away. */
        const double tmag = std::sqrt(tx * tx + ty * ty + tz * tz);
        const double vmag = std::sqrt(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
        const double tau  = tmag > vmag ? (boosting ? 20.0 : 60.0) : 110.0;   // ms
        const double k    = 1.0 - std::exp(-static_cast<double>(dt) / tau);
        vel.x += (tx - vel.x) * k;
        vel.y += (ty - vel.y) * k;
        vel.z += (tz - vel.z) * k;
        const double s = static_cast<double>(dt) / 1000.0;
        pos.x += vel.x * s;
        pos.y += vel.y * s;
        pos.z += vel.z * s;

        writeCamera();
    }

public:
    Freecam() : Module("Freecam", "Detached camera: WASD, Space/Shift, Ctrl to boost, wheel for speed", Category::Visuals) {
        registerSetting(&speed);
        registerSetting(&boost);
    }
    ~Freecam() override { clear(); }

    void onInit() override {
        listen<events::GameLoopEvent>([this](events::GameLoopEvent& e) {
            if (!isEnabled()) return;
            try { tick(e.deltaMs); }
            catch (const native::Failure& ex) { VLOG_WARN("freecam", "{}", ex.what()); setEnabled(false); }
        });
    }

    /* No JNI here: the toggle may come from the panel or a keybind, and the
       camera is taken on the game thread by the next pump. */
    void onEnable() override { spectate::stop(); }
    void onDisable() override { clear(); }

    std::string displayInfo() const override {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", static_cast<double>(speed.value()));
        return buf;
    }
};
VANTA_REGISTER_MODULE(Freecam);

} // namespace vanta::modules
