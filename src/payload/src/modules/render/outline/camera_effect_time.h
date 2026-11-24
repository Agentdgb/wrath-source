#pragma once

#include <jni.h>
#include <cmath>

namespace vanta::modules::outline::view {

// renderWorld passes Camera.getLastTickProgress() to both hurt tilt and bob.
// Keep the caller's tick progress as a fallback for older camera mappings.
inline float camera_effect_time(JNIEnv* env, jobject renderer, float fallback)
{
    jclass renderer_class = env->GetObjectClass(renderer);
    if (!renderer_class) { env->ExceptionClear(); return fallback; }
    static jmethodID get_camera = nullptr;
    if (!get_camera)
        get_camera = env->GetMethodID(renderer_class, "method_19418", "()Lnet/minecraft/class_4184;");
    env->DeleteLocalRef(renderer_class);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return fallback; }
    if (!get_camera) return fallback;
    jobject camera = env->CallObjectMethod(renderer, get_camera);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return fallback; }
    if (!camera) return fallback;
    jclass camera_class = env->GetObjectClass(camera);
    static jmethodID get_progress = nullptr;
    if (camera_class && !get_progress)
        get_progress = env->GetMethodID(camera_class, "method_55437", "()F");
    if (camera_class) env->DeleteLocalRef(camera_class);
    float progress = fallback;
    if (!env->ExceptionCheck() && get_progress)
        progress = env->CallFloatMethod(camera, get_progress);
    if (env->ExceptionCheck()) { env->ExceptionClear(); progress = fallback; }
    env->DeleteLocalRef(camera);
    return std::isfinite(progress) ? progress : fallback;
}

} // namespace vanta::modules::outline::view
