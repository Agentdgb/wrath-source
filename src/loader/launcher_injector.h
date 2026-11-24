#pragma once
#ifndef LAUNCHER_INJECTOR_H
#define LAUNCHER_INJECTOR_H

#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>

#include "loader/Paths.h"`n#include "shared/stealth/StealthDefs.h"
#include "inject/map/ManualMap.h"
#include "inject/NtInjector.h"
#include "shared/diagnostics/SessionDiagnostics.h"

namespace launcher
{

inline ULONG_PTR FindRemoteModuleBase(HANDLE process, const wchar_t* module_name)
{
    HMODULE psapi = LoadLibraryW(L"psapi.dll");
    if (!psapi)
        return 0;

    ULONG_PTR result = 0;
    using EnumModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
    using ModuleNameFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);
    auto enum_modules = reinterpret_cast<EnumModulesFn>(GetProcAddress(psapi, "EnumProcessModulesEx"));
    auto module_name_fn = reinterpret_cast<ModuleNameFn>(GetProcAddress(psapi, "GetModuleBaseNameW"));

    if (enum_modules && module_name_fn)
    {
        DWORD needed = 0;
        enum_modules(process, nullptr, 0, &needed, 3 /* LIST_MODULES_ALL */);
        if (needed > 0)
        {
            std::vector<HMODULE> modules(needed / sizeof(HMODULE));
            if (enum_modules(process, modules.data(), needed, &needed, 3))
            {
                for (HMODULE module : modules)
                {
                    wchar_t name[MAX_PATH]{};
                    if (module_name_fn(process, module, name, MAX_PATH) > 0 &&
                        _wcsicmp(name, module_name) == 0)
                    {
                        result = reinterpret_cast<ULONG_PTR>(module);
                        break;
                    }
                }
            }
        }
    }

    FreeLibrary(psapi);
    return result;
}

inline ULONG_PTR FindRemoteModuleBaseWithRetry(HANDLE process, DWORD pid, const wchar_t* module_name)
{
    if (!module_name || !*module_name) return 0;
    for (int retry = 0; retry < 10; ++retry)
    {
        ULONG_PTR base = FindRemoteModuleBase(process, module_name);
        if (base != 0)
            return base;

        if (pid != 0)
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap != INVALID_HANDLE_VALUE)
            {
                MODULEENTRY32W me{};
                me.dwSize = sizeof(me);
                if (Module32FirstW(snap, &me))
                {
                    do
                    {
                        if (_wcsicmp(me.szModule, module_name) == 0)
                        {
                            base = reinterpret_cast<ULONG_PTR>(me.modBaseAddr);
                            break;
                        }
                    } while (Module32NextW(snap, &me));
                }
                CloseHandle(snap);
            }
            if (base != 0)
                return base;
        }

        Sleep(25);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// NT System Call Definitions & Shared Stealth Primitives (Source/bypass)
// -----------------------------------------------------------------------------
using NTSTATUS = wrath::bypass::NTSTATUS;
using pfnNtCreateThreadEx = wrath::bypass::pfnNtCreateThreadEx;
#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER wrath::bypass::kThreadCreateFlagsHideFromDebugger

/**
 * Elevates the current process token to SE_DEBUG_NAME.
 * Guarantees OpenProcess never fails with ERROR_ACCESS_DENIED against elevated JVMs.
 */
inline bool EnableDebugPrivilege()
{
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid{};
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid))
    {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(token);
    return true;
}

struct MinecraftProcess
{
    DWORD pid = 0;
    std::wstring exe_name;
    std::wstring window_title;
    bool is_injected = false;
};

inline std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string s(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), size, nullptr, nullptr);
    return s;
}

struct WindowSearchContext
{
    std::vector<MinecraftProcess>* list = nullptr;
};

inline BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    WindowSearchContext* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
    if (!ctx || !ctx->list) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;

    wchar_t title[256]{};
    int len = GetWindowTextW(hwnd, title, 256);
    if (len <= 0) return TRUE;

    for (auto& proc : *(ctx->list))
    {
        if (proc.pid == pid)
        {
            // If we don't have a title yet, or this window is visible, take it
            if (proc.window_title.empty() || IsWindowVisible(hwnd))
            {
                proc.window_title = title;
                if (IsWindowVisible(hwnd)) break;
            }
        }
    }
    return TRUE;
}

inline std::wstring SentinelMutexNameForPid(DWORD pid)
{
    return wrath::bypass::sentinelMutexNameForPid(pid);
}

inline bool IsWrathLoadedInProcess(DWORD pid)
{
    if (pid == 0) return false;

    // El objeto nombrado por PID (mimetizado como staging WIL de ntdll) confirma
    // que el bootstrap de ese proceso especï¿½fico estï¿½ activo y aï¿½n no hizo unload (F8).
    const std::wstring mtx_name = SentinelMutexNameForPid(pid);
    HANDLE hMutex = OpenMutexW(SYNCHRONIZE, FALSE, mtx_name.c_str());
    if (hMutex)
    {
        CloseHandle(hMutex);
        return true;
    }
    return false;
}


inline bool IsTargetMinecraftExecutable(const wchar_t* exe)
{
    if (!exe) return false;
    return _wcsicmp(exe, L"javaw.exe") == 0 ||
           _wcsicmp(exe, L"java.exe") == 0 ||
           wcsstr(exe, L"Minecraft") != nullptr ||
           wcsstr(exe, L"Lunar") != nullptr ||
           wcsstr(exe, L"Badlion") != nullptr ||
           wcsstr(exe, L"Feather") != nullptr;
}

inline std::vector<MinecraftProcess> ScanMinecraftProcesses()
{
    std::vector<MinecraftProcess> candidates;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return candidates;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (IsTargetMinecraftExecutable(entry.szExeFile))
            {
                MinecraftProcess p;
                p.pid = entry.th32ProcessID;
                p.exe_name = entry.szExeFile;
                candidates.push_back(p);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (candidates.empty()) return candidates;

    WindowSearchContext ctx;
    ctx.list = &candidates;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&ctx));

    std::vector<MinecraftProcess> active_games;
    for (auto& proc : candidates)
    {
        if (proc.window_title.empty())
        {
            proc.window_title = proc.exe_name + L" (PID " + std::to_wstring(proc.pid) + L")";
        }
        proc.is_injected = IsWrathLoadedInProcess(proc.pid);
        active_games.push_back(proc);
    }
    return active_games;
}

inline bool ResolvePayloadDllPath(std::wstring& out_path)
{
    out_path = wrath::loader::resolve_payload();
    return !out_path.empty();
}

// dll_override: if non-empty and the file exists it is used directly,
// bypassing ResolvePayloadDllPath.  Pass a %TEMP%-staged copy for stealth.
inline bool InjectDllIntoProcess(DWORD pid, std::wstring& out_status,
                                  bool hide_from_debugger = true,
                                  const std::wstring& dll_override = {})
{
    // -- 0. Evitar reinyeccion duplicada ---------------------------------------
    if (IsWrathLoadedInProcess(pid))
    {
        out_status = L"Wrath ya se encuentra inyectado y activo en Minecraft (PID: "
                     + std::to_wstring(pid) + L").";
        return true;
    }

    EnableDebugPrivilege();

    std::wstring dll_path;
    if (!dll_override.empty() &&
        GetFileAttributesW(dll_override.c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        dll_path = dll_override;
    }
    else if (!ResolvePayloadDllPath(dll_path))
    {
        out_status = L"No se encontrï¿½ WrathPayload.dll junto a WrathLoader.exe.";
        return false;
    }

    // -- Abrir handle al proceso con permisos mï¿½nimos necesarios ---------------
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
        PROCESS_VM_OPERATION  | PROCESS_VM_WRITE,
        FALSE, pid);

    if (!process || process == INVALID_HANDLE_VALUE)
    {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
        {
            out_status = L"Acceso denegado (PID: " + std::to_wstring(pid)
                         + L"). Minecraft se ejecuta con permisos elevados. Ejecuta WrathLoader como Administrador.";
        }
        else
        {
            out_status = L"Error abriendo proceso de Minecraft (PID: " + std::to_wstring(pid)
                         + L", Codigo Win32: " + std::to_wstring(err) + L").";
        }
        return false;
    }

    // -- Validar arquitectura: confirmar que el proceso no sea Java de 32 bits -
    BOOL isTargetWow64 = FALSE;
    if (IsWow64Process(process, &isTargetWow64) && isTargetWow64)
    {
        CloseHandle(process);
        out_status = L"Incompatibilidad de arquitectura: Minecraft se ejecuta en un Java de 32-bit (x86). Wrath requiere Java de 64-bit.";
        return false;
    }

    const size_t path_size = (dll_path.length() + 1) * sizeof(wchar_t);
    LPVOID remote_mem = VirtualAllocEx(
        process, nullptr, path_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote_mem)
    {
        const DWORD err = GetLastError();
        CloseHandle(process);
        out_status = L"VirtualAllocEx fallï¿½ en el proceso de Minecraft (PID: " + std::to_wstring(pid)
                     + L", Error: " + std::to_wstring(err) + L").";
        return false;
    }

    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(process, remote_mem, dll_path.c_str(), path_size, &bytes_written)
        || bytes_written != path_size)
    {
        const DWORD err = GetLastError();
        VirtualFreeEx(process, remote_mem, 0, MEM_RELEASE);
        CloseHandle(process);
        out_status = L"WriteProcessMemory fallï¿½ al transferir la ruta (Error: " + std::to_wstring(err) + L").";
        return false;
    }

    HMODULE kernel32       = GetModuleHandleW(L"kernel32.dll");
    LPVOID  load_library_w = kernel32
        ? reinterpret_cast<LPVOID>(GetProcAddress(kernel32, "LoadLibraryW"))
        : nullptr;
    if (!load_library_w)
    {
        std::vector<BYTE> zeroes(path_size, 0);
        WriteProcessMemory(process, remote_mem, zeroes.data(), path_size, nullptr);
        VirtualFreeEx(process, remote_mem, 0, MEM_RELEASE);
        CloseHandle(process);
        out_status = L"LoadLibraryW no resoluble en kernel32.dll.";
        return false;
    }

    // Hilo ï¿½nico sigiloso: NtCreateThreadEx con HIDE_FROM_DEBUGGER y derechos mï¿½nimos.
    // DllMain(DLL_PROCESS_ATTACH) arranca el hilo interno de bootstrap sin requerir
    // exportaciones en el PE ni un segundo hilo remoto.
    HANDLE remote_thread = nullptr;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll && hide_from_debugger)
    {
        auto NtCreateThreadEx = reinterpret_cast<pfnNtCreateThreadEx>(
            GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (NtCreateThreadEx)
        {
            NTSTATUS st = NtCreateThreadEx(
                &remote_thread,
                SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
                nullptr,
                process,
                load_library_w,
                remote_mem,
                THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER,
                0, 0, 0, nullptr);
            if (!NT_SUCCESS(st)) remote_thread = nullptr;
        }
    }
    if (!remote_thread)
    {
        remote_thread = CreateRemoteThread(
            process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(load_library_w),
            remote_mem, 0, nullptr);
    }
    if (!remote_thread)
    {
        const DWORD err = GetLastError();
        std::vector<BYTE> zeroes(path_size, 0);
        WriteProcessMemory(process, remote_mem, zeroes.data(), path_size, nullptr);
        VirtualFreeEx(process, remote_mem, 0, MEM_RELEASE);
        CloseHandle(process);
        out_status = L"No se pudo crear el hilo remoto en Minecraft (Error Win32: " + std::to_wstring(err) + L").";
        return false;
    }

    const DWORD wait_res = WaitForSingleObject(remote_thread, 8000);
    DWORD thread_exit_code = 0;
    GetExitCodeThread(remote_thread, &thread_exit_code);
    CloseHandle(remote_thread);

    // Zeroing y liberaciï¿½n inmediata del bloque de ruta en el proceso remoto
    std::vector<BYTE> zeroes(path_size, 0);
    WriteProcessMemory(process, remote_mem, zeroes.data(), path_size, nullptr);
    VirtualFreeEx(process, remote_mem, 0, MEM_RELEASE);
    CloseHandle(process);

    // Borrar archivo .tmp del disco si aplica
    if (!dll_path.empty() && dll_path.size() > 4)
    {
        std::wstring ext = dll_path.substr(dll_path.size() - 4);
        if (_wcsicmp(ext.c_str(), L".tmp") == 0)
            DeleteFileW(dll_path.c_str());
    }

    if (wait_res == WAIT_TIMEOUT)
    {
        out_status = L"Tiempo de espera agotado: el hilo remoto tardï¿½ demasiado en responder.";
        return false;
    }
    if (thread_exit_code == 0)
    {
        out_status = L"LoadLibraryW retornï¿½ NULL en Minecraft (Error al cargar dependencias del DLL).";
        return false;
    }

    // -- Verificaciï¿½n final: objeto centinela por PID --------------------------
    bool is_loaded = false;
    for (int i = 0; i < 40; ++i)
    {
        if (IsWrathLoadedInProcess(pid)) { is_loaded = true; break; }
        Sleep(100);
    }

    if (is_loaded)
    {
        wrath::diagnostics::recordLoadMethod(pid, wrath::diagnostics::LoadMethod::LoadLibrary);
        out_status = L"Inyecciï¿½n exitosa en Minecraft 1.21. Presiona Right Shift o Insert para abrir el menï¿½.";
        return true;
    }
    out_status = L"El DLL fue cargado pero el centinela del cliente no respondiï¿½ (PID: " + std::to_wstring(pid) + L"). Verifica que sea Minecraft Fabric.";
    return false;
}

inline bool InjectPayloadFromMemoryIntoProcess(
    DWORD pid,
    const std::vector<uint8_t>& dll_bytes,
    std::wstring& out_status,
    const std::string& server_host = "",
    int server_port = 0,
    const std::string& hwid = "",
    const std::string& session_token = "",
    const std::string& session_id = "",
    const std::string& client_nonce = "")
{
    if (IsWrathLoadedInProcess(pid))
    {
        out_status = L"Wrath ya se encuentra inyectado y activo en Minecraft (PID: " + std::to_wstring(pid) + L").";
        return true;
    }

    if (dll_bytes.empty())
    {
        out_status = L"Buffer de payload vacio en memoria.";
        return false;
    }

    EnableDebugPrivilege();

    // -- Publicar bloque de contexto para Heartbeat en vivo dentro del juego --
    HANDLE hSection = nullptr;
    if (!session_token.empty() || !server_host.empty())
    {
        std::wstring secName = wrath::bypass::heartbeatSectionNameForPid(pid);
        hSection = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(wrath::bypass::HeartbeatSharedContext), secName.c_str());
        if (hSection)
        {
            auto* ctx = reinterpret_cast<wrath::bypass::HeartbeatSharedContext*>(
                MapViewOfFile(hSection, FILE_MAP_WRITE, 0, 0, sizeof(wrath::bypass::HeartbeatSharedContext)));
            if (ctx)
            {
                SecureZeroMemory(ctx, sizeof(*ctx));
                ctx->magic = wrath::bypass::kHeartbeatMagic;
                ctx->version = wrath::bypass::kHeartbeatVersion;
                ctx->server_port = server_port;
                strncpy_s(ctx->server_host, server_host.c_str(), sizeof(ctx->server_host) - 1);
                strncpy_s(ctx->hwid, hwid.c_str(), sizeof(ctx->hwid) - 1);
                strncpy_s(ctx->session_token, session_token.c_str(), sizeof(ctx->session_token) - 1);
                strncpy_s(ctx->session_id, session_id.c_str(), sizeof(ctx->session_id) - 1);
                strncpy_s(ctx->client_nonce, client_nonce.c_str(), sizeof(ctx->client_nonce) - 1);
                ctx->created_timestamp = GetTickCount64();
                UnmapViewOfFile(ctx);
            }
        }
    }

    // Inyeccion reflectiva pura en memoria (M03 / M04 / M09) - Cero disco, Cero LoadLibraryW
    std::wstring inject_err;
    bool ok = wrath::core::nt::nt_inject(pid, dll_bytes.data(), dll_bytes.size(), &inject_err);
    if (!ok)
    {
        out_status = inject_err.empty() ? L"Error durante la inyeccion reflectiva en javaw.exe." : inject_err;
        if (hSection) CloseHandle(hSection);
        return false;
    }

    // Mantener la seccion viva en segundo plano hasta que el payload en Minecraft haya mapeado el contexto
    if (hSection)
    {
        std::thread([hSection, pid]() {
            for (int i = 0; i < 40; ++i) {
                if (IsWrathLoadedInProcess(pid)) break;
                Sleep(250);
            }
            Sleep(8000); // 8 segundos de margen garantizado para que el Watchdog lea el token
            CloseHandle(hSection);
        }).detach();
    }

    // Verificacion del centinela
    bool is_loaded = false;
    for (int i = 0; i < 40; ++i)
    {
        if (IsWrathLoadedInProcess(pid)) { is_loaded = true; break; }
        Sleep(100);
    }

    if (is_loaded)
    {
        wrath::diagnostics::recordLoadMethod(pid, wrath::diagnostics::LoadMethod::ManualMap);
        out_status = L"Inyeccion reflectiva exitosa en Minecraft 1.21. Presiona Right Shift o Insert para abrir el menu.";
        return true;
    }

    out_status = L"El payload reflectivo fue inyectado pero el centinela no respondio (PID: " + std::to_wstring(pid) + L").";
    return false;
}

} // namespace launcher

#endif // LAUNCHER_INJECTOR_H
