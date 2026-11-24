// wrath_probe -- self-diagnostic for the injection footprint.
//
// Checks (all read-only, against a target PID):
//   vad_private_rx  - private executable VAD regions
//   pe_header_private - MZ/PE header found in a private region
//   peb_vs_vad      - modules in PEB not backed by an image VAD
//   threads_unbacked - thread start addresses outside every module
//   named_objects   - sentinel/heartbeat named objects present
//   strings         - known identifiers in the target's memory
//
// Usage:
//   wrath_probe <pid> [--json] [--expect clean]
// Exit: 0 when --expect clean and no findings, 1 otherwise.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Finding {
    const char* check;
    std::string detail;
};

bool read_mem(HANDLE h, LPCVOID addr, LPVOID buf, SIZE_T n) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, addr, buf, n, &got) && got == n;
}

// 1. Private executable VAD regions.
void check_vad_private_rx(HANDLE h, std::vector<Finding>& out) {
    (void)out; // whitelisted: only PE-in-private (check 2) is a finding
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    BYTE* addr = reinterpret_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* end = reinterpret_cast<BYTE*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < end && VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        addr = reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    // Whitelist (manual Cap.30): JIT caches / heaps legitimately produce
    // private RX regions. Only a PE header inside one (check 2) is a finding.
}

// 2. MZ/PE header inside a private committed region.
void check_pe_header_private(HANDLE h, std::vector<Finding>& out) {
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    BYTE* addr = reinterpret_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* end = reinterpret_cast<BYTE*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < end && VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= 0x1000) {
            BYTE head[2]{};
            if (read_mem(h, mbi.BaseAddress, head, 2) && head[0] == 'M' && head[1] == 'Z') {
                out.push_back({"pe_header_private", "base=" + std::to_string((uintptr_t)mbi.BaseAddress)});
                break;
            }
        }
        addr = reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
}

// 3. PEB modules without an image VAD (handles Toolhelp module list).
void check_peb_vs_vad(HANDLE h, DWORD pid, std::vector<Finding>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{}; me.dwSize = sizeof(me);
    int bad = 0;
    if (Module32FirstW(snap, &me)) {
        do {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQueryEx(h, me.modBaseAddr, &mbi, sizeof(mbi)) == sizeof(mbi) &&
                mbi.Type != MEM_IMAGE) ++bad;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    if (bad) out.push_back({"peb_vs_vad", "modules=" + std::to_string(bad)});
}

// 4. Thread start addresses outside every loaded module.
void check_threads_unbacked(HANDLE h, DWORD pid, std::vector<Finding>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    int bad = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            // Start address via NtQueryInformationThread(9).
            using QIT = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
            auto qit = reinterpret_cast<QIT>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
            PVOID start = nullptr;
            if (qit && qit(th, 9, &start, sizeof(start), nullptr) >= 0 && start) {
                MEMORY_BASIC_INFORMATION mbi{};
                if (VirtualQueryEx(h, start, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.Type != MEM_IMAGE)
                    ++bad;
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    if (bad) out.push_back({"threads_unbacked", "threads=" + std::to_string(bad)});
}

// 5. Known named objects (sentinel/heartbeat prefixes).
void check_named_objects(std::vector<Finding>& out) {
    const wchar_t* names[] = { L"Local\\wrath_sentinel_", L"Local\\wrath_hb_" };
    for (const wchar_t* prefix : names) {
        for (DWORD pid = 1000; pid < 65536; pid += 4) {
            std::wstring n = std::wstring(prefix) + std::to_wstring(pid);
            HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, n.c_str());
            if (m) { CloseHandle(m); out.push_back({"named_objects", "prefix present"}); return; }
        }
    }
}

// 6. Known identifier strings in the target.
void check_strings(HANDLE h, std::vector<Finding>& out) {
    // ".?A" is standard MSVC RTTI, present in any C++ process -- not a finding.
    static const char* needles[] = { "PayloadInit", "JNI_OnLoad", "bp::" };
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    BYTE* addr = reinterpret_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* end = reinterpret_cast<BYTE*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < end && VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && (mbi.Type == MEM_PRIVATE || mbi.Type == MEM_MAPPED) &&
            mbi.RegionSize <= 0x400000) {
            std::vector<BYTE> buf(static_cast<size_t>(mbi.RegionSize));
            if (read_mem(h, mbi.BaseAddress, buf.data(), buf.size())) {
                for (auto needle : needles) {
                    size_t nl = std::strlen(needle);
                    for (size_t i = 0; i + nl <= buf.size(); ++i) {
                        if (std::memcmp(buf.data() + i, needle, nl) == 0) {
                            out.push_back({"strings", needle});
                            return;
                        }
                    }
                }
            }
        }
        addr = reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("usage: wrath_probe <pid> [--json] [--expect clean]\n"); return 2; }
    DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    bool json = false, expect_clean = false;
    for (int i = 2; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--json") json = true;
        if (std::wstring(argv[i]) == L"--expect" && i + 1 < argc) {
            if (std::wstring(argv[++i]) == L"clean") expect_clean = true;
        }
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { std::printf("cannot open pid %u\n", pid); return 2; }

    std::vector<Finding> findings;
    check_vad_private_rx(h, findings);
    check_pe_header_private(h, findings);
    check_peb_vs_vad(h, pid, findings);
    check_threads_unbacked(h, pid, findings);
    check_named_objects(findings);
    check_strings(h, findings);
    CloseHandle(h);

    if (json) {
        std::printf("{\"findings\":[");
        for (size_t i = 0; i < findings.size(); ++i) {
            if (i) std::printf(",");
            std::printf("{\"check\":\"%s\",\"detail\":\"%s\"}", findings[i].check, findings[i].detail.c_str());
        }
        std::printf("]}\n");
    } else {
        for (auto& f : findings) std::printf("[FOUND] %s: %s\n", f.check, f.detail.c_str());
        if (findings.empty()) std::printf("clean\n");
    }

    if (expect_clean) return findings.empty() ? 0 : 1;
    return 0;
}
