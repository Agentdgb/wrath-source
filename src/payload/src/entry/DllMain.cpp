#include "entry/Bootstrap.h"
#include "platform/Win32Process.h"
#include "platform/Win32Window.h"
#include "platform/StealthThread.h"
#include "shared/stealth/StealthDefs.h"
#include "vgui/vgui.h"

#include <windows.h>
#include <cstdio>

namespace {

HINSTANCE g_self = nullptr;
HANDLE    g_thread = nullptr;
HANDLE    g_abortEvent = nullptr;
bool      s_isLoadLib = true;

constexpr int kHotkeyToggleId = 1;
constexpr int kHotkeyUnloadId = 2;

void setThreadName(const wchar_t* name) noexcept {
    using SetThreadDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    auto fn = reinterpret_cast<SetThreadDescFn>(GetProcAddress(k32, "SetThreadDescription"));
    if (fn) fn(GetCurrentThread(), name);
}

bool readLoadStrategyIsLoadLibrary(HMODULE hMod) noexcept {
    wchar_t secName[64]{};
    _snwprintf_s(secName, _countof(secName), _TRUNCATE, L"%s%lu",
                 wrath::bypass::kHeartbeatPrefix, GetCurrentProcessId());

    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, secName);
    if (hMap) {
        auto* ctx = reinterpret_cast<const wrath::bypass::HeartbeatSharedContext*>(
            MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(wrath::bypass::HeartbeatSharedContext)));
        if (ctx) {
            if (ctx->magic == wrath::bypass::kHeartbeatMagic &&
                ctx->version == wrath::bypass::kHeartbeatVersion) {
                bool isLoadLib = (ctx->strategy == wrath::bypass::Strategy_LoadLibrary);
                UnmapViewOfFile(ctx);
                CloseHandle(hMap);
                return isLoadLib;
            }
            UnmapViewOfFile(ctx);
        }
        CloseHandle(hMap);
    }

    return dt::platform::isRegisteredDll(hMod);
}

DWORD WINAPI worker_main(LPVOID) {
    setThreadName(L"Native Helper");
    dt::platform::setCurrentModule(g_self);

    s_isLoadLib = readLoadStrategyIsLoadLibrary(g_self);
    if (!s_isLoadLib) {
        dt::platform::installStealthThreadHook();
    }

    if (g_abortEvent && WaitForSingleObject(g_abortEvent, 0) == WAIT_OBJECT_0) {
        return 0;
    }

    // Wait for GLFW game window before touching JVM/Knot.
    HWND wnd = dt::platform::waitForGameWindow(60000, nullptr);
    if (!wnd) Sleep(2000);
    else Sleep(500);

    if (g_abortEvent && WaitForSingleObject(g_abortEvent, 0) == WAIT_OBJECT_0) {
        return 0;
    }

    if (!dt::entry::isAgentPresent() && !dt::entry::bootstrapFinished()) {
        try {
            dt::entry::bootstrap(nullptr, nullptr, dt::entry::Door::NativeInject);
        } catch (...) {
        }
    }

    // WP-6.1/6.2 + C-7: Register hotkeys on this worker thread (Ctrl+Shift+K toggle, Ctrl+Shift+U unload)
    // using keys present on all TKL/60% layouts. Check return value and warn on collision.
    constexpr UINT kToggleMods = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
    constexpr UINT kToggleVk   = 'K';
    constexpr UINT kUnloadMods = MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT;
    constexpr UINT kUnloadVk   = 'U';

    const BOOL hkToggle = RegisterHotKey(nullptr, kHotkeyToggleId, kToggleMods, kToggleVk);
    if (!hkToggle && GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED) {
        MessageBoxW(nullptr,
            L"La combinacion Ctrl+Shift+K ya esta registrada por otro proceso.",
            L"Display Tuner", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }
    const BOOL hkUnload = RegisterHotKey(nullptr, kHotkeyUnloadId, kUnloadMods, kUnloadVk);
    if (!hkUnload && GetLastError() == ERROR_HOTKEY_ALREADY_REGISTERED) {
        MessageBoxW(nullptr,
            L"La combinacion Ctrl+Shift+U ya esta registrada por otro proceso.",
            L"Display Tuner", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    bool doUnload = false;
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_abortEvent && WaitForSingleObject(g_abortEvent, 0) == WAIT_OBJECT_0) {
            break;
        }
        if (msg.message == WM_HOTKEY) {
            if (msg.wParam == kHotkeyToggleId) {
                dt::vgui::toggleVisible();
            } else if (msg.wParam == kHotkeyUnloadId) {
                doUnload = true;
                break;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hkToggle) UnregisterHotKey(nullptr, kHotkeyToggleId);
    if (hkUnload) UnregisterHotKey(nullptr, kHotkeyUnloadId);

    if (doUnload) {
        dt::entry::requestFullUnload();
        while (!dt::entry::bootstrapFinished()) Sleep(50);
    }

    if (!s_isLoadLib) {
        dt::platform::uninstallStealthThreadHook();
    }

    if (g_abortEvent) { CloseHandle(g_abortEvent); g_abortEvent = nullptr; }
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
    if (doUnload && s_isLoadLib && g_self) {
        FreeLibraryAndExitThread(g_self, 0);
    }

    return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        g_self = hinst;
        g_abortEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
        return g_thread != nullptr;
    }
    if (reason == DLL_PROCESS_DETACH) {
        if (g_abortEvent) SetEvent(g_abortEvent);
    }
    return TRUE;
}
