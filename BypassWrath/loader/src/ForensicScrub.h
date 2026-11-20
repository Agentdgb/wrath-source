#pragma once
// ForensicScrub.h -- P4-02: Surgical, atomic forensic scrub for loader execution traces.
//
// Rules enforced (per forensic-bypass-audit & implementation brief):
//   1. CERO detención de servicios (DPS, SysMain, EventLog, etc. intactos).
//   2. CERO truncado de USN Journal: truncar o recrear el journal genera flags evidentes
//      (USNshrink / journal recreation) en scanners forenses (Echo, Paladin, USNJournalParser).
//      El rastro transitorio del ejecutable se asume como trade-off menor frente a journal tampering.
//   3. CERO borrado masivo de Prefetch (solo se elimina el .pf específico del loader).
//   4. Restauración de LastWriteTime de claves de registro modificadas (T18).
//   5. Limpieza quirúrgica de BAM (T19), MuiCache, UserAssist y reportes WER (T17).
//   6. Ejecución diferida vía re-ejecución con --scrub <pid> (T4).

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>
#include <fstream>
#include <filesystem>

#include "../shared/StealthDefs.h"
#include "SelfDelete.h"

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

inline void EnablePrivilege(LPCWSTR privName) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) return;
    TOKEN_PRIVILEGES tp{};
    if (LookupPrivilegeValueW(nullptr, privName, &tp.Privileges[0].Luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
}

// T18: Restore original LastWriteTime of a registry key via NtSetInformationKey
inline void RestoreKeyWriteTime(HKEY hKey, const FILETIME& orig_ft) {
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) return;
    auto pfnNtSetKey = reinterpret_cast<pfnNtSetInformationKey_t>(
        GetProcAddress(hNt, "NtSetInformationKey"));
    if (pfnNtSetKey) {
        LARGE_INTEGER li{};
        li.LowPart = orig_ft.dwLowDateTime;
        li.HighPart = orig_ft.dwHighDateTime;
        constexpr ULONG KeyWriteTimeInformation = 6;
        pfnNtSetKey(reinterpret_cast<HANDLE>(hKey), KeyWriteTimeInformation, &li, sizeof(li));
    }
}

// ── 1. Scrub BAM (Background Activity Moderator) ───────────────────────────
// Removes only the value corresponding to our own executable in UserSettings\<SID>.
// T18: preserves key LastWriteTime.
// Bug 51 fix: saves BOTH original Owner SID and DACL, restoring both upon completion.

struct BW_SavedSecurity {
    std::vector<BYTE> owner_sid;
    std::vector<BYTE> dacl_buf;
    bool valid = false;
};

inline bool OpenKeyTakeOwnership(HKEY root, const std::wstring& sub, REGSAM acc,
                                  HKEY* out, BW_SavedSecurity* saved_sec = nullptr)
{
    *out = nullptr;
    // Fast path: normal open succeeds
    if (RegOpenKeyExW(root, sub.c_str(), 0, acc, out) == ERROR_SUCCESS) return true;

    // Slow path: privilege elevation + ownership transfer
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);

    // Step 1: open with READ_CONTROL | WRITE_OWNER
    HKEY hOwn = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0,
                      READ_CONTROL | WRITE_OWNER, &hOwn) != ERROR_SUCCESS)
        return false;

    // Bug 51 fix: Capture original Owner SID and DACL BEFORE changing owner!
    if (saved_sec) {
        PSECURITY_DESCRIPTOR pSD = nullptr;
        PSID pOwner = nullptr;
        PACL pDacl = nullptr;
        if (GetSecurityInfo(hOwn, SE_REGISTRY_KEY,
                            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
                            &pOwner, nullptr, &pDacl, nullptr, &pSD) == ERROR_SUCCESS && pSD) {
            if (pOwner && IsValidSid(pOwner)) {
                DWORD sid_len = GetLengthSid(pOwner);
                saved_sec->owner_sid.assign(reinterpret_cast<BYTE*>(pOwner),
                                            reinterpret_cast<BYTE*>(pOwner) + sid_len);
            }
            if (pDacl) {
                saved_sec->dacl_buf.assign(reinterpret_cast<BYTE*>(pDacl),
                                           reinterpret_cast<BYTE*>(pDacl) + pDacl->AclSize);
            }
            saved_sec->valid = true;
            LocalFree(pSD);
        }
    }

    // Assign temporary ownership to Builtin Administrators
    PSID pAdm = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &pAdm)) {
        RegCloseKey(hOwn); return false;
    }

    LSTATUS owner_st = SetSecurityInfo(hOwn, SE_REGISTRY_KEY,
                                       OWNER_SECURITY_INFORMATION,
                                       pAdm, nullptr, nullptr, nullptr);
    FreeSid(pAdm);
    RegCloseKey(hOwn);
    if (owner_st != ERROR_SUCCESS) return false;

    // Step 2: open with WRITE_DAC to set temporary permissive DACL
    HKEY hDacl = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0,
                      READ_CONTROL | WRITE_DAC, &hDacl) != ERROR_SUCCESS)
        return false;

    PSECURITY_DESCRIPTOR pSD = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;WD)", SDDL_REVISION_1, &pSD, nullptr)) {
        RegCloseKey(hDacl); return false;
    }
    BOOL dacl_present = FALSE, dacl_default = FALSE;
    PACL pDacl = nullptr;
    GetSecurityDescriptorDacl(pSD, &dacl_present, &pDacl, &dacl_default);
    SetSecurityInfo(hDacl, SE_REGISTRY_KEY, DACL_SECURITY_INFORMATION,
                    nullptr, nullptr, pDacl, nullptr);
    LocalFree(pSD);
    RegCloseKey(hDacl);

    // Step 3: open with requested access
    return RegOpenKeyExW(root, sub.c_str(), 0, acc, out) == ERROR_SUCCESS;
}

inline bool ScrubBAM() {
    std::wstring sid = GetCurrentUserSid();
    if (sid.empty()) return false;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    const wchar_t* bam_bases[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\",
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\UserSettings\\",
        nullptr
    };

    bool any_success = false;
    for (int b = 0; bam_bases[b]; ++b) {
        std::wstring subkey = std::wstring(bam_bases[b]) + sid;
        HKEY hKey = nullptr;
        BW_SavedSecurity saved_sec;

        if (!OpenKeyTakeOwnership(HKEY_LOCAL_MACHINE, subkey,
                                  KEY_QUERY_VALUE | KEY_WRITE, &hKey, &saved_sec))
            continue;

        // Capture original LastWriteTime BEFORE any mutation
        FILETIME orig_ft{};
        RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, &orig_ft);

        // Enumerate and delete matching values
        DWORD idx = 0;
        wchar_t valName[1024];
        DWORD valLen = 1024;
        std::vector<std::wstring> to_delete;

        while (RegEnumValueW(hKey, idx, valName, &valLen, nullptr,
                            nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring v(valName, valLen);
            if (ToLower(v).find(exe_lower) != std::wstring::npos)
                to_delete.push_back(v);
            ++idx;
            valLen = 1024;
        }
        for (const auto& v : to_delete)
            RegDeleteValueW(hKey, v.c_str());

        // Bug 51 fix: Restore original DACL AND original Owner
        if (saved_sec.valid) {
            HKEY hSecRestore = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0,
                              READ_CONTROL | WRITE_DAC | WRITE_OWNER, &hSecRestore) == ERROR_SUCCESS) {
                // 1. Restore original DACL first
                if (!saved_sec.dacl_buf.empty()) {
                    SetSecurityInfo(hSecRestore, SE_REGISTRY_KEY,
                                    DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr,
                                    reinterpret_cast<PACL>(saved_sec.dacl_buf.data()), nullptr);
                }
                // 2. Restore original Owner (e.g. NT AUTHORITY\SYSTEM)
                if (!saved_sec.owner_sid.empty()) {
                    SetSecurityInfo(hSecRestore, SE_REGISTRY_KEY,
                                    OWNER_SECURITY_INFORMATION,
                                    reinterpret_cast<PSID>(saved_sec.owner_sid.data()),
                                    nullptr, nullptr, nullptr);
                }
                RegCloseKey(hSecRestore);
            }
        }

        // Restore LastWriteTime after DACL and owner restored
        RestoreKeyWriteTime(hKey, orig_ft);
        RegCloseKey(hKey);
        any_success = true;
    }
    return any_success;
}

// ── 2. Scrub MuiCache ───────────────────────────────────────────────────────
inline bool ScrubMuiCache() {
    const wchar_t* mui_key =
        L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\Shell\\MuiCache";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, mui_key, 0,
                      KEY_QUERY_VALUE | KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        FILETIME orig_ft{};
        RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, nullptr, &orig_ft);

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

        RestoreKeyWriteTime(hKey, orig_ft);
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

// ── 3. Scrub UserAssist ─────────────────────────────────────────────────────
inline bool ScrubUserAssist() {
    const wchar_t* ua_base =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\UserAssist";
    HKEY hBase = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, ua_base, 0, KEY_READ, &hBase) != ERROR_SUCCESS)
        return false;

    std::wstring exe_lower = ToLower(GetOwnExeName());
    DWORD subIdx = 0;
    wchar_t subName[256];
    DWORD subLen = 256;

    while (RegEnumKeyExW(hBase, subIdx, subName, &subLen, nullptr,
                         nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::wstring countKeyPath = std::wstring(subName) + L"\\Count";
        HKEY hCount = nullptr;
        if (RegOpenKeyExW(hBase, countKeyPath.c_str(), 0,
                          KEY_QUERY_VALUE | KEY_WRITE, &hCount) == ERROR_SUCCESS) {
            FILETIME orig_ft{};
            RegQueryInfoKeyW(hCount, nullptr, nullptr, nullptr, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr, &orig_ft);

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

            RestoreKeyWriteTime(hCount, orig_ft);
            RegCloseKey(hCount);
        }
        ++subIdx;
        subLen = 256;
    }
    RegCloseKey(hBase);
    return true;
}

// ── 4. Scrub Prefetch ───────────────────────────────────────────────────────
// Bug 52 fix: Poll until a .pf with LastWriteTime >= parent_exit_time appears
// rather than checking blind existence, preventing premature exit.
inline bool ScrubOwnPrefetch(const FILETIME& min_time) {
    wchar_t sysroot[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH))
        wcscpy_s(sysroot, L"C:\\Windows");

    std::wstring exe_name = GetOwnExeName();
    auto dot = exe_name.rfind(L'.');
    std::wstring base_stem = (dot != std::wstring::npos) ? exe_name.substr(0, dot) : exe_name;
    std::wstring pf_dir    = std::wstring(sysroot) + L"\\Prefetch\\";
    std::wstring pattern   = pf_dir + base_stem + L"-*.pf";

    ULARGE_INTEGER min_uli;
    min_uli.LowPart = min_time.dwLowDateTime;
    min_uli.HighPart = min_time.dwHighDateTime;
    // 2-second tolerance
    if (min_uli.QuadPart > 20000000ULL) min_uli.QuadPart -= 20000000ULL;

    bool found = false;
    for (int attempt = 0; attempt < 60 && !found; ++attempt) {
        WIN32_FIND_DATAW probe{};
        HANDLE hProbe = FindFirstFileW(pattern.c_str(), &probe);
        if (hProbe != INVALID_HANDLE_VALUE) {
            do {
                ULARGE_INTEGER pf_time;
                pf_time.LowPart = probe.ftLastWriteTime.dwLowDateTime;
                pf_time.HighPart = probe.ftLastWriteTime.dwHighDateTime;
                if (pf_time.QuadPart >= min_uli.QuadPart) {
                    found = true;
                    break;
                }
            } while (FindNextFileW(hProbe, &probe));
            FindClose(hProbe);
        }
        if (!found) Sleep(500);
    }

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    bool deleted = false;
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (DeleteFileW((pf_dir + fd.cFileName).c_str()))
                deleted = true;
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
    return deleted;
}

// ── 5. Scrub WER Reports (P2 - T17) ────────────────────────────────────────
inline void ScrubWerFolder(const std::wstring& root) {
    if (!std::filesystem::exists(root)) return;
    std::wstring exe_lower = ToLower(GetOwnExeName());

    try {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_directory()) {
                std::wstring report_file = entry.path().wstring() + L"\\Report.wer";
                if (std::filesystem::exists(report_file)) {
                    std::ifstream f(report_file, std::ios::binary);
                    if (f) {
                        std::string content((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
                        std::wstring wcontent;
                        wcontent.reserve(content.size() / 2);
                        for (size_t i = 0; i + 1 < content.size(); i += 2) {
                            wchar_t ch = static_cast<wchar_t>(
                                static_cast<unsigned char>(content[i]) |
                                (static_cast<unsigned char>(content[i + 1]) << 8));
                            wcontent.push_back(ch);
                        }
                        if (ToLower(wcontent).find(exe_lower) != std::wstring::npos) {
                            f.close();
                            std::error_code ec;
                            std::filesystem::remove_all(entry.path(), ec);
                        }
                    }
                }
            }
        }
    } catch (...) {}
}

inline void ScrubWerReports() {
    wchar_t local_app[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH)) {
        std::wstring base = std::wstring(local_app) + L"\\Microsoft\\Windows\\WER\\";
        ScrubWerFolder(base + L"ReportArchive");
        ScrubWerFolder(base + L"ReportQueue");
    }

    wchar_t prog_data[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH)) {
        std::wstring base = std::wstring(prog_data) + L"\\Microsoft\\Windows\\WER\\";
        ScrubWerFolder(base + L"ReportArchive");
        ScrubWerFolder(base + L"ReportQueue");
    }
}

// ── 5b. Dedicated CrashDumps Scrubber (Bug 13 fix) ──────────────────────────
// %LOCALAPPDATA%\CrashDumps contains standalone .dmp files, not Report.wer subdirectories.
inline void ScrubCrashDumps() {
    wchar_t local_app[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH)) return;
    std::wstring cd_dir = std::wstring(local_app) + L"\\CrashDumps\\";
    std::wstring pat = cd_dir + L"*.dmp";

    std::wstring exe_lower = ToLower(GetOwnExeName());
    auto dot = exe_lower.rfind(L'.');
    std::wstring stem = (dot != std::wstring::npos) ? exe_lower.substr(0, dot) : exe_lower;

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pat.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::wstring fname = ToLower(fd.cFileName);
            if (fname.find(stem) != std::wstring::npos) {
                DeleteFileW((cd_dir + fd.cFileName).c_str());
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

// ── 6. Scrub Amcache (P2-[10] & Bug 11 fix) ─────────────────────────────────
// SRUM (SRUDB.dat, ESE) is NOT touched — requires ESE API or offline tooling;
// documented as a known gap. Amcache.hve is reachable via RegLoadKey.
//
// Bug 11 fix:
//   - Matches subkey names directly (InventoryApplicationFile subkeys look like <name>|<hash>)
//   - Covers Root\Programs (used in Win10 1809+)
//   - In Root\File\<vol>\<id>, checks values 15 and 17 (file paths)
//   - Retries RegLoadKey on ERROR_SHARING_VIOLATION
inline bool ScrubAmcache() {
    EnablePrivilege(SE_BACKUP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);

    wchar_t sysroot[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
    std::wstring hve_path = std::wstring(sysroot) + L"\\AppCompat\\Programs\\Amcache.hve";
    if (!std::filesystem::exists(hve_path)) return false;

    constexpr wchar_t kMount[] = L"TMP_BWAMC";

    // Retry on sharing violation (e.g. AppIDSvc or AV holding hive open)
    LSTATUS lst = ERROR_SHARING_VIOLATION;
    for (int retry = 0; retry < 5 && (lst == ERROR_SHARING_VIOLATION || lst == ERROR_ACCESS_DENIED); ++retry) {
        lst = RegLoadKeyW(HKEY_LOCAL_MACHINE, kMount, hve_path.c_str());
        if (lst == ERROR_SUCCESS) break;
        Sleep(200);
    }
    if (lst != ERROR_SUCCESS) return false;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    auto scrub_inventory = [&](const std::wstring& base_path) {
        HKEY hBase = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base_path.c_str(), 0, KEY_READ | KEY_WRITE, &hBase) != ERROR_SUCCESS) return;

        DWORD idx = 0;
        wchar_t sub[512]; DWORD sub_len = 512;
        std::vector<std::wstring> to_delete;

        while (RegEnumKeyExW(hBase, idx, sub, &sub_len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring sname(sub, sub_len);
            bool hit = (ToLower(sname).find(exe_lower) != std::wstring::npos);
            if (!hit) {
                HKEY hChild = nullptr;
                if (RegOpenKeyExW(hBase, sname.c_str(), 0, KEY_READ, &hChild) == ERROR_SUCCESS) {
                    DWORD vi = 0; wchar_t vn[512]; DWORD vn_len = 512; wchar_t vd[1024]; DWORD vd_sz = sizeof(vd); DWORD vt = 0;
                    while (!hit && RegEnumValueW(hChild, vi, vn, &vn_len, nullptr, &vt, reinterpret_cast<LPBYTE>(vd), &vd_sz) == ERROR_SUCCESS) {
                        if (vt == REG_SZ || vt == REG_EXPAND_SZ) {
                            if (ToLower(std::wstring(vd, vd_sz / sizeof(wchar_t))).find(exe_lower) != std::wstring::npos)
                                hit = true;
                        }
                        ++vi; vn_len = 512; vd_sz = sizeof(vd);
                    }
                    RegCloseKey(hChild);
                }
            }
            if (hit) to_delete.push_back(sname);
            ++idx; sub_len = 512;
        }

        for (const auto& s : to_delete)
            RegDeleteTreeW(hBase, s.c_str());

        RegCloseKey(hBase);
    };

    auto scrub_file_entries = [&](const std::wstring& root_file_path) {
        HKEY hRoot = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_file_path.c_str(), 0, KEY_READ, &hRoot) != ERROR_SUCCESS) return;

        DWORD vi = 0; wchar_t vname[512]; DWORD vlen = 512;
        while (RegEnumKeyExW(hRoot, vi, vname, &vlen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring vol_path = root_file_path + L"\\" + std::wstring(vname, vlen);
            HKEY hVol = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, vol_path.c_str(), 0, KEY_READ | KEY_WRITE, &hVol) == ERROR_SUCCESS) {
                DWORD fi = 0; wchar_t fid[512]; DWORD flen = 512;
                std::vector<std::wstring> file_delete;
                while (RegEnumKeyExW(hVol, fi, fid, &flen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                    std::wstring f_sub = std::wstring(fid, flen);
                    HKEY hF = nullptr;
                    if (RegOpenKeyExW(hVol, f_sub.c_str(), 0, KEY_READ, &hF) == ERROR_SUCCESS) {
                        wchar_t path_val[1024]{}; DWORD pv_sz = sizeof(path_val); DWORD pvt = 0;
                        bool hit = false;
                        if (RegQueryValueExW(hF, L"15", nullptr, &pvt, reinterpret_cast<LPBYTE>(path_val), &pv_sz) == ERROR_SUCCESS) {
                            if (ToLower(std::wstring(path_val, pv_sz / sizeof(wchar_t))).find(exe_lower) != std::wstring::npos) hit = true;
                        }
                        if (!hit) {
                            pv_sz = sizeof(path_val);
                            if (RegQueryValueExW(hF, L"17", nullptr, &pvt, reinterpret_cast<LPBYTE>(path_val), &pv_sz) == ERROR_SUCCESS) {
                                if (ToLower(std::wstring(path_val, pv_sz / sizeof(wchar_t))).find(exe_lower) != std::wstring::npos) hit = true;
                            }
                        }
                        RegCloseKey(hF);
                        if (hit) file_delete.push_back(f_sub);
                    }
                    ++fi; flen = 512;
                }
                for (const auto& fd_name : file_delete)
                    RegDeleteTreeW(hVol, fd_name.c_str());
                RegCloseKey(hVol);
            }
            ++vi; vlen = 512;
        }
        RegCloseKey(hRoot);
    };

    std::wstring mount_prefix = std::wstring(L"TMP_BWAMC\\");
    scrub_inventory(mount_prefix + L"Root\\InventoryApplicationFile");
    scrub_inventory(mount_prefix + L"Root\\Programs");
    scrub_file_entries(mount_prefix + L"Root\\File");

    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kMount);
    return true;
}

// ── Scrubber Process Loop (P0 - T4) ─────────────────────────────────────────
// Re-executes the same binary with "--scrub <parent_pid>". Waits for parent
// to exit, allows SysMain to write the prefetch file, then purges all traces
// and self-deletes the executable from disk.
inline void ScrubberMain(DWORD parent_pid, const std::wstring& optional_log_path = L"") {
    auto log_step = [&](const wchar_t* step, bool ok) {
        if (optional_log_path.empty()) return;
        HANDLE h = CreateFileW(optional_log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t buf[256];
            swprintf_s(buf, L"[%s] %s\n", ok ? L"OK" : L"FAIL", step);
            DWORD written = 0;
            WriteFile(h, buf, static_cast<DWORD>(wcslen(buf) * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(h);
        }
    };

    FILETIME parent_exit{};
    if (parent_pid) {
        HANDLE hParent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent_pid);
        if (hParent) {
            WaitForSingleObject(hParent, 120000);
            FILETIME ct{}, et{}, kt{}, ut{};
            if (GetProcessTimes(hParent, &ct, &et, &kt, &ut)) {
                parent_exit = et;
            }
            CloseHandle(hParent);
        }
    }
    if (parent_exit.dwLowDateTime == 0 && parent_exit.dwHighDateTime == 0) {
        GetSystemTimeAsFileTime(&parent_exit);
    }

    EnablePrivilege(SE_DEBUG_NAME);

    bool pf_ok = ScrubOwnPrefetch(parent_exit);
    log_step(L"Prefetch", pf_ok);

    ScrubWerReports();
    log_step(L"WER", true);

    ScrubCrashDumps();
    log_step(L"CrashDumps", true);

    bool bam_ok = ScrubBAM();
    log_step(L"BAM", bam_ok);

    bool mui_ok = ScrubMuiCache();
    log_step(L"MuiCache", mui_ok);

    bool ua_ok = ScrubUserAssist();
    log_step(L"UserAssist", ua_ok);

    bool amc_ok = ScrubAmcache();
    log_step(L"Amcache", amc_ok);

    if (!optional_log_path.empty() && std::filesystem::exists(optional_log_path)) {
        DeleteFileW(optional_log_path.c_str());
    }

    SelfDelete();

    // Bug 52: Detached child to delete scrubber's own .pf after process termination
    {
        wchar_t sysroot[MAX_PATH]{};
        if (!GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH)) wcscpy_s(sysroot, L"C:\\Windows");
        std::wstring own_name = GetOwnExeName();
        auto dot = own_name.rfind(L'.');
        std::wstring stem = (dot != std::wstring::npos) ? own_name.substr(0, dot) : own_name;
        std::wstring cmd = L"cmd.exe /c \"ping 127.0.0.1 -n 3 >nul & del /f /q \"" +
                           std::wstring(sysroot) + L"\\Prefetch\\" + stem + L"-*.pf\"\"";
        STARTUPINFO si{ sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');
        if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }

    ExitProcess(0);
}

} // namespace bp
