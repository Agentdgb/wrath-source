// main.cpp -- BypassWrath loader.
//
// Compile-time variant:
//   BP_SILENT=1 -> SUBSYSTEM:WINDOWS, no console, no output  (deploy)
//   BP_SILENT=0 -> SUBSYSTEM:CONSOLE, wprintf to stdout       (debug)
//
// CLI flags (both variants, parsed via GetCommandLineW):
//   --scrub <pid>   Child-scrub mode: wait for <pid> to exit, then scrub
//                   Prefetch/WER/BAM/MuiCache/UserAssist and self-delete.
//   --log           Write operational log to %TEMP%\wrath_<ownpid>.log
//                   (cleaned up by ScrubberMain).
//   [path]          (debug only) Path to payload DLL.
//   --txf           (debug only) Force TxF path.
//   --ram           (debug only) Force in-memory path.
//
// Deploy name: RuntimeBroker.exe  (OriginalFilename in version resource must match)

#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#include "ManualMap.h"
#include "ForensicScrub.h"
#include "SelfDelete.h"
#include "resource_ids.h"
#include "build_salt.h"

// ── P4-03: Suppress WER dumps and crash popups ─────────────────────────────
static LONG CALLBACK TopLevelVeh(PEXCEPTION_POINTERS) {
    ExitProcess(0);
    return EXCEPTION_EXECUTE_HANDLER;
}

static void InitWerSuppression() {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32) {
        using pfnWerSetFlags = HRESULT(WINAPI*)(DWORD);
        auto pWer = reinterpret_cast<pfnWerSetFlags>(GetProcAddress(k32, "WerSetFlags"));
        if (pWer) {
            constexpr DWORD kFlags = 0x0001 /*NO_HEAP*/ | 0x0020 /*NO_UI*/;
            pWer(kFlags);
        }
    }
    AddVectoredExceptionHandler(1, TopLevelVeh);
}

// ── Config ──────────────────────────────────────────────────────────────────
static constexpr const wchar_t* kTargetProcess  = L"javaw.exe";
static constexpr const wchar_t* kDefaultPayload = L"bypass_payload.dll";
static constexpr DWORD           kPollMs         = 500;
static constexpr DWORD           kMaxWaitMs      = 60'000;

// ── T22: Optional file log (populated only if --log is present) ─────────────
static std::wstring g_log_path;

static void LogLine(const wchar_t* fmt, ...) {
    if (g_log_path.empty()) return;
    wchar_t buf[1024]{};
    va_list va;
    va_start(va, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, va);
    va_end(va);
    // Append to file
    HANDLE h = CreateFileW(g_log_path.c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(h, buf, static_cast<DWORD>(wcslen(buf) * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(h);
    }
}

// Output: debug console (silent build: no-ops)
#if BP_SILENT
#  define LOG(fmt, ...) LogLine(fmt, ##__VA_ARGS__)
#else
#  define LOG(fmt, ...) do { wprintf(fmt, ##__VA_ARGS__); LogLine(fmt, ##__VA_ARGS__); } while(0)
#endif

// ── NT types needed for PEB/cmdline read (P3-04) ───────────────────────────
struct BW_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct BW_RTL_USER_PROCESS_PARAMETERS {
    BYTE             Reserved1[16];
    PVOID            Reserved2[10];
    BW_UNICODE_STRING ImagePathName;
    BW_UNICODE_STRING CommandLine;
};

struct BW_PEB {
    BYTE  Reserved1[2];
    BYTE  BeingDebugged;
    BYTE  Reserved2[1];
    PVOID Reserved3[2];
    PVOID Ldr;
    BW_RTL_USER_PROCESS_PARAMETERS* ProcessParameters;
};

struct BW_PROCESS_BASIC_INFORMATION {
    PVOID     Reserved1;
    BW_PEB*   PebBaseAddress;
    PVOID     Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID     Reserved3;
};

using pfnNtQIP_t = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);

// ── P3-05: Enable/Drop SeDebugPrivilege ─────────────────────────────────────
static void EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;
    TOKEN_PRIVILEGES tp{};
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege",
                               &tp.Privileges[0].Luid)) {
        tp.PrivilegeCount             = 1;
        tp.Privileges[0].Attributes  = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
}

static void DropDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;
    TOKEN_PRIVILEGES tp{};
    if (LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege",
                               &tp.Privileges[0].Luid)) {
        tp.PrivilegeCount             = 1;
        tp.Privileges[0].Attributes  = SE_PRIVILEGE_REMOVED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
}

// ── T6: Scoring heuristics for Minecraft javaw.exe candidates ───────────────
// Highest score wins. Abort only on tie at top score among 2+ candidates.
struct McCandidate {
    DWORD        pid;
    int          score;
    std::wstring cmdline_fragment;
};

static int ScoreMinecraftCmdLine(const std::wstring& lower) {
    int s = 0;
    // Strong marker: actual Minecraft main class (vanilla or modloaders)
    if (lower.find(L"net.minecraft.client.main.main") != std::wstring::npos ||
        lower.find(L"knotclient") != std::wstring::npos ||
        lower.find(L"launchwrapper") != std::wstring::npos ||
        lower.find(L"bootstraplauncher") != std::wstring::npos ||
        lower.find(L"net.minecraftforge") != std::wstring::npos) s += 3;
    // Native library path indicates game, not launcher JVM
    if (lower.find(L"-djava.library.path") != std::wstring::npos &&
        lower.find(L"natives") != std::wstring::npos) s += 2;
    // .minecraft in cmdline
    if (lower.find(L".minecraft") != std::wstring::npos) s += 2;
    // class path marker (weaker — launchers set this too)
    if (lower.find(L"-djava.class.path") != std::wstring::npos) s += 1;
    // Generic minecraft string as fallback
    if (s == 0 && lower.find(L"minecraft") != std::wstring::npos) s += 1;
    return s;
}

// ── P3-04 + T6: FindMinecraftPids with scoring ──────────────────────────────
static std::vector<McCandidate> FindMinecraftPids() {
    HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
    if (!hNt) return {};
    auto NtQIP = reinterpret_cast<pfnNtQIP_t>(
        GetProcAddress(hNt, "NtQueryInformationProcess"));
    if (!NtQIP) return {};

    std::vector<McCandidate> results;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};

    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return {}; }

    do {
        if (_wcsicmp(pe.szExeFile, L"javaw.exe") != 0) continue;
        DWORD pid = pe.th32ProcessID;

        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE, pid);
        if (!hProc) continue;

        BW_PROCESS_BASIC_INFORMATION pbi{};
        ULONG ret_len = 0;
        LONG st = NtQIP(hProc, 0 /*ProcessBasicInformation*/,
                        &pbi, sizeof(pbi), &ret_len);
        if (st < 0 || !pbi.PebBaseAddress) { CloseHandle(hProc); continue; }

        BW_PEB peb{};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, pbi.PebBaseAddress,
                               &peb, sizeof(peb), &rd)) {
            CloseHandle(hProc); continue;
        }

        BW_RTL_USER_PROCESS_PARAMETERS params{};
        if (!peb.ProcessParameters ||
            !ReadProcessMemory(hProc, peb.ProcessParameters,
                               &params, sizeof(params), &rd)) {
            CloseHandle(hProc); continue;
        }

        USHORT cmd_len = params.CommandLine.Length;
        if (!params.CommandLine.Buffer || cmd_len == 0) {
            CloseHandle(hProc); continue;
        }
        USHORT wchars = static_cast<USHORT>(
            (std::min)(static_cast<DWORD>(cmd_len / sizeof(wchar_t)),
                       static_cast<DWORD>(4096)));
        std::wstring cmdline(wchars, L'\0');
        if (!ReadProcessMemory(hProc, params.CommandLine.Buffer,
                               cmdline.data(), wchars * sizeof(wchar_t), &rd)) {
            CloseHandle(hProc); continue;
        }
        CloseHandle(hProc);

        std::wstring lower = cmdline;
        for (auto& c : lower) c = towlower(c);

        int score = ScoreMinecraftCmdLine(lower);
        if (score == 0) continue; // not a Minecraft JVM

        std::wstring frag = cmdline.substr(0, (std::min)((size_t)120, cmdline.size()));
        results.push_back({ pid, score, std::move(frag) });

    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return results;
}

// Returns the single best candidate, or {0,0,""} on failure.
// P3-[18]: If two candidates share the max score, break tie by lower PID
// (heuristic tie-breaker to avoid aborting on multiple processes)
static McCandidate PickBestCandidate(std::vector<McCandidate>& candidates) {
    if (candidates.empty()) return { 0, 0, L"" };
    if (candidates.size() == 1) return candidates[0];

    // Sort descending by score; on tie, sort ascending by pid
    std::sort(candidates.begin(), candidates.end(),
              [](const McCandidate& a, const McCandidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.pid < b.pid;
              });

    return candidates[0];
}

// ── File helpers ─────────────────────────────────────────────────────────────
static std::vector<uint8_t> LoadFile(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    auto sep = p.rfind(L'\\');
    return (sep != std::wstring::npos) ? p.substr(0, sep) : L".";
}

static std::wstring ExePath() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::wstring(buf);
}

// ── P3-06: PE validation ─────────────────────────────────────────────────────
static bool ValidatePayloadPE(const uint8_t* buf, size_t sz, std::wstring& err) {
    if (sz < sizeof(IMAGE_DOS_HEADER)) { err = L"PE: demasiado pequeno."; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err = L"PE: firma DOS invalida."; return false; }
    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > sz) { err = L"PE: e_lfanew fuera de rango."; return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { err = L"PE: firma NT invalida."; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        err = L"PE: payload no es x64."; return false;
    }
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        err = L"PE: payload no tiene IMAGE_FILE_DLL."; return false;
    }
    // T9: Static TLS verification — manually mapped payload must not have static TLS directory
    if (nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size > 0) {
        err = L"PE: payload contiene directorio TLS estatico (.tls). No compatible con manual map en JVM.";
        return false;
    }
    // T7: Verify loader itself is not WoW64
    USHORT procMachine = 0, nativeMachine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &procMachine, &nativeMachine)) {
        if (procMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            err = L"PE: loader esta en WoW64 -- usar binario x64."; return false;
        }
    }
    return true;
}

// ── T7: Validate target is native x64, not WoW64 ─────────────────────────────
static bool ValidateTargetArch(HANDLE hProc, std::wstring& err) {
    USHORT procMachine = 0, nativeMachine = 0;
    if (IsWow64Process2(hProc, &procMachine, &nativeMachine)) {
        if (procMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            err = L"Target es WoW64 (32-bit) -- no compatible con loader x64.";
            return false;
        }
    }
    return true;
}

// ── T15: Try OpenProcess without SeDebug first ───────────────────────────────
// Returns valid handle or nullptr. Sets *elevated if SeDebug was needed.
static HANDLE OpenTargetProcess(DWORD pid, bool* elevated) {
    constexpr DWORD kAccessMask =
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION;

    *elevated = false;

    // First attempt: without SeDebug
    HANDLE h = OpenProcess(kAccessMask, FALSE, pid);
    if (h) return h;

    DWORD err = GetLastError();
    if (err != ERROR_ACCESS_DENIED) return nullptr; // some other error

    // Second attempt: with SeDebug
    EnableDebugPrivilege();
    *elevated = true;
    h = OpenProcess(kAccessMask, FALSE, pid);
    return h; // nullptr if still fails
}

// ── T4: Scrub child launcher ─────────────────────────────────────────────────
// Spawns a detached child process with --scrub <own_pid> [--log <path>].
// The child waits for us to exit, then scrubs Prefetch and self-deletes.
static void LaunchScrubChild(const std::wstring& log_path) {
    wchar_t own_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, own_path, MAX_PATH);

    DWORD own_pid = GetCurrentProcessId();

    std::wstring cmdline = L"\"";
    cmdline += own_path;
    cmdline += L"\" --scrub ";
    cmdline += std::to_wstring(own_pid);
    if (!log_path.empty()) {
        cmdline += L" --log \"";
        cmdline += log_path;
        cmdline += L"\"";
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // CREATE_NO_WINDOW + DETACHED_PROCESS: child survives our exit
    if (CreateProcessW(own_path, cmdline.data(),
                       nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// ── T4: Parse --scrub <pid> and optional --log <path> from cmdline ───────────
// Returns true if --scrub was found; sets out_pid and out_log.
static bool ParseScrubArgs(const wchar_t* cmdline, DWORD* out_pid, std::wstring* out_log) {
    if (!cmdline) return false;
    *out_pid = 0;
    out_log->clear();

    // Simple linear scan; not using CommandLineToArgvW to avoid shell32 load
    const wchar_t* p = cmdline;

    // skip executable token (may be quoted)
    if (*p == L'"') {
        ++p;
        while (*p && *p != L'"') ++p;
        if (*p) ++p;
    } else {
        while (*p && *p != L' ') ++p;
    }

    while (*p == L' ') ++p;

    // tokenise rest
    std::vector<std::wstring> tokens;
    while (*p) {
        std::wstring tok;
        bool quoted = (*p == L'"');
        if (quoted) ++p;
        while (*p && (quoted ? *p != L'"' : *p != L' ')) tok += *p++;
        if (quoted && *p) ++p;
        while (*p == L' ') ++p;
        tokens.push_back(std::move(tok));
    }

    bool found_scrub = false;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (_wcsicmp(tokens[i].c_str(), L"--scrub") == 0) {
            if (i + 1 < tokens.size()) {
                *out_pid = static_cast<DWORD>(_wtoi(tokens[i + 1].c_str()));
                ++i;
                found_scrub = true;
            }
        } else if (_wcsicmp(tokens[i].c_str(), L"--log") == 0) {
            if (i + 1 < tokens.size()) {
                *out_log = tokens[i + 1];
                ++i;
            }
        }
    }
    return found_scrub && (*out_pid != 0);
}

// ── T22: Build log path from %TEMP%\wrath_<pid>.log ──────────────────────────
static std::wstring BuildLogPath() {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring path(temp);
    path += L"wrath_";
    path += std::to_wstring(GetCurrentProcessId());
    path += L".log";
    return path;
}

// ── T22: Check if --log flag present in cmdline ──────────────────────────────
static bool ParseLogFlag(const wchar_t* cmdline) {
    if (!cmdline) return false;
    return (wcsstr(cmdline, L"--log") != nullptr);
}

// ── P3-[18]: Parse --pid <n> flag ───────────────────────────────────────────
static DWORD ParsePidFlag(const wchar_t* cmdline) {
    if (!cmdline) return 0;
    const wchar_t* p = wcsstr(cmdline, L"--pid");
    if (!p) return 0;
    p += 5;
    while (*p == L' ' || *p == L'=') ++p;
    return static_cast<DWORD>(_wtoi(p));
}

// ── Core injection flow (shared by both variants) ────────────────────────────
// Returns 0 on success, non-zero on failure.
static int RunInject(std::vector<uint8_t>& dll_bytes, DWORD explicit_pid = 0) {
    // P3-06: validate PE
    std::wstring pe_err;
    if (!ValidatePayloadPE(dll_bytes.data(), dll_bytes.size(), pe_err)) {
        LOG(L"[!] %s\n", pe_err.c_str());
        return 1;
    }

    McCandidate best{ 0, 0, L"" };
    if (explicit_pid != 0) {
        // Bug 57: Validate that explicit_pid actually belongs to javaw.exe
        HANDLE hCheck = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, explicit_pid);
        if (!hCheck) {
            LOG(L"[!] --pid %u: no se pudo abrir el proceso (error=%u).\n", explicit_pid, GetLastError());
            return 1;
        }
        wchar_t img_path[MAX_PATH]{};
        DWORD sz = MAX_PATH;
        bool is_javaw = false;
        if (QueryFullProcessImageNameW(hCheck, 0, img_path, &sz)) {
            std::wstring lower = img_path;
            for (auto& c : lower) c = towlower(c);
            if (lower.size() >= 9 && lower.compare(lower.size() - 9, 9, L"javaw.exe") == 0)
                is_javaw = true;
        }
        CloseHandle(hCheck);
        if (!is_javaw) {
            LOG(L"[!] --pid %u no corresponde a javaw.exe (proceso: %s). Abortando.\n", explicit_pid, img_path);
            return 1;
        }
        best.pid = explicit_pid;
        best.score = 100;
        LOG(L"[*] Objetivo verificado via --pid: javaw.exe PID=%u\n", explicit_pid);
    } else {
        // T5: polling loop — wait up to kMaxWaitMs for Minecraft to appear
        LOG(L"[*] Buscando javaw.exe con Minecraft...\n");
        std::vector<McCandidate> candidates;
        DWORD waited = 0;
        while (true) {
            candidates = FindMinecraftPids();
            if (!candidates.empty()) break;
            if (waited >= kMaxWaitMs) {
                LOG(L"[!] Timeout: no se encontro javaw.exe con Minecraft en %u ms.\n", kMaxWaitMs);
                return 1;
            }
            Sleep(kPollMs);
            waited += kPollMs;
        }

        // T6: pick best candidate by score (P3-[18]: tie broken by lower PID)
        best = PickBestCandidate(candidates);
        // Require minimum score of 2 Minecraft markers (allows modloaders and custom launchers)
        if (best.pid == 0 || best.score < 2) {
            LOG(L"[!] No se encontro un javaw.exe con suficientes marcadores de Minecraft (score=%d < 2).\n", best.score);
            return 1;
        }
        LOG(L"[+] Objetivo: javaw.exe PID=%u (score=%d)\n", best.pid, best.score);
    }

    // T15: try without SeDebug first
    bool elevated = false;
    HANDLE hProc = OpenTargetProcess(best.pid, &elevated);
    if (!hProc) {
        LOG(L"[!] OpenProcess fallo (error=%u). Ejecuta como Admin.\n", GetLastError());
        return 1;
    }
    LOG(L"[*] SeDebug: %s\n", elevated ? L"elevado" : L"no necesario");

    // T7: verify target is native x64
    std::wstring arch_err;
    if (!ValidateTargetArch(hProc, arch_err)) {
        LOG(L"[!] %s\n", arch_err.c_str());
        CloseHandle(hProc);
        if (elevated) DropDebugPrivilege();
        return 1;
    }

    std::wstring status;
    LOG(L"[*] ManualMap (SEC_IMAGE + POSIX delete)...\n");
    bool ok = bp::ManualMapTarget(hProc, dll_bytes.data(), dll_bytes.size(), status);

    CloseHandle(hProc);

    // P3-05: drop privilege immediately after inject
    if (elevated) DropDebugPrivilege();

    LOG(L"[%s] %s\n", ok ? L"+" : L"!", status.c_str());
    return ok ? 0 : 2;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Entry points
// ═══════════════════════════════════════════════════════════════════════════════

#if BP_SILENT
// ── Silent variant (RuntimeBroker.exe) ──────────────────────────────────────
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    InitWerSuppression();

    const wchar_t* cmdline = GetCommandLineW();

    // T4: --scrub child mode — handle before anything else
    DWORD scrub_pid = 0;
    std::wstring scrub_log;
    if (ParseScrubArgs(cmdline, &scrub_pid, &scrub_log)) {
        bp::ScrubberMain(scrub_pid, scrub_log);
        return 0;
    }

    // P3-[15]: Clean up any orphaned log files from previous crashed runs.
    {
        wchar_t tmp_dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp_dir);
        std::wstring pattern = std::wstring(tmp_dir) + L"wrath_*.log";
        WIN32_FIND_DATAW fd{};
        HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do { DeleteFileW((std::wstring(tmp_dir) + fd.cFileName).c_str()); }
            while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
    }

    // T22: --log flag
    if (ParseLogFlag(cmdline)) {
        g_log_path = BuildLogPath();
    }

    // P3-[18]: --pid <n> flag
    DWORD explicit_pid = ParsePidFlag(cmdline);

    // Load encrypted payload from RCDATA resource (zero disk touch)
    std::vector<uint8_t> dll_bytes;
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD_BIN), MAKEINTRESOURCEW(10));
    if (hRes) {
        HGLOBAL hGlob = LoadResource(nullptr, hRes);
        if (hGlob) {
            LPVOID pData = LockResource(hGlob);
            DWORD resSz  = SizeofResource(nullptr, hRes);
            if (pData && resSz > 0) {
                dll_bytes.resize(resSz);
                memcpy(dll_bytes.data(), pData, resSz);
                bp::DecryptPayload(dll_bytes.data(), dll_bytes.size());
            }
        }
    }

    // In silent deploy, payload must come from encrypted RCDATA resource
    if (dll_bytes.empty()) return 1;

    // P3-[13]: Launch scrub child BEFORE inject. The child waits for our PID to die,
    // so the timing is identical from the child's perspective — but if inject crashes
    // mid-flight, cleanup still runs regardless.
    LaunchScrubChild(g_log_path);

    int result = RunInject(dll_bytes, explicit_pid);

    // Zero our payload bytes before exit.
    SecureZeroMemory(dll_bytes.data(), dll_bytes.size());

    return result;
}

#else
// ── Debug variant (BypassLoaderDbg.exe) ─────────────────────────────────────
int wmain(int argc, wchar_t** argv) {
    InitWerSuppression();

    const wchar_t* cmdline = GetCommandLineW();

    // T4: --scrub child mode — handle before anything else
    DWORD scrub_pid = 0;
    std::wstring scrub_log;
    if (ParseScrubArgs(cmdline, &scrub_pid, &scrub_log)) {
        bp::ScrubberMain(scrub_pid, scrub_log);
        return 0;
    }

    // T22: --log flag
    if (ParseLogFlag(cmdline)) {
        g_log_path = BuildLogPath();
        LOG(L"[*] Log: %s\n", g_log_path.c_str());
    }

    std::wstring payload_path;

    // P3-[18]: --pid <n> flag
    DWORD explicit_pid = ParsePidFlag(cmdline);

    for (int i = 1; i < argc; ++i) {
        if      (_wcsicmp(argv[i], L"--log") == 0)   { /* already handled */ }
        else if (_wcsicmp(argv[i], L"--scrub") == 0) { ++i; /* already handled */ }
        else if (_wcsicmp(argv[i], L"--pid") == 0)   { ++i; /* already handled */ }
        else payload_path = argv[i];
    }

    if (payload_path.empty())
        payload_path = ExeDir() + L"\\" + kDefaultPayload;

    std::vector<uint8_t> dll_bytes;
    if (std::filesystem::exists(payload_path)) {
        dll_bytes = LoadFile(payload_path);
        LOG(L"[+] Payload desde disco: %s (%zu bytes)\n",
            payload_path.c_str(), dll_bytes.size());
    } else {
        HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_PAYLOAD_BIN), MAKEINTRESOURCEW(10));
        if (hRes) {
            HGLOBAL hGlob = LoadResource(nullptr, hRes);
            if (hGlob) {
                LPVOID pData = LockResource(hGlob);
                DWORD resSz  = SizeofResource(nullptr, hRes);
                if (pData && resSz > 0) {
                    dll_bytes.resize(resSz);
                    memcpy(dll_bytes.data(), pData, resSz);
                    bp::DecryptPayload(dll_bytes.data(), dll_bytes.size());
                    LOG(L"[+] Payload desde RCDATA (%zu bytes)\n", dll_bytes.size());
                }
            }
        }
    }

    if (dll_bytes.empty()) {
        LOG(L"[!] Payload no encontrado: %s\n", payload_path.c_str());
        LOG(L"    Uso: BypassLoaderDbg.exe [ruta_dll] [--log]\n");
        return 1;
    }

    // P3-[13]: Launch scrub child before inject (same logic as silent variant)
    LaunchScrubChild(g_log_path);

    int result = RunInject(dll_bytes, explicit_pid);

    SecureZeroMemory(dll_bytes.data(), dll_bytes.size());
    if (result == 0) {
        LOG(L"[+] Listo. Presiona Enter para salir.\n");
    } else {
        LOG(L"[!] Fallo (codigo=%d). Presiona Enter para salir.\n", result);
    }
    getchar();
    return result;
}
#endif
