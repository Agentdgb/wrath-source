#include "item_shader_hook.h"
#include "core/NativeCallbacks.h"
#include "core/UnloadSafety.h"
#include "item_shader_gl.h"
#include "jvm/JniHelper.h"
#include "jvm/JvmContext.h"
#include "modules/visuals/ports/VisualPorts.h"
#include "util/Log.h"
#include <atomic>
#include <jnihook.h>
#include <windows.h>

namespace vanta::modules::itemshader {
namespace {
jclass owner = nullptr;
jmethodID method = nullptr, original = nullptr;
std::atomic<unsigned> callbacks{0};
std::atomic<bool> stopping{false};
std::atomic<bool> hidden{false}; // set_hands_hidden: Freecam's "no hands" switch
bool tried = false;

void JNICALL render_item(JNIEnv *env, jobject self, jobject player, jfloat delta, jfloat pitch, jobject hand,
                         jfloat swing, jobject item, jfloat equip, jobject matrices, jobject consumers, jint light) {
    vanta::NativeCallback callbackLifetime;
    callbacks.fetch_add(1, std::memory_order_acq_rel);
    struct Leave {
        ~Leave() { callbacks.fetch_sub(1, std::memory_order_acq_rel); }
    } leave;
    /* Freecam: nothing of the first person is drawn. The original pushes and
       pops its own matrix entry, so skipping it whole leaves the stack as the
       caller expects; the glow observer is not notified either, since there
       is no draw for it to find. */
    if (hidden.load(std::memory_order_acquire))
        return;
    if (!stopping.load() && wanted())
        note_firstperson_render_call();
    const bool transformed = vanta::modules::visualports::beforeHand(env, player, hand, swing, matrices);
    if (original)
        env->CallNonvirtualVoidMethod(self, owner, original, player, delta, pitch, hand, transformed ? 0.f : swing,
                                      item, equip, matrices, consumers, light);
    vanta::modules::visualports::afterHand(env, matrices, transformed);
    // Preserve Java exceptions: this hook only observes the vanilla render.
}
} // namespace

bool install_hook() {
    if (tried)
        return original != nullptr;
    vanta::jvm::JniScope scope;
    if (!scope)
        return false;
    tried = true;
    auto *env = scope.get();
    vanta::jvm::LocalClass local(env, vanta::jvm::findClassAnyLoader(env, "net/minecraft/class_759"));
    if (env->ExceptionCheck())
        env->ExceptionClear();
    if (!local)
        return false;
    owner = static_cast<jclass>(env->NewGlobalRef(local.get()));
    if (!owner) {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        return false;
    }
    const char *signatures[] = {"(Lnet/minecraft/class_742;FFLnet/minecraft/class_1268;FLnet/minecraft/"
                                "class_1799;FLnet/minecraft/class_4587;Lnet/minecraft/class_4597;I)V",
                                "(Lnet/minecraft/class_742;FFLnet/minecraft/class_1268;FLnet/minecraft/"
                                "class_1799;FLnet/minecraft/class_4587;Lnet/minecraft/class_11659;I)V"};
    for (auto sig : signatures) {
        method = env->GetMethodID(owner, "method_3228", sig);
        if (env->ExceptionCheck())
            env->ExceptionClear();
        if (method)
            break;
    }
    if (!method) {
        VLOG_WARN("itemshader", "First-person render mapping unavailable");
        return false;
    }
    const auto init = JNIHook_Init(vanta::jvm::ctx().vm.load());
    if (init != JNIHOOK_OK && init != JNIHOOK_ERR_ADD_JVMTI_CAPS)
        return false;
    const auto result = JNIHook_Attach(method, reinterpret_cast<void *>(&render_item), &original);
    VLOG_INFO("itemshader", "First-person observer: {}", result == JNIHOOK_OK ? "installed" : "unavailable");
    return result == JNIHOOK_OK && original;
}

void set_hands_hidden(bool on) { hidden.store(on, std::memory_order_release); }
bool hands_hidden() { return hidden.load(std::memory_order_acquire); }

void shutdown_hook() {
    stopping.store(true);
    hidden.store(false, std::memory_order_release);
    set_wanted(false);
    set_effect(false);
    const bool detached = !original || JNIHook_Detach(method) == JNIHOOK_OK;
    const auto deadline = GetTickCount64() + 2000;
    while (callbacks.load() && GetTickCount64() < deadline)
        Sleep(1);
    if (!detached || callbacks.load()) {
        vanta::blockUnload();
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(&shutdown_hook), &pinned);
        VLOG_WARN("itemshader", "Callback still active; keeping disabled DLL resident");
        return;
    }
    vanta::jvm::JniScope scope;
    if (scope && owner)
        scope.get()->DeleteGlobalRef(owner);
    owner = nullptr;
}
} // namespace vanta::modules::itemshader
