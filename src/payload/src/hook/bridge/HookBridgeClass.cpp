#include "hook/bridge/HookBridgeClass.h"

#include "util/Log.h"

// Byte array of the compiled HookBridge.class. When the shim/ build step is
// wired the CMake pipeline emits this file; until then these fallback symbols
// link a zero-length blob and defineHookBridgeClass() logs and returns nullptr.
namespace vanta::generated {
#if defined(_MSC_VER)
__declspec(selectany) extern const unsigned char hook_bridge_class[1]   = {0};
__declspec(selectany) extern const unsigned      hook_bridge_class_size = 0;
#else
extern const unsigned char hook_bridge_class[]      __attribute__((weak));
extern const unsigned      hook_bridge_class_size   __attribute__((weak));
#endif
} // namespace vanta::generated

namespace vanta::hook::bridge {

jclass defineHookBridgeClass(JNIEnv* env) {
    if (!env) return nullptr;

    const unsigned char* bytes = vanta::generated::hook_bridge_class;
    const unsigned       n     = vanta::generated::hook_bridge_class_size;
    if (!bytes || n == 0) {
        // Not an error: the Java-shim hook path is OPTIONAL and simply not
        // built in this tree (there is no shim/ step yet, so the embedded
        // blob is a zero-length placeholder). Returning null here is the
        // correct, safe outcome — src/asm only ever emits
        // `INVOKESTATIC <kBridgeClassInternalName>.onXxx(...)`, so retransforming
        // without the class present would raise NoClassDefFoundError inside
        // Minecraft. Modules that need a native hook use JNIHook directly
        // (see the triggerbot / autototem), which needs none of this.
        VLOG_INFO("bridge",
            "HookBridge shim not embedded — Java-shim hook path stays dormant "
            "(optional; JNIHook-based modules are unaffected).");
        return nullptr;
    }

    jclass cls = env->DefineClass(
        kBridgeClassInternalName.data(),
        /*loader=*/nullptr,                          // bootstrap loader
        reinterpret_cast<const jbyte*>(bytes),
        static_cast<jsize>(n));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (!cls) {
        VLOG_ERR("bridge", "DefineClass({}) failed", kBridgeClassInternalName);
        return nullptr;
    }

    jclass global = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);
    VLOG_INFO("bridge", "Bridge defined as {} ({} bytes)", kBridgeClassInternalName, n);
    return global;
}

} // namespace vanta::hook::bridge
