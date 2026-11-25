#pragma once
#include <windows.h>
#include <atomic>

namespace dt::platform {

HWND windowFromDC(HDC hdc);
HWND findGameWindow();               // best-effort: prefers HWND owned by this process with GLFW-like class
HWND waitForGameWindow(int timeoutMs, const std::atomic<bool>* cancelled = nullptr);
bool isGameWindow(HWND hwnd);        // filters out launcher/console windows

} // namespace dt::platform
