#include "core/social/EntityFriends.h"

#include "core/social/FriendList.h"
#include "jvm/JniHelper.h"
#include "util/Log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>

namespace vanta::social {

namespace {

/* Intermediary names, grepped out of tools/genmaps/.cache/<ver>/yarn_merged.tiny
   and identical on every version in the supported range (1.21 .. 1.21.11):

     class_1657  PlayerEntity  method_7334  getGameProfile ()Lcom/mojang/authlib/GameProfile;

   The same pair spectate.cpp resolves a watched player's name with. */
struct Cache {
    jclass    player_cls  = nullptr;   // global ref, class_1657 -- for IsInstanceOf
    jmethodID get_profile = nullptr;
    jmethodID prof_name   = nullptr;
};
Cache C{};

/* 0 = not resolved yet (retry, with backoff), 1 = ready, 2 = unavailable. */
std::atomic<int> g_state{ 0 };

std::mutex g_resolve_mx;
double     g_next_try = 0.0;           // ms; guarded by g_resolve_mx

double now_ms()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* @the retry is BACKED OFF, and it has to be.

   Before a world exists the classes are simply not loaded, so this fails and
   must be retried -- but findClassAnyLoader's miss path is not cheap. It climbs
   to the root ThreadGroup, enumerates every thread in the VM and calls
   ClassLoader.loadClass on each one's context loader, throwing and catching a
   Java exception per miss. Retrying that on every query, from a 1 kHz worker,
   would cost more than the feature. Two seconds between attempts makes the
   failed state free. */
bool ensure(JNIEnv* env)
{
    const int st = g_state.load(std::memory_order_acquire);
    if (st == 1) return true;
    if (st == 2) return false;

    std::lock_guard<std::mutex> lk(g_resolve_mx);

    const int st2 = g_state.load(std::memory_order_relaxed);
    if (st2 == 1) return true;
    if (st2 == 2) return false;

    const double now = now_ms();
    if (now < g_next_try) return false;
    g_next_try = now + 2000.0;

    jclass player = ::vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_1657");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!player) return false;         /* no world yet; stay at 0 and retry later */

    C.get_profile = env->GetMethodID(player, "method_7334",
                                     "()Lcom/mojang/authlib/GameProfile;");
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (jclass gp = ::vanta::jvm::findClassAnyLoader(env, "com/mojang/authlib/GameProfile")) {
        C.prof_name = env->GetMethodID(gp, "getName", "()Ljava/lang/String;");
        env->DeleteLocalRef(gp);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass global = nullptr;
    if (C.get_profile && C.prof_name) {
        global = static_cast<jclass>(env->NewGlobalRef(player));
        /* NewGlobalRef throws OutOfMemoryError rather than returning null on
           some VMs, and this is called from inside a detour where a pending
           exception would surface in unrelated game code. */
        if (env->ExceptionCheck()) { env->ExceptionClear(); global = nullptr; }
    }

    env->DeleteLocalRef(player);

    if (!global) {
        VLOG_WARN("friends", "PlayerEntity.getGameProfile did not resolve -- "
                             "friends cannot be matched to entities");
        g_state.store(2, std::memory_order_release);
        return false;
    }

    C.player_cls = global;
    VLOG_INFO("friends", "entity matching armed");
    g_state.store(1, std::memory_order_release);
    return true;
}

} // namespace

bool isFriend(JNIEnv* env, jobject entity)
{
    if (!env || !entity) return false;

    /* One relaxed atomic load. For a user with no friends -- which is most of
       them, most of the time -- the whole feature costs exactly this. */
    FriendList& fl = FriendList::get();
    if (fl.empty()) return false;

    if (!ensure(env)) return false;

    /* A non-player has no GameProfile and calling method_7334 on one throws. */
    if (env->IsInstanceOf(entity, C.player_cls) != JNI_TRUE) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    bool answer = false;

    if (jobject profile = env->CallObjectMethod(entity, C.get_profile)) {
        if (jstring js = static_cast<jstring>(env->CallObjectMethod(profile, C.prof_name))) {
            if (const char* c = env->GetStringUTFChars(js, nullptr)) {
                /* Compared as a view: contains() folds case as it hashes, so
                   there is no std::string built here and nothing allocated on
                   a path that can run thousands of times a second. */
                answer = fl.contains(std::string_view(c));
                env->ReleaseStringUTFChars(js, c);
            }
            env->DeleteLocalRef(js);
        }
        env->DeleteLocalRef(profile);
    }

    if (env->ExceptionCheck()) env->ExceptionClear();
    return answer;
}

} // namespace vanta::social
