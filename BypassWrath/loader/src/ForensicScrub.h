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
// T18: preserves key LastWriteTime. T19: takes real ownership if access denied,
// captures original DACL, restores it after deleting values.

// P0-[3]: Full ownership-take helper with DACL save/restore.
// Returns true if hKey is opened with the requested access (even if ownership was needed).
// Caller must RegCloseKey(*out) when done.
// orig_sd_buf: filled with the original security descriptor so caller can restore it.
struct BW_SavedSD {
    std::vector<BYTE> buf;
    bool valid = false;
};

inline bool OpenKeyTakeOwnership(HKEY root, const std::wstring& sub, REGSAM acc,
                                  HKEY* out, BW_SavedSD* saved_sd = nullptr)
{
    *out = nullptr;
    // Fast path: normal open succeeds
    if (RegOpenKeyExW(root, sub.c_str(), 0, acc, out) == ERROR_SUCCESS) return true;

    // Slow path: attempt privilege elevation + ownership transfer
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    EnablePrivilege(SE_BACKUP_NAME);

    // Step 1: open with WRITE_OWNER to take ownership
    HKEY hOwn = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0,
                      READ_CONTROL | WRITE_OWNER, &hOwn) != ERROR_SUCCESS)
        return false;

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

    // Step 2: open with WRITE_DAC to read + set DACL
    HKEY hDacl = nullptr;
    if (RegOpenKeyExW(root, sub.c_str(), 0,
                      READ_CONTROL | WRITE_DAC, &hDacl) != ERROR_SUCCESS)
        return false;

    // Capture original SD (DACL) so we can restore it after the scrub
    if (saved_sd) {
        DWORD sd_sz = 0;
        RegGetKeySecurity(hDacl, DACL_SECURITY_INFORMATION, nullptr, &sd_sz);
        if (sd_sz) {
            saved_sd->buf.resize(sd_sz);
            if (RegGetKeySecurity(hDacl, DACL_SECURITY_INFORMATION,
                                  reinterpret_cast<PSECURITY_DESCRIPTOR>(saved_sd->buf.data()),
                                  &sd_sz) == ERROR_SUCCESS)
                saved_sd->valid = true;
        }
    }

    // Build permissive DACL for current user: Everyone GENERIC_ALL
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

    // Step 3: now open with the originally requested access
    return RegOpenKeyExW(root, sub.c_str(), 0, acc, out) == ERROR_SUCCESS;
}

inline void ScrubBAM() {
    std::wstring sid = GetCurrentUserSid();
    if (sid.empty()) return;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    const wchar_t* bam_bases[] = {
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\",
        L"SYSTEM\\CurrentControlSet\\Services\\bam\\UserSettings\\",
        nullptr
    };

    for (int b = 0; bam_bases[b]; ++b) {
        std::wstring subkey = std::wstring(bam_bases[b]) + sid;
        HKEY hKey = nullptr;
        BW_SavedSD saved_sd;

        if (!OpenKeyTakeOwnership(HKEY_LOCAL_MACHINE, subkey,
                                  KEY_QUERY_VALUE | KEY_WRITE, &hKey, &saved_sd))
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

        // Restore DACL first, then LastWriteTime (order matters: timestamp restore
        // needs write access, which the restored DACL must still permit)
        if (saved_sd.valid) {
            HKEY hDaclRestore = nullptr;
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subkey.c_str(), 0,
                              READ_CONTROL | WRITE_DAC, &hDaclRestore) == ERROR_SUCCESS) {
                BOOL dp = FALSE, dd = FALSE; PACL pOld = nullptr;
                auto* psd = reinterpret_cast<PSECURITY_DESCRIPTOR>(saved_sd.buf.data());
                GetSecurityDescriptorDacl(psd, &dp, &pOld, &dd);
                if (dp && pOld)
                    SetSecurityInfo(hDaclRestore, SE_REGISTRY_KEY,
                                    DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, pOld, nullptr);
                RegCloseKey(hDaclRestore);
            }
        }

        // Restore LastWriteTime after DACL restored
        RestoreKeyWriteTime(hKey, orig_ft);
        RegCloseKey(hKey);
    }
}

// ── 2. Scrub MuiCache ───────────────────────────────────────────────────────
// Removes Shell MuiCache entries registered for our loader path.

inline void ScrubMuiCache() {
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
}

// ── 4. Scrub Prefetch (Single file only) ────────────────────────────────────
// Deletes strictly the .pf file for our own executable without touching any
// other prefetch entries or services.
// P0-[4]: Use SystemRoot from env (not hardcoded), and poll for file existence
// instead of blind Sleep — SysMain write timing is nondeterministic.
// Note: the scrubber process itself generates a .pf on exit. That entry is handled
// by a trailing glob delete after self-delete via SelfDelete(), which unlinks the
// on-disk binary before ExitProcess, so SysMain won't generate a new entry for it
// after the unlink. Any pre-existing .pf for the scrubber's exe name is swept here.

inline void ScrubOwnPrefetch() {
    // Build %SystemRoot%\Prefetch\<stem>-*.pf
    wchar_t sysroot[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH))
        wcscpy_s(sysroot, L"C:\\Windows"); // safe fallback

    std::wstring exe_name = GetOwnExeName();
    auto dot = exe_name.rfind(L'.');
    std::wstring base_stem = (dot != std::wstring::npos) ? exe_name.substr(0, dot) : exe_name;
    std::wstring pf_dir    = std::wstring(sysroot) + L"\\Prefetch\\";
    std::wstring pattern   = pf_dir + base_stem + L"-*.pf";

    // Poll: wait up to 30 s for the .pf file to appear before attempting delete.
    // SysMain flushes asynchronously; the .pf is usually written within 5-10 s of
    // process termination, but can be delayed on loaded systems.
    bool found = false;
    for (int attempt = 0; attempt < 60 && !found; ++attempt) {
        WIN32_FIND_DATAW probe{};
        HANDLE hProbe = FindFirstFileW(pattern.c_str(), &probe);
        if (hProbe != INVALID_HANDLE_VALUE) {
            FindClose(hProbe);
            found = true;
        } else {
            Sleep(500);
        }
    }

    // Delete all matching .pf files (may be more than one hash variant)
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((pf_dir + fd.cFileName).c_str());
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }
}

// ── 5. Scrub WER Reports (P2 - T17) ────────────────────────────────────────
// Deletes only specific WER report folders referencing our executable.

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
    // %LOCALAPPDATA%\Microsoft\Windows\WER
    wchar_t local_app[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app, MAX_PATH)) {
        std::wstring base = std::wstring(local_app) + L"\\Microsoft\\Windows\\WER\\";
        ScrubWerFolder(base + L"ReportArchive");
        ScrubWerFolder(base + L"ReportQueue");
        // %LOCALAPPDATA%\CrashDumps
        ScrubWerFolder(std::wstring(local_app) + L"\\CrashDumps");
    }

    // C:\ProgramData\Microsoft\Windows\WER (elevated path, needs admin or SE_DEBUG)
    wchar_t prog_data[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", prog_data, MAX_PATH)) {
        std::wstring base = std::wstring(prog_data) + L"\\Microsoft\\Windows\\WER\\";
        ScrubWerFolder(base + L"ReportArchive");
        ScrubWerFolder(base + L"ReportQueue");
    }
}

// ── 6. Scrub Amcache (P2-[10]) ──────────────────────────────────────────────
// SRUM (SRUDB.dat, ESE) is NOT touched — requires ESE API or offline tooling;
// documented as a known gap. Amcache.hve is reachable via RegLoadKey.
//
// Amcache paths that reference our exe:
//   Root\InventoryApplicationFile\<hash>  -- program hash entries
//   Root\File\<vol>\<id>                  -- per-file execution records
//
// Requires SYSTEM or admin. Silently skips if not elevated enough.
inline void ScrubAmcache() {
    EnablePrivilege(SE_BACKUP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);

    // Build path to Amcache.hve
    wchar_t sysroot[MAX_PATH]{};
    GetEnvironmentVariableW(L"SystemRoot", sysroot, MAX_PATH);
    std::wstring hve_path = std::wstring(sysroot) + L"\\AppCompat\\Programs\\Amcache.hve";
    if (!std::filesystem::exists(hve_path)) return;

    constexpr wchar_t kMount[] = L"TMP_BWAMC";

    // Load the hive under HKLM\TMP_BWAMC
    LSTATUS lst = RegLoadKeyW(HKEY_LOCAL_MACHINE, kMount, hve_path.c_str());
    if (lst != ERROR_SUCCESS) return;

    std::wstring exe_lower = ToLower(GetOwnExeName());

    // Helper: open a subkey, enumerate values/subkeys matching our exe, delete them.
    auto scrub_subkeys = [&](const std::wstring& base_path) {
        HKEY hBase = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base_path.c_str(), 0,
                          KEY_READ | KEY_WRITE, &hBase) != ERROR_SUCCESS) return;

        DWORD idx = 0;
        wchar_t sub[512]; DWORD sub_len = 512;
        std::vector<std::wstring> to_delete;

        while (RegEnumKeyExW(hBase, idx, sub, &sub_len, nullptr,
                             nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            std::wstring sname(sub, sub_len);
            // Check if any value inside this subkey references our exe
            HKEY hChild = nullptr;
            if (RegOpenKeyExW(hBase, sname.c_str(), 0, KEY_READ, &hChild) == ERROR_SUCCESS) {
                DWORD vi = 0;
                wchar_t vn[512]; DWORD vn_len = 512;
                wchar_t vd[1024]; DWORD vd_sz = sizeof(vd);
                DWORD vt = 0;
                bool hit = false;
                while (!hit && RegEnumValueW(hChild, vi, vn, &vn_len, nullptr,
                                             &vt, reinterpret_cast<LPBYTE>(vd), &vd_sz)
                                == ERROR_SUCCESS) {
                    if (vt == REG_SZ || vt == REG_EXPAND_SZ) {
                        if (ToLower(std::wstring(vd, vd_sz / sizeof(wchar_t))).find(exe_lower)
                                != std::wstring::npos)
                            hit = true;
                    }
                    ++vi; vn_len = 512; vd_sz = sizeof(vd);
                }
                RegCloseKey(hChild);
                if (hit) to_delete.push_back(sname);
            }
            ++idx; sub_len = 512;
        }

        for (const auto& s : to_delete)
            RegDeleteTreeW(hBase, s.c_str());

        RegCloseKey(hBase);
    };

    std::wstring mount_prefix = std::wstring(L"TMP_BWAMC\\");
    scrub_subkeys(mount_prefix + L"Root\\InventoryApplicationFile");
    scrub_subkeys(mount_prefix + L"Root\\File");

    RegUnLoadKeyW(HKEY_LOCAL_MACHINE, kMount);
}

// ── Scrubber Process Loop (P0 - T4) ─────────────────────────────────────────
// Re-executes the same binary with "--scrub <parent_pid>". Waits for parent
// to exit, allows SysMain to write the prefetch file, then purges all traces
// and self-deletes the executable from disk.

inline void ScrubberMain(DWORD parent_pid, const std::wstring& optional_log_path = L"") {
    if (parent_pid) {
        HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
        if (hParent) {
            WaitForSingleObject(hParent, 120000);
            CloseHandle(hParent);
        }
    }

    // ScrubOwnPrefetch() polls internally (500ms x 60) for the .pf to appear.
    // No blind Sleep needed here.
    EnablePrivilege(SE_DEBUG_NAME);
    ScrubOwnPrefetch();
    ScrubWerReports();
    ScrubBAM();
    ScrubMuiCache();
    ScrubUserAssist();
    ScrubAmcache();

    if (!optional_log_path.empty() && std::filesystem::exists(optional_log_path)) {
        DeleteFileW(optional_log_path.c_str());
    }

    SelfDelete();
    ExitProcess(0);
}

} // namespace bp
