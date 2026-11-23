#pragma once

#include <cstdint>
#include <jni.h>

namespace vanta::modules::throwpot {

// Reserve one ordinary use-key press. Minecraft consumes it in
// handleInputEvents and owns slot sync, item use, rotation, swing and packet
// ordering.
inline bool queueUse(JNIEnv *env, jobject key, jfieldID clicks) {
    if (!env || !key || !clicks)
        return false;
    const jint pending = env->GetIntField(key, clicks);
    if (env->ExceptionCheck() || pending != 0)
        return false;
    env->SetIntField(key, clicks, 1);
    return !env->ExceptionCheck();
}

// Cancel only our single reservation; preserve any additional physical clicks.
inline void cancelUse(JNIEnv *env, jobject key, jfieldID clicks) {
    if (!env || !key || !clicks)
        return;
    const jint pending = env->GetIntField(key, clicks);
    if (!env->ExceptionCheck() && pending > 0)
        env->SetIntField(key, clicks, pending - 1);
}

enum class UseProgress { Waiting, Thrown, Cancelled, Rejected, TimedOut };

inline constexpr UseProgress useProgress(int before, int remaining, bool sameSlot, int pending,
                                         std::uint64_t elapsed) noexcept {
    if (!sameSlot)
        return UseProgress::Cancelled;
    if (remaining < before)
        return UseProgress::Thrown;
    if (pending == 0)
        return UseProgress::Rejected;
    if (elapsed >= 500)
        return UseProgress::TimedOut;
    return UseProgress::Waiting;
}

} // namespace vanta::modules::throwpot
