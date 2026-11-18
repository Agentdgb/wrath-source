// Payload.cpp -- resident logic of bypass_payload.dll.
// Runs inside javaw.exe after Manual Map + CRT init.
// Currently: minimal stub that stays resident until F8 panic-unload.
// Extend PayloadMain() with whatever needs to run inside the JVM process.

#include <windows.h>

// ── Panic unload (F8) ───────────────────────────────────────────────────────
// Zeroes our header bytes and queues FreeLibraryAndExitThread so the DLL
// unmaps cleanly without crashing the JVM.

static HMODULE  g_hSelf    = nullptr;
static bool     g_running  = false;

static void PanicUnload() {
    g_running = false;
    // Zero DOS/NT headers to remove PE signature from memory scan.
    if (g_hSelf) {
        DWORD old = 0;
        VirtualProtect(g_hSelf, 0x1000, PAGE_READWRITE, &old);
        SecureZeroMemory(g_hSelf, 0x1000);
        VirtualProtect(g_hSelf, 0x1000, old, &old);
    }
    // Unload on a fresh thread so we don't free under ourselves.
    HANDLE t = CreateThread(nullptr, 0,
        [](LPVOID) -> DWORD {
            Sleep(50);
            FreeLibraryAndExitThread(g_hSelf, 0);
        },
        nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

// ── Payload entry ───────────────────────────────────────────────────────────

void PayloadMain() {
    // Capture module handle for panic unload
    g_hSelf   = GetModuleHandleW(nullptr); // placeholder; real handle via VEH or DllMain arg
    g_running = true;

    // ── Insert your logic here ──────────────────────────────────────────────
    // e.g. hook wglSwapBuffers IAT, hook JNI functions, read JVM fields, etc.
    // The JVM is live and CRT is initialized at this point.
    // ───────────────────────────────────────────────────────────────────────

    // Resident loop: poll F8 for panic unload.
    // Replace or extend with your actual event loop.
    while (g_running) {
        if (GetAsyncKeyState(VK_F8) & 1) {
            PanicUnload();
            break;
        }
        Sleep(50);
    }
}
