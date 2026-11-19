// main.cpp -- BypassWrath loader.
//
// Compile-time variant:
//   BP_SILENT=1 → SUBSYSTEM:WINDOWS, no console, no output  (deploy)
//   BP_SILENT=0 → SUBSYSTEM:CONSOLE, wprintf to stdout       (debug)
//
// CLI flags (debug variant only; silent variant ignores argv):
//   [path]   Path to payload DLL. Default: bypass_payload.dll next to exe.
//   --txf    Use TxF (SEC_IMAGE) path instead of RAM path.
//            TxF is NOT the automatic fallback; it must be requested explicitly.
//
// Deploy name: wmic.exe  (OriginalFilename in version resource must match)

#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#include "ManualMap.h"

// ── Config ──────────────────────────────────────────────────────────────────
static constexpr const wchar_t* kTargetProcess  = L"javaw.exe";
static constexpr const wchar_t* kDefaultPayload = L"bypass_payload.dll";
static constexpr DWORD           kPollMs         = 500;
static constexpr DWORD           kMaxWaitMs      = 60000;

// Minecraft command-line markers (case-insensitive wcsstr search).
// A javaw.exe that has neither is not a Minecraft instance.
static constexpr const wchar_t* kMcMarker1 = L"-Djava.class.path";
static constexpr const wchar_t* kMcMarker2 = L"minecraft";

// ── Output (no-ops in silent build) ────────────────────────────────────────
#if BP_SILENT
#  define LOG(fmt, ...) (void)0
#else
#  define LOG(fmt, ...) wprintf(fmt, ##__VA_ARGS__)
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
    // rest not needed
};

struct BW_PROCESS_BASIC_INFORMATION {
    PVOID  Reserved1;
    BW_PEB* PebBaseAddress;
    PVOID  Reserved2[2];
    ULONG_PTR UniqueProcessId;
    PVOID  Reserved3;
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
        tp.PrivilegeCount              = 1;
        tp.Privileges[0].Attributes   = SE_PRIVILEGE_ENABLED;
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
        tp.PrivilegeCount              = 1;
        tp.Privileges[0].Attributes   = SE_PRIVILEGE_REMOVED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(hToken);
}

// ── P3-04: FindPid with Minecraft command-line filter ───────────────────────
// Reads PEB.ProcessParameters.CommandLine for every javaw.exe via
// NtQueryInformationProcess + ReadProcessMemory.
// Returns 0 on 0 or >1 candidates (logs them).
struct McCandidate { DWORD pid; std::wstring cmdline_fragment; };

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

        // Open with minimal rights for PEB read
        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
            FALSE, pid);
        if (!hProc) continue;

        BW_PROCESS_BASIC_INFORMATION pbi{};
        ULONG ret_len = 0;
        LONG st = NtQIP(hProc, 0 /*ProcessBasicInformation*/,
                        &pbi, sizeof(pbi), &ret_len);
        if (st < 0 || !pbi.PebBaseAddress) { CloseHandle(hProc); continue; }

        // Read PEB
        BW_PEB peb{};
        SIZE_T rd = 0;
        if (!ReadProcessMemory(hProc, pbi.PebBaseAddress,
                               &peb, sizeof(peb), &rd)) {
            CloseHandle(hProc); continue;
        }

        // Read RTL_USER_PROCESS_PARAMETERS pointer
        BW_RTL_USER_PROCESS_PARAMETERS params{};
        if (!peb.ProcessParameters ||
            !ReadProcessMemory(hProc, peb.ProcessParameters,
                               &params, sizeof(params), &rd)) {
            CloseHandle(hProc); continue;
        }

        // Read CommandLine buffer (cap at 4096 wchars)
        USHORT cmd_len = params.CommandLine.Length; // bytes
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

        // Case-insensitive check: either marker must appear
        std::wstring lower = cmdline;
        for (auto& c : lower) c = towlower(c);
        bool is_mc = (lower.find(L"-djava.class.path") != std::wstring::npos)
                  || (lower.find(L"minecraft")         != std::wstring::npos);
        if (!is_mc) continue;

        // Keep first 120 chars as fragment for disambiguation log
        std::wstring frag = cmdline.substr(0, (std::min)((size_t)120, cmdline.size()));
        results.push_back({ pid, std::move(frag) });

    } while (Process32NextW(snap, &pe));

    CloseHandle(snap);
    return results;
}

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

// ── P3-06: PE validation ─────────────────────────────────────────────────────
static bool ValidatePayloadPE(const uint8_t* buf, size_t sz, std::wstring& err) {
    if (sz < sizeof(IMAGE_DOS_HEADER)) { err = L"PE: demasiado pequeño."; return false; }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(buf);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err = L"PE: firma DOS invalida."; return false; }
    if (dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > sz) { err = L"PE: e_lfanew fuera de rango."; return false; }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(buf + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { err = L"PE: firma NT invalida."; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        err = L"PE: payload no es x64 (Machine mismatch)."; return false;
    }
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
        err = L"PE: payload no tiene IMAGE_FILE_DLL."; return false;
    }
    // Loader must NOT be WoW64 (would fail to open an x64 process)
    USHORT procMachine = 0, nativeMachine = 0;
    if (IsWow64Process2(GetCurrentProcess(), &procMachine, &nativeMachine)) {
        if (procMachine != IMAGE_FILE_MACHINE_UNKNOWN) {
            err = L"PE: loader esta en WoW64 -- usar binario x64."; return false;
        }
    }
    return true;
}

// ── Entry ────────────────────────────────────────────────────────────────────

#if BP_SILENT
// Silent variant: WinMain, no console, no argv parsing.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::wstring payload_path = ExeDir() + L"\\" + kDefaultPayload;
    if (!std::filesystem::exists(payload_path)) return 1;

    auto dll_bytes = LoadFile(payload_path);
    if (dll_bytes.empty()) return 1;

    std::wstring pe_err;
    if (!ValidatePayloadPE(dll_bytes.data(), dll_bytes.size(), pe_err)) return 1;

    // P3-05
    EnableDebugPrivilege();

    // P3-04: find exactly one Minecraft javaw.exe
    auto candidates = FindMinecraftPids();
    if (candidates.size() != 1) { DropDebugPrivilege(); return 1; }
    DWORD pid = candidates[0].pid;

    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!hProc) { DropDebugPrivilege(); return 1; }

    std::wstring status;
    bool ok = bp::ManualMapInMemory(hProc, dll_bytes.data(), dll_bytes.size(), status);
    CloseHandle(hProc);

    // P3-05: drop privilege immediately after inject
    DropDebugPrivilege();
    return ok ? 0 : 1;
}

#else
// Debug variant: wmain, console, full output, --txf flag.
int wmain(int argc, wchar_t** argv) {
    std::wstring payload_path;
    bool use_txf = false;

    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--txf") == 0) {
            use_txf = true;
        } else {
            payload_path = argv[i];
        }
    }
    if (payload_path.empty())
        payload_path = ExeDir() + L"\\" + kDefaultPayload;

    if (!std::filesystem::exists(payload_path)) {
        LOG(L"[!] Payload no encontrado: %s\n", payload_path.c_str());
        LOG(L"    Uso: BypassLoaderDbg.exe [ruta_dll] [--txf]\n");
        return 1;
    }

    auto dll_bytes = LoadFile(payload_path);
    if (dll_bytes.empty()) {
        LOG(L"[!] No se pudo leer: %s\n", payload_path.c_str());
        return 1;
    }
    LOG(L"[+] Payload: %s (%zu bytes)\n", payload_path.c_str(), dll_bytes.size());

    // P3-06: validate PE before touching any process
    std::wstring pe_err;
    if (!ValidatePayloadPE(dll_bytes.data(), dll_bytes.size(), pe_err)) {
        LOG(L"[!] %s\n", pe_err.c_str());
        return 1;
    }

    LOG(L"[*] Ruta de inyeccion: %s\n", use_txf ? L"TxF (SEC_IMAGE)" : L"RAM (ManualMap)");

    // P3-05: elevate before OpenProcess
    EnableDebugPrivilege();

    // P3-04: find Minecraft javaw.exe by command line
    LOG(L"[*] Buscando javaw.exe con Minecraft...\n");
    auto candidates = FindMinecraftPids();

    if (candidates.empty()) {
        LOG(L"[!] No se encontro javaw.exe con Minecraft en cmdline.\n");
        DropDebugPrivilege();
        return 1;
    }
    if (candidates.size() > 1) {
        LOG(L"[!] Multiples instancias de Minecraft -- ambiguo:\n");
        for (auto& c : candidates)
            LOG(L"    PID=%u  %s\n", c.pid, c.cmdline_fragment.c_str());
        DropDebugPrivilege();
        return 1;
    }
    DWORD pid = candidates[0].pid;
    LOG(L"[+] Objetivo: javaw.exe PID=%u\n", pid);

    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        LOG(L"[!] OpenProcess fallo (error=%u). Ejecuta como Admin.\n", GetLastError());
        DropDebugPrivilege();
        return 1;
    }

    std::wstring status;
    bool ok = false;

    if (use_txf) {
        LOG(L"[*] ManualMap TxF...\n");
        ok = bp::ManualMapViaTxf(hProc, dll_bytes.data(), dll_bytes.size(), status);
    } else {
        LOG(L"[*] ManualMap RAM...\n");
        ok = bp::ManualMapInMemory(hProc, dll_bytes.data(), dll_bytes.size(), status);
    }

    CloseHandle(hProc);

    // P3-05: drop privilege after inject
    DropDebugPrivilege();

    if (ok) {
        LOG(L"[+] %s\n", status.c_str());
        LOG(L"[+] Listo. Presiona Enter para salir.\n");
        getchar();
        return 0;
    } else {
        LOG(L"[!] Fallo: %s\n", status.c_str());
        LOG(L"    Presiona Enter para salir.\n");
        getchar();
        return 2;
    }
}
#endif
