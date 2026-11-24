#include "platform/Win32Process.h"
#include "core/config/ProfileStorage.h"
#include "shared/strings/mc_names.h"

#include <psapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <mutex>

namespace vanta::platform {

namespace {
HMODULE            g_self  = nullptr;
size_t             g_selfSize = 0;
std::once_flag     g_selfOnce;
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
    HANDLE  proc = GetCurrentProcess();
    HMODULE mods[1024];
    DWORD   needed = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return out;
    DWORD n = needed / sizeof(HMODULE);
    out.reserve(n);
    for (DWORD i = 0; i < n; ++i) {
        wchar_t buf[MAX_PATH];
        if (GetModuleFileNameExW(proc, mods[i], buf, MAX_PATH) == 0) continue;
        MODULEINFO mi{};
        GetModuleInformation(proc, mods[i], &mi, sizeof(mi));
        out.push_back({mods[i], buf, static_cast<std::uint8_t*>(mi.lpBaseOfDll), mi.SizeOfImage});
    }
    return out;
}

HMODULE findJvmModule() {
    HANDLE proc = GetCurrentProcess();
    HMODULE modsStack[1024];
    DWORD needed = 0;

    if (!EnumProcessModulesEx(proc, modsStack, sizeof(modsStack), &needed, LIST_MODULES_ALL)) {
        return nullptr;
    }

    HMODULE* pMods = modsStack;
    std::vector<HMODULE> modsHeap;
    if (needed > sizeof(modsStack)) {
        modsHeap.resize(needed / sizeof(HMODULE));
        if (EnumProcessModulesEx(proc, modsHeap.data(), static_cast<DWORD>(modsHeap.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
            pMods = modsHeap.data();
        }
    }

    auto jniProcName = wrath::strings::kJniGetCreatedJavaVMs.decrypt();
    const DWORD count = needed / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i) {
        if (!pMods[i]) continue;
        if (GetProcAddress(pMods[i], jniProcName.get()) != nullptr) {
            return pMods[i];
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

} // namespace vanta::platform
