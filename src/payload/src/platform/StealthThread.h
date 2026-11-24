#pragma once
#include <windows.h>

namespace vanta::platform {

// Installs a detour on CreateThread (KernelBase.dll / kernel32.dll) so that any
// thread created with a start address inside display_service64.dll has its
// Win32StartAddress recorded in the kernel as ntdll!RtlUserThreadStart.
// Passes through any foreign/JVM threads completely untouched.
bool installStealthThreadHook();
void uninstallStealthThreadHook();

// Direct helper to create a thread with Win32StartAddress spoofed to ntdll
HANDLE createStealthThread(LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags = 0, LPDWORD tid = nullptr);

} // namespace vanta::platform
