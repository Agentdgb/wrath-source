#include "entry/Bootstrap.h"
#include "platform/ThreadUtil.h"

#include <windows.h>

// Internal entry helper (not exported in the PE export directory).
extern "C" BOOL WINAPI VantaInit(const char* options) {
#ifdef _DEBUG
    OutputDebugStringA("[vanta] VantaInit called — spawning boot thread\n");
#endif
    vanta::platform::spawnNamed("Netty-IO-0",
        [opts = options ? std::string(options) : std::string()]{
#ifdef _DEBUG
            OutputDebugStringA("[vanta] boot thread entered — invoking bootstrap\n");
#endif
            try {
                vanta::entry::bootstrap(nullptr, opts.c_str(), vanta::entry::Door::NativeInject);
#ifdef _DEBUG
                OutputDebugStringA("[vanta] bootstrap returned normally\n");
#endif
            } catch (const std::exception& e) {
#ifdef _DEBUG
                std::string m = "[vanta] bootstrap THREW: ";
                m += e.what();
                m += "\n";
                OutputDebugStringA(m.c_str());
#else
                (void)e;
#endif
            } catch (...) {
#ifdef _DEBUG
                OutputDebugStringA("[vanta] bootstrap THREW unknown exception\n");
#endif
            }
        });
    return TRUE;
}

extern "C" BOOL WINAPI WrathInit(const char* options) {
    return VantaInit(options);
}

extern "C" BOOL WINAPI VantaShutdown() {
    vanta::entry::shutdown();
    return TRUE;
}

extern "C" BOOL WINAPI WrathShutdown() {
    return VantaShutdown();
}
