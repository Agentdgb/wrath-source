#pragma once
// ManualMap.h -- Surgical Manual-Map injection engine for BypassWrath.
//
// Hardened Production Path:
//   ManualMapFileBacked (SEC_IMAGE + POSIX delete -- VAD backed as MEM_IMAGE)
//
// Features:
//   - T1: IAT resolution delegated to RemoteStub inside target; remote kernel32 base
//         resolved via PEB->Ldr traversal so jvm.dll and non-system DLLs resolve cleanly.
//   - T2: CFG registration via SetProcessValidCallTargets before thread dispatch (prevents 0xC0000409).
//   - T8: ScopedRemoteMem zeroed and released on all error and completion paths.
//   - T11: Linker /OPT:NOICF preserves sentinel stub size measurement.
//   - T13: MmData cleaned of dead fields.
//   - T16: Decoy file pattern jnaXXXX.tmp matches benign JVM native extraction.
//   - T20: Win32StartAddress spoofed to jvm.dll export via remote export walk.
//   - Loader Lock: LdrLockLoaderLock synchronization around _DllMainCRTStartup.

#pragma comment(linker, "/OPT:NOICF")

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
        if (m_ptr) {
            VirtualFreeEx(m_proc, m_ptr, 0, MEM_RELEASE);
            m_ptr = nullptr;
        }
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

// ── DJB2 & Hash Resolver ────────────────────────────────────────────────────

static constexpr uint32_t Djb2(const char* s, uint32_t h = 5381) {
    return *s ? Djb2(s + 1, h * 33u ^ (unsigned char)*s) : h;
}

static constexpr uint32_t kHashTppWorker = Djb2("TppWorkerThread");
static constexpr uint32_t kHashRtlUser   = Djb2("RtlUserThreadStart");

inline FARPROC ResolveByHash(HMODULE hNtdll, uint32_t target_hash) {
    if (!hNtdll) return nullptr;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(hNtdll);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(hNtdll) + dos->e_lfanew);
    const auto& ee = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!ee.VirtualAddress) return nullptr;
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const BYTE*>(hNtdll) + ee.VirtualAddress);
    const DWORD* names  = reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(hNtdll) + exp->AddressOfNames);
    const WORD*  ords   = reinterpret_cast<const WORD*>(
        reinterpret_cast<const BYTE*>(hNtdll) + exp->AddressOfNameOrdinals);
    const DWORD* funcs  = reinterpret_cast<const DWORD*>(
        reinterpret_cast<const BYTE*>(hNtdll) + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(
            reinterpret_cast<const BYTE*>(hNtdll) + names[i]);
        uint32_t h = 5381;
        for (const char* p = name; *p; ++p) h = h * 33u ^ (unsigned char)*p;
        if (h == target_hash) {
            return reinterpret_cast<FARPROC>(
                reinterpret_cast<const BYTE*>(hNtdll) + funcs[ords[i]]);
        }
    }
    return nullptr;
}

// ── Remote PEB Module Walk (P0 - T1 & P2 - T20) ────────────────────────────
// Finds the base address of a module inside target process by walking PEB->Ldr.
inline PVOID GetRemoteModuleBase(HANDLE hProc, const wchar_t* target_mod_name) {
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) return nullptr;
    using pfnNtQIP = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto NtQIP = reinterpret_cast<pfnNtQIP>(
        GetProcAddress(hNt, "NtQueryInformationProcess"));
    if (!NtQIP) return nullptr;

    struct BW_PBI {
        PVOID Reserved1;
        PVOID PebBaseAddress;
        PVOID Reserved2[2];
        ULONG_PTR UniqueProcessId;
        PVOID Reserved3;
    } pbi{};

    ULONG ret_len = 0;
    if (NtQIP(hProc, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &ret_len) < 0 || !pbi.PebBaseAddress)
        return nullptr;

    // Read PEB.Ldr offset (at 0x18 on x64)
    PVOID pLdr = nullptr;
    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(pbi.PebBaseAddress) + 0x18, &pLdr, sizeof(pLdr), nullptr) || !pLdr)
        return nullptr;

    BW_PEB_LDR_DATA ldr{};
    if (!ReadProcessMemory(hProc, pLdr, &ldr, sizeof(ldr), nullptr))
        return nullptr;

    // P0-[2]: remoteHead must be the address of the field *inside the remote process*,
    // not a local stack pointer. Comparing curr != &local_struct caused infinite walk.
    LIST_ENTRY* remoteHead = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<BYTE*>(pLdr) + offsetof(BW_PEB_LDR_DATA, InMemoryOrderModuleList));
    LIST_ENTRY* curr = ldr.InMemoryOrderModuleList.Flink;

    for (int i = 0; i < 256 && curr && curr != remoteHead; ++i) {
        // Validate curr is a user-mode address before RPM
        const uintptr_t currva = reinterpret_cast<uintptr_t>(curr);
        if (currva < 0x10000 || currva > 0x7FFFFFFFFFFEull || (currva & 0x7) != 0) break;

        BW_LDR_DATA_TABLE_ENTRY entry{};
        BYTE* entry_addr = reinterpret_cast<BYTE*>(curr) - offsetof(BW_LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks);
        if (!ReadProcessMemory(hProc, entry_addr, &entry, sizeof(entry), nullptr)) break;

        if (entry.BaseDllName.Buffer && entry.BaseDllName.Length > 0) {
            USHORT wchars = entry.BaseDllName.Length / sizeof(wchar_t);
            std::wstring modName(wchars, L'\0');
            if (ReadProcessMemory(hProc, entry.BaseDllName.Buffer, modName.data(), entry.BaseDllName.Length, nullptr)) {
                if (_wcsicmp(modName.c_str(), target_mod_name) == 0) {
                    return entry.DllBase;
                }
            }
        }
        curr = entry.InMemoryOrderLinks.Flink;
    }
    return nullptr;
}

// ── Control Flow Guard (CFG) Target Registration (P0 - T2) ──────────────────
// Registers the remote stub and payload entrypoint as valid indirect call targets
// in the target's CFG bitmap before any thread is created.
inline void RegisterCfgCallTargets(HANDLE hProc, PVOID remote_stub, size_t stub_sz,
                                   PVOID base, const IMAGE_NT_HEADERS* nt) {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg_policy{};
    if (!GetProcessMitigationPolicy(hProc, ProcessControlFlowGuardPolicy, &cfg_policy, sizeof(cfg_policy)) ||
        !cfg_policy.EnableControlFlowGuard) {
        return; // CFG not enabled on target
    }

    HMODULE hKb = LoadLibraryW(L"kernelbase.dll");
    if (!hKb) return;
    auto pfnSetCfg = reinterpret_cast<pfnSetProcessValidCallTargets_t>(
        GetProcAddress(hKb, "SetProcessValidCallTargets"));
    if (!pfnSetCfg) {
        FreeLibrary(hKb);
        return;
    }

    // 1. Stub start offset (offset 0 in remote_stub page)
    BW_CFG_CALL_TARGET_INFO stub_target{ 0, CFG_CALL_TARGET_VALID };
    pfnSetCfg(hProc, remote_stub, stub_sz, 1, &stub_target);

    // 2. P1-[9]: Register all executable sections in the payload, not just EP.
    // Every indirect call site in .text needs to be in the CFG bitmap; registering
    // only the EP causes 0xC0000409 on the first indirect call inside a non-EP section.
    if (nt && base) {
        const auto* sec = IMAGE_FIRST_SECTION(nt);
        for (UINT i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            DWORD sec_sz = (std::max)(sec->SizeOfRawData, sec->Misc.VirtualSize);
            if (!sec_sz) continue;
            BW_CFG_CALL_TARGET_INFO ti{ sec->VirtualAddress, CFG_CALL_TARGET_VALID };
            pfnSetCfg(hProc, base, nt->OptionalHeader.SizeOfImage, 1, &ti);
        }
        // Also mark EP explicitly (may be in a thunk stub outside named sections)
        if (nt->OptionalHeader.AddressOfEntryPoint) {
            BW_CFG_CALL_TARGET_INFO ep_target{ nt->OptionalHeader.AddressOfEntryPoint, CFG_CALL_TARGET_VALID };
            pfnSetCfg(hProc, base, nt->OptionalHeader.SizeOfImage, 1, &ep_target);
        }
    }
    FreeLibrary(hKb);
}

// ── T20: Resolve an export from a remote module via ReadProcessMemory ────────
// No LoadLibrary on loader side — walks remote export directory entirely in-process.
inline FARPROC GetRemoteExportByName(HANDLE hProc, PVOID remote_base, const char* export_name) {
    if (!hProc || !remote_base || !export_name) return nullptr;

    IMAGE_DOS_HEADER dos{};
    if (!ReadProcessMemory(hProc, remote_base, &dos, sizeof(dos), nullptr)
        || dos.e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    IMAGE_NT_HEADERS nt{};
    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(remote_base) + dos.e_lfanew,
                           &nt, sizeof(nt), nullptr)
        || nt.Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    const auto& exp_dd = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!exp_dd.VirtualAddress || !exp_dd.Size) return nullptr;

    IMAGE_EXPORT_DIRECTORY exp{};
    if (!ReadProcessMemory(hProc,
            reinterpret_cast<BYTE*>(remote_base) + exp_dd.VirtualAddress,
            &exp, sizeof(exp), nullptr))
        return nullptr;

    if (!exp.NumberOfNames) return nullptr;

    std::vector<DWORD> names(exp.NumberOfNames);
    std::vector<WORD>  ords(exp.NumberOfNames);
    std::vector<DWORD> funcs(exp.NumberOfFunctions);

    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(remote_base) + exp.AddressOfNames,
            names.data(), names.size() * sizeof(DWORD), nullptr)) return nullptr;
    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(remote_base) + exp.AddressOfNameOrdinals,
            ords.data(), ords.size() * sizeof(WORD), nullptr)) return nullptr;
    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(remote_base) + exp.AddressOfFunctions,
            funcs.data(), funcs.size() * sizeof(DWORD), nullptr)) return nullptr;

    for (DWORD i = 0; i < exp.NumberOfNames; ++i) {
        char name_buf[128]{};
        if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(remote_base) + names[i],
                name_buf, sizeof(name_buf) - 1, nullptr))
            continue;
        if (strcmp(name_buf, export_name) == 0) {
            if (ords[i] >= exp.NumberOfFunctions) continue;
            return reinterpret_cast<FARPROC>(
                reinterpret_cast<BYTE*>(remote_base) + funcs[ords[i]]);
        }
    }
    return nullptr;
}

// ── Thread Start Address Spoofing (P2 - T20) ────────────────────────────────
// Prefers jvm.dll!JNI_GetCreatedJavaVMs resolved via remote export walk.
// Fallback: ntdll!TppWorkerThread / RtlUserThreadStart via hash.
using pfnNtSetInfoThread_t = bp::NTSTATUS(NTAPI*)(HANDLE, DWORD, PVOID, ULONG);

inline void SpoofWin32StartAddress(HANDLE hProc, HANDLE hThread) {
    FARPROC spoof_va = nullptr;

    // T20: Resolve jvm.dll!JNI_GetCreatedJavaVMs via remote export walk (no LoadLibrary)
    PVOID remoteJvm = GetRemoteModuleBase(hProc, L"jvm.dll");
    if (remoteJvm)
        spoof_va = GetRemoteExportByName(hProc, remoteJvm, "JNI_GetCreatedJavaVMs");

    // Fallback: ntdll!TppWorkerThread / RtlUserThreadStart
    if (!spoof_va) {
        HMODULE hNtLocal = GetModuleHandleW(L"ntdll.dll");
        if (hNtLocal) {
            spoof_va = ResolveByHash(hNtLocal, kHashTppWorker);
            if (!spoof_va) spoof_va = ResolveByHash(hNtLocal, kHashRtlUser);
        }
    }
    if (!spoof_va) return;

    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) return;
    auto NtSIT = reinterpret_cast<pfnNtSetInfoThread_t>(
        GetProcAddress(hNt, "NtSetInformationThread"));
    if (!NtSIT) return;

    constexpr DWORD kThreadWin32StartAddress = 9;
    NtSIT(hThread, kThreadWin32StartAddress,
          reinterpret_cast<PVOID*>(&spoof_va), sizeof(spoof_va));
}

// ── PE helpers ─────────────────────────────────────────────────────────────

// ── Remote loader stub (PIC, runs inside target) ───────────────────────────
// T13: MmData cleaned of unused fields.
// IAT fix: virtProt passed so stub can unprotect .idata before writing thunks.
struct MmData {
    BYTE*                    base;
    PIMAGE_NT_HEADERS        nt;
    PIMAGE_BASE_RELOCATION   reloc;
    PIMAGE_IMPORT_DESCRIPTOR imp;
    DWORD                    imp_rva;   // VirtualAddress of import dir
    DWORD                    imp_size;  // Size of import dir

    using fnLoadLibA   = HMODULE(WINAPI*)(LPCSTR);
    using fnGetProc    = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using fnDllMain    = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    using fnVirtProt   = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
    using fnLdrLock    = NTSTATUS(NTAPI*)(ULONG, ULONG*, PVOID*);
    using fnLdrUnlock  = NTSTATUS(NTAPI*)(ULONG, PVOID);

    fnLoadLibA  loadLib;
    fnGetProc   getProc;
    fnVirtProt  virtProt;  // VirtualProtect from remote kernel32
    fnLdrLock   ldrLock;   // LdrLockLoaderLock from ntdll
    fnLdrUnlock ldrUnlock; // LdrUnlockLoaderLock from ntdll
};

#pragma check_stack(off)
#pragma runtime_checks("", off)
__declspec(noinline) inline void WINAPI RemoteStub(MmData* d) {
    if (!d) return;
    BYTE*                    loc   = d->base;
    auto*                    nt    = d->nt;
    auto*                    reloc = d->reloc;
    auto*                    imp   = d->imp;

    // 1. Base relocations (if reloc non-null)
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

    // 2. IAT resolution inside target process (P0 - T1)
    // Unprotect .idata before writing thunks (required on SEC_IMAGE path where
    // .idata is mapped PAGE_EXECUTE_READ; VirtualProtect creates COW pages).
    auto loadLib  = d->loadLib;
    auto getProc  = d->getProc;
    auto virtProt = d->virtProt;

    if (imp && loadLib && getProc) {
        // Unprotect import directory range (covers descriptors + name hint table)
        DWORD old_imp = 0;
        if (virtProt && d->imp_size)
            virtProt(loc + d->imp_rva, d->imp_size, PAGE_EXECUTE_READWRITE, &old_imp);

        while (imp->Characteristics || imp->Name) {
            const char* mod = reinterpret_cast<const char*>(loc + imp->Name);
            HMODULE h = loadLib(mod);
            if (h) {
                auto* thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(
                    loc + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
                auto* func  = reinterpret_cast<PIMAGE_THUNK_DATA>(loc + imp->FirstThunk);

                // Also unprotect FirstThunk array (may be in a different section).
                // P0-[5]: compute real thunk span instead of assuming 2 pages.
                DWORD old_ft = 0;
                auto* ft_first = reinterpret_cast<PIMAGE_THUNK_DATA>(loc + imp->FirstThunk);
                PIMAGE_THUNK_DATA ft_scan = ft_first;
                while (ft_scan->u1.AddressOfData) ++ft_scan;
                size_t thunk_span = static_cast<size_t>(
                    reinterpret_cast<BYTE*>(ft_scan + 1) - reinterpret_cast<BYTE*>(ft_first));
                BYTE* ft_page = reinterpret_cast<BYTE*>(
                    reinterpret_cast<uintptr_t>(ft_first) & ~static_cast<uintptr_t>(0xFFF));
                size_t ft_protect_sz = thunk_span +
                    static_cast<size_t>(reinterpret_cast<BYTE*>(ft_first) - ft_page);
                // Align up to page boundary
                ft_protect_sz = (ft_protect_sz + 0xFFF) & ~static_cast<size_t>(0xFFF);
                if (virtProt)
                    virtProt(ft_page, ft_protect_sz, PAGE_EXECUTE_READWRITE, &old_ft);

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

                if (virtProt && old_ft)
                    virtProt(ft_page, ft_protect_sz, old_ft, &old_ft);
            }
            ++imp;
        }

        if (virtProt && old_imp && d->imp_size)
            virtProt(loc + d->imp_rva, d->imp_size, old_imp, &old_imp);
    }

    // 3. CRT init + DllMain via AddressOfEntryPoint (_DllMainCRTStartup)
    // Synchronized with target loader via LdrLockLoaderLock to prevent race with JVM threads
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        auto ep = reinterpret_cast<MmData::fnDllMain>(
            loc + nt->OptionalHeader.AddressOfEntryPoint);
        ULONG disp = 0;
        PVOID cookie = nullptr;
        if (d->ldrLock) d->ldrLock(0, &disp, &cookie);
        ep(reinterpret_cast<HINSTANCE>(loc), DLL_PROCESS_ATTACH, nullptr);
        if (d->ldrUnlock && cookie) d->ldrUnlock(0, cookie);
    }
}
#pragma runtime_checks("", restore)
#pragma check_stack()

#pragma check_stack(off)
#pragma runtime_checks("", off)
__declspec(noinline) inline void WINAPI RemoteStubSentinel() {}
#pragma runtime_checks("", restore)
#pragma check_stack()

inline size_t RealStubSize() {
    const size_t sz = static_cast<size_t>(
        reinterpret_cast<const BYTE*>(&RemoteStubSentinel) -
        reinterpret_cast<const BYTE*>(&RemoteStub));
    return sz; // No clamp — caller validates
}

// ── P1-[7]: Insert module into PEB->Ldr ──────────────────────────────────────
// A MEM_IMAGE region absent from PEB->Ldr is an orphan — trivially flagged by
// pe-sieve, Process Hacker, and any screenshare tool that calls EnumProcessModules.
// This inserts a full LDR_DATA_TABLE_ENTRY into all three Ldr lists so the
// mapped image looks like a legitimately loaded DLL.
//
// Call AFTER the thread join (payload already initialized so DllBase is live).
// base       : remote image base (from NtMapViewOfSection)
// nt_local   : pointer to local copy of IMAGE_NT_HEADERS (SizeOfImage, EP, timestamp)
// hProc      : target process handle (VM_OPERATION|VM_WRITE|VM_READ)
// spoof_name : plausible FullDllName, e.g. L"%APPDATA%\\.minecraft\\natives\\lwjgl_opengl.dll"
inline bool InsertModuleInLdr(HANDLE hProc, BYTE* remote_base,
                               const IMAGE_NT_HEADERS* nt_local,
                               const std::wstring& spoof_name)
{
    if (!hProc || !remote_base || !nt_local) return false;

    // 1. Read PEB->Ldr address from remote process
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtDll) return false;
    using pfnNtQIP = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto NtQIP = reinterpret_cast<pfnNtQIP>(GetProcAddress(hNtDll, "NtQueryInformationProcess"));
    if (!NtQIP) return false;

    struct { PVOID r1; PVOID PebBase; PVOID r2[2]; ULONG_PTR pid; PVOID r3; } pbi{};
    ULONG rlen = 0;
    if (NtQIP(hProc, 0, &pbi, sizeof(pbi), &rlen) < 0 || !pbi.PebBase) return false;

    PVOID pLdr = nullptr;
    if (!ReadProcessMemory(hProc, reinterpret_cast<BYTE*>(pbi.PebBase) + 0x18,
                           &pLdr, sizeof(pLdr), nullptr) || !pLdr) return false;

    BW_PEB_LDR_DATA ldr{};
    if (!ReadProcessMemory(hProc, pLdr, &ldr, sizeof(ldr), nullptr)) return false;

    // 2. Allocate remote storage: LDR entry + FullDllName buffer + BaseDllName buffer
    const std::wstring base_name = [&]() -> std::wstring {
        auto sep = spoof_name.rfind(L'\\');
        return (sep != std::wstring::npos) ? spoof_name.substr(sep + 1) : spoof_name;
    }();

    const size_t full_buf_sz = (spoof_name.size() + 1) * sizeof(wchar_t);
    const size_t base_buf_sz = (base_name.size()  + 1) * sizeof(wchar_t);
    const size_t alloc_sz    = sizeof(BW_LDR_DATA_TABLE_ENTRY) + full_buf_sz + base_buf_sz;

    PVOID remote_ldr_mem = VirtualAllocEx(hProc, nullptr, alloc_sz,
                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_ldr_mem) return false;

    BYTE* r_entry    = reinterpret_cast<BYTE*>(remote_ldr_mem);
    BYTE* r_full_buf = r_entry + sizeof(BW_LDR_DATA_TABLE_ENTRY);
    BYTE* r_base_buf = r_full_buf + full_buf_sz;

    // 3. Write name buffers
    if (!WriteProcessMemory(hProc, r_full_buf, spoof_name.c_str(), full_buf_sz, nullptr) ||
        !WriteProcessMemory(hProc, r_base_buf, base_name.c_str(),  base_buf_sz, nullptr)) {
        VirtualFreeEx(hProc, remote_ldr_mem, 0, MEM_RELEASE);
        return false;
    }

    // 4. Build LDR entry — read all three list heads from the remote Ldr
    BW_LDR_DATA_TABLE_ENTRY ldr_entry{};

    LIST_ENTRY* remote_load_head = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<BYTE*>(pLdr) + offsetof(BW_PEB_LDR_DATA, InLoadOrderModuleList));
    LIST_ENTRY  load_head_remote{};
    ReadProcessMemory(hProc, remote_load_head, &load_head_remote, sizeof(load_head_remote), nullptr);

    LIST_ENTRY* remote_mem_head = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<BYTE*>(pLdr) + offsetof(BW_PEB_LDR_DATA, InMemoryOrderModuleList));
    LIST_ENTRY  mem_head_remote{};
    ReadProcessMemory(hProc, remote_mem_head, &mem_head_remote, sizeof(mem_head_remote), nullptr);

    LIST_ENTRY* remote_init_head = reinterpret_cast<LIST_ENTRY*>(
        reinterpret_cast<BYTE*>(pLdr) + offsetof(BW_PEB_LDR_DATA, InInitializationOrderModuleList));
    LIST_ENTRY  init_head_remote{};
    ReadProcessMemory(hProc, remote_init_head, &init_head_remote, sizeof(init_head_remote), nullptr);

    // Insert at tail of each list
    LIST_ENTRY* old_load_blink = load_head_remote.Blink;
    ldr_entry.InLoadOrderLinks.Flink  = remote_load_head;
    ldr_entry.InLoadOrderLinks.Blink  = old_load_blink;

    LIST_ENTRY* old_mem_blink = mem_head_remote.Blink;
    ldr_entry.InMemoryOrderLinks.Flink = remote_mem_head;
    ldr_entry.InMemoryOrderLinks.Blink = old_mem_blink;

    LIST_ENTRY* old_init_blink = init_head_remote.Blink;
    ldr_entry.InInitializationOrderLinks.Flink = remote_init_head;
    ldr_entry.InInitializationOrderLinks.Blink = old_init_blink;

    ldr_entry.DllBase        = remote_base;
    ldr_entry.EntryPoint     = remote_base + nt_local->OptionalHeader.AddressOfEntryPoint;
    ldr_entry.SizeOfImage    = nt_local->OptionalHeader.SizeOfImage;
    ldr_entry.TimeDateStamp  = nt_local->FileHeader.TimeDateStamp;
    ldr_entry.LoadCount      = 0xFFFF; // pinned
    ldr_entry.TlsIndex       = 0;

    ldr_entry.FullDllName.Length        = static_cast<USHORT>(spoof_name.size() * sizeof(wchar_t));
    ldr_entry.FullDllName.MaximumLength = static_cast<USHORT>(full_buf_sz);
    ldr_entry.FullDllName.Buffer        = reinterpret_cast<PWSTR>(r_full_buf);

    ldr_entry.BaseDllName.Length        = static_cast<USHORT>(base_name.size() * sizeof(wchar_t));
    ldr_entry.BaseDllName.MaximumLength = static_cast<USHORT>(base_buf_sz);
    ldr_entry.BaseDllName.Buffer        = reinterpret_cast<PWSTR>(r_base_buf);

    // 5. Write LDR entry
    if (!WriteProcessMemory(hProc, r_entry, &ldr_entry, sizeof(ldr_entry), nullptr)) {
        VirtualFreeEx(hProc, remote_ldr_mem, 0, MEM_RELEASE);
        return false;
    }

    // 6. Splice into all three doubly-linked lists (append at tail)
    auto splice_tail = [&](LIST_ENTRY* remote_head, LIST_ENTRY* old_blink,
                           size_t links_offset) -> bool {
        LIST_ENTRY* r_links = reinterpret_cast<LIST_ENTRY*>(r_entry + links_offset);
        LIST_ENTRY* flink_ptr = reinterpret_cast<LIST_ENTRY*>(
            reinterpret_cast<BYTE*>(old_blink) + offsetof(LIST_ENTRY, Flink));
        if (!WriteProcessMemory(hProc, flink_ptr, &r_links, sizeof(r_links), nullptr)) return false;
        LIST_ENTRY* blink_ptr = reinterpret_cast<LIST_ENTRY*>(
            reinterpret_cast<BYTE*>(remote_head) + offsetof(LIST_ENTRY, Blink));
        return WriteProcessMemory(hProc, blink_ptr, &r_links, sizeof(r_links), nullptr) != FALSE;
    };

    if (!splice_tail(remote_load_head, old_load_blink,
                     offsetof(BW_LDR_DATA_TABLE_ENTRY, InLoadOrderLinks))) {
        VirtualFreeEx(hProc, remote_ldr_mem, 0, MEM_RELEASE);
        return false;
    }
    splice_tail(remote_mem_head,  old_mem_blink,
                offsetof(BW_LDR_DATA_TABLE_ENTRY, InMemoryOrderLinks));
    splice_tail(remote_init_head, old_init_blink,
                offsetof(BW_LDR_DATA_TABLE_ENTRY, InInitializationOrderLinks));

    // remote_ldr_mem intentionally NOT freed — must remain live in the Ldr list.
    return true;
}

// ── P4-01: ManualMapFileBacked (SEC_IMAGE + POSIX delete) ──────────────────
inline bool ManualMapFileBacked(HANDLE hProc, const uint8_t* buf, size_t sz,
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

    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) { status = L"ntdll no disponible."; return false; }

    auto NtCreateSection = reinterpret_cast<bp::pfnNtCreateSection_t>(
        GetProcAddress(hNt, "NtCreateSection"));
    auto NtMapView = reinterpret_cast<bp::pfnNtMapViewOfSection_t>(
        GetProcAddress(hNt, "NtMapViewOfSection"));
    auto NtUnmap = reinterpret_cast<bp::pfnNtUnmapViewOfSection_t>(
        GetProcAddress(hNt, "NtUnmapViewOfSection"));

    if (!NtCreateSection || !NtMapView || !NtUnmap) {
        status = L"No se resolvieron APIs NT."; return false;
    }

    // 1. Temporary decoy file named jnaXXXX.tmp (P2 - T16)
    wchar_t temp_dir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp_dir);
    wchar_t temp_file[MAX_PATH]{};
    if (!GetTempFileNameW(temp_dir, L"jna", 0, temp_file)) {
        status = L"Fallo al generar archivo temporal."; return false;
    }

    HANDLE hFile = CreateFileW(temp_file,
        GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        DeleteFileW(temp_file);
        status = L"Fallo al crear archivo temporal."; return false;
    }

    DWORD written = 0;
    if (!WriteFile(hFile, buf, static_cast<DWORD>(sz), &written, nullptr) || written != sz) {
        CloseHandle(hFile);
        DeleteFileW(temp_file);
        status = L"Fallo al escribir archivo temporal."; return false;
    }
    FlushFileBuffers(hFile);

    // 2. NtCreateSection(SEC_IMAGE)
    HANDLE hSec = nullptr;
    NTSTATUS st = NtCreateSection(&hSec,
                                  SECTION_ALL_ACCESS,
                                  nullptr,
                                  nullptr,
                                  PAGE_EXECUTE_READ,
                                  SEC_IMAGE,
                                  hFile);
    if (!NT_SUCCESS(st) || !hSec) {
        CloseHandle(hFile);
        DeleteFileW(temp_file);
        status = L"NtCreateSection(SEC_IMAGE) fallo."; return false;
    }

    // 3. NtMapViewOfSection into target process
    PVOID remote_base = nullptr;
    SIZE_T remote_sz = 0;
    st = NtMapView(hSec,
                   hProc,
                   &remote_base,
                   0,
                   0,
                   nullptr,
                   &remote_sz,
                   1, /* ViewShare */
                   0,
                   PAGE_EXECUTE_READ);
    if (!NT_SUCCESS(st) || !remote_base) {
        CloseHandle(hSec);
        CloseHandle(hFile);
        DeleteFileW(temp_file);
        status = L"NtMapViewOfSection fallo."; return false;
    }

    // 4. POSIX delete immediately unlinks the file from directory
    BW_FILE_DISPOSITION_INFO_EX disp_ex{};
    disp_ex.Flags = FILE_DISPOSITION_FLAG_DELETE |
                    FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (!SetFileInformationByHandle(hFile,
                                    FileDispositionInfoEx,
                                    &disp_ex,
                                    sizeof(disp_ex))) {
        FILE_DISPOSITION_INFO disp{};
        disp.DeleteFile = TRUE;
        SetFileInformationByHandle(hFile, FileDispositionInfo, &disp, sizeof(disp));
    }
    CloseHandle(hFile);
    CloseHandle(hSec);

    BYTE* base = reinterpret_cast<BYTE*>(remote_base);

    // 5. Remote kernel32 resolution for target-side IAT dispatch (P0 - T1)
    PVOID remoteK32 = GetRemoteModuleBase(hProc, L"kernel32.dll");
    if (!remoteK32) remoteK32 = GetModuleHandleW(L"kernel32.dll"); // fallback: same boot base

    HMODULE localK32 = GetModuleHandleW(L"kernel32.dll");
    DWORD rvaLoadLib = static_cast<DWORD>(
        reinterpret_cast<uintptr_t>(GetProcAddress(localK32, "LoadLibraryA")) -
        reinterpret_cast<uintptr_t>(localK32));
    DWORD rvaGetProc = static_cast<DWORD>(
        reinterpret_cast<uintptr_t>(GetProcAddress(localK32, "GetProcAddress")) -
        reinterpret_cast<uintptr_t>(localK32));
    DWORD rvaVirtProt = static_cast<DWORD>(
        reinterpret_cast<uintptr_t>(GetProcAddress(localK32, "VirtualProtect")) -
        reinterpret_cast<uintptr_t>(localK32));

    const auto& imp_dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    MmData md{};
    md.base     = base;
    md.nt       = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    md.reloc    = nullptr; // Kernel applied relocations via SEC_IMAGE
    md.imp      = imp_dd.Size ? reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
                      base + imp_dd.VirtualAddress) : nullptr;
    md.imp_rva  = imp_dd.VirtualAddress;
    md.imp_size = imp_dd.Size;
    md.loadLib  = reinterpret_cast<MmData::fnLoadLibA>(reinterpret_cast<BYTE*>(remoteK32) + rvaLoadLib);
    md.getProc  = reinterpret_cast<MmData::fnGetProc>(reinterpret_cast<BYTE*>(remoteK32) + rvaGetProc);
    md.virtProt = reinterpret_cast<MmData::fnVirtProt>(reinterpret_cast<BYTE*>(remoteK32) + rvaVirtProt);
    HMODULE hNtLocal = GetModuleHandleW(L"ntdll.dll");
    md.ldrLock   = reinterpret_cast<MmData::fnLdrLock>(GetProcAddress(hNtLocal, "LdrLockLoaderLock"));
    md.ldrUnlock = reinterpret_cast<MmData::fnLdrUnlock>(GetProcAddress(hNtLocal, "LdrUnlockLoaderLock"));

    // 6. Zero out Exception Directory in remote headers so HotSpot doesn't conflict
    DWORD exc_dir_off = dos->e_lfanew +
        static_cast<DWORD>(offsetof(IMAGE_NT_HEADERS, OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION]));
    IMAGE_DATA_DIRECTORY zero_dir{};
    DWORD old_p = 0;
    if (VirtualProtectEx(hProc, base + exc_dir_off, sizeof(zero_dir), PAGE_READWRITE, &old_p)) {
        WriteProcessMemory(hProc, base + exc_dir_off, &zero_dir, sizeof(zero_dir), nullptr);
        VirtualProtectEx(hProc, base + exc_dir_off, sizeof(zero_dir), old_p, &old_p);
    }

    const size_t kStubSz = RealStubSize();
    // P0-[2]: Hard fail — a zero or oversized stub means the sentinel measurement is broken.
    if (kStubSz == 0 || kStubSz > 0x1000) {
        status = L"Stub size invalido (0 o >4KB). Verificar /OPT:NOICF y sentinel.";
        return false;
    }
    ScopedRemoteMem stub_mem(hProc, nullptr,
        kStubSz + sizeof(MmData), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub_mem.IsValid()) {
        NtUnmap(hProc, remote_base);
        status = L"Alloc stub fallo."; return false;
    }

    BYTE* remote_stub = stub_mem.Get();
    BYTE* remote_data = remote_stub + kStubSz;

    if (!WriteProcessMemory(hProc, remote_data, &md, sizeof(MmData), nullptr) ||
        !WriteProcessMemory(hProc, remote_stub, reinterpret_cast<LPCVOID>(&RemoteStub), kStubSz, nullptr)) {
        stub_mem.Release();
        NtUnmap(hProc, remote_base);
        status = L"WPM stub fallo."; return false;
    }

    // 7. CFG Call Target Registration (P0 - T2)
    RegisterCfgCallTargets(hProc, remote_stub, kStubSz, base, nt);

    // P1-[6]: Instead of RWX across the entire image (which makes the VAD look hybrid),
    // only unprotect the PE headers and .idata section. SEC_IMAGE applies the correct
    // per-section protection from PE characteristics; touching only these two ranges
    // creates COW pages only where needed — identical to what LoadLibrary does.
    DWORD old_hdr_p = 0, old_imp_p = 0;
    VirtualProtectEx(hProc, base, nt->OptionalHeader.SizeOfHeaders, PAGE_READWRITE, &old_hdr_p);
    if (imp_dd.Size)
        VirtualProtectEx(hProc, base + imp_dd.VirtualAddress, imp_dd.Size, PAGE_READWRITE, &old_imp_p);

    // 8. Thread Creation
    ScopedHandle th;
    auto NtCTE = reinterpret_cast<bp::pfnNtCreateThreadEx>(
        GetProcAddress(hNt, "NtCreateThreadEx"));
    if (NtCTE) {
        HANDLE raw = nullptr;
        NTSTATUS st_th = NtCTE(&raw,
            SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION,
            nullptr, hProc,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_data,
            bp::kHideThread,
            0, 0, 0, nullptr);
        if (NT_SUCCESS(st_th) && raw) th.Reset(raw);
    }
    if (!th.IsValid()) {
        th.Reset(CreateRemoteThread(hProc, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_data, 0, nullptr));
    }
    if (!th.IsValid()) {
        stub_mem.Release();
        NtUnmap(hProc, remote_base);
        status = L"CreateRemoteThread fallo."; return false;
    }

    // 9. Thread Start Address Spoofing (P2 - T20)
    SpoofWin32StartAddress(hProc, th.Get());

    if (WaitForSingleObject(th.Get(), 15000) != WAIT_OBJECT_0) {
        th.Reset();
        // T8: Zero and free stub on timeout
        std::vector<BYTE> z(kStubSz + sizeof(MmData), 0);
        WriteProcessMemory(hProc, remote_stub, z.data(), z.size(), nullptr);
        stub_mem.Release();
        NtUnmap(hProc, remote_base);
        status = L"Timeout esperando stub remoto."; return false;
    }
    th.Reset();

    // Restore PE headers to read-only. .idata stays PAGE_READWRITE — a real loaded DLL
    // has writable .idata. All other sections keep their SEC_IMAGE-assigned protection.
    if (old_hdr_p)
        VirtualProtectEx(hProc, base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old_hdr_p);
    // .idata left writable intentionally (no restore).

    // 10. P1-[8]: Wipe complete PE header (SizeOfHeaders), not just MZ signature.
    // Zeroing only the WORD left the DOS stub, NT signature, section table, and all
    // metadata readable — a 2-byte wipe is cosmetic. Full header wipe matches what
    // production manual-mappers do and closes the plaintext PE fingerprint.
    {
        const DWORD hdr_sz = nt->OptionalHeader.SizeOfHeaders;
        std::vector<BYTE> zero_hdr(hdr_sz, 0);
        DWORD old_wipe_p = 0;
        if (VirtualProtectEx(hProc, base, hdr_sz, PAGE_READWRITE, &old_wipe_p)) {
            WriteProcessMemory(hProc, base, zero_hdr.data(), hdr_sz, nullptr);
            VirtualProtectEx(hProc, base, hdr_sz, PAGE_READONLY, &old_wipe_p);
        }
    }

    // T8: Zero + free stub on success
    std::vector<BYTE> z(kStubSz + sizeof(MmData), 0);
    WriteProcessMemory(hProc, remote_stub, z.data(), z.size(), nullptr);
    stub_mem.Release();

    // P1-[7]: Insert into PEB->Ldr so the mapped region is not a VAD orphan.
    // Spoof name: pick a plausible .minecraft native path that exists on most installs.
    {
        wchar_t appdata[MAX_PATH]{};
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        std::wstring spoof = std::wstring(appdata) + L"\\.minecraft\\natives\\lwjgl_opengl.dll";
        InsertModuleInLdr(hProc, base, nt, spoof);
    }

    status = L"Inyectado OK (SEC_IMAGE + POSIX delete).";
    return true;
}





// Clean production alias
inline bool ManualMapTarget(HANDLE hProc, const uint8_t* buf, size_t sz, std::wstring& status) {
    return ManualMapFileBacked(hProc, buf, sz, status);
}

} // namespace bp
