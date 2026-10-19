#include "core/NativeCallbacks.h"
// spectate.cpp — see spectate.h.

#include "spectate.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"

#include <windows.h>
#include <jnihook.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

namespace vanta::modules::spectate {

namespace {

struct cache {
    bool ok    = false;
    bool tried = false;

    jclass    mc_class     = nullptr;   // global
    jclass    player_class = nullptr;   // global, class_1657
    jclass    camera_class = nullptr;   // global, class_4184 — the hooked class
    jmethodID mc_instance  = nullptr;
    jfieldID  mc_world     = nullptr;
    jfieldID  mc_player    = nullptr;
    jmethodID mc_setCamera = nullptr;
    jmethodID mc_getCamera = nullptr;
    jfieldID  mc_camera    = nullptr;   // field_1719, written raw around a packet build

    jmethodID world_getPlayers    = nullptr;
    jmethodID world_getEntityById = nullptr;   // method_8469, optional
    jmethodID list_size = nullptr;
    jmethodID list_get  = nullptr;

    jmethodID player_profile = nullptr;
    jmethodID profile_name   = nullptr;

    jmethodID ent_getId    = nullptr;   // method_5628, optional
    jmethodID ent_getYaw   = nullptr;   // read on the LOCAL player only
    jmethodID ent_getPitch = nullptr;

    jmethodID cam_setRotation = nullptr;  // method_19325 (FF)V
    jfieldID  cam_yaw   = nullptr;        // field_18718
    jfieldID  cam_pitch = nullptr;        // field_18717

    jclass    cplayer_class = nullptr;    // global, class_746 ClientPlayerEntity
    jmethodID cp_isCamera   = nullptr;    // method_3134 ()Z, the hooked one

    jfieldID  liv_hurtTime  = nullptr;    // class_1309 field_6235, optional
};

cache C{};

/* The request. Written from the panel and read by the tick, which today are
   the same wglSwapBuffers call a few dozen lines apart — but the string is
   behind a mutex anyway so a keybind or another thread can ask later without
   this file changing. */
std::mutex        g_mx;
std::string       g_want;
std::atomic<bool> g_active{ false };
std::atomic<bool> g_resolved{ false };

/* Whether the camera is ours to put back. Set only after a successful attach,
   so a restore never fires for a camera we never moved. */
bool g_attached = false;

/* @the pin. See the header.

   A GLOBAL ref to the entity the camera is inside, plus its id for the
   per-frame identity check. Owned by the game thread; request()/stop() never
   touch it, they only raise flags. */
jobject g_target    = nullptr;
int     g_target_id = -1;
double  g_next_walk = 0.0;   // ms; throttle on the name walk

/* Published for the render modules, same pattern as chams' self id. */
std::atomic<int> g_cam_id{ -1 };

/* The detour substitutes while this is set. Set by tick() once the camera is
   attached, cleared by every path that gives the camera back. */
std::atomic<bool> g_override{ false };

/* The hook. ORIG is the original Camera.setRotation, called non-virtually on
   the camera class the way every other JNIHook user in this tree does it. */
jmethodID         ORIG_setRotation = nullptr;
std::atomic<bool> g_hooked{ false };
bool              g_hook_tried = false;

/* @the 1.21.7+ trap, and the one check that catches it.

   triggerbot_hook.cpp records that under MC 1.21.7+ (JVM 21, stricter
   verification) the class COPY JNIHook hands back as "the original" can be a
   silent no-op: CallNonvirtual returns, no exception, nothing ran. For
   tickMovement that froze the player. For setRotation it would freeze the
   CAMERA — for all play, spectating or not, because even the passthrough
   routes through the copy — and nothing would say so.

   setRotation stores its two arguments verbatim into field_18718/field_18717
   (read out of the bytecode on 1.21 and 1.21.11), so the detour can PROVE the
   original ran by reading them back. Checked on the first few calls only;
   once proven the check is dropped. A miss sets g_orig_dead, tick() detaches
   on the same thread at the end of that frame, and free look is reported
   unavailable rather than the camera being dead. */
std::atomic<bool>     g_orig_dead{ false };
std::atomic<unsigned> g_verified{ 0 };
constexpr unsigned    k_verify_calls = 8;

/* @the second hook: ClientPlayerEntity.isCamera, and why the anticheat cares.

   sendMovementPackets (method_3136) begins with `if (!isCamera()) return;`,
   read out of the bytecode on 1.21 (offset 53 -> 476) and 1.21.11 (5 -> 387).
   So while the camera is in somebody else's head the client sends the server
   NO position packet at all, and vanilla otherwise sends one at least every 20
   ticks even standing still. Grim's BadPacketsR counts exactly that silence
   ("positions=0" over ~2 s) and flags it, which is what the user saw.

   isCamera is literally `client.getCameraEntity() == this` on both versions,
   nine instructions. The detour computes that itself and never calls the
   copy, so the 1.21.7+ no-op trap above cannot reach it; while spectating it
   answers true instead, and the movement packets flow as if the camera were
   home. The only instance of class_746 is the local player, so `thiz` needs no
   identity check. */
std::atomic<bool> g_cam_hooked{ false };
std::atomic<bool> g_lie_camera{ false };     // true while attached to somebody else

/* @the external hold and override (see the header). Freecam's. */
std::atomic<bool>             g_ext_hold{ false };
jobject                       g_ext_cam = nullptr;      // global ref, owned here
std::atomic<RotationOverride> g_rot_override{ nullptr };

/* @taking a hit ends the watch.

   Watching somebody else while a fight starts on your own body is the one state
   this feature can put you in that you cannot see your way out of -- the camera
   is across the map and the first thing you know about it is the death screen.

   hurtTime is the signal the rest of the client already uses for "damage just
   landed" (hitfx polls it on other players, AutoJumpReset on the local one). It
   is set to its maximum the instant damage applies and counts down from there,
   so a RISING edge is exactly one hit. No packet hook, no event, and it works
   on every version and launcher.

   Seeded to -1 rather than 0 so the first frame of a watch establishes a
   baseline instead of reading as an edge: attach while already mid-knockback
   from something else and a plain zero would fire immediately. */
int g_last_hurt = -1;

/* The camera is on loan to the local player for the length of one packet
   build. Game thread only, and always paired. */
bool g_borrowed = false;

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

std::string read_jstr(JNIEnv* env, jstring js)
{
    if (!js) return {};
    const char* c = env->GetStringUTFChars(js, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(js, c);
    env->DeleteLocalRef(js);
    return s;
}

std::string profile_name_of(JNIEnv* env, jobject player)
{
    std::string pname;
    if (jobject profile = env->CallObjectMethod(player, C.player_profile)) {
        pname = read_jstr(env,
            static_cast<jstring>(env->CallObjectMethod(profile, C.profile_name)));
        env->DeleteLocalRef(profile);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    return pname;
}

// ─────────────────────────────────────────────────────────────────────────────
// The detour.
//
// Fires for every Camera.setRotation, spectating or not — the guard is one
// atomic read. While overriding, the incoming (yaw, pitch) is one of:
//
//   A. entity.getYaw(td), entity.getPitch(td)   — the target's look. REPLACED
//      with the local player's, read live from mc.player so the mouse has
//      already moved it this frame.
//   B. this.yaw + 180, -this.pitch              — the third-person-front flip.
//      Computed from the fields call A just wrote, i.e. from OUR values, so it
//      is passed through untouched. Recognised by exactly that relation to
//      the camera's current fields.
//   C. bedDirection - 180, 0                    — a sleeping target. Replaced
//      like A: you keep looking where you look.
//
// A CallNonvirtual on the original with the (possibly substituted) arguments is
// what the game sees, so the rotation quaternion, the frustum and everything
// downstream are built from the substituted value with no seam.
/* Case B above: the third-person-front flip, recognised by its exact relation
   to the fields the previous call wrote. */
bool is_flip(jfloat yaw, jfloat pitch, jfloat cy, jfloat cp)
{
    return std::fabs(yaw - (cy + 180.f)) < 1e-3f
        && std::fabs(pitch + cp)         < 1e-3f;
}

void JNICALL hkSetRotation(JNIEnv* env, jobject thiz, jfloat yaw, jfloat pitch)
{
    vanta::NativeCallback callbackLifetime;
    /* Spectate's own substitution wins; the external override (Freecam) only
       runs when spectate is not overriding, and the two are mutually
       exclusive at the module level anyway. */
    const bool spectating = g_override.load(std::memory_order_relaxed) && C.ok;
    const RotationOverride ext = spectating ? nullptr
                                            : g_rot_override.load(std::memory_order_relaxed);

    if ((spectating || ext) && C.cam_yaw && C.cam_pitch) {
        const jfloat cy = env->GetFloatField(thiz, C.cam_yaw);
        const jfloat cp = env->GetFloatField(thiz, C.cam_pitch);
        const bool flip = is_flip(yaw, pitch, cy, cp);

        if (!flip && ext) {
            /* The holder decides; a false return or a non-finite answer keeps
               the game's own values. Raw JNI only on that side, never a
               throw, and the pending-exception rule is the same as ours. */
            float y = yaw, p = pitch;
            if (ext(env, thiz, y, p) && std::isfinite(y) && std::isfinite(p)) {
                yaw = y; pitch = p;
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        } else if (!flip) {
            if (jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance)) {
                if (jobject self = env->GetObjectField(mc, C.mc_player)) {
                    const jfloat sy = env->CallFloatMethod(self, C.ent_getYaw);
                    const jfloat sp = env->CallFloatMethod(self, C.ent_getPitch);
                    if (!env->ExceptionCheck() && std::isfinite(sy) && std::isfinite(sp)) {
                        yaw = sy; pitch = sp;
                    }
                    env->DeleteLocalRef(self);
                }
                env->DeleteLocalRef(mc);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

    bool ran = false;
    if (ORIG_setRotation && C.camera_class) {
        env->CallNonvirtualVoidMethod(thiz, C.camera_class, ORIG_setRotation, yaw, pitch);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else ran = true;
    }

    /* Prove the original ran (see g_orig_dead). Exact compare is right: the
       method stores the floats untouched, and both are finite by construction
       on every path that reaches here. */
    if (ran && !g_orig_dead.load(std::memory_order_relaxed)
        && g_verified.load(std::memory_order_relaxed) < k_verify_calls
        && C.cam_yaw && C.cam_pitch && std::isfinite(yaw) && std::isfinite(pitch)) {
        const jfloat wy = env->GetFloatField(thiz, C.cam_yaw);
        const jfloat wp = env->GetFloatField(thiz, C.cam_pitch);
        if (wy == yaw && wp == pitch)
            g_verified.fetch_add(1, std::memory_order_relaxed);
        else
            g_orig_dead.store(true, std::memory_order_release);
    }

    /* Dead original, detach not landed yet (it lands at swap, this frame): keep
       the two angle fields honest so nothing downstream reads stale ones. The
       rotation quaternion cannot be rebuilt from here; that is what the detach
       is for. */
    if (g_orig_dead.load(std::memory_order_relaxed) && C.cam_yaw && C.cam_pitch) {
        env->SetFloatField(thiz, C.cam_yaw,   yaw);
        env->SetFloatField(thiz, C.cam_pitch, pitch);
    }
}

/* Remove a detour. True when the game's own bytecode is back and that detour's
   state may be released; false when the method is STILL native and still bound
   into this DLL, in which case every id it reads must stay alive and the hook
   must stay callable.

   TWO functions, and that is the point. The camera hook's original can turn
   out to be a no-op copy (see g_orig_dead) and has to come off mid-session;
   the isCamera hook never calls a copy at all, so that has nothing to do with
   it — tearing them down together meant one bad build silently lost the
   anticheat fix as well as the free look. */
bool detach_rotation_hook(const char* why)
{
    if (!g_hooked.exchange(false, std::memory_order_acq_rel)) return true;
    if (!C.cam_setRotation) return true;

    const jnihook_result_t r = JNIHook_Detach(C.cam_setRotation);
    if (r == JNIHOOK_OK) {
        VLOG_INFO("spectate", "Camera.setRotation unhooked ({})", why);
        ORIG_setRotation = nullptr;
        return true;
    }

    /* The method is still ACC_NATIVE and still points at hkSetRotation. A
       detour with a null ORIG is a frozen camera; a detour in an unloaded DLL
       is a crash. Leave everything it needs in place and say so. */
    g_hooked.store(true, std::memory_order_release);
    VLOG_ERR("spectate", "JNIHook_Detach(Camera.setRotation) FAILED ({}) while {} — the "
               "detour stays live and its state is kept; DO NOT UNLOAD the client",
               static_cast<int>(r), why);
    return false;
}

bool detach_camera_hook(const char* why)
{
    if (!g_cam_hooked.exchange(false, std::memory_order_acq_rel)) return true;
    if (!C.cp_isCamera) return true;

    const jnihook_result_t r = JNIHook_Detach(C.cp_isCamera);
    if (r == JNIHOOK_OK) return true;

    g_cam_hooked.store(true, std::memory_order_release);
    VLOG_ERR("spectate", "JNIHook_Detach(isCamera) FAILED ({}) while {} — the detour "
             "stays live; DO NOT UNLOAD the client", static_cast<int>(r), why);
    return false;
}

/* Both, independently: one failure must not leave the other in place. */
bool detach_hook(const char* why)
{
    const bool a = detach_rotation_hook(why);
    const bool b = detach_camera_hook(why);
    return a && b;
}

/* @who is asking, and why only one caller may be lied to.

   isCamera has three callers on ClientPlayerEntity: sendMovementPackets, which
   is the one the anticheat cares about, and tickMovement / tickMovementInput,
   which decide whether the keyboard reaches your body. Lie to those two and
   WASD walks your real body around, blind, while you are looking out of
   somebody else's head — off a cliff you cannot see. So the frame above this
   one is inspected and only the packet builder gets the lie.

   The copy class JNIHook makes keeps the original method NAMES, so this is
   still right when the caller is a copy of sendMovementPackets. */
bool caller_builds_movement_packet()
{
    jvmtiEnv* jt = ::vanta::jvm::ctx().jvmti.load();
    if (!jt) return true;   /* cannot tell callers apart; the flag matters more */

    jmethodID caller = nullptr;
    jlocation where  = 0;
    /* depth 0 is this native frame; depth 1 is whoever called it. */
    if (jt->GetFrameLocation(nullptr, 1, &caller, &where) != JVMTI_ERROR_NONE || !caller)
        return true;

    /* jmethodIDs are stable for the life of the class and this only ever runs
       on the game thread, so one remembered answer each way is the whole cache. */
    static jmethodID s_yes = nullptr;
    static jmethodID s_no  = nullptr;
    if (caller == s_yes) return true;
    if (caller == s_no)  return false;

    char* name = nullptr;
    bool  yes  = false;
    if (jt->GetMethodName(caller, &name, nullptr, nullptr) == JVMTI_ERROR_NONE && name)
        yes = std::strcmp(name, "method_3136") == 0;
    if (name) jt->Deallocate(reinterpret_cast<unsigned char*>(name));

    (yes ? s_yes : s_no) = caller;
    return yes;
}

jboolean JNICALL hkIsCamera(JNIEnv* env, jobject thiz)
{
    vanta::NativeCallback callbackLifetime;
    if ((g_lie_camera.load(std::memory_order_relaxed) || g_ext_hold.load(std::memory_order_relaxed))
        && caller_builds_movement_packet())
        return JNI_TRUE;

    jboolean real = JNI_FALSE;
    if (C.mc_class && C.mc_instance && C.mc_getCamera) {
        if (jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance)) {
            if (jobject cam = env->CallObjectMethod(mc, C.mc_getCamera)) {
                real = env->IsSameObject(cam, thiz);
                env->DeleteLocalRef(cam);
            }
            env->DeleteLocalRef(mc);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    return real;
}

/* SEH around the attach, in a function with no C++ objects to unwind. Lunar
   and Feather apply aggressive mixins to the render classes and JNIHook can
   fault while rewriting one — the triggerbot and the chams culler hit exactly
   this and guard it the same way. */
jnihook_result_t attach_guarded(jmethodID mid, void* detour, jmethodID* orig, unsigned long* code)
{
    jnihook_result_t r = JNIHOOK_ERR_JVMTI_OPERATION;
    __try {
        r = JNIHook_Attach(mid, detour, orig);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = static_cast<unsigned long>(GetExceptionCode());
        r = JNIHOOK_ERR_JVMTI_OPERATION;
    }
    return r;
}

bool ensure_hook(JNIEnv* env)
{
    if (g_hooked.load(std::memory_order_acquire)) return true;
    if (g_hook_tried) return false;   // one attempt per process; a fault is not retried
    g_hook_tried = true;

    JavaVM* vm = ::vanta::jvm::ctx().vm.load();
    if (!vm && env->GetJavaVM(&vm) != JNI_OK) vm = nullptr;
    if (!vm) return false;

    /* Idempotent in practice; the triggerbot, the silent aim and the chams
       culler each call it. ADD_JVMTI_CAPS is the "already initialised" case. */
    const jnihook_result_t ir = JNIHook_Init(vm);
    if (ir != JNIHOOK_OK && ir != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        VLOG_WARN("spectate", "JNIHook_Init failed ({}) — free look unavailable", static_cast<int>(ir));
        return false;
    }

    unsigned long code = 0;
    const jnihook_result_t ar = attach_guarded(C.cam_setRotation,
        reinterpret_cast<void*>(hkSetRotation), &ORIG_setRotation, &code);
    /* JNIHook returns its JNI failures with the Java exception still pending. */
    if (env->ExceptionCheck()) env->ExceptionClear();

    const bool free_look = (ar == JNIHOOK_OK);
    if (free_look) {
        g_hooked.store(true, std::memory_order_release);
        VLOG_INFO("spectate", "Camera.setRotation hooked — free look armed");
    } else {
        ORIG_setRotation = nullptr;
        if (code)
            VLOG_WARN("spectate", "JNIHook_Attach(Camera.setRotation) faulted (SEH {}) — the host has mixins on the camera; free look unavailable", code);
        else
            VLOG_WARN("spectate", "JNIHook_Attach(Camera.setRotation) returned {} — free look unavailable", static_cast<int>(ar));
    }

    /* @attempted even when the camera attach failed, and that is deliberate.

       These two hooks share nothing but the library. The anticheat half is the
       one that must not be lost to an unrelated failure — being flagged is
       worse than looking where the target looks. Gated on mc_getCamera because
       the detour's HONEST answer is computed from it; without it the detour
       would answer false for every caller and freeze the player. No original is
       asked for: nothing calls it, and asking makes JNIHook resolve a method on
       the copy class, which is one more way for the attach to fail. */
    if (C.cp_isCamera && C.mc_getCamera) {
        unsigned long code2 = 0;
        const jnihook_result_t ar2 = attach_guarded(C.cp_isCamera,
            reinterpret_cast<void*>(hkIsCamera), nullptr, &code2);
        if (env->ExceptionCheck()) env->ExceptionClear();

        if (ar2 == JNIHOOK_OK) {
            g_cam_hooked.store(true, std::memory_order_release);
            VLOG_INFO("spectate", "ClientPlayerEntity.isCamera hooked — movement packets keep flowing while spectating");
        } else {
            VLOG_WARN("spectate", "JNIHook_Attach(isCamera) failed ({}, SEH {}) — the camera "
                      "loan around the packet build is the only cover left",
                      static_cast<int>(ar2), code2);
        }
    }
    return free_look;
}

bool ensure(JNIEnv* env)
{
    if (C.ok) return true;
    if (!env) return false;

    jclass mc     = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_310");
    jclass world  = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_638");
    jclass entity = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1297");
    jclass player = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1657");
    jclass camera = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4184");
    jclass cplay  = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_746");
    if (env->ExceptionCheck()) env->ExceptionClear();

    struct scoped { JNIEnv* e; jobject o; ~scoped() { if (o) e->DeleteLocalRef(o); } };
    scoped s1{ env, mc }, s2{ env, world }, s3{ env, entity }, s4{ env, player }, s5{ env, camera },
           s6{ env, cplay };

    if (!mc || !world || !entity || !player || !camera) return false;   // no world yet

    C.mc_instance  = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    C.mc_world     = env->GetFieldID(mc, "field_1687", "Lnet/minecraft/class_638;");
    C.mc_player    = env->GetFieldID(mc, "field_1724", "Lnet/minecraft/class_746;");
    C.mc_setCamera = env->GetMethodID(mc, "method_1504", "(Lnet/minecraft/class_1297;)V");
    C.mc_getCamera = env->GetMethodID(mc, "method_1560", "()Lnet/minecraft/class_1297;");
    C.mc_camera    = env->GetFieldID(mc, "field_1719", "Lnet/minecraft/class_1297;");

    C.world_getPlayers    = env->GetMethodID(world, "method_18456", "()Ljava/util/List;");
    C.world_getEntityById = env->GetMethodID(world, "method_8469",
                                             "(I)Lnet/minecraft/class_1297;");
    if (jclass lst = env->FindClass("java/util/List")) {
        C.list_size = env->GetMethodID(lst, "size", "()I");
        C.list_get  = env->GetMethodID(lst, "get", "(I)Ljava/lang/Object;");
        env->DeleteLocalRef(lst);
    }

    C.player_profile = env->GetMethodID(player, "method_7334",
                                        "()Lcom/mojang/authlib/GameProfile;");
    if (jclass gp = ::vanta::jvm::findClassAnyLoader(env, "com/mojang/authlib/GameProfile")) {
        C.profile_name = env->GetMethodID(gp, "getName", "()Ljava/lang/String;");
        env->DeleteLocalRef(gp);
    }

    C.ent_getId    = env->GetMethodID(entity, "method_5628",  "()I");
    C.ent_getYaw   = env->GetMethodID(entity, "method_36454", "()F");
    C.ent_getPitch = env->GetMethodID(entity, "method_36455", "()F");

    C.cam_setRotation = env->GetMethodID(camera, "method_19325", "(FF)V");
    C.cam_yaw         = env->GetFieldID(camera, "field_18718", "F");
    C.cam_pitch       = env->GetFieldID(camera, "field_18717", "F");
    if (env->ExceptionCheck()) env->ExceptionClear();

    /* Optional: the auto-stop on taking damage. A miss costs the safety net,
       not the feature, so it is not in `essential` below. */
    if (jclass living = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1309")) {
        C.liv_hurtTime = env->GetFieldID(living, "field_6235", "I");
        env->DeleteLocalRef(living);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    /* Optional: the anticheat half. Guarded against a second global ref — this
       function is retried every frame until it succeeds, and without the guard
       a build that never satisfies `essential` would leak one per frame. */
    if (cplay && !C.cplayer_class) {
        C.cp_isCamera = env->GetMethodID(cplay, "method_3134", "()Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (C.cp_isCamera) C.cplayer_class = static_cast<jclass>(env->NewGlobalRef(cplay));
    }

    /* getEntityById and getId are optional: a miss costs the id fast path, not
       the feature. Everything else is load-bearing. */
    const bool essential =
        C.mc_instance && C.mc_world && C.mc_player && C.mc_setCamera &&
        C.world_getPlayers && C.list_size && C.list_get &&
        C.player_profile && C.profile_name &&
        C.ent_getYaw && C.ent_getPitch &&
        C.cam_setRotation && C.cam_yaw && C.cam_pitch;

    if (!essential) {
        if (!C.tried) {
            C.tried = true;
            VLOG_WARN("spectate", "camera lookup incomplete — spectate will do nothing");
        }
        return false;
    }

    C.mc_class     = static_cast<jclass>(env->NewGlobalRef(mc));
    C.player_class = static_cast<jclass>(env->NewGlobalRef(player));
    C.camera_class = static_cast<jclass>(env->NewGlobalRef(camera));
    if (!C.mc_class || !C.camera_class) return false;

    C.ok = true;
    VLOG_INFO("spectate", "camera attach armed (id fast path: {})",
              (C.world_getEntityById && C.ent_getId) ? "yes" : "no");
    return true;
}

/* The loaded player answering to `name`, or null. Returns a LOCAL ref the caller
   owns. Matching on the profile name rather than on the display name: a server
   can rewrite the display name to anything, and the tab list this is being
   matched against carries the profile. */
jobject find_player(JNIEnv* env, jobject world, const std::string& name)
{
    jobject list = env->CallObjectMethod(world, C.world_getPlayers);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!list) return nullptr;

    const jint n = env->CallIntMethod(list, C.list_size);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(list); return nullptr; }

    jobject found = nullptr;

    for (jint i = 0; i < n && !found; ++i) {
        jobject p = env->CallObjectMethod(list, C.list_get, i);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!p) continue;

        if (profile_name_of(env, p) == name) found = p;
        else                                 env->DeleteLocalRef(p);
    }

    env->DeleteLocalRef(list);
    return found;
}

void drop_target(JNIEnv* env)
{
    if (g_target) { env->DeleteGlobalRef(g_target); g_target = nullptr; }
    g_target_id = -1;
    g_cam_id.store(-1, std::memory_order_relaxed);
}

/* Is the pinned entity still in the world? One JNI call by id, then identity. */
bool still_present(JNIEnv* env, jobject world)
{
    if (!g_target) return false;
    if (!C.world_getEntityById || g_target_id < 0) return true;   // no way to ask; trust the walk

    jobject e = env->CallObjectMethod(world, C.world_getEntityById, static_cast<jint>(g_target_id));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!e) return false;

    const bool same = env->IsSameObject(e, g_target) == JNI_TRUE;
    env->DeleteLocalRef(e);
    return same;
}

/* The target as a LOCAL ref the caller owns, or null. Pinned first, walked
   second, and the walk is throttled because "not loaded" is a NORMAL steady
   state for a tab-list name. */
jobject resolve_target(JNIEnv* env, jobject world, const std::string& name, jobject self)
{
    const double t = now_ms();

    if (g_target) {
        if (still_present(env, world)) {
            /* Without getEntityById the pin cannot be checked cheaply; re-walk on
               the throttle so a despawn is noticed within a quarter second. */
            if (C.world_getEntityById || t < g_next_walk)
                return env->NewLocalRef(g_target);
        } else {
            drop_target(env);
        }
    }

    if (t < g_next_walk) return nullptr;
    g_next_walk = t + 250.0;

    jobject found = find_player(env, world, name);
    if (!found) { drop_target(env); return nullptr; }

    /* Your own row. The camera is already yours, and latching "Watching" on it
       would be a lie; treat it as a stop. */
    if (self && env->IsSameObject(found, self) == JNI_TRUE) {
        env->DeleteLocalRef(found);
        drop_target(env);
        g_active.store(false, std::memory_order_relaxed);
        return nullptr;
    }

    drop_target(env);
    g_target = env->NewGlobalRef(found);
    if (C.ent_getId) {
        const jint id = env->CallIntMethod(found, C.ent_getId);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else g_target_id = static_cast<int>(id);
    }
    return found;
}

void restore(JNIEnv* env, jobject mc)
{
    g_override.store(false, std::memory_order_relaxed);
    g_lie_camera.store(false, std::memory_order_relaxed);
    g_borrowed = false;
    drop_target(env);
    if (!g_attached) return;

    jobject self = env->GetObjectField(mc, C.mc_player);
    if (self) {
        env->CallVoidMethod(mc, C.mc_setCamera, self);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(self);
    }
    g_attached = false;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
void request(const std::string& name)
{
    { std::lock_guard<std::mutex> lk(g_mx); g_want = name; }
    g_last_hurt = -1;   /* a new watch starts with no history; see g_last_hurt */
    g_next_walk = 0.0;   // a click is not a retry; do not make it wait
    g_resolved.store(false, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_relaxed);
}

void stop()
{
    g_active.store(false, std::memory_order_relaxed);
    g_resolved.store(false, std::memory_order_relaxed);
}

bool lying()
{
    return g_lie_camera.load(std::memory_order_relaxed)
        || g_ext_hold.load(std::memory_order_relaxed);
}

/* @lend the camera back to its owner for the length of one packet build.

   A RAW write to field_1719, never setCameraEntity: the setter also runs the
   GameRenderer's camera-changed hook, and firing that twenty times a second
   would be both wasteful and visible. Both halves run inside one synchronous
   call on the game thread, so no renderer, no tick and no other module can
   observe the camera in the borrowed state. */
void borrow_camera(JNIEnv* env)
{
    if (g_borrowed || !env || !C.ok || !C.mc_camera) return;
    if (!g_lie_camera.load(std::memory_order_relaxed)
        && !g_ext_hold.load(std::memory_order_relaxed)) return;

    if (jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance)) {
        if (jobject self = env->GetObjectField(mc, C.mc_player)) {
            env->SetObjectField(mc, C.mc_camera, self);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else                       g_borrowed = true;
            env->DeleteLocalRef(self);
        }
        env->DeleteLocalRef(mc);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void return_camera(JNIEnv* env)
{
    if (!g_borrowed) return;
    g_borrowed = false;

    if (!env || !C.ok || !C.mc_camera) return;

    /* Whoever the camera was in: the external holder's entity, else the
       watched player. */
    jobject back = g_ext_cam ? g_ext_cam : g_target;
    if (!back) return;

    if (jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance)) {
        env->SetObjectField(mc, C.mc_camera, back);
        env->DeleteLocalRef(mc);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// ─────────────────────────────────────────────────────────────────────────────
// The external hold (see the header). Game thread only, like everything else.

ArmState arm(JNIEnv* env)
{
    ArmState st{ false, false, false };
    if (!env || !ensure(env)) return st;
    st.cache = true;
    ensure_hook(env);   /* one attempt per process; a repeat call is a load */
    st.free_look = g_hooked.load(std::memory_order_acquire)
                && !g_orig_dead.load(std::memory_order_relaxed);
    st.cam_lie   = g_cam_hooked.load(std::memory_order_acquire);
    return st;
}

bool free_look_available()
{
    return g_hooked.load(std::memory_order_acquire)
        && !g_orig_dead.load(std::memory_order_relaxed);
}

void set_rotation_override(RotationOverride fn)
{
    g_rot_override.store(fn, std::memory_order_release);
}

void hold_camera(JNIEnv* env, jobject camera)
{
    if (!env || !camera) return;
    if (g_ext_cam) { env->DeleteGlobalRef(g_ext_cam); g_ext_cam = nullptr; }
    g_ext_cam = env->NewGlobalRef(camera);
    if (env->ExceptionCheck()) env->ExceptionClear();
    g_ext_hold.store(g_ext_cam != nullptr, std::memory_order_release);
    VLOG_INFO("spectate", "external hold armed by Freecam (free_look={} cam_lie={} held={})",
              free_look_available() ? 1 : 0,
              g_cam_hooked.load(std::memory_order_acquire) ? 1 : 0,
              g_ext_cam ? 1 : 0);
}

void release_camera(JNIEnv* env)
{
    g_ext_hold.store(false, std::memory_order_release);
    /* No env means no way to free the ref; a leak at teardown beats a call
       into a VM that is not there. */
    if (g_ext_cam && env) env->DeleteGlobalRef(g_ext_cam);
    g_ext_cam = nullptr;
}

bool holds_camera()
{
    return g_active.load(std::memory_order_relaxed) || g_attached || g_target != nullptr;
}

bool active()   { return g_active.load(std::memory_order_relaxed); }
bool resolved() { return g_resolved.load(std::memory_order_relaxed); }
int  camera_entity_id() { return g_cam_id.load(std::memory_order_relaxed); }

std::string target()
{
    std::lock_guard<std::mutex> lk(g_mx);
    return g_want;
}

// ─────────────────────────────────────────────────────────────────────────────
void tick(JNIEnv* env)
{
    /* Nothing asked for and nothing to undo: the common case, and it must cost
       one atomic read rather than a JNI round trip. */
    /* The trap fired during this frame's render. Same thread, so the detach is
       safe here; it is the one thing this tick must do before anything else. */
    if (g_orig_dead.load(std::memory_order_acquire) && g_hooked.load(std::memory_order_acquire)) {
        g_override.store(false, std::memory_order_relaxed);
        if (detach_rotation_hook("the original silently no-ops (1.21.7+ JNIHook trap)"))
            VLOG_WARN("spectate", "free look unavailable on this build — spectate still "
                      "attaches, the camera looks where the target looks, and movement "
                      "packets still flow");
    }

    if (!g_active.load(std::memory_order_relaxed) && !g_attached && !g_target) return;
    if (!ensure(env)) return;

    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!mc) return;

    struct scoped { JNIEnv* e; jobject o; ~scoped() { if (o) e->DeleteLocalRef(o); } };
    scoped s_mc{ env, mc };

    if (!g_active.load(std::memory_order_relaxed)) { restore(env, mc); return; }

    jobject world = env->GetObjectField(mc, C.mc_world);
    jobject self  = env->GetObjectField(mc, C.mc_player);

    /* Between worlds. Vanilla has ALREADY repointed the camera itself on
       respawn, dimension change and join, so there is nothing to undo — only
       the pin to drop before its id names somebody else in the next world. */
    if (!world || !self) {
        if (world) env->DeleteLocalRef(world);
        if (self)  env->DeleteLocalRef(self);
        g_attached = false;
        g_override.store(false, std::memory_order_relaxed);
        g_lie_camera.store(false, std::memory_order_relaxed);
        drop_target(env);
        g_resolved.store(false, std::memory_order_relaxed);
        return;
    }

    scoped s_w{ env, world };
    scoped s_s{ env, self };

    /* @hit while watching: come home.

       Read on the LOCAL player, before the target is resolved, so it fires even
       on a frame where the target has wandered out of range and the watch is
       only pending. Everything below this returns early on one path or another;
       this must not be behind any of them. */
    if (C.liv_hurtTime) {
        const jint hurt = env->GetIntField(self, C.liv_hurtTime);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else {
            const int now_hurt = static_cast<int>(hurt);

            /* Rising edge FROM ZERO. hurtTime counts down a tick at a time and
               this runs once a FRAME, so it is read many times at the same
               value between ticks; requiring the previous read to be zero is
               what makes a second hit landing before the first has decayed
               count once rather than twice. */
            const bool just_hit = g_last_hurt == 0 && now_hurt > 0;
            g_last_hurt = now_hurt;

            if (just_hit) {
                VLOG_INFO("spectate", "took a hit while watching — camera comes home");
                restore(env, mc);
                g_active.store(false, std::memory_order_relaxed);
                g_resolved.store(false, std::memory_order_relaxed);
                return;
            }
        }
    }

    /* The hook is what makes the free look; without it the camera would still
       attach but look wherever the target looks. Installed lazily, here, on the
       game thread, and only once. */
    const bool free_look = ensure_hook(env)
                        && !g_orig_dead.load(std::memory_order_relaxed);

    std::string want;
    { std::lock_guard<std::mutex> lk(g_mx); want = g_want; }

    jobject victim = want.empty() ? nullptr : resolve_target(env, world, want, self);

    /* @restore on loss.

       Nothing in vanilla resets the camera when its entity despawns; the view
       would sit frozen at that entity's last coordinates. The identity check in
       resolve_target is what makes despawn, render-distance exit, logout and
       death all the same event: the camera comes home. A request that merely
       has not resolved YET is kept standing and keeps looking. */
    if (!victim) {
        restore(env, mc);
        g_resolved.store(false, std::memory_order_relaxed);
        return;
    }

    scoped s_v{ env, victim };

    /* Attach only on CHANGE. setCameraEntity is a field write plus a
       GameRenderer hook that swaps the creeper/spider post shader — a no-op for
       a player target, but re-firing it 200 times a second buys nothing. */
    bool attach = true;
    if (C.mc_getCamera) {
        if (jobject cur = env->CallObjectMethod(mc, C.mc_getCamera)) {
            attach = env->IsSameObject(cur, victim) != JNI_TRUE;
            env->DeleteLocalRef(cur);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (attach) {
        env->CallVoidMethod(mc, C.mc_setCamera, victim);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    }

    g_attached = true;
    g_resolved.store(true, std::memory_order_relaxed);
    g_cam_id.store(g_target_id, std::memory_order_relaxed);
    g_override.store(free_look, std::memory_order_relaxed);
    /* "The camera is in somebody else's head" — a fact about the WORLD, not
       about which of our hooks landed. The isCamera detour reads it, and so
       does the camera loan around the packet build, and either one has to work
       when the other could not be installed. */
    g_lie_camera.store(true, std::memory_order_relaxed);
}

void shutdown(JNIEnv* env)
{
    g_active.store(false, std::memory_order_relaxed);
    g_resolved.store(false, std::memory_order_relaxed);
    g_override.store(false, std::memory_order_relaxed);
    g_lie_camera.store(false, std::memory_order_relaxed);
    g_borrowed = false;

    /* The external holder first: a Freecam still enabled at teardown must not
       be called out of the detour that is about to go, and its own clear()
       tolerates finding nothing here. */
    g_rot_override.store(nullptr, std::memory_order_release);
    g_ext_hold.store(false, std::memory_order_release);

    if (!env) { g_ext_cam = nullptr; return; }

    if (g_ext_cam) { env->DeleteGlobalRef(g_ext_cam); g_ext_cam = nullptr; }

    if (C.ok) {
        if (jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance)) {
            restore(env, mc);
            env->DeleteLocalRef(mc);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    drop_target(env);

    /* The detour lives in this DLL. Detach BEFORE the class ref goes, and
       before the DLL does — the triggerbot's shutdown does the same. And ONLY
       if the detach lands: a failed detach leaves the game calling
       hkSetRotation, which reads C.mc_class, C.camera_class and C.ok, so those
       must outlive this call in that case. A leaked global ref beats a dead
       camera, and both beat a jump into freed memory. */
    if (!detach_hook("shutting down")) {
        if (C.player_class) { env->DeleteGlobalRef(C.player_class); C.player_class = nullptr; }
        return;
    }

    if (C.camera_class)  { env->DeleteGlobalRef(C.camera_class);  C.camera_class = nullptr; }
    if (C.cplayer_class) { env->DeleteGlobalRef(C.cplayer_class); C.cplayer_class = nullptr; }
    if (C.player_class)  { env->DeleteGlobalRef(C.player_class);  C.player_class = nullptr; }
    if (C.mc_class)      { env->DeleteGlobalRef(C.mc_class);      C.mc_class = nullptr; }
    C.ok = false;
}

} // namespace vanta::modules::spectate
