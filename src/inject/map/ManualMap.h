#pragma once
#ifndef WRATH_MANUAL_MAP_H
#define WRATH_MANUAL_MAP_H

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdint>

#include "shared/stealth/StealthDefs.h"

namespace wrath::injector
{

using NTSTATUS = wrath::bypass::NTSTATUS;
using pfnNtCreateThreadEx = wrath::bypass::pfnNtCreateThreadEx;
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER wrath::bypass::kThreadCreateFlagsHideFromDebugger

// NT API typedefs for TxF-backed SEC_IMAGE injection
using pfnNtCreateSection_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
using pfnNtMapViewOfSection_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
using pfnNtUnmapViewOfSection_t = NTSTATUS(NTAPI*)(HANDLE, PVOID);
using pfnCreateTransaction_t = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD, DWORD, DWORD, LPWSTR);
using pfnRollbackTransaction_t = BOOL(WINAPI*)(HANDLE);

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// Estructura de parámetros que se pasan a la shellcode remota dentro del proceso de Minecraft
struct ManualMappingData
{
    BYTE* base;
    PIMAGE_NT_HEADERS nt_headers;
    PIMAGE_BASE_RELOCATION base_reloc;
    PIMAGE_IMPORT_DESCRIPTOR import_directory;

    using fnLoadLibraryA = HMODULE(WINAPI*)(LPCSTR);
    using fnGetProcAddress = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    using fnDllMain = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
    using fnRtlAddFunctionTable = BOOLEAN(NTAPI*)(PRUNTIME_FUNCTION, DWORD, DWORD64);

    fnLoadLibraryA load_library_a;
    fnGetProcAddress get_proc_address;
    fnRtlAddFunctionTable rtl_add_function_table;
};

#pragma check_stack(off)
#pragma runtime_checks("", off)
// Rutina de bootstrap PIC que se ejecuta DENTRO del proceso de Minecraft
__declspec(noinline) inline void WINAPI RemoteLoaderStub(ManualMappingData* data)
{
    if (!data) return;

    BYTE* location = data->base;
    auto* nt_headers = data->nt_headers;
    auto* reloc = data->base_reloc;
    auto* import_desc = data->import_directory;

    auto load_library_a = data->load_library_a;
    auto get_proc_address = data->get_proc_address;

    // 1. Aplicar Relocations (Base Relocations)
    const uintptr_t delta = reinterpret_cast<uintptr_t>(location) - nt_headers->OptionalHeader.ImageBase;
    if (delta != 0 && reloc)
    {
        while (reloc->VirtualAddress)
        {
            if (reloc->SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION))
            {
                const size_t count = (reloc->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                const WORD* list = reinterpret_cast<const WORD*>(reloc + 1);

                for (size_t i = 0; i < count; ++i)
                {
                    if (list[i])
                    {
                        const WORD type = list[i] >> 12;
                        const WORD offset = list[i] & 0xFFF;
                        if (type == IMAGE_REL_BASED_DIR64)
                        {
                            auto* patch = reinterpret_cast<uintptr_t*>(location + reloc->VirtualAddress + offset);
                            *patch += delta;
                        }
                        else if (type == IMAGE_REL_BASED_HIGHLOW)
                        {
                            auto* patch = reinterpret_cast<DWORD*>(location + reloc->VirtualAddress + offset);
                            *patch += static_cast<DWORD>(delta);
                        }
                    }
                }
            }
            reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(reinterpret_cast<BYTE*>(reloc) + reloc->SizeOfBlock);
        }
    }

    // 2. Resolver Import Address Table (IAT)
    if (import_desc)
    {
        while (import_desc->Characteristics || import_desc->Name)
        {
            const char* szMod = reinterpret_cast<const char*>(location + import_desc->Name);
            HMODULE hMod = load_library_a(szMod);

            if (hMod)
            {
                auto* thunk_ref = reinterpret_cast<PIMAGE_THUNK_DATA>(location + (import_desc->OriginalFirstThunk ? import_desc->OriginalFirstThunk : import_desc->FirstThunk));
                auto* func_ref = reinterpret_cast<PIMAGE_THUNK_DATA>(location + import_desc->FirstThunk);

                while (thunk_ref->u1.AddressOfData)
                {
                    if (IMAGE_SNAP_BY_ORDINAL(thunk_ref->u1.Ordinal))
                    {
                        func_ref->u1.Function = reinterpret_cast<uintptr_t>(get_proc_address(hMod, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk_ref->u1.Ordinal))));
                    }
                    else
                    {
                        auto* import_by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(location + thunk_ref->u1.AddressOfData);
                        func_ref->u1.Function = reinterpret_cast<uintptr_t>(get_proc_address(hMod, import_by_name->Name));
                    }
                    thunk_ref++;
                    func_ref++;
                }
            }
            import_desc++;
        }
    }

    // 3. Registrar tabla de excepciones x64 SEH para C++ (soporte try/catch y stack unwinding)
    if (data->rtl_add_function_table && nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size)
    {
        auto* exc_dir = &nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc_dir->VirtualAddress && exc_dir->Size)
        {
            auto* func_table = reinterpret_cast<PRUNTIME_FUNCTION>(location + exc_dir->VirtualAddress);
            DWORD count = exc_dir->Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
            data->rtl_add_function_table(func_table, count, reinterpret_cast<DWORD64>(location));
        }
    }

    // 4. Ejecutar TLS Callbacks (si existen y son válidos)
    if (nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size)
    {
        auto* tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(location + nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        if (tls && tls->AddressOfCallBacks)
        {
            auto* callback = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
            while (callback && *callback)
            {
                (*callback)(location, DLL_PROCESS_ATTACH, nullptr);
                callback++;
            }
        }
    }

    // 5. Invocar DllMain(DLL_PROCESS_ATTACH)
    if (nt_headers->OptionalHeader.AddressOfEntryPoint)
    {
        auto entry_point = reinterpret_cast<ManualMappingData::fnDllMain>(location + nt_headers->OptionalHeader.AddressOfEntryPoint);
        entry_point(reinterpret_cast<HINSTANCE>(location), DLL_PROCESS_ATTACH, nullptr);
    }
}

// ─────────────────────────────────────────────────────────────────────────────

// System32 DLLs used as TxF transaction targets.
// Small on-disk, never imported by JVM, not on screenshare watch-lists.
static constexpr const wchar_t* kTxfSacrifices[] = {
    L"winmm.dll",
    L"mpr.dll",
    L"ksuser.dll",
    L"version.dll",
    L"winspool.drv",
};

// Minimal remote stub for TxF path: relocs + IAT already resolved by loader.
// Only handles RtlAddFunctionTable, TLS callbacks, and DllMain.
#pragma check_stack(off)
#pragma runtime_checks("", off)
__declspec(noinline) inline void WINAPI TxfLoaderStub(ManualMappingData* data)
{
    if (!data) return;
    BYTE* base = data->base;
    auto* nt   = data->nt_headers;
    if (data->rtl_add_function_table &&
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size)
    {
        auto* exc = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc->VirtualAddress && exc->Size) {
            auto* tbl = reinterpret_cast<PRUNTIME_FUNCTION>(base + exc->VirtualAddress);
            DWORD cnt = exc->Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
            data->rtl_add_function_table(tbl, cnt, reinterpret_cast<DWORD64>(base));
        }
    }
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
        auto* tls = reinterpret_cast<PIMAGE_TLS_DIRECTORY>(
            base + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
        if (tls && tls->AddressOfCallBacks) {
            auto* cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
            while (cb && *cb) { (*cb)(base, DLL_PROCESS_ATTACH, nullptr); cb++; }
        }
    }
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        auto ep = reinterpret_cast<ManualMappingData::fnDllMain>(
            base + nt->OptionalHeader.AddressOfEntryPoint);
        ep(reinterpret_cast<HINSTANCE>(base), DLL_PROCESS_ATTACH, nullptr);
    }
}
#pragma runtime_checks("", restore)
#pragma check_stack()
// C++20 RAII Resource Guards para Memoria Remota y Descriptores del Sistema
// ─────────────────────────────────────────────────────────────────────────────
class ScopedRemoteMemory
{
public:
    ScopedRemoteMemory(HANDLE process, LPVOID base_addr, size_t size, DWORD alloc_type, DWORD protect)
        : m_process(process)
    {
        m_address = reinterpret_cast<BYTE*>(VirtualAllocEx(process, base_addr, size, alloc_type, protect));
    }

    ~ScopedRemoteMemory()
    {
        Release();
    }

    ScopedRemoteMemory(const ScopedRemoteMemory&) = delete;
    ScopedRemoteMemory& operator=(const ScopedRemoteMemory&) = delete;

    ScopedRemoteMemory(ScopedRemoteMemory&& other) noexcept
        : m_process(other.m_process), m_address(other.m_address)
    {
        other.m_address = nullptr;
    }

    ScopedRemoteMemory& operator=(ScopedRemoteMemory&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_process = other.m_process;
            m_address = other.m_address;
            other.m_address = nullptr;
        }
        return *this;
    }

    [[nodiscard]] BYTE* Get() const noexcept { return m_address; }
    [[nodiscard]] bool IsValid() const noexcept { return m_address != nullptr; }

    BYTE* Detach() noexcept
    {
        BYTE* temp = m_address;
        m_address = nullptr;
        return temp;
    }

    void Release() noexcept
    {
        if (m_address && m_process)
        {
            VirtualFreeEx(m_process, m_address, 0, MEM_RELEASE);
            m_address = nullptr;
        }
    }

private:
    HANDLE m_process = nullptr;
    BYTE* m_address = nullptr;
};

class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : m_handle(h) {}
    ~ScopedHandle() noexcept { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = nullptr; }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    [[nodiscard]] HANDLE Get() const noexcept { return m_handle; }
    [[nodiscard]] bool IsValid() const noexcept { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }

    void Reset(HANDLE new_handle = nullptr) noexcept
    {
        if (IsValid())
        {
            CloseHandle(m_handle);
        }
        m_handle = new_handle;
    }

private:
    HANDLE m_handle = nullptr;
};

// Inyección por Manual Mapping directo desde un buffer en memoria RAM
inline bool ManualMapInMemory(HANDLE hProcess, const uint8_t* dll_bytes, size_t dll_size, std::wstring& out_status)
{
    if (!dll_bytes || dll_size < sizeof(IMAGE_DOS_HEADER))
    {
        out_status = L"Buffer de payload inválido o vacío.";
        return false;
    }

    const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_bytes);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
    {
        out_status = L"Firma DOS de PE inválida en payload descargado.";
        return false;
    }

    const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(dll_bytes + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
    {
        out_status = L"Firma NT inválida en payload descargado.";
        return false;
    }

    // 1. Reservar memoria remota protegida con RAII
    ScopedRemoteMemory target_mem(
        hProcess,
        reinterpret_cast<LPVOID>(nt_headers->OptionalHeader.ImageBase),
        nt_headers->OptionalHeader.SizeOfImage,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE);

    if (!target_mem.IsValid())
    {
        target_mem = ScopedRemoteMemory(
            hProcess,
            nullptr,
            nt_headers->OptionalHeader.SizeOfImage,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
    }

    if (!target_mem.IsValid())
    {
        out_status = L"Fallo al reservar memoria remota (SizeOfImage) en Minecraft.";
        return false;
    }

    BYTE* target_base = target_mem.Get();

    // 2. Escribir los PE Headers en el proceso remoto
    if (!WriteProcessMemory(hProcess, target_base, dll_bytes, nt_headers->OptionalHeader.SizeOfHeaders, nullptr))
    {
        out_status = L"Fallo escribiendo PE Headers en memoria remota.";
        return false;
    }

    // 3. Mapear cada sección (.text, .rdata, .data, etc.)
    const auto* section = IMAGE_FIRST_SECTION(nt_headers);
    for (UINT i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (section->SizeOfRawData)
        {
            if (!WriteProcessMemory(
                    hProcess,
                    target_base + section->VirtualAddress,
                    dll_bytes + section->PointerToRawData,
                    section->SizeOfRawData,
                    nullptr))
            {
                out_status = L"Fallo escribiendo secciones del DLL en memoria remota.";
                return false;
            }
        }
    }

    // 4. Preparar la estructura de datos para la shellcode
    ManualMappingData mapping_data{};
    mapping_data.base = target_base;
    mapping_data.nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(target_base + dos_header->e_lfanew);

    if (nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size)
    {
        mapping_data.base_reloc = reinterpret_cast<PIMAGE_BASE_RELOCATION>(
            target_base + nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
    }
    if (nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
    {
        mapping_data.import_directory = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(
            target_base + nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    mapping_data.load_library_a = reinterpret_cast<ManualMappingData::fnLoadLibraryA>(GetProcAddress(hKernel32, "LoadLibraryA"));
    mapping_data.get_proc_address = reinterpret_cast<ManualMappingData::fnGetProcAddress>(GetProcAddress(hKernel32, "GetProcAddress"));
    mapping_data.rtl_add_function_table = reinterpret_cast<ManualMappingData::fnRtlAddFunctionTable>(GetProcAddress(hKernel32, "RtlAddFunctionTable"));

    // 5. Reservar memoria remota para la estructura y la función bootstrap stub (RAII)
    const size_t stub_size = 0x1000;
    ScopedRemoteMemory stub_mem(
        hProcess, nullptr, sizeof(ManualMappingData) + stub_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

    if (!stub_mem.IsValid())
    {
        out_status = L"Fallo al alojar memoria para shellcode de bootstrap.";
        return false;
    }

    BYTE* remote_stub = stub_mem.Get();
    BYTE* remote_data = remote_stub + stub_size;
    WriteProcessMemory(hProcess, remote_data, &mapping_data, sizeof(ManualMappingData), nullptr);
    WriteProcessMemory(hProcess, remote_stub, reinterpret_cast<LPCVOID>(&RemoteLoaderStub), stub_size, nullptr);

    // 6. Ejecutar RemoteLoaderStub con NtCreateThreadEx protegido con ScopedHandle RAII
    ScopedHandle thread_handle;
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll)
    {
        auto NtCreateThreadEx = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(hNtdll, "NtCreateThreadEx"));
        if (NtCreateThreadEx)
        {
            HANDLE raw_th = nullptr;
            NTSTATUS status = NtCreateThreadEx(
                &raw_th,
                SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                nullptr,
                hProcess,
                reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
                remote_data,
                THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
                0, 0, 0, nullptr);
            if (NT_SUCCESS(status) && raw_th)
            {
                thread_handle.Reset(raw_th);
            }
        }
    }

    if (!thread_handle.IsValid())
    {
        thread_handle.Reset(CreateRemoteThread(hProcess, nullptr, 0,
                                               reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
                                               remote_data, 0, nullptr));
    }

    if (!thread_handle.IsValid())
    {
        out_status = L"Fallo al crear hilo remoto para Manual Map.";
        return false;
    }

    // 7. Esperar a que el stub termine de resolver importaciones y DllMain
    const DWORD wait_res = WaitForSingleObject(thread_handle.Get(), 15000);
    thread_handle.Reset();

    if (wait_res != WAIT_OBJECT_0)
    {
        out_status = L"Tiempo de espera agotado o fallo en la ejecución del hilo remoto de inyección.";
        return false;
    }

    // 8. Restaurar permisos legítimos a cada sección PE (Eliminar huella de páginas RWX completa)
    const auto* sec_protect = IMAGE_FIRST_SECTION(nt_headers);
    for (UINT i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i, ++sec_protect)
    {
        const DWORD section_size = (std::max)(sec_protect->SizeOfRawData, sec_protect->Misc.VirtualSize);
        if (section_size > 0)
        {
            DWORD old_prot = 0;
            DWORD new_prot = PAGE_READONLY;
            const bool r = (sec_protect->Characteristics & IMAGE_SCN_MEM_READ) != 0;
            const bool w = (sec_protect->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
            const bool x = (sec_protect->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;

            if (x)
                new_prot = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
            else
                new_prot = w ? PAGE_READWRITE : (r ? PAGE_READONLY : PAGE_NOACCESS);

            VirtualProtectEx(hProcess, target_base + sec_protect->VirtualAddress, section_size, new_prot, &old_prot);
        }
    }

    // 9. Proteger los PE Headers en el proceso remoto (Solo lectura para permitir lectura SEH del CRT)
    DWORD old_hdr_prot = 0;
    VirtualProtectEx(hProcess, target_base, nt_headers->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old_hdr_prot);

    // 10. Limpiar la shellcode de bootstrap y liberarla
    std::vector<BYTE> zeroes(stub_size + sizeof(ManualMappingData), 0);
    WriteProcessMemory(hProcess, remote_stub, zeroes.data(), zeroes.size(), nullptr);
    stub_mem.Release();

    // 11. Éxito: Desvincular el módulo principal de RAII para que persista en el juego
    target_mem.Detach();

    out_status = L"Cliente Wrath inyectado exitosamente en memoria (RAM) sin tocar disco.";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread-hijack execution helpers (used by ManualMapViaTxfSection)
// ─────────────────────────────────────────────────────────────────────────────

// Builds a ≤64-byte x64 wrapper shellcode at |buf|.
// Emits: sub rsp,28h / xor ecx,ecx / mov rax,stub / call rax / add rsp,28h
//        / mov byte[rip+disp32],1 (sets flag_slot) / ret
// rcx=null causes TxfLoaderStub to exit immediately; real invocation happens via
// the NtCreateThreadEx/CreateRemoteThread fallback that passes remote_data correctly.
inline size_t BuildHijackWrapper(BYTE* buf, BYTE* /*wrapper_slot*/,
                                  BYTE* stub, BYTE* flag_slot)
{
    BYTE* p = buf;
    // sub rsp, 0x28
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x28;
    // xor ecx, ecx
    *p++ = 0x31; *p++ = 0xC9;
    // mov rax, imm64
    *p++ = 0x48; *p++ = 0xB8;
    memcpy(p, &stub, 8); p += 8;
    // call rax
    *p++ = 0xFF; *p++ = 0xD0;
    // add rsp, 0x28
    *p++ = 0x48; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x28;
    // mov byte ptr [rip+disp32], 1
    *p++ = 0xC6; *p++ = 0x05;
    {
        BYTE* next_instr = p + 4 + 1 + 1; // disp(4) + imm8(1) + ret(1)
        int32_t disp = static_cast<int32_t>(flag_slot - next_instr);
        memcpy(p, &disp, 4); p += 4;
        *p++ = 0x01;
    }
    // ret
    *p++ = 0xC3;
    return static_cast<size_t>(p - buf);
}

// Hijacks the first suspendable thread in |pid| to run |wrapper_slot|.
// Polls |flag_slot| (remote BYTE) up to 3 s; returns true when it turns non-zero.
// Returns false on any failure → caller falls back to NtCreateThreadEx.
inline bool HijackExec(HANDLE hProcess, DWORD pid,
                        BYTE* wrapper_slot, BYTE* /*remote_data*/, BYTE* flag_slot)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    HANDLE hThread = nullptr;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(hSnap, &te))
    {
        do {
            if (te.th32OwnerProcessID == pid)
            {
                HANDLE ht = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                    THREAD_SET_CONTEXT | SYNCHRONIZE,
                    FALSE, te.th32ThreadID);
                if (ht && ht != INVALID_HANDLE_VALUE)
                { hThread = ht; break; }
            }
        } while (Thread32Next(hSnap, &te));
    }
    CloseHandle(hSnap);
    if (!hThread) return false;

    if (SuspendThread(hThread) == (DWORD)-1)
    { CloseHandle(hThread); return false; }

    CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(hThread, &ctx))
    { ResumeThread(hThread); CloseHandle(hThread); return false; }

    const DWORD64 saved_rip = ctx.Rip;
    ctx.Rip = reinterpret_cast<DWORD64>(wrapper_slot);
    if (!SetThreadContext(hThread, &ctx))
    { ResumeThread(hThread); CloseHandle(hThread); return false; }
    ResumeThread(hThread);

    bool completed  = false;
    BYTE flag_val   = 0;
    const DWORD deadline = GetTickCount() + 3000;
    while (GetTickCount() < deadline)
    {
        SIZE_T rd = 0;
        ReadProcessMemory(hProcess, flag_slot, &flag_val, 1, &rd);
        if (rd == 1 && flag_val) { completed = true; break; }
        Sleep(5);
    }

    if (!completed && SuspendThread(hThread) != (DWORD)-1)
    {
        CONTEXT ctx2{}; ctx2.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(hThread, &ctx2))
        { ctx2.Rip = saved_rip; SetThreadContext(hThread, &ctx2); }
        ResumeThread(hThread);
    }

    CloseHandle(hThread);
    return completed;
}



// ─────────────────────────────────────────────────────────────────────────────
// Module Stomp path (Priority 2 fallback — size-gated, returns false when no
// candidate DLL has a .text section >= payload SizeOfImage).
// For WrathPayload.dll (SizeOfImage ~4.80 MB) this path will always fast-fail.
// Kept for forward-compatibility if a larger candidate is added later.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const wchar_t* kStompCandidates[] = {
    L"d3dcompiler_47.dll",
    L"d3d11.dll",
    L"dxgi.dll",
    L"opengl32.dll",
    nullptr
};

// Resolves the in-process .text section size of a loaded module.
// Returns 0 if the module is not loaded or has no executable section.
inline size_t GetTextSectionSize(HANDLE hProcess, const wchar_t* mod_name)
{
    HMODULE h = GetModuleHandleW(mod_name);
    if (!h) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(h);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt  = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const BYTE*>(h) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) &&
            (sec->Characteristics & IMAGE_SCN_MEM_READ))
        {
            return static_cast<size_t>(sec->Misc.VirtualSize
                ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        }
    }
    return 0;
}

// Attempts to stomp a candidate DLL that is already mapped in the target process.
// Fails immediately (returns false) when no candidate's .text is large enough.
inline bool ManualMapInMemoryWithStomp(
    HANDLE hProcess, DWORD pid,
    const uint8_t* dll_bytes, size_t dll_size,
    std::wstring& out_status)
{
    if (!dll_bytes || dll_size < sizeof(IMAGE_DOS_HEADER))
    { out_status = L"Stomp: buffer invalido."; return false; }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    { out_status = L"Stomp: firma DOS invalida."; return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(dll_bytes + dos->e_lfanew);
    const size_t payload_image_sz = nt->OptionalHeader.SizeOfImage;

    // Walk candidates; need a mapped module with .text >= payload image size.
    HMODULE hCandidate = nullptr;
    for (int i = 0; kStompCandidates[i]; ++i)
    {
        const size_t ts = GetTextSectionSize(hProcess, kStompCandidates[i]);
        if (ts >= payload_image_sz + 0x1000)
        {
            hCandidate = GetModuleHandleW(kStompCandidates[i]);
            if (hCandidate) break;
        }
    }

    if (!hCandidate)
    {
        out_status = L"Stomp: ningún candidato con sección .text suficientemente grande. Payload="
                   + std::to_wstring(payload_image_sz / 1024) + L" KB.";
        return false;
    }

    // Candidate found: overwrite its mapping with the payload image.
    // Lift protection on the candidate's full SizeOfImage range, WPM in payload.
    BYTE* remote_base = reinterpret_cast<BYTE*>(hCandidate);
    DWORD old_prot = 0;
    if (!VirtualProtectEx(hProcess, remote_base, payload_image_sz,
                          PAGE_EXECUTE_READWRITE, &old_prot))
    { out_status = L"Stomp: fallo VirtualProtectEx."; return false; }

    // Copy sections.
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (sec->SizeOfRawData && sec->VirtualAddress)
        {
            WriteProcessMemory(hProcess, remote_base + sec->VirtualAddress,
                dll_bytes + sec->PointerToRawData,
                sec->SizeOfRawData, nullptr);
        }
    }

    // Restore section protections.
    const auto* sp = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sp)
    {
        const DWORD sz = sp->Misc.VirtualSize ? sp->Misc.VirtualSize : sp->SizeOfRawData;
        if (!sz) continue;
        const bool r = (sp->Characteristics & IMAGE_SCN_MEM_READ)    != 0;
        const bool w = (sp->Characteristics & IMAGE_SCN_MEM_WRITE)   != 0;
        const bool x = (sp->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        DWORD new_prot = x ? (w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE))
                           :     (w ? PAGE_READWRITE      : (r ? PAGE_READONLY     : PAGE_NOACCESS));
        DWORD tmp = 0;
        VirtualProtectEx(hProcess, remote_base + sp->VirtualAddress, sz, new_prot, &tmp);
    }

    // Execute via NtCreateThreadEx / CreateRemoteThread at the payload entry point.
    ManualMappingData mdata{};
    mdata.base        = remote_base;
    mdata.nt_headers  = reinterpret_cast<PIMAGE_NT_HEADERS>(remote_base + dos->e_lfanew);
    mdata.base_reloc  = nullptr; // relocs assumed resolved by caller or zero-based image
    mdata.import_directory = nullptr;
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    mdata.load_library_a   = reinterpret_cast<ManualMappingData::fnLoadLibraryA>(
        GetProcAddress(hK32, "LoadLibraryA"));
    mdata.get_proc_address = reinterpret_cast<ManualMappingData::fnGetProcAddress>(
        GetProcAddress(hK32, "GetProcAddress"));
    mdata.rtl_add_function_table = reinterpret_cast<ManualMappingData::fnRtlAddFunctionTable>(
        GetProcAddress(hK32, "RtlAddFunctionTable"));

    // Allocate remote scratch for the RemoteLoaderStub + mdata.
    const size_t stub_sz = 0x800;
    ScopedRemoteMemory scratch(hProcess, nullptr, stub_sz + sizeof(ManualMappingData),
                               MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!scratch.Get())
    { out_status = L"Stomp: fallo alocando scratch."; return false; }

    BYTE* remote_stub = scratch.Get();
    BYTE* remote_data = remote_stub + stub_sz;
    WriteProcessMemory(hProcess, remote_stub,
        reinterpret_cast<LPCVOID>(&RemoteLoaderStub), stub_sz, nullptr);
    WriteProcessMemory(hProcess, remote_data, &mdata, sizeof(ManualMappingData), nullptr);

    ScopedHandle th;
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    auto NtCTEx = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(hNtdll, "NtCreateThreadEx"));
    if (NtCTEx) {
        HANDLE raw = nullptr;
        NTSTATUS s = NtCTEx(&raw, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
            nullptr, hProcess,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_data, THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER, 0, 0, 0, nullptr);
        if (NT_SUCCESS(s) && raw) th.Reset(raw);
    }
    if (!th.IsValid())
        th.Reset(CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub), remote_data, 0, nullptr));
    if (!th.IsValid())
    { out_status = L"Stomp: fallo creando hilo remoto."; return false; }

    if (WaitForSingleObject(th.Get(), 15000) != WAIT_OBJECT_0)
    { out_status = L"Stomp: timeout hilo remoto."; return false; }

    scratch.Release();
    out_status = L"Cliente Wrath inyectado (Module Stomp, MEM_IMAGE).";
    return true;
}

// TxF-backed SEC_IMAGE injection.
// Writes payload into a transacted NTFS view of a System32 sacrifice DLL.
// NtCreateSection(SEC_IMAGE, txf_handle) -> VAD Type = MEM_IMAGE.
// Relocs + IAT resolved from loader via WriteProcessMemory (CoW, VAD stays MEM_IMAGE).
// Transaction rolled back at end — disk never modified.
inline bool ManualMapViaTxfSection(
    HANDLE hProcess, DWORD pid,
    const uint8_t* dll_bytes, size_t dll_size,
    std::wstring& out_status)
{
    if (!dll_bytes || dll_size < sizeof(IMAGE_DOS_HEADER)) {
        out_status = L"TxF: buffer invalido."; return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        out_status = L"TxF: firma DOS invalida."; return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(dll_bytes + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        out_status = L"TxF: firma NT invalida."; return false;
    }
    const WORD  n_sections = nt->FileHeader.NumberOfSections;
    const auto* sections   = IMAGE_FIRST_SECTION(nt);
    // RVA -> pointer inside raw dll_bytes
    auto rva_to_raw = [&](DWORD rva) -> const BYTE* {
        for (WORD i = 0; i < n_sections; ++i) {
            const auto& s = sections[i];
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + s.SizeOfRawData)
                return dll_bytes + s.PointerToRawData + (rva - s.VirtualAddress);
        }
        return nullptr;
    };
    // NT APIs
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) { out_status = L"TxF: ntdll no resolvible."; return false; }
    auto NtCreateSection =
        reinterpret_cast<pfnNtCreateSection_t>(GetProcAddress(hNtdll, "NtCreateSection"));
    auto NtMapViewOfSection =
        reinterpret_cast<pfnNtMapViewOfSection_t>(GetProcAddress(hNtdll, "NtMapViewOfSection"));
    auto NtUnmapViewOfSection =
        reinterpret_cast<pfnNtUnmapViewOfSection_t>(GetProcAddress(hNtdll, "NtUnmapViewOfSection"));
    if (!NtCreateSection || !NtMapViewOfSection || !NtUnmapViewOfSection) {
        out_status = L"TxF: APIs NT de seccion no resolvibles."; return false;
    }
    HMODULE hKtm = LoadLibraryW(L"KtmW32.dll");
    if (!hKtm) { out_status = L"TxF: KtmW32.dll no disponible."; return false; }
    auto pCreateTransaction   = reinterpret_cast<pfnCreateTransaction_t>  (GetProcAddress(hKtm, "CreateTransaction"));
    auto pRollbackTransaction = reinterpret_cast<pfnRollbackTransaction_t>(GetProcAddress(hKtm, "RollbackTransaction"));
    if (!pCreateTransaction || !pRollbackTransaction) {
        FreeLibrary(hKtm); out_status = L"TxF: exports KtmW32 no resolvibles."; return false;
    }
    // Try sacrifice DLLs
    HANDLE hTx = INVALID_HANDLE_VALUE, hTxFile = INVALID_HANDLE_VALUE, hSection = nullptr;
    wchar_t sys_dir[MAX_PATH]{};
    GetSystemDirectoryW(sys_dir, MAX_PATH);
    for (auto* sacrifice : kTxfSacrifices) {
        wchar_t full_path[MAX_PATH]{};
        swprintf_s(full_path, L"%s\\%s", sys_dir, sacrifice);
        HANDLE tx = pCreateTransaction(nullptr, nullptr, 0, 0, 0, 0, nullptr);
        if (tx == INVALID_HANDLE_VALUE) continue;
        HANDLE f = CreateFileTransactedW(full_path,
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr, tx, nullptr, nullptr);
        if (f == INVALID_HANDLE_VALUE) { pRollbackTransaction(tx); CloseHandle(tx); continue; }
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        DWORD written = 0;
        if (!WriteFile(f, dll_bytes, static_cast<DWORD>(dll_size), &written, nullptr)
            || written != static_cast<DWORD>(dll_size))
        { CloseHandle(f); pRollbackTransaction(tx); CloseHandle(tx); continue; }
        LARGE_INTEGER end_pos; end_pos.QuadPart = static_cast<LONGLONG>(dll_size);
        SetFilePointerEx(f, end_pos, nullptr, FILE_BEGIN);
        SetEndOfFile(f);
        FlushFileBuffers(f);
        HANDLE sec = nullptr;
        NTSTATUS st = NtCreateSection(&sec, SECTION_ALL_ACCESS, nullptr, nullptr,
            PAGE_READONLY, SEC_IMAGE, f);
        if (!NT_SUCCESS(st) || !sec) { CloseHandle(f); pRollbackTransaction(tx); CloseHandle(tx); continue; }
        hTx = tx; hTxFile = f; hSection = sec; break;
    }
    if (!hSection) {
        FreeLibrary(hKtm);
        out_status = L"TxF: ningun candidato pudo generar seccion SEC_IMAGE.";
        return false;
    }
    // Map locally to get preferred base (then unmap)
    PVOID local_base = nullptr; SIZE_T local_size = 0;
    NtMapViewOfSection(hSection, GetCurrentProcess(), &local_base, 0, 0,
        nullptr, &local_size, 1, 0, PAGE_READONLY);
    if (local_base) NtUnmapViewOfSection(GetCurrentProcess(), local_base);
    // Map into target process
    PVOID  remote_base = nullptr; SIZE_T remote_size = 0;
    NTSTATUS st_r = NtMapViewOfSection(hSection, hProcess, &remote_base,
        0, 0, nullptr, &remote_size, 1, 0, PAGE_EXECUTE_READ);
    if (!NT_SUCCESS(st_r) || !remote_base) {
        CloseHandle(hSection); CloseHandle(hTxFile);
        pRollbackTransaction(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        out_status = L"TxF: NtMapViewOfSection (remote) fallo."; return false;
    }
    // Apply base relocations from loader side (WPM bypasses page protection; CoW, VAD stays MEM_IMAGE)
    const uintptr_t actual_base    = reinterpret_cast<uintptr_t>(remote_base);
    const intptr_t  delta          = static_cast<intptr_t>(actual_base - static_cast<uintptr_t>(nt->OptionalHeader.ImageBase));
    if (delta != 0 && nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
        const DWORD reloc_rva  = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
        const DWORD reloc_size = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
        const BYTE* reloc_raw  = rva_to_raw(reloc_rva);
        if (reloc_raw) {
            const BYTE* reloc_end = reloc_raw + reloc_size;
            const auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reloc_raw);
            while (reinterpret_cast<const BYTE*>(block) < reloc_end && block->VirtualAddress) {
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                const DWORD entry_count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                const WORD* entries     = reinterpret_cast<const WORD*>(block + 1);
                for (DWORD i = 0; i < entry_count; ++i) {
                    const WORD type      = entries[i] >> 12;
                    const DWORD entry_rva = block->VirtualAddress + (entries[i] & 0xFFF);
                    if (type == IMAGE_REL_BASED_DIR64) {
                        const BYTE* rp = rva_to_raw(entry_rva); if (!rp) continue;
                        DWORD64 ov; memcpy(&ov, rp, 8);
                        DWORD64 nv = ov + static_cast<DWORD64>(delta);
                        WriteProcessMemory(hProcess,
                            reinterpret_cast<BYTE*>(remote_base) + entry_rva, &nv, 8, nullptr);
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        const BYTE* rp = rva_to_raw(entry_rva); if (!rp) continue;
                        DWORD ov; memcpy(&ov, rp, 4);
                        DWORD nv = ov + static_cast<DWORD>(delta);
                        WriteProcessMemory(hProcess,
                            reinterpret_cast<BYTE*>(remote_base) + entry_rva, &nv, 4, nullptr);
                    }
                }
                block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<const BYTE*>(block) + block->SizeOfBlock);
            }
        }
    }
    // Resolve IAT from loader side (session-wide ASLR: local GetProcAddress == remote VA)
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
        const BYTE* import_raw = rva_to_raw(
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
        if (import_raw) {
            const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(import_raw);
            while (desc->Characteristics || desc->Name) {
                const char* mod_name = reinterpret_cast<const char*>(rva_to_raw(desc->Name));
                if (!mod_name) { desc++; continue; }
                HMODULE hMod = GetModuleHandleA(mod_name);
                if (!hMod) hMod = LoadLibraryA(mod_name);
                if (!hMod) { desc++; continue; }
                const DWORD ot_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                const BYTE* ot_raw = rva_to_raw(ot_rva);
                if (!ot_raw) { desc++; continue; }
                const auto* orig = reinterpret_cast<const IMAGE_THUNK_DATA*>(ot_raw);
                DWORD iat_off = desc->FirstThunk;
                while (orig->u1.AddressOfData) {
                    FARPROC fn = nullptr;
                    if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal))
                        fn = GetProcAddress(hMod, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(orig->u1.Ordinal)));
                    else {
                        const BYTE* ibn = rva_to_raw(static_cast<DWORD>(orig->u1.AddressOfData));
                        if (ibn) fn = GetProcAddress(hMod,
                            reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(ibn)->Name);
                    }
                    if (fn) {
                        const uintptr_t va = reinterpret_cast<uintptr_t>(fn);
                        WriteProcessMemory(hProcess,
                            reinterpret_cast<BYTE*>(remote_base) + iat_off, &va, sizeof(va), nullptr);
                    }
                    orig++; iat_off += sizeof(IMAGE_THUNK_DATA);
                }
                desc++;
            }
        }
    }
    // Minimal scratch page for TxfLoaderStub + ManualMappingData + wrapper + flag
    const size_t stub_sz    = 0x800;
    const size_t scratch_sz = stub_sz + sizeof(ManualMappingData) + 64 + 4;
    ScopedRemoteMemory scratch(hProcess, nullptr,
        scratch_sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!scratch.IsValid()) {
        NtUnmapViewOfSection(hProcess, remote_base);
        CloseHandle(hSection); CloseHandle(hTxFile);
        pRollbackTransaction(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        out_status = L"TxF: fallo alocando scratch."; return false;
    }
    BYTE* remote_stub  = scratch.Get();
    BYTE* remote_data  = remote_stub  + stub_sz;
    BYTE* wrapper_slot = remote_data  + sizeof(ManualMappingData);
    BYTE* flag_slot    = wrapper_slot + 64;
    ManualMappingData mdata{};
    mdata.base       = reinterpret_cast<BYTE*>(remote_base);
    mdata.nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(
        reinterpret_cast<BYTE*>(remote_base) + dos->e_lfanew);
    mdata.base_reloc       = nullptr;  // already resolved
    mdata.import_directory = nullptr;  // already resolved
    mdata.load_library_a   = nullptr;
    mdata.get_proc_address = nullptr;
    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    mdata.rtl_add_function_table = reinterpret_cast<ManualMappingData::fnRtlAddFunctionTable>(
        GetProcAddress(hK32, "RtlAddFunctionTable"));
    WriteProcessMemory(hProcess, remote_stub,
        reinterpret_cast<LPCVOID>(&TxfLoaderStub), stub_sz, nullptr);
    WriteProcessMemory(hProcess, remote_data, &mdata, sizeof(ManualMappingData), nullptr);
    // Execute TxfLoaderStub via NtCreateThreadEx (THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER)
    ScopedHandle th;
    auto NtCTEx = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(hNtdll, "NtCreateThreadEx"));
    if (NtCTEx) {
        HANDLE raw = nullptr;
        NTSTATUS s2 = NtCTEx(&raw, SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
            nullptr, hProcess,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub),
            remote_data, THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER, 0, 0, 0, nullptr);
        if (NT_SUCCESS(s2) && raw) th.Reset(raw);
    }
    if (!th.IsValid())
        th.Reset(CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_stub), remote_data, 0, nullptr));
    if (!th.IsValid()) {
        scratch.Release(); NtUnmapViewOfSection(hProcess, remote_base);
        CloseHandle(hSection); CloseHandle(hTxFile);
        pRollbackTransaction(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        out_status = L"TxF: fallo creando hilo remoto."; return false;
    }
    DWORD wr = WaitForSingleObject(th.Get(), 15000);
    if (wr != WAIT_OBJECT_0) {
        scratch.Release(); NtUnmapViewOfSection(hProcess, remote_base);
        CloseHandle(hSection); CloseHandle(hTxFile);
        pRollbackTransaction(hTx); CloseHandle(hTx); FreeLibrary(hKtm);
        out_status = L"TxF: timeout hilo remoto."; return false;
    }
    // Wipe scratch page and free (zero MEM_PRIVATE executable pages remain)
    { std::vector<BYTE> z(scratch_sz, 0);
      WriteProcessMemory(hProcess, remote_stub, z.data(), z.size(), nullptr); }
    scratch.Release();
    // Rollback transaction: disk reverts, mappings persist
    pRollbackTransaction(hTx);
    CloseHandle(hSection); CloseHandle(hTxFile); CloseHandle(hTx);
    FreeLibrary(hKtm);
    out_status = L"Cliente Wrath inyectado (TxF SEC_IMAGE, MEM_IMAGE, cero paginas privadas ejecutables).";
    return true;
}
} // namespace wrath::injector

#endif // WRATH_MANUAL_MAP_H
