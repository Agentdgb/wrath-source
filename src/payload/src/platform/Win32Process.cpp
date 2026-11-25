#include "platform/Win32Process.h"
#include "core/config/ProfileStorage.h"

#include <winternl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mutex>

namespace dt::platform {

namespace {
HMODULE            g_self  = nullptr;
size_t             g_selfSize = 0;
std::once_flag     g_selfOnce;

struct LdrEntryFull {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID      DllBase;
    PVOID      EntryPoint;
    ULONG      SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

struct PebLdrFull {
    ULONG      Length;
    BOOLEAN    Initialized;
    PVOID      SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct PebFull {
    BYTE       Reserved1[2];
    BYTE       BeingDebugged;
    BYTE       Reserved2[1];
    PVOID      Reserved3[2];
    PebLdrFull* Ldr;
};

consteval uint64_t fnv1aUpper(const wchar_t* s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; s[i] != L'\0'; ++i) {
        wchar_t c = s[i];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        h ^= static_cast<uint64_t>(c & 0xFFFFu);
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t hashUnicodeUpper(const UNICODE_STRING& u) noexcept {
    uint64_t h = 14695981039346656037ULL;
    if (!u.Buffer || u.Length == 0) return h;
    const size_t count = u.Length / sizeof(wchar_t);
    for (size_t i = 0; i < count; ++i) {
        wchar_t c = u.Buffer[i];
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        h ^= static_cast<uint64_t>(c & 0xFFFFu);
        h *= 1099511628211ULL;
    }
    return h;
}

constexpr uint64_t kJvmDllHash = fnv1aUpper(L"JVM.DLL");

} // namespace

void setCurrentModule(HMODULE m) noexcept {
    std::call_once(g_selfOnce, [m]{
        g_self = m;
        if (m) {
            uintptr_t base = reinterpret_cast<uintptr_t>(m);
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    g_selfSize = nt->OptionalHeader.SizeOfImage;
                }
            }
            if (g_selfSize == 0) {
                g_selfSize = 0x500000;
            }
        }
    });
}

HMODULE currentModule() noexcept {
    return g_self;
}

bool isAddressInOurModule(const void* ptr) noexcept {
    if (!ptr || !g_self) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(g_self);
    return (addr >= base && addr < base + (g_selfSize ? g_selfSize : 0x500000));
}

bool isRegisteredDll(HMODULE m) noexcept {
    if (!m) return false;
    HMODULE check = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(m),
            &check) && check == m) {
        return true;
    }
    return false;
}

std::vector<ModuleInfo> enumProcessModules() {
    std::vector<ModuleInfo> out;
    auto* peb = reinterpret_cast<const PebFull*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (!peb || !peb->Ldr) return out;

    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (const LIST_ENTRY* cur = head->Flink; cur != head; cur = cur->Flink) {
        const auto* entry = CONTAINING_RECORD(cur, LdrEntryFull, InMemoryOrderLinks);
        if (!entry->DllBase) continue;
        std::wstring fullPath;
        if (entry->FullDllName.Buffer && entry->FullDllName.Length > 0) {
            fullPath.assign(entry->FullDllName.Buffer, entry->FullDllName.Length / sizeof(wchar_t));
        }
        out.push_back({
            static_cast<HMODULE>(entry->DllBase),
            std::move(fullPath),
            static_cast<std::uint8_t*>(entry->DllBase),
            entry->SizeOfImage
        });
    }
    return out;
}

HMODULE findJvmModule() {
    auto* peb = reinterpret_cast<const PebFull*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    if (!peb || !peb->Ldr) return nullptr;

    const LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (const LIST_ENTRY* cur = head->Flink; cur != head; cur = cur->Flink) {
        const auto* entry = CONTAINING_RECORD(cur, LdrEntryFull, InMemoryOrderLinks);
        if (!entry->DllBase) continue;
        if (hashUnicodeUpper(entry->BaseDllName) == kJvmDllHash) {
            return static_cast<HMODULE>(entry->DllBase);
        }
    }
    return nullptr;
}

std::wstring processExePath() {
    wchar_t buf[MAX_PATH];
    DWORD   n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return {buf, n};
}

std::wstring localAppDataDir() {
    PWSTR value = nullptr;
    const auto result = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value);
    std::wstring path;
    if (SUCCEEDED(result) && value) path = value;
    if (value) CoTaskMemFree(value);
    return path;
}

std::wstring appDataDir() {
    static std::once_flag prepared;
    static std::wstring directory;
    std::call_once(prepared, [] {
        directory = storage::prepareProfileRoot(localAppDataDir()).wstring();
    });
    return directory;
}

std::wstring dllDirectory() {
    HMODULE self = currentModule();
    if (!self) return L".";
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0) return L".";
    std::wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

std::wstring logDir() {
#ifndef _DEBUG
    return {};
#else
    auto p = dllDirectory();
    HANDLE probe = CreateFileW((p + L"\\.wrath_write_probe").c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return p;
    }
    return appDataDir();
#endif
}

} // namespace dt::platform
