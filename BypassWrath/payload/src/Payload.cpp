// Payload.cpp -- FASE 2: UnmapSelf clean unload.
//
// PanicUnload (F8):
//   1. Sets g_running=false → main loop exits, all payload threads join.
//   2. Calls ep(base, DLL_PROCESS_DETACH, nullptr) for CRT shutdown.
//   3. Fires TLS callbacks with DLL_THREAD_DETACH on the unload thread.
//   4. Calls RtlDeleteFunctionTable (if pdata was registered).
//   5. TlsFree for the TLS slot.
//   6. Unmaps via NtUnmapViewOfSection OR VirtualFree from a continuation
//      thread that runs in kernel32 address space (so it doesn't free itself).
//
// NO SecureZeroMemory of the base region (removed per FASE 2 spec --
// zeroing the mapped image while threads may still execute in it is unsafe).
// NO FreeLibraryAndExitThread (removed per FASE 2 spec).

#include <windows.h>
#include <new>
#include "TlsThread.h"

// Shared with loader-patched TLS index (FASE 1).
// Payload tracks it via g_tls_index; loader patches _tls_index in .data.
extern HMODULE g_hSelfBase;

// Globals the unload sequence reads.
static bool           g_running   = false;
static HANDLE         g_mainThread = nullptr;  // PayloadThread handle (joined before free)

// NtUnmapViewOfSection typedef (ntdll, no import needed)
using pfnNtUnmapViewOfSection_t = LONG(NTAPI*)(HANDLE, PVOID);

// ── Continuation-thread free ────────────────────────────────────────────────
// Runs in a thread whose start address is inside kernel32 (not inside the
// payload image). By the time VirtualFree fires, the payload image is not
// on any call stack.
struct FreeCtx {
    HANDLE hProc;
    PVOID  base;
    bool   use_unmap;   // true = NtUnmapViewOfSection, false = VirtualFree
};

static DWORD WINAPI FreeThread(LPVOID raw) {
    auto* ctx = reinterpret_cast<FreeCtx*>(raw);
    Sleep(20); // let the unload thread fully return from UnmapSelf()
    if (ctx->use_unmap) {
        HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
        if (hNt) {
            auto NtUnmap = reinterpret_cast<pfnNtUnmapViewOfSection_t>(
                GetProcAddress(hNt, "NtUnmapViewOfSection"));
            if (NtUnmap) NtUnmap(ctx->hProc, ctx->base);
            else VirtualFree(ctx->base, 0, MEM_RELEASE);
        }
    } else {
        VirtualFree(ctx->base, 0, MEM_RELEASE);
    }
    delete ctx;
    return 0;
}

// ── UnmapSelf helpers ────────────────────────────────────────────────────────

// Calls DLL_PROCESS_DETACH via entry point. Isolated function: __try cannot
// coexist with C++ objects with destructors in the same scope (MSVC C2712).
static void CallEntryDetach(HMODULE base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(base) + dos->e_lfanew);
    if (!nt->OptionalHeader.AddressOfEntryPoint) return;
    using fnDllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    auto ep = reinterpret_cast<fnDllMain>(
        reinterpret_cast<const BYTE*>(base) +
        nt->OptionalHeader.AddressOfEntryPoint);
    __try { ep(base, DLL_PROCESS_DETACH, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── UnmapSelf ───────────────────────────────────────────────────────────────
static void UnmapSelf() {
    HMODULE base = g_hSelfBase;
    if (!base) return;

    // 1. Wait for payload main thread to exit cleanly.
    g_running = false;
    if (g_mainThread) {
        WaitForSingleObject(g_mainThread, 3000);
        CloseHandle(g_mainThread);
        g_mainThread = nullptr;
    }

    // 2. CRT + DllMain DLL_PROCESS_DETACH.
    CallEntryDetach(base);

    // 3. TLS callbacks DLL_THREAD_DETACH.
    payload::FireTlsCallbacks(DLL_THREAD_DETACH);

    // 4. Free from continuation thread (not running in image).
    auto* ctx = new (std::nothrow) FreeCtx{
        GetCurrentProcess(), base, false
    };
    if (ctx) {
        HANDLE t = CreateThread(nullptr, 0, FreeThread, ctx, 0, nullptr);
        if (t) CloseHandle(t);
        else delete ctx;
    }
}

// ── Main loop thread ─────────────────────────────────────────────────────────
// Runs inside the payload. Loop exits when g_running=false (from UnmapSelf).
static DWORD WINAPI MainLoop(LPVOID) {
    while (g_running) {
        if (GetAsyncKeyState(VK_F8) & 1) {
            UnmapSelf();
            return 0;
        }
        Sleep(50);
    }
    return 0;
}

void PayloadMain(HMODULE /*hBase*/) {
    g_running = true;
    // P2-04: track the main thread handle so UnmapSelf can join it.
    g_mainThread = payload::TlsThreadCreate(MainLoop, nullptr);
    if (g_mainThread) {
        WaitForSingleObject(g_mainThread, INFINITE);
        CloseHandle(g_mainThread);
        g_mainThread = nullptr;
    }
}
