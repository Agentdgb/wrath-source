// main.cpp -- BypassWrath loader (CLI, no ImGui, no auth).
// Usage: BypassLoader.exe [path_to_payload.dll]
// If no argument: looks for "bypass_payload.dll" next to the exe.
// Injection order: ManualMapInMemory (primary) → ManualMapViaTxf (fallback).
// Target: javaw.exe (Minecraft JVM).

#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <filesystem>

#include "ManualMap.h"

// ── Config ─────────────────────────────────────────────────────────────────
static constexpr const wchar_t* kTargetProcess   = L"javaw.exe";
static constexpr const wchar_t* kDefaultPayload  = L"bypass_payload.dll";
static constexpr DWORD           kPollMs          = 500;    // scan interval
static constexpr DWORD           kMaxWaitMs       = 60000;  // 60s max wait for target

// ── Process helpers ─────────────────────────────────────────────────────────

static DWORD FindPid(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static std::vector<uint8_t> ReadFile(const std::wstring& path) {
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

// ── Entry ───────────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t** argv) {
    // Payload path: argv[1] or exe_dir\bypass_payload.dll
    std::wstring payload_path;
    if (argc >= 2) {
        payload_path = argv[1];
    } else {
        payload_path = ExeDir() + L"\\" + kDefaultPayload;
    }

    // Verify payload exists
    if (!std::filesystem::exists(payload_path)) {
        wprintf(L"[!] Payload no encontrado: %s\n", payload_path.c_str());
        wprintf(L"    Uso: BypassLoader.exe [ruta_al_dll]\n");
        return 1;
    }

    // Load payload bytes
    auto dll_bytes = ReadFile(payload_path);
    if (dll_bytes.empty()) {
        wprintf(L"[!] No se pudo leer: %s\n", payload_path.c_str());
        return 1;
    }
    wprintf(L"[+] Payload: %s (%zu bytes)\n", payload_path.c_str(), dll_bytes.size());

    // Wait for target
    wprintf(L"[*] Esperando %s ...\n", kTargetProcess);
    DWORD pid = 0;
    for (DWORD waited = 0; waited < kMaxWaitMs && !pid; waited += kPollMs) {
        pid = FindPid(kTargetProcess);
        if (!pid) Sleep(kPollMs);
    }
    if (!pid) {
        wprintf(L"[!] %s no encontrado en %u ms.\n", kTargetProcess, kMaxWaitMs);
        return 1;
    }
    wprintf(L"[+] Objetivo: %s PID=%u\n", kTargetProcess, pid);

    // Open with minimal rights (PROCESS_VM_OPERATION + PROCESS_VM_WRITE +
    // PROCESS_VM_READ + PROCESS_CREATE_THREAD + PROCESS_QUERY_LIMITED_INFORMATION)
    HANDLE hProc = OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        wprintf(L"[!] OpenProcess fallo (error=%u). Ejecuta como Admin.\n",
            GetLastError());
        return 1;
    }

    std::wstring status;

    // Primary: ManualMap RAM
    wprintf(L"[*] Intentando ManualMap (RAM)...\n");
    bool ok = bp::ManualMapInMemory(hProc, dll_bytes.data(), dll_bytes.size(), status);
    if (!ok) {
        wprintf(L"[-] ManualMap RAM fallo: %s\n", status.c_str());
        wprintf(L"[*] Intentando ManualMap (TxF)...\n");
        ok = bp::ManualMapViaTxf(hProc, dll_bytes.data(), dll_bytes.size(), status);
    }

    CloseHandle(hProc);

    if (ok) {
        wprintf(L"[+] %s\n", status.c_str());
        wprintf(L"[+] Listo. El payload vive en javaw.exe\n");
        return 0;
    } else {
        wprintf(L"[!] Fallo: %s\n", status.c_str());
        return 2;
    }
}
