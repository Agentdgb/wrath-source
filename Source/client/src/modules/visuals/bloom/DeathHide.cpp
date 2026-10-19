#include "DeathEffects.h"
#include "core/NativeCallbacks.h"
#include "modules/visuals/ports/PortJni.h"
#include <atomic>
#include <jnihook.h>
namespace vanta::modules::bloom {
namespace {
std::atomic<bool> interested{false};
jclass owner = nullptr;
jmethodID original = nullptr, idMethod = nullptr;
bool tried = false;
std::string hookError;
jboolean JNICALL visible(JNIEnv *e, jobject self, jobject entity, jobject frustum, jdouble x, jdouble y, jdouble z) {
    NativeCallback callback;
    if (deathSettings.enabled && deathSettings.hide && entity) {
        int id = e->CallIntMethod(entity, idMethod);
        if (e->ExceptionCheck())
            return JNI_FALSE;
        try {
            if (death::i(id))
                return JNI_FALSE;
        } catch (...) {
        }
    }
    return e->CallNonvirtualBooleanMethod(self, owner, original, entity, frustum, x, y, z);
}
} // namespace
bool deathInterested() { return interested.load(std::memory_order_acquire); }
void setDeathInterest(bool value) { interested.store(value, std::memory_order_release); }
void installDeathHide(JNIEnv *e) {
    if (tried)
        return;
    tried = true;
    try {
        namespace jni = visualports::jni;
        auto c = jni::cls(e, "net/minecraft/class_897");
        owner = static_cast<jclass>(e->NewGlobalRef(c));
        jni::check(e);
        if (!owner)
            throw std::runtime_error("Death body class unavailable");
        idMethod = jni::method(e, jni::cls(e, "net/minecraft/class_1297"), "method_5628", "()I");
        auto method =
            jni::method(e, owner, "method_3933", "(Lnet/minecraft/class_1297;Lnet/minecraft/class_4604;DDD)Z");
        if (JNIHook_Attach(method, reinterpret_cast<void *>(&visible), &original) != JNIHOOK_OK)
            throw std::runtime_error("Death body hook unavailable");
    } catch (const std::exception &ex) {
        if (e->ExceptionCheck())
            e->ExceptionClear();
        hookError = ex.what();
    }
}
const char *deathHideError() { return hookError.empty() ? nullptr : hookError.c_str(); }
void freeDeathHide(JNIEnv *e) {
    if (owner)
        e->DeleteGlobalRef(owner);
    owner = nullptr;
}
} // namespace vanta::modules::bloom
