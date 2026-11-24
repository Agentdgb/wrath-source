// outline_view.cpp — see outline_view.h.

#include "outline_view.h"

#include "jvm/JniHelper.h"
#include "modules/render/outline/camera_effect_time.h"
#include "util/Log.h"

namespace vanta::modules::outline::view {

namespace {

struct cache {
    bool ok    = false;
    bool tried = false;

    jclass    mc_class    = nullptr;   // global ref
    jclass    stack_class = nullptr;   // global ref, MatrixStack
    jmethodID mc_instance = nullptr;
    jfieldID  mc_renderer = nullptr;   // gameRenderer
    jmethodID mc_rtc      = nullptr;
    jmethodID rtc_progress= nullptr;

    jmethodID gr_bobView  = nullptr;
    jmethodID gr_tiltHurt = nullptr;   // tiltViewWhenHurt; optional
    jmethodID stack_ctor  = nullptr;
    jmethodID stack_peek  = nullptr;
    jmethodID entry_matrix= nullptr;
    jmethodID m4_get      = nullptr;

    /* The video option. Optional: when it does not resolve the bob is applied
       unconditionally, which is right for the default setting and wrong only
       for someone who turned bobbing off -- and for them bobView returns the
       identity anyway, so the correction is a no-op rather than damage. */
    jfieldID  mc_options    = nullptr;
    jmethodID opts_bobView  = nullptr;
    jmethodID simple_value  = nullptr;
    jmethodID bool_value    = nullptr;
};

cache C{};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
bool ensure_cache(JNIEnv* env)
{
    if (C.ok) return true;
    if (!env) return false;

    jclass mc    = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_310");
    jclass gr    = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_757");
    jclass stack = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4587");
    jclass entry = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_4587$class_4665");
    jclass rtc   = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_9779");
    jclass m4    = env->FindClass("org/joml/Matrix4f");
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (!mc || !gr || !stack || !entry || !rtc || !m4) {
        // Normal before a world exists; the caller retries.
        return false;
    }

    C.mc_instance = env->GetStaticMethodID(mc, "method_1551", "()Lnet/minecraft/class_310;");
    C.mc_renderer = env->GetFieldID(mc, "field_1773", "Lnet/minecraft/class_757;");
    C.mc_rtc      = env->GetMethodID(mc, "method_61966", "()Lnet/minecraft/class_9779;");
    C.rtc_progress= env->GetMethodID(rtc, "method_60637", "(Z)F");

    C.gr_bobView   = env->GetMethodID(gr, "method_3186", "(Lnet/minecraft/class_4587;F)V");
    C.gr_tiltHurt  = env->GetMethodID(gr, "method_3198", "(Lnet/minecraft/class_4587;F)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    C.stack_ctor   = env->GetMethodID(stack, "<init>", "()V");
    C.stack_peek   = env->GetMethodID(stack, "method_23760",
                                      "()Lnet/minecraft/class_4587$class_4665;");
    C.entry_matrix = env->GetMethodID(entry, "method_23761", "()Lorg/joml/Matrix4f;");
    C.m4_get       = env->GetMethodID(m4, "get", "([F)[F");

    /* The View Bobbing toggle, through GameOptions.getBobView() -> SimpleOption
       -> getValue() -> Boolean. Every step is allowed to fail. */
    C.mc_options = env->GetFieldID(mc, "field_1690", "Lnet/minecraft/class_315;");
    if (jclass go = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_315")) {
        C.opts_bobView = env->GetMethodID(go, "method_42448", "()Lnet/minecraft/class_7172;");
        env->DeleteLocalRef(go);
    }
    if (jclass so = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_7172")) {
        C.simple_value = env->GetMethodID(so, "method_41753", "()Ljava/lang/Object;");
        env->DeleteLocalRef(so);
    }
    if (jclass bc = env->FindClass("java/lang/Boolean")) {
        C.bool_value = env->GetMethodID(bc, "booleanValue", "()Z");
        env->DeleteLocalRef(bc);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    const bool essential =
        C.mc_instance && C.mc_renderer && C.gr_bobView &&
        C.stack_ctor && C.stack_peek && C.entry_matrix && C.m4_get;

    if (!essential) {
        if (!C.tried) {
            C.tried = true;
            /* Named plainly, because the symptom of this failing is subtle:
               the contour simply swims while walking, which reads as a bad
               module rather than as a missing lookup. */
            VLOG_WARN("outline", "view-bob correction unavailable — the contour "
                                 "will drift from the model while walking");
        }
        return false;
    }

    C.mc_class    = static_cast<jclass>(env->NewGlobalRef(mc));
    C.stack_class = static_cast<jclass>(env->NewGlobalRef(stack));
    if (!C.mc_class || !C.stack_class) return false;

    C.ok = true;
    VLOG_INFO("outline", "view-bob correction armed");
    return true;
}

void release_cache(JNIEnv* env)
{
    if (!env) return;
    if (C.mc_class)    { env->DeleteGlobalRef(C.mc_class);    C.mc_class = nullptr; }
    if (C.stack_class) { env->DeleteGlobalRef(C.stack_class); C.stack_class = nullptr; }
    C.ok    = false;
    C.tried = false;
}

// ─────────────────────────────────────────────────────────────────────────────
bool bob_matrix(JNIEnv* env, float out[16])
{
    if (!C.ok || !env) return false;

    /* Local refs are deleted eagerly rather than left to a frame pop: this runs
       once per rendered frame, which at 300 fps is 300 stacks a second. */
    jobject mc = env->CallStaticObjectMethod(C.mc_class, C.mc_instance);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!mc) return false;

    struct scoped { JNIEnv* e; jobject o; ~scoped() { if (o) e->DeleteLocalRef(o); } };
    scoped s_mc{ env, mc };

    /* @the video option gates the BOB, never the tilt.

       This used to return false outright when View Bobbing was off, which threw
       away the whole correction -- and the damage tilt travels on the same
       matrix. So with bobbing off (a common competitive setting) the contour
       came away from the player on every hit and there was nothing to put it
       back. The flag is now carried down to the two calls instead. */
    bool bob_enabled = true;
    if (C.mc_options && C.opts_bobView && C.simple_value && C.bool_value) {
        bool on = true;
        if (jobject opts = env->GetObjectField(mc, C.mc_options)) {
            if (jobject so = env->CallObjectMethod(opts, C.opts_bobView)) {
                if (jobject val = env->CallObjectMethod(so, C.simple_value)) {
                    on = env->CallBooleanMethod(val, C.bool_value) == JNI_TRUE;
                    env->DeleteLocalRef(val);
                }
                env->DeleteLocalRef(so);
            }
            env->DeleteLocalRef(opts);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        bob_enabled = on;
    }

    /* Nothing to contribute: no tilt available and the bob switched off. */
    if (!bob_enabled && !C.gr_tiltHurt) return false;

    float tp = 0.f;
    if (C.mc_rtc && C.rtc_progress) {
        if (jobject rtc = env->CallObjectMethod(mc, C.mc_rtc)) {
            tp = env->CallFloatMethod(rtc, C.rtc_progress, JNI_TRUE);
            env->DeleteLocalRef(rtc);
        }
        if (env->ExceptionCheck()) { env->ExceptionClear(); tp = 0.f; }
    }

    jobject renderer = env->GetObjectField(mc, C.mc_renderer);
    if (!renderer) return false;
    scoped s_r{ env, renderer };

    /* A FRESH stack, so what comes back is the bob ALONE rather than the bob
       composed with whatever the game had pushed. That is what makes it safe to
       pre-multiply onto the captured modelview. */
    jobject stack = env->NewObject(C.stack_class, C.stack_ctor);
    if (!stack) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    scoped s_s{ env, stack };

// Match renderWorld: camera interpolation, then hurt tilt followed by walking bob.
    tp = ::vanta::modules::outline::view::camera_effect_time(env, renderer, tp);
    if (C.gr_tiltHurt) env->CallVoidMethod(renderer, C.gr_tiltHurt, stack, tp);
    if (bob_enabled)   env->CallVoidMethod(renderer, C.gr_bobView,  stack, tp);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    jobject entry = env->CallObjectMethod(stack, C.stack_peek);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!entry) return false;
    scoped s_e{ env, entry };

    jobject m4 = env->CallObjectMethod(entry, C.entry_matrix);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!m4) return false;
    scoped s_m{ env, m4 };

    jfloatArray arr = env->NewFloatArray(16);
    if (!arr) { if (env->ExceptionCheck()) env->ExceptionClear(); return false; }
    scoped s_a{ env, arr };

    env->CallObjectMethod(m4, C.m4_get, arr);   // joml get(float[]) — column-major
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    env->GetFloatArrayRegion(arr, 0, 16, out);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }

    return true;
}

} // namespace vanta::modules::outline::view
