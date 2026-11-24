#include "platform/Win32Console.h"

#include <cstdio>
#include <windows.h>

namespace vanta::platform {

namespace { bool g_owned = false; }

void openConsole(const wchar_t* title) {
    if (g_owned) return;
    if (!AllocConsole()) return;
    SetConsoleTitleW(title);

    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONIN$",  "r", stdin);
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    g_owned = true;
}

void closeConsole() {
    if (!g_owned) return;
    FreeConsole();
    g_owned = false;
}

} // namespace vanta::platform
