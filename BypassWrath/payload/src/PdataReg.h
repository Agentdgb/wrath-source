#pragma once
// PdataReg.h -- Dynamic .pdata runtime function registration inside target process.
// Registers the RUNTIME_FUNCTION table for [hBase, hBase + SizeOfImage) via
// ntdll!RtlAddFunctionTable. This ensures clean SEH and C++ unwinding within our
// module without touching foreign HotSpot JIT regions or registering ambiguous tables.

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace bp {

struct DynamicPdata {
    PRUNTIME_FUNCTION table = nullptr;
    DWORD             count = 0;
};

inline DynamicPdata RegisterOwnPdata(HMODULE hBase) {
    DynamicPdata res{};
    if (!hBase) return res;

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return res;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(hBase) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return res;

    // Locate .pdata section directly from section headers
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (memcmp(sec->Name, ".pdata", 6) == 0) {
            DWORD vsz = (sec->Misc.VirtualSize > 0) ? sec->Misc.VirtualSize : sec->SizeOfRawData;
            if (sec->VirtualAddress && vsz >= sizeof(RUNTIME_FUNCTION)) {
                res.table = reinterpret_cast<PRUNTIME_FUNCTION>(
                    reinterpret_cast<BYTE*>(hBase) + sec->VirtualAddress);
                res.count = vsz / sizeof(RUNTIME_FUNCTION);
                break;
            }
        }
    }

    if (res.table && res.count) {
        using pfnRtlAddFunctionTable_t =
            BOOLEAN(NTAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);
        HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
        if (hNt) {
            auto pfnAdd = reinterpret_cast<pfnRtlAddFunctionTable_t>(
                GetProcAddress(hNt, "RtlAddFunctionTable"));
            if (pfnAdd) {
                if (!pfnAdd(res.table, res.count, reinterpret_cast<DWORD64>(hBase))) {
                    res.table = nullptr;
                    res.count = 0;
                }
            }
        }
    }
    return res;
}

inline void UnregisterOwnPdata(DynamicPdata& pdata) {
    if (pdata.table) {
        using pfnRtlDeleteFunctionTable_t =
            BOOLEAN(NTAPI*)(PRUNTIME_FUNCTION);
        HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
        if (hNt) {
            auto pfnDel = reinterpret_cast<pfnRtlDeleteFunctionTable_t>(
                GetProcAddress(hNt, "RtlDeleteFunctionTable"));
            if (pfnDel) {
                pfnDel(pdata.table);
            }
        }
        pdata.table = nullptr;
        pdata.count = 0;
    }
}

} // namespace bp
