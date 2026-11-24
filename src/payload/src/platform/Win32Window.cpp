#include "platform/Win32Window.h"

#include <cwchar>

namespace vanta::platform {

HWND windowFromDC(HDC hdc) {
    return WindowFromDC(hdc);
}

namespace {

// Resolve in the game process. A manually mapped image can otherwise inherit
// an IAT address from the loader's process-specific compatibility shims.
DWORD windowProcessId(HWND window) {
    using Function = DWORD (WINAPI*)(HWND, LPDWORD);
    static const auto function = []() -> Function {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<Function>(
            GetProcAddress(user32, "GetWindowThreadProcessId")) : nullptr;
    }();
    DWORD pid = 0;
    if (function) function(window, &pid);
    return pid;
}

bool windowIsOurs(HWND h) {
    if (!h || !IsWindow(h)) return false;
    return windowProcessId(h) == GetCurrentProcessId();
}

// Enumeration state. Two candidates are tracked separately because a GLFW/LWJGL
// class is a certainty while "some large visible window" is only a guess — the
// guess must never win over the certainty just because it enumerated first.
struct FindCtx {
    DWORD pid;
    HWND  glfw;
    HWND  other;
};

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lp) {
    auto& ctx = *reinterpret_cast<FindCtx*>(lp);

    if (windowProcessId(hwnd) != ctx.pid) return TRUE;

    if (!IsWindowVisible(hwnd)) return TRUE;

    // GLFW creates a hidden 1x1 helper window ("GLFW3 Helper") for its message
    // loop and marks it WS_EX_TOOLWINDOW. Subclassing that one instead of the
    // real game window means the overlay never sees a single input message.
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);

    if ((wcsstr(cls, L"GLFW") || wcsstr(cls, L"LWJGL")) && !ctx.glfw) {
        ctx.glfw = hwnd;
        return TRUE;
    }

    // Launcher chrome. Lunar, Feather and friends run a Swing/JavaFX splash in
    // the same process; it is visible, top-level and completely wrong.
    if (wcsstr(cls, L"SunAwt") || wcsstr(cls, L"JavaFX") || wcsstr(cls, L"javafx"))
        return TRUE;

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w > 320 && h > 240 && !ctx.other) ctx.other = hwnd;

    return TRUE;
}

} // namespace

HWND findGameWindow() {
    // Fast path: ask for the exact classes GLFW registers on Win32. Which one
    // is used depends on the LWJGL build, so all three are tried.
    static const char* kClasses[] = { "LWJGL", "GLFW30", "GLFW30W" };
    for (const char* cn : kClasses) {
        HWND h = FindWindowA(cn, nullptr);
        if (windowIsOurs(h)) return h;
    }

    FindCtx ctx{ GetCurrentProcessId(), nullptr, nullptr };
    EnumWindows(&enumProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.glfw ? ctx.glfw : ctx.other;
}

HWND waitForGameWindow(int timeoutMs, const std::atomic<bool>* cancelled) {
    // Injecting from a launcher can land well before the game window exists:
    // Lunar and Feather boot through a JavaFX splash, authenticate, and only
    // then create the GLFW window. Polling is the difference between "the GUI
    // never opens" and "the GUI opens a few seconds in".
    for (int waited = 0; waited < timeoutMs; waited += 200) {
        if (cancelled && cancelled->load()) return nullptr;
        if (HWND h = findGameWindow()) return h;
        Sleep(200);
    }
    return findGameWindow();
}

bool isGameWindow(HWND hwnd) {
    if (!hwnd) return false;
    return windowProcessId(hwnd) == GetCurrentProcessId();
}

} // namespace vanta::platform
