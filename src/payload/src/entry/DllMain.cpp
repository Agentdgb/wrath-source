#include "entry/Bootstrap.h"
#include "platform/Win32Process.h"
#include "platform/Win32Window.h"
#include "platform/StealthThread.h"
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

DWORD WINAPI wrathSelfStart(LPVOID) {
#ifdef _DEBUG
    OutputDebugStringA("[wrath] self-start thread running (DllMain path)\n");
#endif
    // Give Agent_OnLoad time to set its precedence flag if loaded directly as a JVMTI agent
    Sleep(250);
    if (vanta::entry::isAgentPresent() || vanta::entry::bootstrapFinished()) {
        return 0;
    }

    // Generic external injector path: wait for GLFW game window before touching JVM/Knot
    HWND wnd = vanta::platform::waitForGameWindow(60000, nullptr);
    if (!wnd) Sleep(8000);
    else Sleep(500);

    if (vanta::entry::isAgentPresent() || vanta::entry::bootstrapFinished()) {
        return 0;
    }

    try {
        vanta::entry::bootstrap(nullptr, nullptr, vanta::entry::Door::NativeInject);
#ifdef _DEBUG
        OutputDebugStringA("[wrath] self-start bootstrap returned\n");
#endif
    } catch (...) {
#ifdef _DEBUG
        OutputDebugStringA("[wrath] self-start bootstrap threw\n");
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
            vanta::platform::installStealthThreadHook();
#ifdef _DEBUG
            OutputDebugStringA("[wrath] DllMain PROCESS_ATTACH — DLL mapped, starting bootstrap thread\n");
#endif

            // Stealth thread spoofed to ntdll!RtlUserThreadStart
            HANDLE t = vanta::platform::createStealthThread(&wrathSelfStart, nullptr);
            if (t) CloseHandle(t);
#ifdef _DEBUG
            else   OutputDebugStringA("[wrath] CreateThread for self-start FAILED\n");
#endif
            break;
        }
        case DLL_PROCESS_DETACH: {
#ifdef _DEBUG
            OutputDebugStringA("[wrath] DllMain PROCESS_DETACH\n");
#endif
            vanta::platform::uninstallStealthThreadHook();
            // No filesystem, singleton initialization or locks under loader
            // lock. Normal unload saves through ConfigManager::shutdown().
            break;
        }
        default: break;
    }
    return TRUE;
}
