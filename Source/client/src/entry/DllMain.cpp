#include "entry/Bootstrap.h"
#include "platform/Win32Process.h"
#include "core/config/ConfigManager.h"
#include "core/module/ModuleManager.h"

#include <windows.h>

// Loader-lock rules: DllMain may NOT do real work (no JNI, no CRT-heavy init,
// no LoadLibrary). It MAY call CreateThread — the new thread is queued and does
// not start executing until the loader lock is released, which is exactly the
// standard bootstrap pattern for an injectable DLL.
//
// This self-start matters: a generic injector (Xenos, Process Hacker, Extreme
// Injector, a manual-map loader, ...) only ever calls LoadLibrary. It does NOT
// know about our VantaInit export. Without the thread below, such an injector
// would map the DLL and nothing would ever happen — no log, no GUI.
//
// vanta::entry::bootstrap is guarded by AtomicOnce, so if the caller ALSO
// invokes VantaInit (our own injector does) the second call is a no-op.

namespace {

DWORD WINAPI vantaSelfStart(LPVOID) {
#ifdef _DEBUG
    OutputDebugStringA("[vanta] self-start thread running (DllMain path)\n");
#endif
    try {
        vanta::entry::bootstrap(nullptr, nullptr, vanta::entry::Door::NativeInject);
#ifdef _DEBUG
        OutputDebugStringA("[vanta] self-start bootstrap returned\n");
#endif
    } catch (...) {
#ifdef _DEBUG
        OutputDebugStringA("[vanta] self-start bootstrap threw\n");
#endif
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            DisableThreadLibraryCalls(hMod);
            vanta::platform::setCurrentModule(hMod);
#ifdef _DEBUG
            OutputDebugStringA("[vanta] DllMain PROCESS_ATTACH — DLL mapped, starting bootstrap thread\n");
#endif

            // Raw CreateThread, NOT std::thread: std::thread's ctor can touch
            // the CRT in ways that are unsafe under the loader lock.
            HANDLE t = CreateThread(nullptr, 0, &vantaSelfStart, nullptr, 0, nullptr);
            if (t) CloseHandle(t);
#ifdef _DEBUG
            else   OutputDebugStringA("[vanta] CreateThread for self-start FAILED\n");
#endif
            break;
        }
        case DLL_PROCESS_DETACH: {
#ifdef _DEBUG
            OutputDebugStringA("[vanta] DllMain PROCESS_DETACH\n");
#endif
            // No filesystem, singleton initialization or locks under loader
            // lock. Normal unload saves through ConfigManager::shutdown().
            break;
        }
        default: break;
    }
    return TRUE;
}
