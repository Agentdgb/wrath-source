#include "core/NativeCallbacks.h"
#include "target_attack.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "util/Log.h"
#include <jnihook.h>
#include <windows.h>
#include <atomic>
#include <mutex>
#include <chrono>

namespace vanta::modules::targetesp::attack {
namespace {
std::atomic<bool> enabled{false};
jclass manager_class = nullptr;
jmethodID method = nullptr, original = nullptr;
jobject pending = nullptr;
std::chrono::steady_clock::time_point pending_time{};
std::mutex mutex;
std::chrono::steady_clock::time_point next_attempt{};

void JNICALL on_attack(JNIEnv* env, jobject manager, jobject player, jobject target) {
    vanta::NativeCallback callbackLifetime;
    // Preserve vanilla's attack and any exception; observation never cancels it.
    env->CallNonvirtualVoidMethod(manager, manager_class, original, player, target);
    if (env->ExceptionCheck() || !target || !enabled.load()) return;
    std::lock_guard lock(mutex);
    if (!enabled.load()) return;
    jobject ref = env->NewGlobalRef(target);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
    if (pending) env->DeleteGlobalRef(pending);
    pending = ref;
    pending_time = std::chrono::steady_clock::now();
}

jnihook_result_t attach(jmethodID id) {
    __try { return JNIHook_Attach(id, reinterpret_cast<void*>(on_attack), &original); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return JNIHOOK_ERR_JVMTI_OPERATION; }
}
}

bool install(JNIEnv* env) {
    if (method) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < next_attempt) return false;
    next_attempt = now + std::chrono::seconds(3);
    jclass cls = jvm::findClassAnyLoader(env, "net/minecraft/class_636");
    if (!cls) return false;
    jmethodID id = env->GetMethodID(cls, "method_2918", "(Lnet/minecraft/class_1657;Lnet/minecraft/class_1297;)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!id) { env->DeleteLocalRef(cls); VLOG_WARN("targetesp", "attackEntity lookup failed"); return false; }
    manager_class = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    if (!manager_class) return false;
    auto result = attach(id);
    // Init replaces JNIHook's registry, so only initialize when Attach reports
    // that the runtime does not exist. Existing combat/render hooks stay intact.
    if (result == JNIHOOK_ERR_GET_JVMTI && jvm::ctx().vm.load() &&
        JNIHook_Init(jvm::ctx().vm.load()) == JNIHOOK_OK) result = attach(id);
    if (result != JNIHOOK_OK) {
        env->DeleteGlobalRef(manager_class); manager_class = nullptr;
        original = nullptr;
        VLOG_WARN("targetesp", "attack observer installation failed: {}", int(result));
        return false;
    }
    method = id;
    VLOG_INFO("targetesp", "attackEntity observer installed");
    return true;
}
void set_enabled(bool value) { enabled.store(value); }
void reset(JNIEnv* env) {
    std::lock_guard lock(mutex);
    if (pending) env->DeleteGlobalRef(pending);
    pending = nullptr;
}
jobject consume(JNIEnv* env, double* age_seconds) {
    std::lock_guard lock(mutex);
    if (!pending) return nullptr;
    if (age_seconds) *age_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-pending_time).count();
    jobject local = env->NewLocalRef(pending);
    env->DeleteGlobalRef(pending); pending = nullptr;
    return local;
}
void shutdown() {
    enabled.store(false);
    // Retain the trampoline/class if detaching failed; never clear live hook data.
    if (method && JNIHook_Detach(method) != JNIHOOK_OK) {
        VLOG_WARN("targetesp", "attack observer detach failed");
        return;
    }
    method = original = nullptr;
    jvm::JniScope scope;
    if (scope) {
        reset(scope.get());
        if (manager_class) scope->DeleteGlobalRef(manager_class);
    }
    manager_class = nullptr;
}
}
