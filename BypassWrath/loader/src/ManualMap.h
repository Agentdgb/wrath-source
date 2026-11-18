#pragma once
// ManualMap.h -- Manual-Map injection engine for BypassWrath.
// Primary path : ManualMapInMemory  (plain VirtualAllocEx + WPM)
// Secondary    : ManualMapViaTxf    (TxF-backed SEC_IMAGE -- VAD shows MEM_IMAGE)
// Both paths:
//   - DLL never appears in PEB->Ldr list
//   - Not visible to EnumProcessModules / TH32CS_SNAPMODULE
//   - Thread hidden via NtCreateThreadEx(HIDE_FROM_DEBUGGER)
//   - No RtlAddFunctionTable (would corrupt HotSpot safepoint handler chain)

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdint>
#include <algorithm>

#include "../shared/StealthDefs.h"

namespace bp {

// ── RAII guards ────────────────────────────────────────────────────────────

class ScopedRemoteMem {
public:
    ScopedRemoteMem(HANDLE proc, LPVOID hint, SIZE_T sz, DWORD type, DWORD prot)
        : m_proc(proc)
    {
        m_ptr = reinterpret_cast<BYTE*>(
            VirtualAllocEx(proc, hint, sz, type, prot));
    }
    ~ScopedRemoteMem() { Release(); }
    ScopedRemoteMem(const ScopedRemoteMem&) = delete;
    ScopedRemoteMem& operator=(const ScopedRemoteMem&) = delete;
    ScopedRemoteMem(ScopedRemoteMem&& o) noexcept
        : m_proc(o.m_proc), m_ptr(o.m_ptr) { o.m_ptr = nullptr; }
    ScopedRemoteMem& operator=(ScopedRemoteMem&& o) noexcept {
        if (this != &o) { Release(); m_proc = o.m_proc; m_ptr = o.m_ptr; o.m_ptr = nullptr; }
        return *this;
    }

    [[nodiscard]] BYTE* Get()      const noexcept { return m_ptr; }
    [[nodiscard]] bool  IsValid()  const noexcept { return m_ptr != nullptr; }
    BYTE* Detach() noexcept { BYTE* t = m_ptr; m_ptr = nullptr; return t; }
    void Release() noexcept {
        if (m_ptr) { VirtualFreeEx(m_proc, m_ptr, 0, MEM_RELEASE); m_ptr = nullptr; }
    }

private:
    HANDLE m_proc = nullptr;
    BYTE*  m_ptr  = nullptr;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : m_h(h) {}
    ~ScopedHandle() noexcept { Reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& o) noexcept : m_h(o.m_h) { o.m_h = nullptr; }

    [[nodiscard]] HANDLE Get()     const noexcept { return m_h; }
    [[nodiscard]] bool   IsValid() const noexcept {
        return m_h && m_h != INVALID_HANDLE_VALUE;
    }
    void Reset(HANDLE h = nullptr) noexcept {
        if (IsValid()) CloseHandle(m_h);
        m_h = h;
    }

private:
    HANDLE m_h = nullptr;
};

// ── PE helpers ─────────────────────────────────────────────────────────────

inline DWORD RvaToFileOff(const IMAGE_NT_HEADERS* nt, DWORD rva) {
    const auto* s = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
        if (rva >= s->VirtualAddress && rva < s->VirtualAddress + s->Misc.VirtualSize)
            return (rva - s->VirtualAddress) + s->PointerToRawData;
    return rva;
}

inline DWORD ResolveExportRva(const BYTE* bytes, const char* name) {
    if (!bytes) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto& e = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!e.VirtualAddress) return 0;
    DWORD eo = RvaToFileOff(nt, e.VirtualAddress);
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(bytes + eo);
    const auto* names  = reinterpret_cast<const DWORD*>(bytes + RvaToFileOff(nt, exp->AddressOfNames));
    const auto* ords   = reinterpret_cast<const WORD* >(bytes + RvaToFileOff(nt, exp->AddressOfNameOrdinals));
    const auto* funcs  = reinterpret_cast<const DWORD*>(bytes + RvaToFileOff(nt, exp->AddressOfFunctions));
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = reinterpret_cast<const char*>(bytes + RvaToFileOff(nt, names[i]));
        if (strcmp(n, name) == 0) return funcs[ords[i]];
    }
    return 0;
}

// ── Remote loader stub (PIC, runs inside javaw.exe) ───────────────────────
// Steps: relocations → IAT → TLS callbacks → DllMain (CRT init).
// NEVER calls RtlAddFunctionTable -- HotSpot safepoint handler chain must
// not be touched.

struct MmData {
    BYTE*                   base;
    PIMAGE_NT_HEADERS       nt;
    PIMAGE_BASE_RELOCATION  reloc;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    DWORD                   _pad;   // bootstrap_rva kept for struct compat but unused by stub

    using fnLoadLibA    = HMODULE(WINAPI*)(LPCSTR);
    using fnGetProc     = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using fnDllMain     = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);

    fnLoadLibA  loadLib;
    fnGetProc   getProc;
};

#pragma check_stack(off)
#pragma runtime_checks("", off)
__declspec(noinline) inline void WINAPI RemoteStub(MmData* d) {
    if (!d) return;
    BYTE*                    loc    = d->base;
    auto*                    nt     = d->nt;
    auto*                    reloc  = d->reloc;
    auto*                    imp    = d->imp;
    auto                     loadLib = d->loadLib;
    auto                     getProc = d->getProc;

    // 1. Base relocations
    const uintptr_t delta = reinterpret_cast<uintptr_t>(loc)
                          - nt->OptionalHeader.ImageBase;
    if (delta && reloc) {
        while (reloc->VirtualAddress) {
            if (reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION)) {
                DWORD cnt = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                const WORD* e = reinterpret_cast<const WORD*>(reloc + 1);
                for (DWORD i = 0; i < cnt; ++i) {
                    if (!e[i]) continue;
                    WORD type = e[i] >> 12, off = e[i] & 0xFFF;
                    if (type == IMAGE_REL_BASED_DIR64)
                        *reinterpret_cast<uintptr_t*>(loc + reloc->VirtualAddress + off) += delta;
                    else if (type == IMAGE_REL_BASED_HIGHLOW)
                        *reinterpret_cast<DWORD*>(loc + reloc->VirtualAddress + off) += (DWORD)delta;
                }
            }
            reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
                reinterpret_cast<BYTE*>(reloc) + reloc->SizeOfBlock);
        }
    }

    // 2. IAT
    if (imp) {
        while (imp->Characteristics || imp->Name) {
            const char* mod = reinterpret_cast<const char*>(loc + imp->Name);
            HMODULE h = loadLib(mod);
            if (h) {
                auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
                    loc + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                auto* func  = reinterpret_cast<PIMAGE_THUNK_DATA>(loc + imp->FirstThunk);
                while (thunk->u1.AddressOfData) {
                    if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal))
                        func->u1.Function = reinterpret_cast<uintptr_t>(
                            getProc(h, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk->u1.Ordinal))));
                    else {
                        auto* ibn = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(loc + thunk->u1.AddressOfData);
                        func->u1.Function = reinterpret_cast<uintptr_t>(getProc(h, ibn->Name));
                    }
                    ++thunk; ++func;
                }
            }
            ++imp;
        }
    }

    // 3. TLS callbacks (safe: fires before CRT, payload must handle gracefully)
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(
            loc + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        if (tls && tls->AddressOfCallBacks) {
            auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
            while (cb && *cb) { (*cb)(loc, DLL_PROCESS_ATTACH, nullptr); ++cb; }
        }
    }

    // 4. CRT init + DllMain via AddressOfEntryPoint (_DllMainCRTStartup)
    //    CRT MUST be initialized before any static constructors / heap alloc.
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        auto ep = reinterpret_cast<MmData::fnDllMain>(
            loc + nt->OptionalHeader.AddressOfEntryPoint);
        ep(reinterpret_cast<HINSTANCE>(loc), DLL_PROCESS_ATTACH, nullptr);
    }
}
#pragma runtime_checks("", restore)
#pragma check_stack()

// ── ManualMapInMemory -- primary path ──────────────────────────────────────
// DLL bytes in RAM → VirtualAllocEx → WPM sections → RemoteStub thread.
// Invisible to PEB->Ldr, EnumProcessModules, TH32CS_SNAPMODULE.

inline bool ManualMapInMemory(HANDLE hProc, const uint8_t* buf, size_t sz,
                               std::wstring& status)
{
    if (!buf || sz < sizeof(IMAGE_DOS_HEADER)) {
        status = L"Buffer invalido."; return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        status = L"Firma DOS invalida."; return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        status = L"Firma NT invalida."; return false;
    }

    // Alloc at preferred base, fallback anywhere
    ScopedRemoteMem mem(hProc,
        reinterpret_cast<LPVOID>(nt->OptionalHeader.ImageBase),
        nt->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem.IsValid())
        mem = ScopedRemoteMem(hProc, nullptr,
            nt->OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem.IsValid()) {
        status = L"VirtualAllocEx fallo."; return false;
    }

    BYTE* base = mem.Get();

    // Headers
    if (!WriteProcessMemory(hProc, base, buf, nt->OptionalHeader.SizeOfHeaders, nullptr)) {
        status = L"WPM headers fallo."; return false;
    }

    // Sections
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->SizeOfRawData &&
            !WriteProcessMemory(hProc, base + sec->VirtualAddress,
                buf + sec->PointerToRawData, sec->SizeOfRawData, nullptr)) {
            status = L"WPM seccion fallo."; return false;
        }
    }

    // Build MmData for the stub
    MmData md{};
    md.base  = base;
    md.nt    = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size)
        md.reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
            base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
        md.imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    md.loadLib = reinterpret_cast<MmData::fnLoadLibA>(GetProcAddress(k32, "LoadLibraryA"));
    md.getProc = reinterpret_cast<MmData::fnGetProc>(GetProcAddress(k32, "GetProcAddress"));
    md._pad    = ResolveExportRva(buf, "PayloadInit"); // diagnostic only

    // Stub allocation
    constexpr size_t kStubSz = 0x1000;
    ScopedRemoteMem stub_mem(hProc, nullptr,
        kStubSz + sizeof(MmData), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub_mem.IsValid()) {
        status = L"Alloc stub fallo."; return false;
    }

    BYTE* remote_stub = stub_mem.Get();
    BYTE* remote_data = remote_stub + kStubSz;
    WriteProcessMemory(hProc, remote_data, &md, sizeof(MmData), nullptr);
    WriteProcessMemory(hProc, remote_stub,
        reinterpret_cast<LPCVOID>(&RemoteStub), kStubSz, nullptr);

    // Spawn thread hidden from debugger/scanner
    ScopedHandle th;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto NtCTE = reinterpret_cast<bp::pfnNtCreateThreadEx>(
            GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (NtCTE) {
            HANDLE raw = nullptr;
            NTSTATUS st = NtCTE(&raw,
                SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                nullptr, hProc,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
                remote_data,
                bp::kHideThread,   // hidden from debugger + thread-list scanners
                0, 0, 0, nullptr);
            if (NT_SUCCESS(st) && raw) th.Reset(raw);
        }
    }
    if (!th.IsValid()) {
        th.Reset(CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_data, 0, nullptr));
    }
    if (!th.IsValid()) {
        status = L"CreateRemoteThread fallo."; return false;
    }

    if (WaitForSingleObject(th.Get(), 15000) != WAIT_OBJECT_0) {
        status = L"Timeout esperando stub remoto."; return false;
    }
    th.Reset();

    // Restore per-section protection (remove full-image RWX footprint)
    sec = IMAGE_FIRST_SECTION(nt);
    for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        DWORD ssz = (std::max)(sec->SizeOfRawData, sec->Misc.VirtualSize);
        if (!ssz) continue;
        bool r = (sec->Characteristics & IMAGE_SCN_MEM_READ)    != 0;
        bool w = (sec->Characteristics & IMAGE_SCN_MEM_WRITE)   != 0;
        bool x = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        DWORD prot = x ? (w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE))
                       : (w ? PAGE_READWRITE          : (r ? PAGE_READONLY     : PAGE_NOACCESS));
        DWORD old = 0;
        VirtualProtectEx(hProc, base + sec->VirtualAddress, ssz, prot, &old);
    }
    // Headers read-only (CRT SEH needs them readable)
    DWORD old = 0;
    VirtualProtectEx(hProc, base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);

    // Zero + free stub (removes injection artifact from remote address space)
    std::vector<BYTE> z(kStubSz + sizeof(MmData), 0);
    WriteProcessMemory(hProc, remote_stub, z.data(), z.size(), nullptr);
    stub_mem.Release();

    // Detach RAII -- DLL lives in target from here
    mem.Detach();
    status = L"Inyectado OK (ManualMap RAM).";
    return true;
}

// ── TxF sacrifices ─────────────────────────────────────────────────────────
// VAD entry shows MEM_IMAGE (not MEM_PRIVATE) -- scanners that check VAD type
// see a "legitimate" mapped image region. The on-disk DLL bytes are rolled back
// by the transaction; nothing is written to disk.

static constexpr const wchar_t* kTxfTargets[] = {
    L"winmm.dll",
    L"mpr.dll",
    L"ksuser.dll",
    L"version.dll",
    L"winspool.drv",
    nullptr
};

// ManualMapViaTxf -- secondary path
// Builds a TxF SEC_IMAGE section from dll_bytes, maps into target, resolves
// relocs+IAT from loader side (session-wide ASLR: local VA == remote VA for
// system DLLs), then spawns a hidden thread to run TLS + DllMain.

inline bool ManualMapViaTxf(HANDLE hProc, const uint8_t* buf, size_t sz,
                              std::wstring& status)
{
    if (!buf || sz < sizeof(IMAGE_DOS_HEADER)) {
        status = L"TxF: buffer invalido."; return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(buf + dos->e_lfanew);

    // Load KTM + ntdll functions
    HMODULE hKtm  = LoadLibraryW(L"ktmw32.dll");
    HMODULE hNt   = GetModuleHandleW(L"ntdll.dll");
    if (!hKtm || !hNt) {
        if (hKtm) FreeLibrary(hKtm);
        status = L"TxF: ktmw32/ntdll no disponibles."; return false;
    }

    auto pCreateTx = reinterpret_cast<bp::pfnCreateTransaction_t>(
        GetProcAddress(hKtm, "CreateTransaction"));
    auto pRollback = reinterpret_cast<bp::pfnRollbackTransaction_t>(
        GetProcAddress(hKtm, "RollbackTransaction"));
    auto NtCreateSection = reinterpret_cast<bp::pfnNtCreateSection_t>(
        GetProcAddress(hNt, "NtCreateSection"));
    auto NtMapView = reinterpret_cast<bp::pfnNtMapViewOfSection_t>(
        GetProcAddress(hNt, "NtMapViewOfSection"));
    auto NtUnmap   = reinterpret_cast<bp::pfnNtUnmapViewOfSection_t>(
        GetProcAddress(hNt, "NtUnmapViewOfSection"));

    if (!pCreateTx || !pRollback || !NtCreateSection || !NtMapView || !NtUnmap) {
        FreeLibrary(hKtm);
        status = L"TxF: no se resolvieron funciones NT."; return false;
    }

    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);

    HANDLE hTx = INVALID_HANDLE_VALUE, hFile = INVALID_HANDLE_VALUE, hSec = nullptr;
    for (int i = 0; kTxfTargets[i]; ++i) {
        wchar_t path[MAX_PATH];
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\%s", sys, kTxfTargets[i]);

        HANDLE tx = pCreateTx(nullptr, nullptr, 0, 0, 0, 0, nullptr);
        if (tx == INVALID_HANDLE_VALUE) continue;

        HANDLE f = CreateFileTransactedW(path, GENERIC_READ | GENERIC_WRITE, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
            nullptr, tx, nullptr, nullptr);
        if (f == INVALID_HANDLE_VALUE) { pRollback(tx); CloseHandle(tx); continue; }

        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        DWORD written = 0;
        BOOL ok = WriteFile(f, buf, (DWORD)sz, &written, nullptr);
        if (!ok || written != (DWORD)sz) {
            CloseHandle(f); pRollback(tx); CloseHandle(tx); continue;
        }
        LARGE_INTEGER end{}; end.QuadPart = (LONGLONG)sz;
        SetFilePointerEx(f, end, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        FlushFileBuffers(f);

        HANDLE sec = nullptr;
        NTSTATUS st = NtCreateSection(&sec, SECTION_ALL_ACCESS,
            nullptr, nullptr, PAGE_READONLY, SEC_IMAGE, f);
        if (!NT_SUCCESS(st) || !sec) {
            CloseHandle(f); pRollback(tx); CloseHandle(tx); continue;
        }
        hTx = tx; hFile = f; hSec = sec;
        break;
    }

    if (!hSec) {
        FreeLibrary(hKtm);
        status = L"TxF: ningun candidato genero seccion SEC_IMAGE."; return false;
    }

    // Map into target
    PVOID remote_base = nullptr; SIZE_T remote_sz = 0;
    NTSTATUS st = NtMapView(hSec, hProc, &remote_base,
        0, 0, nullptr, &remote_sz, 1, 0, PAGE_EXECUTE_READ);
    if (!NT_SUCCESS(st) || !remote_base) {
        CloseHandle(hSec); CloseHandle(hFile);
        pRollback(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        status = L"TxF: NtMapViewOfSection fallo."; return false;
    }

    // Resolve relocations via WPM from loader side
    const uintptr_t rbase = reinterpret_cast<uintptr_t>(remote_base);
    const intptr_t  delta = static_cast<intptr_t>(rbase - nt->OptionalHeader.ImageBase);

    auto rva_to_raw = [&](DWORD rva) -> const BYTE* {
        DWORD off = RvaToFileOff(nt, rva);
        return (off < sz) ? buf + off : nullptr;
    };

    if (delta && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        DWORD reloc_rva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        DWORD reloc_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        const BYTE* rp   = rva_to_raw(reloc_rva);
        if (rp) {
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(rp);
            const BYTE* end_p = rp + reloc_size;
            while (reinterpret_cast<const BYTE*>(block) < end_p && block->VirtualAddress) {
                DWORD cnt = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                const WORD* e = reinterpret_cast<const WORD*>(block + 1);
                for (DWORD i = 0; i < cnt; ++i) {
                    WORD type = e[i] >> 12;
                    DWORD erva = block->VirtualAddress + (e[i] & 0xFFF);
                    if (type == IMAGE_REL_BASED_DIR64) {
                        const BYTE* rv = rva_to_raw(erva); if (!rv) continue;
                        DWORD64 ov; memcpy(&ov, rv, 8);
                        DWORD64 nv = ov + (DWORD64)delta;
                        WriteProcessMemory(hProc,
                            reinterpret_cast<BYTE*>(remote_base) + erva, &nv, 8, nullptr);
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        const BYTE* rv = rva_to_raw(erva); if (!rv) continue;
                        DWORD ov; memcpy(&ov, rv, 4);
                        DWORD nv = ov + (DWORD)delta;
                        WriteProcessMemory(hProc,
                            reinterpret_cast<BYTE*>(remote_base) + erva, &nv, 4, nullptr);
                    }
                }
                block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<const BYTE*>(block) + block->SizeOfBlock);
            }
        }
    }

    // Resolve IAT from loader side (session-wide ASLR: same VA in loader + target)
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        DWORD imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        const BYTE* irp = rva_to_raw(imp_rva);
        if (irp) {
            const auto* imp_desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(irp);
            while (imp_desc->Characteristics || imp_desc->Name) {
                const BYTE* mn = rva_to_raw(imp_desc->Name);
                if (mn) {
                    HMODULE hMod = LoadLibraryA(reinterpret_cast<const char*>(mn));
                    if (hMod) {
                        DWORD oft_rva = imp_desc->OriginalFirstThunk
                            ? imp_desc->OriginalFirstThunk : imp_desc->FirstThunk;
                        const BYTE* oft_raw = rva_to_raw(oft_rva);
                        const BYTE* ft_raw  = rva_to_raw(imp_desc->FirstThunk);
                        if (oft_raw && ft_raw) {
                            const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(oft_raw);
                            DWORD idx = 0;
                            while (thunk->u1.AddressOfData) {
                                FARPROC fn = nullptr;
                                if (IMAGE_SNAP_BY_ORDINAL(thunk->u1.Ordinal))
                                    fn = GetProcAddress(hMod,
                                        reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk->u1.Ordinal)));
                                else {
                                    const BYTE* ibn_raw = rva_to_raw((DWORD)thunk->u1.AddressOfData);
                                    if (ibn_raw) {
                                        const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(ibn_raw);
                                        fn = GetProcAddress(hMod, ibn->Name);
                                    }
                                }
                                if (fn) {
                                    DWORD ft_entry_rva = imp_desc->FirstThunk + idx * sizeof(IMAGE_THUNK_DATA);
                                    uintptr_t fptr = reinterpret_cast<uintptr_t>(fn);
                                    WriteProcessMemory(hProc,
                                        reinterpret_cast<BYTE*>(remote_base) + ft_entry_rva,
                                        &fptr, sizeof(fptr), nullptr);
                                }
                                ++thunk; ++idx;
                            }
                        }
                    }
                }
                ++imp_desc;
            }
        }
    }

    // TLS + DllMain via hidden thread (re-use RemoteStub logic)
    // Build a minimal MmData pointing at the already-mapped remote region
    MmData md{};
    md.base = reinterpret_cast<BYTE*>(remote_base);
    md.nt   = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(remote_base) + dos->e_lfanew);
    // reloc/IAT already resolved by loader; pass null so stub skips them
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    md.loadLib = reinterpret_cast<MmData::fnLoadLibA>(GetProcAddress(k32, "LoadLibraryA"));
    md.getProc = reinterpret_cast<MmData::fnGetProc>(GetProcAddress(k32, "GetProcAddress"));

    constexpr size_t kStubSz = 0x1000;
    ScopedRemoteMem stub_mem(hProc, nullptr,
        kStubSz + sizeof(MmData), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub_mem.IsValid()) {
        CloseHandle(hSec); CloseHandle(hFile);
        pRollback(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        status = L"TxF: alloc stub fallo."; return false;
    }

    BYTE* remote_stub = stub_mem.Get();
    BYTE* remote_md   = remote_stub + kStubSz;
    WriteProcessMemory(hProc, remote_md,   &md,            sizeof(MmData), nullptr);
    WriteProcessMemory(hProc, remote_stub, (LPCVOID)&RemoteStub, kStubSz, nullptr);

    ScopedHandle th;
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto NtCTE = reinterpret_cast<bp::pfnNtCreateThreadEx>(
            GetProcAddress(hNtdll, "NtCreateThreadEx"));
        if (NtCTE) {
            HANDLE raw = nullptr;
            NTSTATUS st2 = NtCTE(&raw,
                SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                nullptr, hProc,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
                remote_md, bp::kHideThread, 0, 0, 0, nullptr);
            if (NT_SUCCESS(st2) && raw) th.Reset(raw);
        }
    }
    if (!th.IsValid()) {
        th.Reset(CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_md, 0, nullptr));
    }
    if (!th.IsValid()) {
        CloseHandle(hSec); CloseHandle(hFile);
        pRollback(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        status = L"TxF: CreateRemoteThread fallo."; return false;
    }

    if (WaitForSingleObject(th.Get(), 15000) != WAIT_OBJECT_0) {
        th.Reset();
        CloseHandle(hSec); CloseHandle(hFile);
        pRollback(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        status = L"TxF: timeout stub."; return false;
    }
    th.Reset();

    // Zero + free stub
    std::vector<BYTE> z(kStubSz + sizeof(MmData), 0);
    WriteProcessMemory(hProc, remote_stub, z.data(), z.size(), nullptr);
    stub_mem.Release();

    // Rollback transaction (removes on-disk write -- DLL bytes never persisted)
    pRollback(hTx);
    CloseHandle(hFile);
    CloseHandle(hTx);
    CloseHandle(hSec);
    FreeLibrary(hKtm);

    status = L"Inyectado OK (TxF SEC_IMAGE).";
    return true;
}

} // namespace bp
