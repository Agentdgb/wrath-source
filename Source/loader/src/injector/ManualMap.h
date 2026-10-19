#pragma once
#ifndef WRATH_MANUAL_MAP_H
#define WRATH_MANUAL_MAP_H

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <cstdint>

#include "bypass/StealthDefs.h"
#include "VMProtectSDK.h"

namespace wrath::injector
{

using NTSTATUS = wrath::bypass::NTSTATUS;
using pfnNtCreateThreadEx = wrath::bypass::pfnNtCreateThreadEx;
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER wrath::bypass::kThreadCreateFlagsHideFromDebugger

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
    VMProtectBeginUltra("Wrath_Manual_Map");
    if (!dll_bytes || dll_size < sizeof(IMAGE_DOS_HEADER))
    {
        out_status = L"Buffer de payload inválido o vacío.";
        VMProtectEnd();
        return false;
    }

    const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(dll_bytes);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
    {
        out_status = L"Firma DOS de PE inválida en payload descargado.";
        VMProtectEnd();
        return false;
    }

    const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(dll_bytes + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE)
    {
        out_status = L"Firma NT inválida en payload descargado.";
        VMProtectEnd();
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
        VMProtectEnd();
        return false;
    }

    BYTE* target_base = target_mem.Get();

    // 2. Escribir los PE Headers en el proceso remoto
    if (!WriteProcessMemory(hProcess, target_base, dll_bytes, nt_headers->OptionalHeader.SizeOfHeaders, nullptr))
    {
        out_status = L"Fallo escribiendo PE Headers en memoria remota.";
        VMProtectEnd();
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
                VMProtectEnd();
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
        VMProtectEnd();
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
        VMProtectEnd();
        return false;
    }

    // 7. Esperar a que el stub termine de resolver importaciones y DllMain
    const DWORD wait_res = WaitForSingleObject(thread_handle.Get(), 15000);
    thread_handle.Reset();

    if (wait_res != WAIT_OBJECT_0)
    {
        out_status = L"Tiempo de espera agotado o fallo en la ejecución del hilo remoto de inyección.";
        VMProtectEnd();
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
    VMProtectEnd();
    return true;
}

} // namespace wrath::injector

#endif // WRATH_MANUAL_MAP_H
