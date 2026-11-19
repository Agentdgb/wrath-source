// DllMain.cpp -- P1-01: g_hSelfBase, P1-04: TlsThreadCreate for PayloadThread.

#include <windows.h>
#include <new>
#include "TlsThread.h"

HMODULE g_hSelfBase = nullptr;

void PayloadMain(HMODULE hBase);

namespace {

DWORD WINAPI PayloadThread(LPVOID) {
    Sleep(500);
    __try {
        PayloadMain(g_hSelfBase);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hSelfBase = hMod;
        // P1-04: use TlsThreadCreate so PayloadThread gets DLL_THREAD_ATTACH/DETACH
        HANDLE t = payload::TlsThreadCreate(PayloadThread, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
