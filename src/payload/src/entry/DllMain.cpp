#include "entry/Bootstrap.h"
#include "platform/Win32Process.h"
#include "platform/Win32Window.h"
#include "platform/StealthThread.h"
#include "core/config/ConfigManager.h"
#include "core/module/ModuleManager.h"
#include "shared/stealth/StealthDefs.h"

#include <windows.h>
#include <cstdio>

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

bool readLoadStrategyIsLoadLibrary(HMODULE hMod) noexcept {
    wchar_t secName[64];
    _snwprintf_s(secName, _countof(secName), _TRUNCATE, L"Local\\DisplayTuner.Hb.%lu", GetCurrentProcessId());

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, secName);
    if (hMap) {
        auto* ctx = reinterpret_cast<const wrath::bypass::HeartbeatSharedContext*>(
            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(wrath::bypass::HeartbeatSharedContext)));
        if (ctx) {
            if (ctx->magic == wrath::bypass::kHeartbeatMagic && ctx->version == wrath::bypass::kHeartbeatVersion) {
                bool isLoadLib = (ctx->strategy == wrath::bypass::Strategy_LoadLibrary);
                UnmapViewOfFile(ctx);
                CloseHandle(hMap);
                return isLoadLib;
            }
            UnmapViewOfFile(ctx);
        }
        CloseHandle(hMap);
    }

    // Sin sección o versión inválida: la verdad la tiene el Ldr de NT
    return vanta::platform::isRegisteredDll(hMod);
}

static bool s_isLoadLib = true;

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

            s_isLoadLib = readLoadStrategyIsLoadLibrary(hMod);

            if (!s_isLoadLib) {
                vanta::platform::installStealthThreadHook();
            }

            HANDLE t = s_isLoadLib
                ? CreateThread(nullptr, 0, &wrathSelfStart, nullptr, 0, nullptr)
                : vanta::platform::createStealthThread(&wrathSelfStart, nullptr);

            if (t) CloseHandle(t);
            break;
        }
        case DLL_PROCESS_DETACH: {
            if (!s_isLoadLib) {
                vanta::platform::uninstallStealthThreadHook();
            }
            break;
        }
        default: break;
    }
    return TRUE;
}
