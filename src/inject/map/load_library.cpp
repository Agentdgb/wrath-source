#include "inject/Injector.h"
#include "shared/stealth/StealthDefs.h"

#include <windows.h>
#include <winternl.h>
#include <cstdio>
#include <cwchar>

namespace wrath::inject {

namespace {

struct RemoteLdrEntry {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

struct RemotePebLdr {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct RemotePbi { PVOID r1; PVOID peb; PVOID r2[2]; ULONG_PTR pid; PVOID r3; };

bool is_module_in_remote_peb(HANDLE hProc, const std::wstring& dll_path) {
    if (!hProc || hProc == INVALID_HANDLE_VALUE) return false;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    using QIP = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto qip = reinterpret_cast<QIP>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!qip) return false;

    RemotePbi pbi{};
    ULONG len = 0;
    if (qip(hProc, 0, &pbi, sizeof(pbi), &len) < 0 || !pbi.peb) return false;

    PVOID ldr_ptr = nullptr;
    SIZE_T got = 0;
    if (!ReadProcessMemory(hProc, reinterpret_cast<const BYTE*>(pbi.peb) + 0x18, &ldr_ptr, sizeof(ldr_ptr), &got) || !ldr_ptr)
        return false;

    RemotePebLdr ldr{};
    if (!ReadProcessMemory(hProc, ldr_ptr, &ldr, sizeof(ldr), &got) || got != sizeof(ldr))
        return false;

    size_t slash = dll_path.find_last_of(L"\\/");
    std::wstring target_name = (slash != std::wstring::npos) ? dll_path.substr(slash + 1) : dll_path;

    const uintptr_t head_remote = reinterpret_cast<uintptr_t>(ldr_ptr) + offsetof(RemotePebLdr, InLoadOrderModuleList);
    LIST_ENTRY cur = ldr.InLoadOrderModuleList;
    for (int i = 0; i < 512 && cur.Flink && cur.Flink != reinterpret_cast<LIST_ENTRY*>(head_remote); ++i) {
        RemoteLdrEntry entry{};
        if (!ReadProcessMemory(hProc, cur.Flink, &entry, sizeof(entry), &got) || got != sizeof(entry))
            break;
        if (entry.BaseDllName.Length > 0 && entry.BaseDllName.Length < 512 && entry.BaseDllName.Buffer) {
            std::wstring name(entry.BaseDllName.Length / sizeof(wchar_t), L'\0');
            if (ReadProcessMemory(hProc, entry.BaseDllName.Buffer, name.data(), entry.BaseDllName.Length, &got) &&
                _wcsicmp(name.c_str(), target_name.c_str()) == 0) {
                return true;
            }
        }
        cur = entry.InLoadOrderLinks;
    }
    return false;
}

bool is_absolute_wide_path(const std::wstring& p) {
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    if (p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\') return true;
    return false;
}

// C-8: 3-state idempotency guard:
//  - section absent -> inject (false)
//  - flag active + module IN PEB -> do not inject (true)
//  - flag active + module ABSENT -> desync/orphan flag -> inject (false)
bool is_already_attached(HANDLE hProc, DWORD pid, const std::wstring& dll_path) {
    if (pid == 0) return false;
    wchar_t sec_name[64]{};
    _snwprintf_s(sec_name, _countof(sec_name), _TRUNCATE, L"%s%lu",
                 wrath::bypass::kHeartbeatPrefix, pid);
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, sec_name);
    if (!hMap) return false;

    bool flag_set = false;
    auto* ctx = reinterpret_cast<const wrath::bypass::HeartbeatSharedContext*>(
        MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(wrath::bypass::HeartbeatSharedContext)));
    if (ctx) {
        if (ctx->magic == wrath::bypass::kHeartbeatMagic &&
            ctx->version == wrath::bypass::kHeartbeatVersion &&
            (ctx->flags & wrath::bypass::kHeartbeatFlagAttached) != 0) {
            flag_set = true;
        }
        UnmapViewOfFile(ctx);
    }
    CloseHandle(hMap);

    if (!flag_set) return false;
    return is_module_in_remote_peb(hProc, dll_path);
}

std::wstring ignored_flags_note(const Options& options) {
    if (!options.unlink || !options.wipe_headers || !options.reprotect || !options.zero_scratch) {
        return L" (flags de mapeo manual ignorados por load-library)";
    }
    return L"";
}

} // namespace

bool load_library(HANDLE hProc, DWORD pid, std::span<const uint8_t> /*payload*/,
                  const Options& options, Result& result) {
    const std::wstring note = ignored_flags_note(options);

    if (options.dll_path.empty() || !is_absolute_wide_path(options.dll_path)) {
        result.detail = L"load-library: se requiere ruta ancha absoluta del DLL." + note;
        return false;
    }

    if (!hProc || hProc == INVALID_HANDLE_VALUE) {
        result.detail = L"load-library: handle de proceso invalido." + note;
        return false;
    }

    // WP-2.5 + C-8: 3-state idempotency guard — avoid bumping LoadCount above 1 if already in PEB.
    if (is_already_attached(hProc, pid, options.dll_path)) {
        result.detail = L"load-library: payload ya adjunto en el proceso." + note;
        return true;
    }

    const wchar_t* path = options.dll_path.c_str();
    const SIZE_T size = (std::wcslen(path) + 1) * sizeof(wchar_t);

    LPVOID buf = VirtualAllocEx(hProc, nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) {
        result.detail = L"load-library: VirtualAllocEx fallo." + note;
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(hProc, buf, path, size, &written) || written != size) {
        VirtualFreeEx(hProc, buf, 0, MEM_RELEASE);
        result.detail = L"load-library: WriteProcessMemory fallo." + note;
        return false;
    }

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto load_lib_w = k32 ? reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(k32, "LoadLibraryW")) : nullptr;
    if (!load_lib_w) {
        VirtualFreeEx(hProc, buf, 0, MEM_RELEASE);
        result.detail = L"load-library: LoadLibraryW no resuelto." + note;
        return false;
    }

    // WP-2.6: Simple CreateRemoteThread; NtCreateThreadEx HIDE_FROM_DEBUGGER only hides from debuggers, not System Informer.
    DWORD tid = 0;
    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, load_lib_w, buf, 0, &tid);
    if (!hThread) {
        VirtualFreeEx(hProc, buf, 0, MEM_RELEASE);
        result.detail = L"load-library: CreateRemoteThread fallo." + note;
        return false;
    }

    DWORD wait_rc = WaitForSingleObject(hThread, 15000);
    DWORD exit_code = 0;
    bool got_exit = (wait_rc == WAIT_OBJECT_0) && (GetExitCodeThread(hThread, &exit_code) != FALSE);

    VirtualFreeEx(hProc, buf, 0, MEM_RELEASE);
    CloseHandle(hThread);

    if (wait_rc != WAIT_OBJECT_0) {
        result.detail = L"load-library: timeout esperando hilo remoto." + note;
        return false;
    }

    // WP-2.3: Exit code is only a boolean success/failure indicator (DWORD truncates x64 HMODULE).
    if (!got_exit || exit_code == 0) {
        result.detail = L"load devolvio 0" + note;
        return false;
    }

    result.detail = L"load-library: carga completada." + note;
    return true;
}

} // namespace wrath::inject
