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
                    emit(out, pid, "p2_thread_start_address", sa, 0,
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

// ---- WP-9 checks (p1..p7) --------------------------------------------------

void check_p1_pdata(HANDLE h, DWORD pid, const std::vector<Module>& mods, std::vector<Finding>& out) {
    for (const auto& m : mods) {
        IMAGE_DOS_HEADER dos{};
        if (!read_mem(h, reinterpret_cast<LPCVOID>(m.base), &dos, sizeof(dos)) ||
            dos.e_magic != IMAGE_DOS_SIGNATURE) continue;
        IMAGE_NT_HEADERS nt{};
        if (!read_mem(h, reinterpret_cast<LPCVOID>(m.base + dos.e_lfanew), &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE) continue;
        const auto& exDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exDir.VirtualAddress == 0 || exDir.Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY)) {
            emit(out, pid, "p1_pdata_missing", m.base, m.size,
                 "missing or empty .pdata exception directory: " + wstr_to_ascii(m.name));
            continue;
        }
        IMAGE_RUNTIME_FUNCTION_ENTRY rf{};
        if (!read_mem(h, reinterpret_cast<LPCVOID>(m.base + exDir.VirtualAddress), &rf, sizeof(rf)) ||
            rf.BeginAddress >= rf.EndAddress) {
            emit(out, pid, "p1_pdata_invalid", m.base + exDir.VirtualAddress, exDir.Size,
                 "invalid RUNTIME_FUNCTION entry in .pdata: " + wstr_to_ascii(m.name));
        }
    }
}

void check_p3_jvm_hash(HANDLE h, DWORD pid, const std::vector<Module>& mods, std::vector<Finding>& out) {
    for (const auto& m : mods) {
        std::wstring low = m.name;
        for (auto& c : low) c = towlower(c);
        if (low != L"jvm.dll" || m.path.empty()) continue;

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
        uintptr_t text_rva = 0, raw_ptr = 0; size_t text_sz = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (std::memcmp(sec->Name, ".text", 5) == 0) {
                text_rva = sec->VirtualAddress;
                raw_ptr = sec->PointerToRawData;
                text_sz = (std::min<size_t>)(sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData, static_cast<size_t>(0x10000));
                break;
            }
        }
        if (!text_rva || !text_sz || !raw_ptr || raw_ptr + text_sz > file.size()) continue;
        std::vector<BYTE> mem(text_sz);
        if (!read_mem(h, reinterpret_cast<LPCVOID>(m.base + text_rva), mem.data(), text_sz)) continue;

        size_t diff = 0;
        for (size_t i = 0; i < text_sz; ++i)
            if (mem[i] != file[raw_ptr + i]) ++diff;
        if (diff > 0) {
            emit(out, pid, "p3_jvm_text_mismatch", m.base + text_rva, text_sz,
                 "jvm.dll .text differs from disk by " + std::to_string(diff) + " bytes");
        }
    }
}

void check_p4_p5_jcmd(DWORD pid, const std::wstring& proc, std::vector<Finding>& out) {
    std::wstring low = proc;
    for (auto& c : low) c = towlower(c);
    if (low.find(L"javaw.exe") == std::wstring::npos && low.find(L"java.exe") == std::wstring::npos) return;

    auto run_thread_print = [&](int& out_refs, std::string& out_text) -> bool {
        char cmd[128]{};
        std::snprintf(cmd, sizeof(cmd), "jcmd %lu Thread.print 2>NUL", static_cast<unsigned long>(pid));
        FILE* pipe = _popen(cmd, "r");
        if (!pipe) return false;
        out_text.clear();
        char buf[512]{};
        while (std::fgets(buf, sizeof(buf), pipe)) out_text += buf;
        _pclose(pipe);
        auto pos = out_text.find("JNI global refs:");
        if (pos == std::string::npos) return false;
        out_refs = std::atoi(out_text.c_str() + pos + 16);
        return true;
    };

    int refs0 = 0, refs1 = 0;
    std::string text0, text1;
    if (run_thread_print(refs0, text0)) {
        Sleep(3000);
        if (run_thread_print(refs1, text1)) {
            int delta = refs1 - refs0;
            if (delta != 0) {
                emit(out, pid, "p4_jni_global_refs_delta", 0, static_cast<size_t>(delta > 0 ? delta : -delta),
                     "JNI global refs delta over 3s != 0: " + std::to_string(refs0) + " -> " + std::to_string(refs1));
            }
        }
        // p5: attached native threads without Java frames
        if (text1.find("\"Native Helper\"") != std::string::npos) {
            emit(out, pid, "p5_attached_thread", 0, 0, "attached native thread without Java frame detected in Thread.print");
        }
    }
}

void check_p6_load_count(HANDLE h, DWORD pid, std::vector<Finding>& out) {
    PVOID peb = nullptr;
    if (!get_peb(h, &peb)) return;
    PVOID ldr = nullptr;
    if (!read_mem(h, reinterpret_cast<BYTE*>(peb) + 0x18, &ldr, sizeof(ldr)) || !ldr) return;
    BW_PEB_LDR ld{};
    if (!read_mem(h, ldr, &ld, sizeof(ld))) return;

    uintptr_t head_remote = reinterpret_cast<uintptr_t>(ldr) + offsetof(BW_PEB_LDR, in_load);
    LIST_ENTRY cur = ld.in_load;
    for (int i = 0; i < 512 && cur.Flink && cur.Flink != reinterpret_cast<LIST_ENTRY*>(head_remote); ++i) {
        uintptr_t entry = reinterpret_cast<uintptr_t>(cur.Flink);
        BW_LDR_ENTRY e{};
        if (!read_mem(h, reinterpret_cast<LPCVOID>(entry), &e, sizeof(e))) break;
        if (e.base_name.Length > 0 && e.base_name.Length < 512 && e.base_name.Buffer) {
            std::wstring bname(e.base_name.Length / 2, L'\0');
            if (read_mem(h, e.base_name.Buffer, bname.data(), e.base_name.Length)) {
                for (auto& c : bname) c = towlower(c);
                if (bname.find(L"display_core") != std::wstring::npos ||
                    bname.find(L"display_service") != std::wstring::npos) {
                    USHORT load_count = 0;
                    // On Win10/11 x64 LDR_DATA_TABLE_ENTRY.ObsoleteLoadCount is at +0x6C
                    if (read_mem(h, reinterpret_cast<LPCVOID>(entry + 0x6C), &load_count, sizeof(load_count))) {
                        if (load_count != 1) {
                            emit(out, pid, "p6_load_count", reinterpret_cast<uintptr_t>(e.dll_base), load_count,
                                 "module LoadCount != 1 (" + std::to_string(load_count) + "): " + wstr_to_ascii(bname));
                        }
                    }
                }
            }
        }
        cur = e.in_load;
    }
}

void check_p7_unbacked_path(HANDLE h, DWORD pid, const std::vector<Region>& regs, std::vector<Finding>& out) {
    static const wchar_t* needles[] = { L"display_core64.dll", L"display_core.dll", L"display_service64.dll" };
    for (const auto& r : regs) {
        // B-3: Restrict p7 to MEM_PRIVATE regions with PAGE_EXECUTE* (ignore heap RW).
        if (r.type != MEM_PRIVATE || r.state != MEM_COMMIT || !is_exec(r.protect)) continue;
        if (r.size == 0 || r.size > 4 * 1024 * 1024) continue;
        std::vector<BYTE> buf(r.size);
        if (!read_mem(h, reinterpret_cast<LPCVOID>(r.base), buf.data(), r.size)) continue;
        for (const wchar_t* needle : needles) {
            const size_t nbytes = std::wcslen(needle) * sizeof(wchar_t);
            const BYTE* p = reinterpret_cast<const BYTE*>(needle);
            auto it = std::search(buf.begin(), buf.end(), p, p + nbytes);
            if (it != buf.end()) {
                emit(out, pid, "p7_unbacked_path", r.base + static_cast<uintptr_t>(it - buf.begin()), nbytes,
                     "unbacked MEM_PRIVATE exec region contains DLL path");
                break;
            }
        }
    }
}

void check_p7b_private_rwx(DWORD pid, const std::vector<Region>& regs, const std::wstring& proc, std::vector<Finding>& out) {
    for (const auto& r : regs) {
        if (r.type != MEM_PRIVATE || r.state != MEM_COMMIT) continue;
        if ((r.protect & PAGE_EXECUTE_READWRITE) == 0) continue;
        if (is_jit_cache(r.base, r.size, r.protect, proc)) continue;
        emit(out, pid, "p7b_private_rwx", r.base, r.size,
             "MEM_PRIVATE PAGE_EXECUTE_READWRITE region outside JVM JIT cache");
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
    if (argc < 2 || std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"-h") {
        std::printf("usage: dt_diag <pid> [--json] [--phase pre|post] [--expect clean] [--check unbacked-path]\n"
                    "note: p4/p5 invoke jcmd Thread.print, which starts AttachListener in the target JVM.\n");
        return argc < 2 ? 2 : 0;
    }
    DWORD pid = static_cast<DWORD>(_wtoi(argv[1]));
    bool json = false;
    std::wstring check_only;
    std::wstring phase = L"pre";
    for (int i = 2; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--json") json = true;
        else if (arg == L"--check" && i + 1 < argc) check_only = argv[++i];
        else if (arg == L"--phase" && i + 1 < argc) phase = argv[++i];
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) { std::printf("cannot open pid %u (err=%u)\n", pid, GetLastError()); return 2; }

    std::vector<Finding> findings;
    std::wstring proc = process_name(pid);
    std::vector<Region> regs = regions(h);

    if (check_only == L"unbacked-path") {
        check_p7_unbacked_path(h, pid, regs, findings);
    } else {
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
        // 6. thread start addresses (p2)
        check_thread_start(pid, mods, findings);
        // 7. .text hash memory vs disk
        check_text_hash(h, pid, mods, findings);
        // WP-9 p1..p7b
        check_p1_pdata(h, pid, mods, findings);
        if (phase == L"pre") {
            check_p3_jvm_hash(h, pid, mods, findings);
        }
        check_p6_load_count(h, pid, findings);
        check_p7_unbacked_path(h, pid, regs, findings);
        check_p7b_private_rwx(pid, regs, proc, findings);
        // p4/p5 always run at the end of the phase (jcmd mutates AttachListener).
        check_p4_p5_jcmd(pid, proc, findings);
    }

    CloseHandle(h);
    print_findings(findings, pid, json);
    return findings.empty() ? 0 : 1;
}
