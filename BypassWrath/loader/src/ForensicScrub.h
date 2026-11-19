#pragma once
// ForensicScrub.h -- P4-02: Surgical, atomic forensic scrub for loader execution traces.
//
// Rules enforced (per forensic-bypass-audit):
//   1. CERO detención de servicios (DPS, SysMain, EventLog, etc. intactos).
//   2. CERO truncado de USN Journal (evita flag USNshrink).
//   3. CERO borrado masivo de Prefetch (solo se elimina el .pf específico del loader).
//   4. CERO desincronización tosca de timestamps.
//   5. Limpieza quirúrgica de BAM, MuiCache y UserAssist.

#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>

namespace bp {

// ── Helpers ─────────────────────────────────────────────────────────────────

inline std::wstring ToLower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

inline std::wstring Rot13(const std::wstring& input) {
    std::wstring out = input;
    for (auto& c : out) {
        if (c >= L'a' && c <= L'z') {
            c = L'a' + (c - L'a' + 13) % 26;
        } else if (c >= L'A' && c <= L'Z') {
            c = L'A' + (c - L'A' + 13) % 26;
        }
    }
    return out;
}

inline std::wstring GetOwnExePath() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

inline std::wstring GetOwnExeName() {
    std::wstring p = GetOwnExePath();
    auto sep = p.rfind(L'\\');
    return (sep != std::wstring::npos) ? p.substr(sep + 1) : p;
}

inline std::wstring GetCurrentUserSid() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return L"";

    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (!len) { CloseHandle(hToken); return L""; }

    std::vector<BYTE> buf(len);
    auto* user = reinterpret_cast<PTOKEN_USER>(buf.data());
    if (!GetTokenInformation(hToken, TokenUser, user, len, &len)) {
        CloseHandle(hToken);
        return L"";
    }
    CloseHandle(hToken);

    LPWSTR sidStr = nullptr;
    if (ConvertSidToStringSidW(user->User.Sid, &sidStr) && sidStr) {
        std::wstring res(sidStr);
        LocalFree(sidStr);
        return res;
    }
    return L"";
}

// ── 1. Scrub BAM (Background Activity Moderator) ───────────────────────────
// Removes only the value corresponding to our own executable in UserSettings\<SID>.

inline void ScrubBAM() {
    std::wstring sid = GetCurrentUserSid();
    if (sid.empty()) return;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    // Path variations across Win10 and Win11
    const wchar_t* bam_bases[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\",
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\UserSettings\\",
        nullptr
    };

    for (int b = 0; bam_bases[b]; ++b) {
        std::wstring subkey = std::wstring(bam_bases[b]) + sid;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            DWORD idx = 0;
            wchar_t valName[1024];
            DWORD valLen = 1024;
            std::vector<std::wstring> to_delete;

            while (RegEnumValueW(hKey, idx, valName, &valLen, nullptr,
                                nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                std::wstring v(valName, valLen);
                if (ToLower(v).find(exe_lower) != std::wstring::npos) {
                    to_delete.push_back(v);
                }
                ++idx;
                valLen = 1024;
            }

            for (const auto& v : to_delete) {
                RegDeleteValueW(hKey, v.c_str());
            }
            RegCloseKey(hKey);
        }
    }
}

// ── 2. Scrub MuiCache ───────────────────────────────────────────────────────
// Removes Shell MuiCache entries registered for our loader path.

inline void ScrubMuiCache() {
    const wchar_t* mui_key =
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, mui_key, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        std::wstring exe_lower = ToLower(GetOwnExeName());
        DWORD idx = 0;
        wchar_t valName[1024];
        DWORD valLen = 1024;
        std::vector<std::wstring> to_delete;

        while (RegEnumValueW(hKey, idx, valName, &valLen, nullptr,
                            nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring v(valName, valLen);
            if (ToLower(v).find(exe_lower) != std::wstring::npos) {
                to_delete.push_back(v);
            }
            ++idx;
            valLen = 1024;
        }

        for (const auto& v : to_delete) {
            RegDeleteValueW(hKey, v.c_str());
        }
        RegCloseKey(hKey);
    }
}

// ── 3. Scrub UserAssist ─────────────────────────────────────────────────────
// Removes ROT13 execution history records under Explorer\UserAssist.

inline void ScrubUserAssist() {
    const wchar_t* ua_base =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist";
    HKEY hBase = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ua_base, 0, KEY_READ, &hBase) != ERROR_SUCCESS)
        return;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    DWORD subIdx = 0;
    wchar_t subName[256];
    DWORD subLen = 256;

    while (RegEnumKeyExW(hBase, subIdx, subName, &subLen, nullptr,
                         nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::wstring countKeyPath = std::wstring(subName) + L"\\Count";
        HKEY hCount = nullptr;
        if (RegOpenKeyExW(hBase, countKeyPath.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_SET_VALUE, &hCount) == ERROR_SUCCESS) {
            DWORD valIdx = 0;
            wchar_t valName[1024];
            DWORD valNameLen = 1024;
            std::vector<std::wstring> to_delete;

            while (RegEnumValueW(hCount, valIdx, valName, &valNameLen, nullptr,
                                nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                std::wstring raw(valName, valNameLen);
                std::wstring decoded = Rot13(raw);
                if (ToLower(decoded).find(exe_lower) != std::wstring::npos) {
                    to_delete.push_back(raw);
                }
                ++valIdx;
                valNameLen = 1024;
            }

            for (const auto& v : to_delete) {
                RegDeleteValueW(hCount, v.c_str());
            }
            RegCloseKey(hCount);
        }
        ++subIdx;
        subLen = 256;
    }
    RegCloseKey(hBase);
}

// ── 4. Scrub Prefetch (Single file only) ────────────────────────────────────
// Deletes strictly the .pf file for our own executable without touching any
// other prefetch entries or services.

inline void ScrubOwnPrefetch() {
    std::wstring exe_name = GetOwnExeName();
    auto dot = exe_name.rfind(L'.');
    std::wstring base_stem = (dot != std::wstring::npos) ? exe_name.substr(0, dot) : exe_name;
    std::wstring pattern = L"C:\\Windows\\Prefetch\\" + base_stem + L"-*.pf";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring pf_path = L"C:\\Windows\\Prefetch\\" + std::wstring(fd.cFileName);
            DeleteFileW(pf_path.c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

// ── Master Scrub Entry ──────────────────────────────────────────────────────
inline void ScrubAllTraces() {
    ScrubBAM();
    ScrubMuiCache();
    ScrubUserAssist();
    ScrubOwnPrefetch();
}

} // namespace bp
