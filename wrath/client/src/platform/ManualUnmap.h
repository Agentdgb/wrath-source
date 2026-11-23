#pragma once
#include <windows.h>

namespace vanta::platform {

// NT prototype for NtUnmapViewOfSection (ntdll export, always present).
using pfnNtUnmapViewOfSection_t =
    LONG(NTAPI*)(HANDLE ProcessHandle, PVOID BaseAddress);

// Resolve NtUnmapViewOfSection at runtime.
// Returns nullptr if ntdll is not loaded (should never happen).
inline pfnNtUnmapViewOfSection_t resolveNtUnmap() noexcept {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<pfnNtUnmapViewOfSection_t>(
        GetProcAddress(ntdll, "NtUnmapViewOfSection"));
}

// Unmap the memory region that contains |self| from the current process.
//
// Used by the reaper when the image is NOT registered in the Windows loader
// (TxF SEC_IMAGE injection or classic ManualMap). In those cases FreeLibrary
// has no effect because the module is not in the PEB Ldr list.
//
// SAFETY CONTRACT — the caller must NOT execute any code that lives inside
// the image after this call returns. The image is gone. The only safe next
// action is ExitThread() or an equivalent that does not touch the unmapped
// pages. The caller must arrange this via a PIC stub on the stack (see
// reapAndFreeManual in Bootstrap.cpp) or by having stored the ExitThread
// function pointer before calling unmapSelfRegion.
//
// Returns true on NT_SUCCESS, false on any failure. A failure means the
// image stays mapped; the caller should report and leave it resident.
inline bool unmapSelfRegion(HMODULE self) noexcept {
    if (!self) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(self, &mbi, sizeof(mbi))) return false;

    void* base = mbi.AllocationBase ? mbi.AllocationBase
                                    : static_cast<void*>(self);

    if (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) {
        auto NtUnmap = resolveNtUnmap();
        if (NtUnmap) {
            LONG st = NtUnmap(GetCurrentProcess(), base);
            if (st >= 0) return true;
        }
    }

    return VirtualFree(base, 0, MEM_RELEASE) != FALSE;
}

// Executes unmap/free and calls ExitThread(0) from an isolated stub outside self,
// guaranteeing zero access violations during module self-destruction.
inline void executeManualUnloadAndExit(HMODULE self) noexcept {
    if (!self) {
        ExitThread(0);
        return;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(self, &mbi, sizeof(mbi))) {
        ExitThread(0);
        return;
    }

    void* base = mbi.AllocationBase ? mbi.AllocationBase : static_cast<void*>(self);

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    void* pExitThread = hK32 ? reinterpret_cast<void*>(GetProcAddress(hK32, "ExitThread")) : nullptr;
    if (!pExitThread) {
        ExitThread(0);
        return;
    }

    bool isSection = (mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED);
    void* pFreeFunc = nullptr;

    if (isSection) {
        auto NtUnmap = resolveNtUnmap();
        if (NtUnmap) {
            pFreeFunc = reinterpret_cast<void*>(NtUnmap);
        }
    }

    if (!pFreeFunc && hK32) {
        pFreeFunc = reinterpret_cast<void*>(GetProcAddress(hK32, "VirtualFree"));
        isSection = false;
    }

    if (!pFreeFunc) {
        ExitThread(0);
        return;
    }

    uint8_t* stub = reinterpret_cast<uint8_t*>(VirtualAlloc(
        nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));

    if (!stub) {
        ExitThread(0);
        return;
    }

    size_t offset = 0;
    // sub rsp, 0x28
    stub[offset++] = 0x48; stub[offset++] = 0x83; stub[offset++] = 0xEC; stub[offset++] = 0x28;

    if (isSection) {
        // mov rcx, -1 (GetCurrentProcess)
        stub[offset++] = 0x48; stub[offset++] = 0xC7; stub[offset++] = 0xC1;
        stub[offset++] = 0xFF; stub[offset++] = 0xFF; stub[offset++] = 0xFF; stub[offset++] = 0xFF;

        // mov rdx, base
        stub[offset++] = 0x48; stub[offset++] = 0xBA;
        memcpy(&stub[offset], &base, sizeof(void*));
        offset += sizeof(void*);
    } else {
        // mov rcx, base
        stub[offset++] = 0x48; stub[offset++] = 0xB9;
        memcpy(&stub[offset], &base, sizeof(void*));
        offset += sizeof(void*);

        // xor edx, edx
        stub[offset++] = 0x31; stub[offset++] = 0xD2;

        // mov r8d, 0x8000 (MEM_RELEASE)
        stub[offset++] = 0x41; stub[offset++] = 0xB8;
        uint32_t freeType = MEM_RELEASE;
        memcpy(&stub[offset], &freeType, sizeof(uint32_t));
        offset += sizeof(uint32_t);
    }

    // mov rax, pFreeFunc
    stub[offset++] = 0x48; stub[offset++] = 0xB8;
    memcpy(&stub[offset], &pFreeFunc, sizeof(void*));
    offset += sizeof(void*);

    // call rax
    stub[offset++] = 0xFF; stub[offset++] = 0xD0;

    // xor ecx, ecx
    stub[offset++] = 0x31; stub[offset++] = 0xC9;

    // mov rax, pExitThread
    stub[offset++] = 0x48; stub[offset++] = 0xB8;
    memcpy(&stub[offset], &pExitThread, sizeof(void*));
    offset += sizeof(void*);

    // jmp rax
    stub[offset++] = 0xFF; stub[offset++] = 0xE0;

    DWORD oldProtect = 0;
    VirtualProtect(stub, 128, PAGE_EXECUTE_READ, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), stub, 128);

    using StubFn = void(*)(void);
    auto runStub = reinterpret_cast<StubFn>(stub);
    runStub();
}

} // namespace vanta::platform
