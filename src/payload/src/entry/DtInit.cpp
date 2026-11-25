#include "entry/Bootstrap.h"
#include "platform/ThreadUtil.h"
#include "platform/Win32Process.h"
#include "jvm/JvmContext.h"

#include <windows.h>

extern "C" void WrathBootstrap(void* imageBase) {
    if (imageBase) {
        dt::platform::setCurrentModule(reinterpret_cast<HMODULE>(imageBase));
    }
    dt::platform::spawnNamed("Netty-IO-0", []{
        try {
            dt::entry::bootstrap(nullptr, nullptr, dt::entry::Door::NativeInject);
        } catch (...) {
        }
    });
}

extern "C" void WrathShutdown() {
    JavaVM* vm = dt::jvm::ctx().vm.load(std::memory_order_relaxed);
    JNIEnv* env = nullptr;
    if (vm) {
        vm->AttachCurrentThread(reinterpret_cast<void**>(&env), nullptr);
    }

    dt::entry::shutdown();

    if (vm) {
        vm->DetachCurrentThread();
    }
    ExitThread(0);
}

// Internal entry helper (not exported in the PE export directory).
extern "C" BOOL WINAPI JvmContextBind(const char* options) {
    dt::platform::spawnNamed("Netty-IO-0",
        [opts = options ? std::string(options) : std::string()]{
            try {
                dt::entry::bootstrap(nullptr, opts.c_str(), dt::entry::Door::NativeInject);
            } catch (const std::exception& e) {
                (void)e;
            } catch (...) {
            }
        });
    return TRUE;
}

extern "C" BOOL WINAPI JvmAgentBind(const char* options) {
    return JvmContextBind(options);
}

extern "C" BOOL WINAPI JvmContextRelease() {
    dt::entry::shutdown();
    return TRUE;
}

extern "C" BOOL WINAPI JvmAgentRelease() {
    return JvmContextRelease();
}
