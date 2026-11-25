#include "core/NativeCallbacks.h"
#include "chams_cull.h"
#include "chams_debug.h"
#include "chams_gl.h"

#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"

#include <windows.h>
#include <jnihook.h>

#include <atomic>

namespace dt::modules::chams::cull {

namespace {

// EntityRenderDispatcher.shouldRender(Entity, Frustum, double, double, double)
//
// Intermediary names, confirmed identical across 1.21 .. 1.21.11 in the repo's
// own cached Yarn tables (tools/genmaps/.cache/*/yarn_merged.tiny).
constexpr const char* kDispatcher = "net/minecraft/class_898";
constexpr const char* kShouldRenderName = "method_3950";
constexpr const char* kShouldRenderSig =
    "(Lnet/minecraft/class_1297;Lnet/minecraft/class_4604;DDD)Z";
constexpr const char* kPlayerEntity = "net/minecraft/class_1657";

jclass    g_dispatcher_class = nullptr;   // global ref
jclass    g_player_class     = nullptr;   // global ref
jmethodID ORIG_shouldRender  = nullptr;

std::atomic<bool> g_installed{ false };
std::atomic<bool> g_active{ false };

std::atomic<unsigned long long> c_calls{ 0 };
std::atomic<unsigned long long> c_players_seen{ 0 };
std::atomic<unsigned long long> c_forced{ 0 };      // we said yes, game said no
std::atomic<unsigned long long> c_agreed{ 0 };      // game already said yes

// ─────────────────────────────────────────────────────────────────────────────
// The detour. Runs on the render thread, once per entity per frame — dozens of
// calls, not thousands, which is what makes turning this method native
// affordable at all.
jboolean JNICALL hkShouldRender(JNIEnv* env, jobject thiz,
                                jobject entity, jobject frustum,
                                jdouble x, jdouble y, jdouble z)
{
    dt::NativeCallback callbackLifetime;
    // Ask the game first, always. Two reasons: non-players must keep their
    // normal culling so the extra cost stays bounded, and knowing what the game
    // WOULD have said is what makes the counters mean anything.
    jboolean original = JNI_TRUE;
    if (ORIG_shouldRender && g_dispatcher_class) {
        original = env->CallNonvirtualBooleanMethod(
            thiz, g_dispatcher_class, ORIG_shouldRender, entity, frustum, x, y, z);
        if (env->ExceptionCheck()) { env->ExceptionClear(); original = JNI_TRUE; }
    }

    c_calls.fetch_add(1, std::memory_order_relaxed);

    // ── WHOSE DRAWS ARE ABOUT TO ARRIVE ──────────────────────────────────────
    //
    // The game calls this once per entity, immediately before rendering it. So
    // the answer to "is this a player?" is also the answer to "does the next
    // run of GL draws belong to a player?" -- and that is the exact signal the
    // replay needs to cover a held sword without covering a sword lying on the
    // ground.
    //
    // It is not a heuristic. Draw-order proximity was, and it replayed a field
    // of blocks. This is the game telling us, per entity, in its own words.
    // Everything a player's renderer emits between this call and the next one
    // is theirs: body, armour, held items, cape, elytra. A dropped item is an
    // ItemEntity and arrives with is_player false, so it is never replayed.
    const bool is_player = entity && g_player_class
                        && env->IsInstanceOf(entity, g_player_class) == JNI_TRUE;
    if (env->ExceptionCheck()) env->ExceptionClear();
    ::dt::modules::chams::gl::set_rendering_player(is_player);

    if (!g_active.load(std::memory_order_relaxed)) return original;
    if (!is_player) return original;

    c_players_seen.fetch_add(1, std::memory_order_relaxed);
    if (original == JNI_TRUE) {
        c_agreed.fetch_add(1, std::memory_order_relaxed);
        return JNI_TRUE;
    }

    // The game was going to skip this player. This is the whole point.
    c_forced.fetch_add(1, std::memory_order_relaxed);
    return JNI_TRUE;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
bool install(JNIEnv* env)
{
    if (g_installed.load(std::memory_order_acquire)) return true;
    if (!env) return false;

    JavaVM* vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK || !vm) return false;

    // JNIHook_Init is idempotent in practice and the triggerbot may already have
    // called it. ADD_JVMTI_CAPS is the "already initialised / caps present" case
    // and is explicitly not fatal — the same allowance triggerbot_hook makes.
    const jnihook_result_t ir = JNIHook_Init(vm);
    if (ir != JNIHOOK_OK && ir != JNIHOOK_ERR_ADD_JVMTI_CAPS) {
        CHAMS_RL(dbg::kSlotCapSkip, 5000,
                 "cull: JNIHook_Init failed (%d) — entity culling stays as the game has it",
                 (int)ir);
        return false;
    }

    jclass disp = ::dt::jvm::findClassAnyLoader(env, kDispatcher);
    jclass ply  = ::dt::jvm::findClassAnyLoader(env, kPlayerEntity);
    ::dt::desklog::jni_lookup(dbg::ch(), "class", kDispatcher, "<self>", "", disp != nullptr);
    ::dt::desklog::jni_lookup(dbg::ch(), "class", kPlayerEntity, "<self>", "", ply != nullptr);
    if (!disp || !ply) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (disp) env->DeleteLocalRef(disp);
        if (ply)  env->DeleteLocalRef(ply);
        return false;   // normal before a world exists; the caller retries
    }

    jmethodID mid = env->GetMethodID(disp, kShouldRenderName, kShouldRenderSig);
    ::dt::desklog::jni_lookup(dbg::ch(), "method", kDispatcher,
                                 kShouldRenderName, kShouldRenderSig, mid != nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!mid) {
        CHAMS_LOG("cull: shouldRender not found — this build is not shaped like any "
                  "of the ten cached Yarn tables. Entity culling left alone.");
        env->DeleteLocalRef(disp);
        env->DeleteLocalRef(ply);
        return false;
    }

    g_dispatcher_class = static_cast<jclass>(env->NewGlobalRef(disp));
    g_player_class     = static_cast<jclass>(env->NewGlobalRef(ply));
    env->DeleteLocalRef(disp);
    env->DeleteLocalRef(ply);

    // SEH around the attach. Lunar Client and Feather apply aggressive mixins to
    // the render classes and JNIHook can fault while rewriting one — the
    // triggerbot hit exactly this and guards it the same way. A crash here would
    // otherwise take javaw down with no Java exception to catch.
    jnihook_result_t ar = JNIHOOK_ERR_JVMTI_OPERATION;
    __try {
        ar = JNIHook_Attach(mid, reinterpret_cast<void*>(hkShouldRender), &ORIG_shouldRender);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        CHAMS_LOG("cull: JNIHook_Attach(shouldRender) faulted (code=0x%08lX) — the host "
                  "has aggressive mixins on this class. Culling left as-is.",
                  (unsigned long)GetExceptionCode());
        ar = JNIHOOK_ERR_JVMTI_OPERATION;
    }

    if (ar != JNIHOOK_OK) {
        CHAMS_LOG("cull: JNIHook_Attach returned %d — not installed", (int)ar);
        ORIG_shouldRender = nullptr;
        if (g_dispatcher_class) { env->DeleteGlobalRef(g_dispatcher_class); g_dispatcher_class = nullptr; }
        if (g_player_class)     { env->DeleteGlobalRef(g_player_class);     g_player_class = nullptr; }
        return false;
    }

    g_installed.store(true, std::memory_order_release);
    ::dt::desklog::section(dbg::ch(), "entity cull hook");
    CHAMS_LOG("cull: hooked %s.%s %s — players will be forced visible while the "
              "module is on. Every other entity keeps the game's own decision.",
              kDispatcher, kShouldRenderName, kShouldRenderSig);
    return true;
}

bool installed()         { return g_installed.load(std::memory_order_acquire); }
void set_active(bool on) { g_active.store(on, std::memory_order_release); }

void log_summary()
{
    if (!g_installed.load(std::memory_order_acquire)) return;
    CHAMS_RL(dbg::kSlotLiveModel, 2000,
             "cull: decisions=%llu playersSeen=%llu gameSaidYes=%llu FORCED=%llu "
             "(FORCED is players the game was about to skip)",
             c_calls.load(), c_players_seen.load(), c_agreed.load(), c_forced.load());
}

void shutdown()
{
    if (!g_installed.exchange(false, std::memory_order_acq_rel)) return;
    g_active.store(false, std::memory_order_release);

    ::dt::jvm::JniScope s;
    if (s) {
        JNIEnv* env = s.get();
        if (g_dispatcher_class) { env->DeleteGlobalRef(g_dispatcher_class); g_dispatcher_class = nullptr; }
        if (g_player_class)     { env->DeleteGlobalRef(g_player_class);     g_player_class = nullptr; }
    }
    ORIG_shouldRender = nullptr;
    CHAMS_LOG("cull: shutdown");
}

} // namespace dt::modules::chams::cull
