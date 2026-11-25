#include "movement_idle.h"

#include "jvm/JniHelper.h"
#include "util/Log.h"

#include <sdk/mappings/mappings.hpp>

#include <windows.h>   // GetTickCount64 for the sprint-reset window

#include <atomic>
#include <cstring>

namespace dt::shared {

namespace {

// Sneak is in the list because Grim's sneaking arm counts it on 1.21.9+
// clients, and because a container action taken mid-crouch reads the same way
// as one taken mid-stride.
//
// Indices 0-5 are the bindings that feed KnownInput.moving(). Sprint is
// resolved alongside them but deliberately sits OUTSIDE that range: it is a
// modifier, not a direction, and Grim scores it on its own axis. The idle and
// suppress loops below therefore stop at 6.
const char* kMovement[7] = {
    "key.forward", "key.back", "key.left", "key.right", "key.jump", "key.sneak",
    "key.sprint"
};
constexpr int kDirectionKeys = 6;

struct Cache {
    bool     tried = false;
    int      attempts = 0;

    jclass   mc_class        = nullptr;
    jfieldID mc_options      = nullptr;
    jfieldID mc_player       = nullptr;
    jclass   key_binding     = nullptr;
    jfieldID pressed         = nullptr;

    jmethodID is_sprinting   = nullptr;

    jfieldID movement[7]     = {};
    int      movement_count  = 0;
};

// The key scan needs a live GameOptions instance, which is not guaranteed to
// exist on the first call. Retrying a bounded number of times beats latching a
// failure forever and silently turning the gate into a no-op.
constexpr int MAX_BUILD_ATTEMPTS = 40;

Cache g_c;

// True while a module (Sprint) is the one holding the sprint binding down.
// The player's own press must never be written over: with "Sprint: Toggle"
// the binding is a StickyKeyBinding whose `pressed` IS the toggle state, and
// a key release never clears it -- so writing false untoggles the player's
// sprint for good, and after every sprint hit they had to press Ctrl again.
std::atomic<bool> g_sprint_key_owned{ false };

jclass global_class(JNIEnv* env, const char* sig)
{
    jclass local = dt::jvm::findClassAnyLoader(env, sig);
    if (!local) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jclass g = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return g;
}

jfieldID field_either(JNIEnv* env, jclass cls, const char* yarn, const char* mojang, const char* sig)
{
    if (!cls) return nullptr;
    const bool mf = sdk::mappings::is_mojang_layout;
    for (const char* n : { mf ? mojang : yarn, mf ? yarn : mojang }) {
        if (!n) continue;
        jfieldID f = env->GetFieldID(cls, n, sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (f) return f;
    }
    return nullptr;
}

// The sole boolean on KeyBinding is `pressed`. Falling back to this rather than
// probing for isPressed() is deliberate: wasPressed() shares the ()Z shape and
// has a side effect -- it decrements timesPressed -- so a probe by signature
// would quietly eat queued key presses.
jfieldID sole_boolean_field(JNIEnv* env, jclass cls)
{
    jclass cls_cls = env->GetObjectClass(cls);
    if (!cls_cls) return nullptr;
    jmethodID get_fields = env->GetMethodID(cls_cls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(cls_cls); return nullptr; }

    jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(cls, get_fields));
    env->DeleteLocalRef(cls_cls);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!arr) return nullptr;

    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    jmethodID get_type = field_cls ? env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;") : nullptr;
    jclass cls_class = env->FindClass("java/lang/Class");
    jmethodID get_name = cls_class ? env->GetMethodID(cls_class, "getName", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    jfieldID found = nullptr;
    int hits = 0;
    if (get_type && get_name) {
        const jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; ++i) {
            jobject f = env->GetObjectArrayElement(arr, i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!f) continue;
            jobject t = env->CallObjectMethod(f, get_type);
            if (!env->ExceptionCheck() && t) {
                jstring nm = static_cast<jstring>(env->CallObjectMethod(t, get_name));
                if (!env->ExceptionCheck() && nm) {
                    const char* s = env->GetStringUTFChars(nm, nullptr);
                    if (s && std::strcmp(s, "boolean") == 0) {
                        ++hits;
                        if (!found) found = env->FromReflectedField(f);
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (s) env->ReleaseStringUTFChars(nm, s);
                    env->DeleteLocalRef(nm);
                } else if (env->ExceptionCheck()) env->ExceptionClear();
                env->DeleteLocalRef(t);
            } else if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(f);
        }
    }
    env->DeleteLocalRef(arr);
    if (field_cls)  env->DeleteLocalRef(field_cls);
    if (cls_class)  env->DeleteLocalRef(cls_class);

    // Only trust it when it really is the sole one.
    return hits == 1 ? found : nullptr;
}

// Walks GameOptions' declared fields and picks out the KeyBinding-typed ones by
// their translation key. Field NAMES for the individual bindings differ between
// mapping flavours and move between versions; the translation keys have been
// stable since 1.7, which is what makes this work on every launcher.
void resolve_movement_keys(JNIEnv* env, jobject options)
{
    if (!g_c.key_binding) return;

    jclass opts_cls = env->GetObjectClass(options);
    if (!opts_cls) return;

    jclass cls_class = env->FindClass("java/lang/Class");
    jmethodID get_fields = cls_class
        ? env->GetMethodID(cls_class, "getDeclaredFields", "()[Ljava/lang/reflect/Field;") : nullptr;
    jclass field_cls = env->FindClass("java/lang/reflect/Field");
    jmethodID get_type   = field_cls ? env->GetMethodID(field_cls, "getType", "()Ljava/lang/Class;") : nullptr;
    jmethodID set_access = field_cls ? env->GetMethodID(field_cls, "setAccessible", "(Z)V") : nullptr;
    jmethodID field_get  = field_cls
        ? env->GetMethodID(field_cls, "get", "(Ljava/lang/Object;)Ljava/lang/Object;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();

    jmethodID get_translation = env->GetMethodID(g_c.key_binding,
        sdk::mappings::key_binding_get_translation_key_name,
        sdk::mappings::key_binding_get_translation_key_sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!get_translation) {
        get_translation = env->GetMethodID(g_c.key_binding, "getName", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (get_fields && get_type && set_access && field_get && get_translation) {
        jobjectArray arr = static_cast<jobjectArray>(env->CallObjectMethod(opts_cls, get_fields));
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (arr) {
            const jsize n = env->GetArrayLength(arr);
            for (jsize i = 0; i < n; ++i) {
                jobject f = env->GetObjectArrayElement(arr, i);
                if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
                if (!f) continue;

                jobject ftype = env->CallObjectMethod(f, get_type);
                if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }
                const bool is_kb = ftype && env->IsSameObject(ftype, g_c.key_binding) == JNI_TRUE;
                if (ftype) env->DeleteLocalRef(ftype);
                if (!is_kb) { env->DeleteLocalRef(f); continue; }

                env->CallVoidMethod(f, set_access, JNI_TRUE);
                if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }

                jobject kb = env->CallObjectMethod(f, field_get, options);
                if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(f); continue; }
                if (!kb) { env->DeleteLocalRef(f); continue; }

                jstring key = static_cast<jstring>(env->CallObjectMethod(kb, get_translation));
                if (env->ExceptionCheck()) env->ExceptionClear();
                else if (key) {
                    if (const char* utf = env->GetStringUTFChars(key, nullptr)) {
                        for (int k = 0; k < 7; ++k) {
                            if (std::strcmp(utf, kMovement[k]) != 0) continue;
                            if (!g_c.movement[k]) {
                                jfieldID fid = env->FromReflectedField(f);
                                if (env->ExceptionCheck()) env->ExceptionClear();
                                else if (fid) { g_c.movement[k] = fid; ++g_c.movement_count; }
                            }
                            break;
                        }
                        env->ReleaseStringUTFChars(key, utf);
                    }
                    env->DeleteLocalRef(key);
                }
                env->DeleteLocalRef(kb);
                env->DeleteLocalRef(f);
            }
            env->DeleteLocalRef(arr);
        }
    }

    env->DeleteLocalRef(opts_cls);
    if (cls_class) env->DeleteLocalRef(cls_class);
    if (field_cls) env->DeleteLocalRef(field_cls);
}

void build(JNIEnv* env, jobject mc)
{
    // Done once the key scan has actually produced something; until then keep
    // retrying, because the scan needs a live GameOptions to walk.
    if (g_c.tried && g_c.movement_count > 0) return;
    if (g_c.attempts >= MAX_BUILD_ATTEMPTS) return;
    ++g_c.attempts;

    const bool first = !g_c.tried;
    g_c.tried = true;

    if (first) {
        g_c.mc_class = global_class(env, sdk::mappings::minecraftclass_sig);
    }
    if (!g_c.mc_class) return;

    g_c.mc_options = field_either(env, g_c.mc_class,
        "field_1690", "options", sdk::mappings::game_options_sig);
    if (!g_c.mc_options) {
        g_c.mc_options = env->GetFieldID(g_c.mc_class, "options",
                                         "Lnet/minecraft/client/Options;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (!g_c.mc_player) {
        g_c.mc_player = env->GetFieldID(g_c.mc_class,
            sdk::mappings::player_name, sdk::mappings::player_sig);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    // Resolved on PlayerEntity: GetMethodID finds it inherited from Entity, so
    // there is no need to name the Entity class here.
    if (!g_c.is_sprinting) {
        if (jclass pe = global_class(env, sdk::mappings::player_entity_class_sig)) {
            const bool mf = sdk::mappings::is_mojang_layout;
            for (const char* n : { mf ? "isSprinting" : sdk::mappings::is_sprinting_name,
                                   mf ? sdk::mappings::is_sprinting_name : "isSprinting" }) {
                g_c.is_sprinting = env->GetMethodID(pe, n, sdk::mappings::is_sprinting_sig);
                if (env->ExceptionCheck()) env->ExceptionClear();
                if (g_c.is_sprinting) break;
            }
            env->DeleteGlobalRef(pe);
        }
    }

    if (!g_c.key_binding) g_c.key_binding = global_class(env, sdk::mappings::key_binding_class_sig);
    if (g_c.key_binding && !g_c.pressed) {
        g_c.pressed = env->GetFieldID(g_c.key_binding,
            sdk::mappings::key_binding_pressed_field_name, "Z");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!g_c.pressed) {
            g_c.pressed = env->GetFieldID(g_c.key_binding, "isDown", "Z");
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (!g_c.pressed) g_c.pressed = sole_boolean_field(env, g_c.key_binding);
    }

    if (g_c.mc_options && mc && g_c.movement_count == 0) {
        jobject options = env->GetObjectField(mc, g_c.mc_options);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (options) {
            resolve_movement_keys(env, options);
            env->DeleteLocalRef(options);
        }
    }

    // Log only when it settles, or on the last attempt, so a few early retries
    // do not spam a line per pump.
    if (g_c.movement_count > 0 || g_c.attempts >= MAX_BUILD_ATTEMPTS) {
        VLOG_INFO("idle", "stand-still gate: pressed={} sprint={} movement={}/7 (attempt {})",
                  g_c.pressed ? "resolved" : "NOT FOUND",
                  g_c.is_sprinting ? "resolved" : "NOT FOUND",
                  g_c.movement_count, g_c.attempts);
    }
}

} // namespace

bool movement_key_pressed(JNIEnv* env, jobject mc, int index)
{
    if (!env || !mc || index < 0 || index > 6) return false;
    build(env, mc);
    if (!g_c.pressed || !g_c.mc_options || !g_c.movement[index]) return false;

    jobject options = env->GetObjectField(mc, g_c.mc_options);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    if (!options) return false;

    bool down = false;
    jobject kb = env->GetObjectField(options, g_c.movement[index]);
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (kb) {
        const jboolean v = env->GetBooleanField(kb, g_c.pressed);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else down = (v == JNI_TRUE);
        env->DeleteLocalRef(kb);
    }
    env->DeleteLocalRef(options);
    return down;
}

void set_movement_key(JNIEnv* env, jobject mc, int index, bool pressed)
{
    if (!env || !mc || index < 0 || index > 6) return;
    build(env, mc);
    if (!g_c.pressed || !g_c.mc_options || !g_c.movement[index]) return;

    jobject options = env->GetObjectField(mc, g_c.mc_options);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;

    jobject kb = env->GetObjectField(options, g_c.movement[index]);
    if (env->ExceptionCheck()) env->ExceptionClear();
    else if (kb) {
        env->SetBooleanField(kb, g_c.pressed, pressed ? JNI_TRUE : JNI_FALSE);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (index == KEY_SPRINT) g_sprint_key_owned.store(pressed, std::memory_order_release);
        env->DeleteLocalRef(kb);
    }
    env->DeleteLocalRef(options);
}

bool sprint_key_owned()
{
    return g_sprint_key_owned.load(std::memory_order_acquire);
}

namespace {
// Long enough to cover the rest of the tick the attack landed in, short enough
// that the sprint resumes on the very next one. The pump that re-presses the
// key runs at the end of a frame and so can never interleave between
// handleInputEvents and sendMovementPackets, which is what makes a window this
// small sufficient.
constexpr unsigned long long SPRINT_RESET_MS = 60;

std::atomic<unsigned long long> g_sprint_block_until{ 0 };     // attack reset
} // namespace

void request_sprint_reset(JNIEnv* env, jobject mc)
{
    // Vanilla's attack() has already cleared the flag; all that is needed here
    // is to stop a MODULE from re-arming it before the packet goes out. Only a
    // module-held binding is released: a player-held one is left exactly as
    // vanilla leaves it, so a toggled sprint resumes on the next tick the way
    // it does without the client (see g_sprint_key_owned).
    if (sprint_key_owned()) set_movement_key(env, mc, KEY_SPRINT, false);
    g_sprint_block_until.store(GetTickCount64() + SPRINT_RESET_MS, std::memory_order_release);
}

bool sprint_reset_active()
{
    return GetTickCount64() < g_sprint_block_until.load(std::memory_order_acquire);
}

void suppress_movement(JNIEnv* env, jobject mc)
{
    if (!env || !mc) return;
    build(env, mc);
    if (!g_c.pressed || !g_c.mc_options || g_c.movement_count == 0) return;

    jobject options = env->GetObjectField(mc, g_c.mc_options);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (!options) return;

    for (int i = 0; i < kDirectionKeys; ++i) {
        if (!g_c.movement[i]) continue;
        jobject kb = env->GetObjectField(options, g_c.movement[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;
        env->SetBooleanField(kb, g_c.pressed, JNI_FALSE);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(kb);
    }
    env->DeleteLocalRef(options);
}

bool movement_is_idle(JNIEnv* env, jobject mc)
{
    if (!env || !mc) return true;
    build(env, mc);

    // Sprint first: it is the condition that outlives the keys, so a gate that
    // only watched the bindings would open while the player is still sprinting
    // and Grim would read sprinting=true on the container action.
    if (g_c.is_sprinting && g_c.mc_player) {
        jobject player = env->GetObjectField(mc, g_c.mc_player);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (player) {
            const jboolean sprinting = env->CallBooleanMethod(player, g_c.is_sprinting);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (sprinting == JNI_TRUE) { env->DeleteLocalRef(player); return false; }
            env->DeleteLocalRef(player);
        }
    }

    // Cannot tell -> report idle. A gate that waits on this must never be able
    // to wedge a module on a host where the mapping did not resolve.
    if (!g_c.pressed || !g_c.mc_options || g_c.movement_count == 0) return true;

    jobject options = env->GetObjectField(mc, g_c.mc_options);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return true; }
    if (!options) return true;

    bool idle = true;
    for (int i = 0; i < kDirectionKeys && idle; ++i) {
        if (!g_c.movement[i]) continue;
        jobject kb = env->GetObjectField(options, g_c.movement[i]);
        if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
        if (!kb) continue;
        const jboolean down = env->GetBooleanField(kb, g_c.pressed);
        if (env->ExceptionCheck()) env->ExceptionClear();
        else if (down == JNI_TRUE) idle = false;
        env->DeleteLocalRef(kb);
    }
    env->DeleteLocalRef(options);
    return idle;
}

} // namespace dt::shared
