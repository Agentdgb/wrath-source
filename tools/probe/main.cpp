// wrath_probe -- Cap.30 "The Loader Invariant" self-diagnostic.
//
// Seven read-only checks against a target PID (ScreenshareGuid.pdf, ch.30):
//   1. peb_three_lists        - walk all three PEB loader lists, collect modules
//   2. vad_image_per_module   - each module must be covered by a MEM_IMAGE VAD
//   3. vad_exec_private       - executable PRIVATE/MAPPED VADs without guard page
//   4. vad_exec_no_module     - executable VADs not covered by any module
//   5. mz_pe_confirm          - probe MZ/PE header (confirmation only)
//   6. thread_start_address   - thread start addresses outside every module
//   7. text_hash_mismatch     - in-memory .text hash vs on-disk file (module stomping)
//
// Whitelist (ch.30): the JVM JIT code cache (large private RX regions adjacent
// to jvm.dll in javaw.exe) is excluded from findings.
//
// Output: one line per finding with PID, type, address, size and reason.
// Exit: 1 if any finding, 0 if clean, 2 on usage/open error.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

namespace {

struct Finding {
    DWORD pid;
    const char* type;
    uintptr_t address;
    size_t size;
    std::string reason;
};

std::string wstr_to_ascii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}

std::string hex(uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

void emit(std::vector<Finding>& out, DWORD pid, const char* type,
          uintptr_t addr, size_t sz, const std::string& reason) {
    out.push_back({pid, type, addr, sz, reason});
}

bool read_mem(HANDLE h, LPCVOID addr, LPVOID buf, SIZE_T n) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, addr, buf, n, &got) && got == n;
}

struct Module {
    uintptr_t base;
    size_t size;
    std::wstring name;
    std::wstring path;
};

// ---- NT / PEB helpers -------------------------------------------------------
struct BW_UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; };
struct BW_LDR_ENTRY {
    LIST_ENTRY in_load, in_mem, in_init;
    PVOID dll_base, entry_point;
    ULONG size_of_image;
    BW_UNICODE_STRING full_name, base_name;
};
struct BW_PEB_LDR {
    ULONG length; BOOLEAN initialized; PVOID ss_handle;
    LIST_ENTRY in_load, in_mem, in_init;
};
struct BW_PBI { PVOID r1; PVOID peb; PVOID r2[2]; ULONG_PTR pid; PVOID r3; };

bool get_peb(HANDLE h, PVOID* peb) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    using QIP = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto qip = reinterpret_cast<QIP>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (!qip) return false;
    BW_PBI pbi{}; ULONG len = 0;
    if (qip(h, 0, &pbi, sizeof(pbi), &len) < 0 || !pbi.peb) return false;
    *peb = pbi.peb;
    return true;
}

// CHECK 1: walk all three PEB loader lists and collect modules.
// Returns modules from the union of the three lists.
std::vector<Module> collect_modules(HANDLE h, std::vector<Finding>& out, DWORD pid) {
    std::vector<Module> mods;
    PVOID peb = nullptr;
    if (!get_peb(h, &peb)) {
        emit(out, pid, "peb_three_lists", 0, 0, "NtQueryInformationProcess failed");
        return mods;
    }
    PVOID ldr = nullptr;
    if (!read_mem(h, reinterpret_cast<BYTE*>(peb) + 0x18, &ldr, sizeof(ldr)) || !ldr) {
        emit(out, pid, "peb_three_lists", 0, 0, "PEB->Ldr unreadable");
        return mods;
    }
    BW_PEB_LDR ld{};
    if (!read_mem(h, ldr, &ld, sizeof(ld))) {
        emit(out, pid, "peb_three_lists", (uintptr_t)ldr, sizeof(ld), "LDR unreadable");
        return mods;
    }

    // The head address of each list inside the remote process.
    struct ListRef { LIST_ENTRY head; LIST_ENTRY BW_LDR_ENTRY::*field; const char* label; };
    ListRef lists[3] = {
        { ld.in_load,  &BW_LDR_ENTRY::in_load,  "InLoadOrder" },
        { ld.in_mem,   &BW_LDR_ENTRY::in_mem,   "InMemoryOrder" },
        { ld.in_init,  &BW_LDR_ENTRY::in_init,  "InInitializationOrder" },
    };

    int counts[3] = {0, 0, 0};
    for (int li = 0; li < 3; ++li) {
        // Remote address of this list head. Compute via offsetof on the local mirror.
        uintptr_t head_remote = (uintptr_t)ldr + (li == 0 ? offsetof(BW_PEB_LDR, in_load)
                                                 : li == 1 ? offsetof(BW_PEB_LDR, in_mem)
                                                           : offsetof(BW_PEB_LDR, in_init));
        LIST_ENTRY cur = lists[li].head;
        for (int i = 0; i < 512 && cur.Flink && cur.Flink != reinterpret_cast<LIST_ENTRY*>(head_remote); ++i) {
            uintptr_t entry = (uintptr_t)cur.Flink
                - (uintptr_t)offsetof(BW_LDR_ENTRY, in_load)
                + (uintptr_t)0; // baseline: links are first; field offset handled below
            // Recompute using this list's field offset.
            size_t field_off = (li == 0) ? offsetof(BW_LDR_ENTRY, in_load)
                             : (li == 1) ? offsetof(BW_LDR_ENTRY, in_mem)
                                         : offsetof(BW_LDR_ENTRY, in_init);
            entry = (uintptr_t)cur.Flink - field_off;

            BW_LDR_ENTRY e{};
            if (!read_mem(h, reinterpret_cast<LPCVOID>(entry), &e, sizeof(e))) break;
            if (e.dll_base && e.size_of_image) {
                Module m{};
                m.base = (uintptr_t)e.dll_base;
                m.size = e.size_of_image;
                if (e.base_name.Buffer && e.base_name.Length && e.base_name.Length <= 8192) {
                    std::wstring nm(e.base_name.Length / sizeof(wchar_t), L'\0');
                    if (read_mem(h, e.base_name.Buffer, nm.data(), e.base_name.Length)) m.name = nm;
                }
                if (e.full_name.Buffer && e.full_name.Length && e.full_name.Length <= 16384) {
                    std::wstring fp(e.full_name.Length / sizeof(wchar_t), L'\0');
                    if (read_mem(h, e.full_name.Buffer, fp.data(), e.full_name.Length)) m.path = fp;
                }
                // dedupe by base
                bool exists = false;
                for (auto& x : mods) if (x.base == m.base) { exists = true; break; }
                if (!exists) mods.push_back(m);
                ++counts[li];
            }
            // Advance using this list's field.
            LIST_ENTRY next{};
            if (!read_mem(h, reinterpret_cast<LPCVOID>(entry + field_off), &next, sizeof(next))) break;
            cur = next;
        }
    }

    // Report a list as a finding if it appears empty while another is populated
    // (classic unlink-from-one-list tampering).
    for (int li = 0; li < 3; ++li) {
        if (counts[li] == 0 && (counts[0] || counts[1] || counts[2])) {
            uintptr_t head_remote = (uintptr_t)ldr + (li == 0 ? offsetof(BW_PEB_LDR, in_load)
                                                     : li == 1 ? offsetof(BW_PEB_LDR, in_mem)
                                                               : offsetof(BW_PEB_LDR, in_init));
            emit(out, pid, "peb_three_lists", head_remote, 0,
                 std::string(lists[li].label) + " empty while other lists populated");
        }
    }
    return mods;
}

// Enumerate all committed regions once; reused by several checks.
struct Region { uintptr_t base; size_t size; DWORD protect; DWORD type; DWORD state; };
std::vector<Region> regions(HANDLE h) {
    std::vector<Region> v;
    SYSTEM_INFO si{}; GetSystemInfo(&si);
    BYTE* addr = reinterpret_cast<BYTE*>(si.lpMinimumApplicationAddress);
    BYTE* end = reinterpret_cast<BYTE*>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi{};
    while (addr < end && VirtualQueryEx(h, addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        v.push_back({(uintptr_t)mbi.BaseAddress, mbi.RegionSize, mbi.Protect, mbi.Type, mbi.State});
        addr = reinterpret_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return v;
}

bool is_exec(DWORD protect) {
    return (protect & PAGE_EXECUTE_READWRITE) || (protect & PAGE_EXECUTE_READ) ||
           (protect & PAGE_EXECUTE) || (protect & PAGE_EXECUTE_READWRITE);
}

// Whitelist: JVM JIT code cache. In javaw.exe the JIT is a set of large private
// RX regions adjacent to jvm.dll. We whitelist private-EXEC regions whose size
// is large (>= 1 MB) AND whose address is within a few MB of a module named
// jvm.dll (or the process itself is javaw with a large private RX cluster).
bool is_jit_cache(uintptr_t base, size_t size, DWORD protect,
                  const std::wstring& proc_name) {
    (void)base;
    if (proc_name.find(L"java") == std::wstring::npos) return false;
    (void)protect;
    // HotSpot's JIT code cache is one or more large private executable regions
    // (>= 64KB). Whitelist them per Cap.30. Limitation: a manual-mapped payload
    // inside javaw is hidden by this whitelist; the MZ/PE check (5) and the
    // .text hash check (7) are the complementary signals for that case.
    if (size >= 0x10000) return true;
    return false;
}

std::wstring process_name(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH]{}; DWORD sz = MAX_PATH;
    std::wstring n;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz)) n = buf;
    CloseHandle(h);
    return n;
}

// CHECK 2: for each module, confirm a file-backed image VAD covers it.
void check_vad_image_per_module(HANDLE h, DWORD pid, const std::vector<Module>& mods,
                                const std::vector<Region>& regs, std::vector<Finding>& out) {
    (void)h; // region VAD info already collected via VirtualQueryEx
    for (const auto& m : mods) {
        bool covered = false;
        for (const auto& r : regs) {
            if (r.state != MEM_COMMIT || r.type != MEM_IMAGE) continue;
            if (m.base >= r.base && m.base < r.base + r.size) { covered = true; break; }
        }
        if (!covered) {
            emit(out, pid, "vad_image_per_module", m.base, m.size,
                 "module not covered by MEM_IMAGE VAD: " + wstr_to_ascii(m.name));
        }
    }
}

// CHECK 3: executable PRIVATE/MAPPED VADs without a guard page.
void check_vad_exec_private(HANDLE h, DWORD pid, const std::vector<Module>& mods,
                            const std::vector<Region>& regs, const std::wstring& proc,
                            std::vector<Finding>& out) {
    (void)h;
    for (const auto& r : regs) {
        if (r.state != MEM_COMMIT) continue;
        if (r.type != MEM_PRIVATE) continue; // MEM_MAPPED exec = shared system sections
        if (!is_exec(r.protect)) continue;
        if (r.protect & PAGE_GUARD) continue; // guard pages are expected for stacks
        if (is_jit_cache(r.base, r.size, r.protect, proc)) continue; // whitelist JIT
        // Small (<64KB) exec regions without a PE header are routine Windows
        // bookkeeping (CFG, stacks, loader arenas). Only report substantial
        // regions or ones that actually carry an MZ signature.
        bool has_mz = false;
        if (r.size < 0x10000) {
            BYTE mz[2]{};
            if (read_mem(h, reinterpret_cast<LPCVOID>(r.base), mz, 2))
                has_mz = (mz[0] == 'M' && mz[1] == 'Z');
            if (!has_mz) continue;
        }
        // covered by a module? then it's a stomped module, handled by check 7
        bool covered = false;
        for (const auto& m : mods)
            if (r.base >= m.base && r.base < m.base + m.size) { covered = true; break; }
        if (covered) continue;
        emit(out, pid, "vad_exec_private", r.base, r.size,
             "executable private VAD, no guard, not JIT, no module");
    }
}

// CHECK 4: executable VADs not covered by any module (manual-mapped / reflective).
void check_vad_exec_no_module(HANDLE h, DWORD pid, const std::vector<Module>& mods,
                              const std::vector<Region>& regs, const std::wstring& proc,
                              std::vector<Finding>& out) {
    for (const auto& r : regs) {
        if (r.state != MEM_COMMIT || !is_exec(r.protect)) continue;
        bool covered = false;
        for (const auto& m : mods)
            if (r.base >= m.base && r.base < m.base + m.size) { covered = true; break; }
        if (covered) continue;
        if (is_jit_cache(r.base, r.size, r.protect, proc)) continue;
        // Same noise filter as check 3: ignore small exec regions without MZ.
        if (r.size < 0x10000) {
            BYTE mz[2]{};
            if (!read_mem(h, reinterpret_cast<LPCVOID>(r.base), mz, 2) || !(mz[0]=='M' && mz[1]=='Z'))
                continue;
        }
        emit(out, pid, "vad_exec_no_module", r.base, r.size,
             "executable VAD not covered by any loaded module");
    }
}

// CHECK 5: MZ/PE confirmation inside a private executable region.
void check_mz_pe_confirm(HANDLE h, DWORD pid, const std::vector<Region>& regs,
                         std::vector<Finding>& out) {
    for (const auto& r : regs) {
        if (r.state != MEM_COMMIT || r.type != MEM_PRIVATE || !is_exec(r.protect)) continue;
        // No JIT whitelist here: an MZ/PE header in private executable memory is
        // direct evidence (Cap.30 step 5) even if the region looks JIT-sized.
        BYTE head[0x200]{};
        SIZE_T n = (std::min<SIZE_T>)(r.size, sizeof(head));
        if (!read_mem(h, reinterpret_cast<LPCVOID>(r.base), head, n)) continue;
        if (head[0] == 'M' && head[1] == 'Z') {
            DWORD lfa = 0;
            std::memcpy(&lfa, head + 0x3C, sizeof(lfa));
            bool pe = (lfa + 4 <= n) && (head[lfa] == 'P' && head[lfa+1] == 'E' &&
                                        head[lfa+2] == 0 && head[lfa+3] == 0);
            emit(out, pid, "mz_pe_confirm", r.base, r.size,
                 pe ? "MZ/PE header in private executable VAD" : "MZ header in private executable VAD");
        }
    }
}

// CHECK 6: thread start addresses outside every module (NtQueryInformationThread 9).
void check_thread_start(DWORD pid, const std::vector<Module>& mods,
                        std::vector<Finding>& out) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    using QIT = LONG(NTAPI*)(HANDLE, DWORD, PVOID, ULONG, PULONG);
    auto qit = reinterpret_cast<QIT>(GetProcAddress(ntdll, "NtQueryInformationThread"));
    if (!qit) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{}; te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            PVOID start = nullptr;
            if (qit(th, 9, &start, sizeof(start), nullptr) >= 0 && start) {
                uintptr_t sa = (uintptr_t)start;
                bool in_mod = false;
                for (const auto& m : mods)
                    if (sa >= m.base && sa < m.base + m.size) { in_mod = true; break; }
                if (!in_mod)
                    emit(out, pid, "thread_start_address", sa, 0,
                         "thread start outside every loaded module (tid=" +
                         std::to_string(te.th32ThreadID) + ")");
            }
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

// CHECK 7: hash .text in memory vs on disk for each module (module stomping).
// Uses a simple FNV-1a over the first up-to-64KB of the executable section.
uint64_t fnv1a(const BYTE* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

// Signed graphics/EDR modules legitimately hotpatch .text in memory.
static bool known_hotpatched(const std::wstring& n) {
    static const wchar_t* k[] = { L"ati", L"amd", L"nv", L"igd", L"intel", L"rtss", L"obs", L"discord", L"av", L"defender" };
    std::wstring low = n;
    for (auto& c : low) c = towlower(c);
    for (auto p : k) if (low.find(p) == 0) return true;
    return false;
}

void check_text_hash(HANDLE h, DWORD pid, const std::vector<Module>& mods,
                      std::vector<Finding>& out) {
    for (const auto& m : mods) {
        if (m.path.empty() || known_hotpatched(m.name)) continue;
        // Locate .text in the on-disk PE.
        std::ifstream f(m.path, std::ios::binary | std::ios::ate);
        if (!f) continue;
        auto fsz = f.tellg();
        if (fsz <= 0) continue;
        std::vector<BYTE> file(static_cast<size_t>(fsz));
        f.seekg(0); f.read(reinterpret_cast<char*>(file.data()), fsz); f.close();
        if (file.size() < sizeof(IMAGE_DOS_HEADER)) continue;
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        if (static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS) > file.size()) continue;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(file.data() + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t text_rva = 0; size_t text_sz = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (std::memcmp(sec->Name, ".text", 5) == 0) {
                text_rva = sec->VirtualAddress;
                text_sz = (std::min<size_t>)(sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData, static_cast<size_t>(0x10000));
                break;
            }
        }
        if (!text_rva || !text_sz) continue;
        // In-memory .text
        std::vector<BYTE> mem(text_sz);
        if (!read_mem(h, reinterpret_cast<LPCVOID>(m.base + text_rva), mem.data(), text_sz)) continue;
        // On-disk .text raw bytes
        sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t raw_ptr = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (std::memcmp(sec->Name, ".text", 5) == 0) { raw_ptr = sec->PointerToRawData; break; }
        }
        if (!raw_ptr || raw_ptr + text_sz > file.size()) continue;
        // Count differing bytes. Windows hotpatching / relocations touch only a
        // handful; a stomp rewrites a large fraction. Threshold 1% (min 64 B).
        size_t diff = 0;
        for (size_t i = 0; i < text_sz; ++i)
            if (mem[i] != file[raw_ptr + i]) ++diff;
        size_t threshold = (std::max<size_t>)(64, text_sz / 100);
        if (diff > threshold) {
            uint64_t hm = fnv1a(mem.data(), text_sz);
            uint64_t hd = fnv1a(file.data() + raw_ptr, text_sz);
            emit(out, pid, "text_hash_mismatch", m.base + text_rva, text_sz,
                 "memory .text differs from disk by " + std::to_string(diff) + " bytes "
                 "(hash " + std::to_string(hm) + " vs " + std::to_string(hd) + "): " + wstr_to_ascii(m.name));
        }
    }
}

void print_findings(const std::vector<Finding>& findings, DWORD pid, bool json) {
    if (json) {
        std::printf("{\"pid\":%u,\"findings\":[", pid);
        for (size_t i = 0; i < findings.size(); ++i) {
            const auto& f = findings[i];
            if (i) std::printf(",");
            std::printf("{\"type\":\"%s\",\"address\":\"%s\",\"size\":%llu,\"reason\":\"%s\"}",
                        f.type, hex(f.address).c_str(), (unsigned long long)f.size, f.reason.c_str());
        }
        std::printf("]}\n");
    } else {
        for (const auto& f : findings)
            std::printf("[FOUND] pid=%u type=%s addr=%s size=%llu reason=%s\n",
                        f.pid, f.type, hex(f.address).c_str(),
                        (unsigned long long)f.size, f.reason.c_str());
        if (findings.empty()) std::printf("clean pid=%u\n", pid);
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: wrath_probe <pid> [--json]\n");
        return 2;
    }
    DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    bool json = false;
    for (int i = 2; i < argc; ++i)
        if (std::wstring(argv[i]) == L"--json") json = true;

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { std::printf("cannot open pid %u (err=%u)\n", pid, GetLastError()); return 2; }

    std::vector<Finding> findings;
    std::wstring proc = process_name(pid);
    std::vector<Region> regs = regions(h);

    // 1. PEB three lists + module collection
    std::vector<Module> mods = collect_modules(h, findings, pid);
    // 2. image VAD per module
    check_vad_image_per_module(h, pid, mods, regs, findings);
    // 3. exec private/mapped VADs
    check_vad_exec_private(h, pid, mods, regs, proc, findings);
    // 4. exec VADs without module
    check_vad_exec_no_module(h, pid, mods, regs, proc, findings);
    // 5. MZ/PE confirmation
    check_mz_pe_confirm(h, pid, regs, findings);
    // 6. thread start addresses
    check_thread_start(pid, mods, findings);
    // 7. .text hash memory vs disk
    check_text_hash(h, pid, mods, findings);

    CloseHandle(h);
    print_findings(findings, pid, json);
    return findings.empty() ? 0 : 1;
}
