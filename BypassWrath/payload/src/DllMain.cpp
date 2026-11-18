// DllMain.cpp -- bypass_payload.dll entry point (BypassWrath payload).
// Manual-mapped into javaw.exe: CRT initialized by _DllMainCRTStartup
// (called by RemoteStub via AddressOfEntryPoint).
// DllMain spawns a thread that calls PayloadMain; no JNI, no game modules.

#include <windows.h>

// Forward declaration -- defined in Payload.cpp
void PayloadMain();

namespace {

DWORD WINAPI PayloadThread(LPVOID) {
    // Brief yield: let JVM finish any pending safepoint before we touch GL.
    // 500ms is enough for Zulu 21 HotSpot to settle after attach.
    Sleep(500);
    __try {
        PayloadMain();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Swallow -- never let an unhandled exception propagate into JVM.
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE /*hMod*/, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Raw CreateThread -- safe under loader lock (thread is queued, not
        // dispatched until loader lock releases).
        HANDLE t = CreateThread(nullptr, 0, PayloadThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
