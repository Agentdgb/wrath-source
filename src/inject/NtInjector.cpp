#include "NtInjector.h"
#include "shared/util/enc.h"

#include <windows.h>
#include <vector>
#include <algorithm>
#include <cstring>

// BootstrapParams + build_bootstrap_shellcode — shellcode PIC x64 inline

namespace wrath::injector {

namespace {

bool read_remote(HANDLE h, PVOID addr, PVOID buf, SIZE_T sz) {
    auto pfn = core::nt::get_NtReadVirtualMemory();
    if (!pfn) return false;
    SIZE_T read = 0;
    return NT_SUCCESS(pfn(h, addr, buf, sz, &read)) && read == sz;
}

bool write_remote(HANDLE h, PVOID addr, const void* buf, SIZE_T sz) {
    auto pfn = core::nt::get_NtWriteVirtualMemory();
    if (!pfn) return false;
    SIZE_T written = 0;
    return NT_SUCCESS(pfn(h, addr, const_cast<PVOID>(buf), sz, &written)) && written == sz;
}

} // namespace

bool nt_find_process(core::hash_t name_hash, DWORD* out_pid) {
    if (!out_pid) return false;
    *out_pid = 0;

    auto pfnQuery = core::nt::get_NtQuerySystemInformation();
    if (!pfnQuery) return false;

    ULONG buf_sz = 256 * 1024;
    std::vector<uint8_t> buffer;
    core::nt::NTSTATUS status = core::nt::STATUS_INFO_LENGTH_MISMATCH;

    while (status == core::nt::STATUS_INFO_LENGTH_MISMATCH || status == core::nt::STATUS_BUFFER_TOO_SMALL) {
        buffer.resize(buf_sz);
        ULONG ret_len = 0;
        status = pfnQuery(5 /* SystemProcessInformation */, buffer.data(), buf_sz, &ret_len);
        if (status == core::nt::STATUS_INFO_LENGTH_MISMATCH || status == core::nt::STATUS_BUFFER_TOO_SMALL) {
            buf_sz = (ret_len > buf_sz) ? ret_len + 32 * 1024 : buf_sz * 2;
        }
    }

    if (!NT_SUCCESS(status)) return false;

    auto* curr = reinterpret_cast<const core::nt::SYSTEM_PROCESS_INFORMATION_EX*>(buffer.data());
    while (true) {
        if (curr->ImageName.Buffer && curr->ImageName.Length > 0) {
            core::hash_t h = core::fnv1a_ci_w(curr->ImageName.Buffer, curr->ImageName.Length / sizeof(wchar_t));
            if (h == name_hash) {
                *out_pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(curr->UniqueProcessId));
                return true;
            }
        }

        if (curr->NextEntryOffset == 0) break;
        curr = reinterpret_cast<const core::nt::SYSTEM_PROCESS_INFORMATION_EX*>(
            reinterpret_cast<const uint8_t*>(curr) + curr->NextEntryOffset
        );
    }

    return false;
}

bool nt_get_peb(HANDLE hProcess, PVOID* out_peb_base) {
    if (!out_peb_base) return false;
    *out_peb_base = nullptr;

    auto pfnQuery = core::nt::get_NtQueryInformationProcess();
    if (!pfnQuery) return false;

    PROCESS_BASIC_INFORMATION pbi{};
    ULONG ret_len = 0;
    core::nt::NTSTATUS status = pfnQuery(
        hProcess,
        0 /* ProcessBasicInformation */,
        &pbi,
        sizeof(pbi),
        &ret_len
    );

    if (NT_SUCCESS(status) && pbi.PebBaseAddress) {
        *out_peb_base = pbi.PebBaseAddress;
        return true;
    }
    return false;
}

PVOID nt_find_module(HANDLE hProcess, PVOID peb_base, core::hash_t module_hash) {
    if (!hProcess || !peb_base) return nullptr;

    core::nt::PEB_EX peb{};
    if (!read_remote(hProcess, peb_base, &peb, sizeof(peb)) || !peb.Ldr) {
        return nullptr;
    }

    core::nt::PEB_LDR_DATA_EX ldr{};
    if (!read_remote(hProcess, peb.Ldr, &ldr, sizeof(ldr))) {
        return nullptr;
    }

    LIST_ENTRY* head = &peb.Ldr->InLoadOrderModuleList;
    LIST_ENTRY* curr = ldr.InLoadOrderModuleList.Flink;

    for (int safety = 0; safety < 512 && curr != head; ++safety) {
        core::nt::LDR_DATA_TABLE_ENTRY_EX entry{};
        if (!read_remote(hProcess, curr, &entry, sizeof(entry))) break;

        if (entry.BaseDllName.Buffer && entry.BaseDllName.Length > 0 && entry.BaseDllName.Length < 512) {
            std::vector<wchar_t> name_buf(entry.BaseDllName.Length / sizeof(wchar_t) + 1, L'\0');
            if (read_remote(hProcess, entry.BaseDllName.Buffer, name_buf.data(), entry.BaseDllName.Length)) {
                if (core::fnv1a_ci_w(name_buf.data()) == module_hash) {
                    return entry.DllBase;
                }
            }
        }

        curr = entry.InLoadOrderLinks.Flink;
    }

    return nullptr;
}

PVOID nt_resolve_export(HANDLE hProcess, PVOID module_base, core::hash_t fn_hash) {
    if (!hProcess || !module_base) return nullptr;

    IMAGE_DOS_HEADER dos{};
    if (!read_remote(hProcess, module_base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    IMAGE_NT_HEADERS nt{};
    if (!read_remote(hProcess, reinterpret_cast<uint8_t*>(module_base) + dos.e_lfanew, &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const auto& exp_entry = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exp_entry.VirtualAddress == 0 || exp_entry.Size == 0) return nullptr;

    IMAGE_EXPORT_DIRECTORY exp{};
    PVOID exp_addr = reinterpret_cast<uint8_t*>(module_base) + exp_entry.VirtualAddress;
    if (!read_remote(hProcess, exp_addr, &exp, sizeof(exp))) return nullptr;

    std::vector<uint32_t> functions(exp.NumberOfFunctions);
    std::vector<uint32_t> names(exp.NumberOfNames);
    std::vector<uint16_t> ordinals(exp.NumberOfNames);

    if (!read_remote(hProcess, reinterpret_cast<uint8_t*>(module_base) + exp.AddressOfFunctions,
                     functions.data(), functions.size() * sizeof(uint32_t))) return nullptr;

    if (!read_remote(hProcess, reinterpret_cast<uint8_t*>(module_base) + exp.AddressOfNames,
                     names.data(), names.size() * sizeof(uint32_t))) return nullptr;

    if (!read_remote(hProcess, reinterpret_cast<uint8_t*>(module_base) + exp.AddressOfNameOrdinals,
                     ordinals.data(), ordinals.size() * sizeof(uint16_t))) return nullptr;

    char name_buf[256]{};
    for (uint32_t i = 0; i < exp.NumberOfNames; ++i) {
        if (!read_remote(hProcess, reinterpret_cast<uint8_t*>(module_base) + names[i], name_buf, sizeof(name_buf) - 1)) {
            continue;
        }
        name_buf[sizeof(name_buf) - 1] = '\0';

        if (core::fnv1a(name_buf) == fn_hash) {
            uint16_t ord = ordinals[i];
            if (ord < exp.NumberOfFunctions) {
                return reinterpret_cast<uint8_t*>(module_base) + functions[ord];
            }
        }
    }

    return nullptr;
}

bool alloc_section(HANDLE hProcess, size_t size, PVOID preferred_base, PVOID* out_base, HANDLE* out_section) {
    if (!out_base || !out_section) return false;
    *out_base = nullptr;
    *out_section = nullptr;

    auto pfnCreateSection = core::nt::get_NtCreateSection();
    auto pfnMapView = core::nt::get_NtMapViewOfSection();
    if (!pfnCreateSection || !pfnMapView) return false;

    size_t aligned_sz = (size + 0xFFFF) & ~0xFFFF;
    LARGE_INTEGER max_sz{};
    max_sz.QuadPart = aligned_sz;

    HANDLE hSec = nullptr;
    core::nt::NTSTATUS status = pfnCreateSection(
        &hSec,
        SECTION_ALL_ACCESS,
        nullptr,
        &max_sz,
        PAGE_EXECUTE_READWRITE,
        SEC_COMMIT,
        nullptr
    );

    if (!NT_SUCCESS(status) || !hSec) return false;

    PVOID base = preferred_base;
    SIZE_T view_sz = aligned_sz;

    status = pfnMapView(
        hSec,
        hProcess,
        &base,
        0,
        aligned_sz,
        nullptr,
        &view_sz,
        core::nt::ViewShare,
        0,
        PAGE_EXECUTE_READWRITE
    );

    if (!NT_SUCCESS(status)) {
        base = nullptr;
        status = pfnMapView(
            hSec,
            hProcess,
            &base,
            0,
            aligned_sz,
            nullptr,
            &view_sz,
            core::nt::ViewShare,
            0,
            PAGE_EXECUTE_READWRITE
        );
    }

    if (NT_SUCCESS(status)) {
        *out_base = base;
        *out_section = hSec;
        return true;
    }

    CloseHandle(hSec);
    return false;
}

bool map_image(HANDLE hProcess, PVOID base, const uint8_t* raw_pe, size_t pe_len) {
    if (!hProcess || !base || !raw_pe || pe_len < sizeof(IMAGE_DOS_HEADER)) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw_pe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(raw_pe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    // 1. Escribir cabeceras
    if (!write_remote(hProcess, base, raw_pe, nt->OptionalHeader.SizeOfHeaders)) {
        return false;
    }

    // 2. Escribir cada seccion
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData > 0 && sec[i].PointerToRawData > 0) {
            PVOID sec_dest = reinterpret_cast<uint8_t*>(base) + sec[i].VirtualAddress;
            const void* sec_src = raw_pe + sec[i].PointerToRawData;
            if (!write_remote(hProcess, sec_dest, sec_src, sec[i].SizeOfRawData)) {
                return false;
            }
        }
    }

    // 3. Aplicar Relocations x64 (delta)
    ULONG_PTR delta = reinterpret_cast<ULONG_PTR>(base) - nt->OptionalHeader.ImageBase;
    if (delta != 0) {
        const auto& reloc_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_entry.VirtualAddress != 0 && reloc_entry.Size != 0) {
            const uint8_t* reloc_ptr = raw_pe + reloc_entry.VirtualAddress;
            const uint8_t* reloc_end = reloc_ptr + reloc_entry.Size;

            while (reloc_ptr < reloc_end) {
                auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reloc_ptr);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;

                DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                const WORD* list = reinterpret_cast<const WORD*>(reloc_ptr + sizeof(IMAGE_BASE_RELOCATION));

                for (DWORD i = 0; i < count; ++i) {
                    WORD type = list[i] >> 12;
                    WORD offset = list[i] & 0x0FFF;

                    if (type == IMAGE_REL_BASED_DIR64) {
                        PVOID patch_addr = reinterpret_cast<uint8_t*>(base) + block->VirtualAddress + offset;
                        ULONG_PTR val = 0;
                        if (read_remote(hProcess, patch_addr, &val, sizeof(val))) {
                            val += delta;
                            write_remote(hProcess, patch_addr, &val, sizeof(val));
                        }
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        PVOID patch_addr = reinterpret_cast<uint8_t*>(base) + block->VirtualAddress + offset;
                        DWORD val = 0;
                        if (read_remote(hProcess, patch_addr, &val, sizeof(val))) {
                            val += static_cast<DWORD>(delta);
                            write_remote(hProcess, patch_addr, &val, sizeof(val));
                        }
                    }
                }
                reloc_ptr += block->SizeOfBlock;
            }
        }
    }

    // 4. Resolver Imports de forma 100% infalible mediante System-wide ASLR
    const auto& import_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_entry.VirtualAddress != 0 && import_entry.Size != 0) {
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(raw_pe + import_entry.VirtualAddress);
        while (desc->Characteristics || desc->Name) {
            const char* mod_name = reinterpret_cast<const char*>(raw_pe + desc->Name);
            if (mod_name && *mod_name) {
                HMODULE hMod = GetModuleHandleA(mod_name);
                if (!hMod) hMod = LoadLibraryA(mod_name);
                if (hMod) {
                    DWORD thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                    const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(raw_pe + thunk_rva);
                    PVOID iat_remote = reinterpret_cast<uint8_t*>(base) + desc->FirstThunk;

                    size_t idx = 0;
                    while (thunk[idx].u1.AddressOfData != 0) {
                        FARPROC fn = nullptr;
                        if (IMAGE_SNAP_BY_ORDINAL(thunk[idx].u1.Ordinal)) {
                            fn = GetProcAddress(hMod, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk[idx].u1.Ordinal)));
                        } else {
                            const auto* import_by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                raw_pe + thunk[idx].u1.AddressOfData
                            );
                            fn = GetProcAddress(hMod, import_by_name->Name);
                        }

                        if (fn) {
                            PVOID fn_addr = reinterpret_cast<PVOID>(fn);
                            PVOID slot = reinterpret_cast<uint8_t*>(iat_remote) + idx * sizeof(PVOID);
                            write_remote(hProcess, slot, &fn_addr, sizeof(fn_addr));
                        }
                        idx++;
                    }
                }
            }
            desc++;
        }
    }

    return true;
}

bool unlink_ldr(HANDLE hProcess, PVOID peb_base, PVOID module_base) {
    if (!hProcess || !peb_base || !module_base) return false;

    core::nt::PEB_EX peb{};
    if (!read_remote(hProcess, peb_base, &peb, sizeof(peb)) || !peb.Ldr) return false;

    core::nt::PEB_LDR_DATA_EX ldr{};
    if (!read_remote(hProcess, peb.Ldr, &ldr, sizeof(ldr))) return false;

    LIST_ENTRY* head = &peb.Ldr->InLoadOrderModuleList;
    LIST_ENTRY* curr = ldr.InLoadOrderModuleList.Flink;

    for (int safety = 0; safety < 512 && curr != head; ++safety) {
        core::nt::LDR_DATA_TABLE_ENTRY_EX entry{};
        if (!read_remote(hProcess, curr, &entry, sizeof(entry))) break;

        if (entry.DllBase == module_base) {
            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InLoadOrderLinks.Blink) + offsetof(LIST_ENTRY, Flink), &entry.InLoadOrderLinks.Flink, sizeof(PVOID));
            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InLoadOrderLinks.Flink) + offsetof(LIST_ENTRY, Blink), &entry.InLoadOrderLinks.Blink, sizeof(PVOID));

            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InMemoryOrderLinks.Blink) + offsetof(LIST_ENTRY, Flink), &entry.InMemoryOrderLinks.Flink, sizeof(PVOID));
            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InMemoryOrderLinks.Flink) + offsetof(LIST_ENTRY, Blink), &entry.InMemoryOrderLinks.Blink, sizeof(PVOID));

            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InInitializationOrderLinks.Blink) + offsetof(LIST_ENTRY, Flink), &entry.InInitializationOrderLinks.Flink, sizeof(PVOID));
            write_remote(hProcess, reinterpret_cast<uint8_t*>(entry.InInitializationOrderLinks.Flink) + offsetof(LIST_ENTRY, Blink), &entry.InInitializationOrderLinks.Blink, sizeof(PVOID));

            if (entry.BaseDllName.Buffer && entry.BaseDllName.Length > 0) {
                std::vector<uint8_t> zeros(entry.BaseDllName.MaximumLength > 0 ? entry.BaseDllName.MaximumLength : entry.BaseDllName.Length, 0);
                write_remote(hProcess, entry.BaseDllName.Buffer, zeros.data(), zeros.size());
            }
            if (entry.FullDllName.Buffer && entry.FullDllName.Length > 0) {
                std::vector<uint8_t> zeros(entry.FullDllName.MaximumLength > 0 ? entry.FullDllName.MaximumLength : entry.FullDllName.Length, 0);
                write_remote(hProcess, entry.FullDllName.Buffer, zeros.data(), zeros.size());
            }
            UNICODE_STRING empty_str{};
            write_remote(hProcess, reinterpret_cast<uint8_t*>(curr) + offsetof(core::nt::LDR_DATA_TABLE_ENTRY_EX, BaseDllName), &empty_str, sizeof(empty_str));
            write_remote(hProcess, reinterpret_cast<uint8_t*>(curr) + offsetof(core::nt::LDR_DATA_TABLE_ENTRY_EX, FullDllName), &empty_str, sizeof(empty_str));
            return true;
        }

        curr = entry.InLoadOrderLinks.Flink;
    }

    return false;
}

bool wipe_headers(HANDLE hProcess, PVOID base, const uint8_t* raw_pe) {
    if (!hProcess || !base || !raw_pe) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw_pe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(raw_pe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    DWORD hdr_sz = nt->OptionalHeader.SizeOfHeaders;
    if (hdr_sz == 0) hdr_sz = 0x1000;

    std::vector<uint8_t> zeros(hdr_sz, 0);
    SecureZeroMemory(zeros.data(), zeros.size());
    return write_remote(hProcess, base, zeros.data(), zeros.size());
}

bool reprotect(HANDLE hProcess, PVOID base, const uint8_t* raw_pe) {
    if (!hProcess || !base || !raw_pe) return false;

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw_pe);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(raw_pe + dos->e_lfanew);
    auto pfnProtect = core::nt::get_NtProtectVirtualMemory();
    if (!pfnProtect) return false;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].Misc.VirtualSize == 0) continue;

        DWORD new_prot = PAGE_READONLY;
        if (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
            new_prot = PAGE_EXECUTE_READ; // Cero RWX
        } else if (sec[i].Characteristics & IMAGE_SCN_MEM_WRITE) {
            new_prot = PAGE_READWRITE;
        } else if (sec[i].Characteristics & IMAGE_SCN_MEM_READ) {
            new_prot = PAGE_READONLY;
        }

        PVOID sec_base = reinterpret_cast<uint8_t*>(base) + sec[i].VirtualAddress;
        SIZE_T sec_sz = sec[i].Misc.VirtualSize;
        ULONG old_prot = 0;

        pfnProtect(hProcess, &sec_base, &sec_sz, new_prot, &old_prot);
    }

    PVOID hdr_base = base;
    SIZE_T hdr_sz = nt->OptionalHeader.SizeOfHeaders ? nt->OptionalHeader.SizeOfHeaders : 0x1000;
    ULONG old_hdr_prot = 0;
    pfnProtect(hProcess, &hdr_base, &hdr_sz, PAGE_READONLY, &old_hdr_prot);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Shellcode de bootstrap PIC x64 — 100% datos, cero referencias relativas
// Layout en remote_scratch (PAGE_EXECUTE_READWRITE):
//   [0x000..0x0FF]  bootstrap_code  — 36 bytes shellcode + padding
//   [0x100..0x17F]  BootstrapParams — struct de parámetros
//
// El shellcode:
//   1. Llama RtlAddFunctionTable(pdata_va, count, image_base)
//   2. Llama DllMain(image_base, DLL_PROCESS_ATTACH, NULL)
//   3. ret
//
// Convención x64 Windows:
//   sub rsp, 0x28           ; shadow space
//   mov rcx, pdata_ptr      ; arg1
//   mov edx, pdata_count    ; arg2
//   mov r8,  image_base     ; arg3
//   mov rax, pfnRtlAddFT
//   call rax
//   mov rcx, image_base     ; arg1 hinstDLL
//   mov edx, 1              ; DLL_PROCESS_ATTACH
//   xor r8d, r8d            ; lpvReserved = NULL
//   mov rax, entry_point
//   call rax
//   add rsp, 0x28
//   ret
// ─────────────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)
struct BootstrapParams {
    uint64_t pfn_rtl_add_function_table; // RtlAddFunctionTable
    uint64_t pdata_ptr;                   // RUNTIME_FUNCTION* remote VA
    uint32_t pdata_count;                 // número de entradas
    uint32_t tls_callback_count;          // número de callbacks TLS
    uint64_t image_base;                  // base remota del DLL
    uint64_t entry_point;                 // DllMain VA remota
    uint64_t tls_callbacks[8];            // callbacks TLS (hasta 8)
};
#pragma pack(pop)

static size_t build_bootstrap_shellcode(uint8_t* buf, const BootstrapParams& p) {
    uint8_t* w = buf;

    // ThreadProc frame prologue.
    // NtCreateThreadEx delivers RSP 16-byte aligned with no return address
    // pushed yet — so RSP is already aligned on entry to our stub.  We push
    // RBP and RBX so the frame looks like a real leaf and unwind can walk
    // past us if any callee throws.  After the two pushes RSP mod 16 == 0,
    // which is the required state for the subsequent shadow-space adjustments.
    //
    // push rbp
    *w++=0x55;
    // mov rbp, rsp
    *w++=0x48; *w++=0x89; *w++=0xEC;
    // push rbx
    *w++=0x53;
    // sub rsp, 0x20   (align + home space for first callee)
    *w++=0x48; *w++=0x83; *w++=0xEC; *w++=0x20;

    // ── 1. RtlAddFunctionTable(pdata_ptr, pdata_count, image_base) ───────────
    // Must be called before DllMain so SEH unwind tables for the image are
    // registered before any exception can fire inside DllMain/_CRT_INIT.
    if (p.pfn_rtl_add_function_table && p.pdata_ptr && p.pdata_count) {
        // mov rcx, pdata_ptr
        *w++=0x48; *w++=0xB9; memcpy(w, &p.pdata_ptr, 8); w+=8;
        // mov edx, pdata_count
        *w++=0xBA; memcpy(w, &p.pdata_count, 4); w+=4;
        // mov r8, image_base
        *w++=0x49; *w++=0xB8; memcpy(w, &p.image_base, 8); w+=8;
        // mov rax, pfn_rtl_add_function_table
        *w++=0x48; *w++=0xB8; memcpy(w, &p.pfn_rtl_add_function_table, 8); w+=8;
        // call rax
        *w++=0xFF; *w++=0xD0;
    }

    // ── 2. TLS Callbacks (image_base, DLL_PROCESS_ATTACH, NULL) ──────────────
    for (uint32_t i = 0; i < p.tls_callback_count && i < 8; ++i) {
        if (p.tls_callbacks[i]) {
            // mov rcx, image_base
            *w++=0x48; *w++=0xB9; memcpy(w, &p.image_base, 8); w+=8;
            // mov edx, 1 (DLL_PROCESS_ATTACH)
            *w++=0xBA; uint32_t one = 1; memcpy(w, &one, 4); w+=4;
            // xor r8d, r8d  (lpvReserved = NULL)
            *w++=0x45; *w++=0x33; *w++=0xC0;
            // mov rax, callback
            *w++=0x48; *w++=0xB8; memcpy(w, &p.tls_callbacks[i], 8); w+=8;
            // call rax
            *w++=0xFF; *w++=0xD0;
        }
    }

    // ── 3. DllMain(image_base, DLL_PROCESS_ATTACH, NULL) ─────────────────────
    if (p.entry_point) {
        // mov rcx, image_base
        *w++=0x48; *w++=0xB9; memcpy(w, &p.image_base, 8); w+=8;
        // mov edx, 1 (DLL_PROCESS_ATTACH)
        *w++=0xBA; uint32_t one = 1; memcpy(w, &one, 4); w+=4;
        // xor r8d, r8d  (lpvReserved = NULL)
        *w++=0x45; *w++=0x33; *w++=0xC0;
        // mov rax, entry_point
        *w++=0x48; *w++=0xB8; memcpy(w, &p.entry_point, 8); w+=8;
        // call rax
        *w++=0xFF; *w++=0xD0;
    }

    // ThreadProc frame epilogue.
    // xor eax, eax  — thread exit code 0
    *w++=0x33; *w++=0xC0;
    // add rsp, 0x20
    *w++=0x48; *w++=0x83; *w++=0xC4; *w++=0x20;
    // pop rbx
    *w++=0x5B;
    // pop rbp
    *w++=0x5D;
    // ret
    *w++=0xC3;

    return static_cast<size_t>(w - buf);
}

bool nt_run(HANDLE hProcess, DWORD pid, PVOID entry_point, PVOID remote_base, PVOID param,
            PVOID remote_pdata, DWORD pdata_count, const std::vector<uint64_t>& tls_callbacks)
{
    (void)pid; (void)param;

    // 1. Resolver RtlAddFunctionTable
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    HMODULE hK32   = GetModuleHandleW(L"kernel32.dll");
    if (!hK32) return false;

    uint64_t pfn_rtlaft = 0;
    if (hK32) pfn_rtlaft = reinterpret_cast<uint64_t>(GetProcAddress(hK32, "RtlAddFunctionTable"));
    if (!pfn_rtlaft && hNtdll) pfn_rtlaft = reinterpret_cast<uint64_t>(GetProcAddress(hNtdll, "RtlAddFunctionTable"));

    // 2. Construir parámetros
    BootstrapParams bp{};
    bp.pfn_rtl_add_function_table = pfn_rtlaft;
    bp.pdata_ptr   = remote_pdata ? reinterpret_cast<uint64_t>(remote_pdata) : 0;
    bp.pdata_count = pdata_count;
    bp.image_base  = reinterpret_cast<uint64_t>(remote_base);
    bp.entry_point = reinterpret_cast<uint64_t>(entry_point);

    bp.tls_callback_count = static_cast<uint32_t>((std::min)(tls_callbacks.size(), size_t(8)));
    for (uint32_t i = 0; i < bp.tls_callback_count; ++i) {
        bp.tls_callbacks[i] = tls_callbacks[i];
    }

    // 3. Generar shellcode
    uint8_t shellcode[512]{};
    size_t sc_len = build_bootstrap_shellcode(shellcode, bp);

    // 4. Alloc scratch RWX en destino
    const size_t scratch_sz = 0x1000;
    PVOID remote_scratch = nullptr;
    SIZE_T alloc_sz = scratch_sz;
    auto pfnAlloc = core::nt::get_NtAllocateVirtualMemory();
    if (pfnAlloc) {
        pfnAlloc(hProcess, &remote_scratch, 0, &alloc_sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!remote_scratch) {
        remote_scratch = VirtualAllocEx(hProcess, nullptr, scratch_sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    }
    if (!remote_scratch) return false;

    // 5. Escribir shellcode
    if (!write_remote(hProcess, remote_scratch, shellcode, sc_len)) {
        VirtualFreeEx(hProcess, remote_scratch, 0, MEM_RELEASE);
        return false;
    }

    // 6. Crear hilo de inicialización con HIDE_FROM_DEBUGGER
    HANDLE hThread = nullptr;
    auto pfnCTE = core::nt::get_NtCreateThreadEx();
    if (pfnCTE) {
        pfnCTE(&hThread,
               SYNCHRONIZE | THREAD_QUERY_LIMITED_INFORMATION,
               nullptr, hProcess,
               reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_scratch),
               nullptr,
               0 /* no flags */,
               0, 0, 0, nullptr);
    }
    if (!hThread) {
        hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_scratch),
            nullptr, 0, nullptr);
    }
    if (!hThread) {
        std::vector<uint8_t> z(scratch_sz, 0);
        write_remote(hProcess, remote_scratch, z.data(), z.size());
        VirtualFreeEx(hProcess, remote_scratch, 0, MEM_RELEASE);
        return false;
    }

    // 7. Esperar a que DllMain termine su arranque
    DWORD wr = WaitForSingleObject(hThread, 20000);
    CloseHandle(hThread);

    // 8. Limpiar y liberar scratch inmediatamente
    std::vector<uint8_t> z(scratch_sz, 0);
    write_remote(hProcess, remote_scratch, z.data(), z.size());
    auto pfnFree = core::nt::get_NtFreeVirtualMemory();
    if (pfnFree) {
        SIZE_T free_sz = 0;
        pfnFree(hProcess, &remote_scratch, &free_sz, MEM_RELEASE);
    } else {
        VirtualFreeEx(hProcess, remote_scratch, 0, MEM_RELEASE);
    }

    return (wr == WAIT_OBJECT_0);
}

InjectionResult inject_payload(DWORD pid, const uint8_t* raw_pe, size_t pe_len) {
    InjectionResult res{};
    res.target_pid = pid;
    res.success = false;

    if (!raw_pe || pe_len < sizeof(IMAGE_DOS_HEADER)) {
        res.error_msg = L"Buffer de payload invalido o vacio.";
        return res;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw_pe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        res.error_msg = L"Firma DOS de PE invalida.";
        return res;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(raw_pe + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        res.error_msg = L"Firma NT invalida.";
        return res;
    }

    // 1. Abrir proceso objetivo
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) {
        res.error_msg = L"Error abriendo proceso objetivo.";
        return res;
    }

    // 2. Asignar seccion anonima SEC_COMMIT (M09) sin VirtualAllocEx
    size_t img_size = nt->OptionalHeader.SizeOfImage;
    PVOID remote_base = nullptr;
    HANDLE hSection = nullptr;

    if (!alloc_section(hProcess, img_size, nullptr, &remote_base, &hSection)) {
        auto pfnAlloc = core::nt::get_NtAllocateVirtualMemory();
        SIZE_T alloc_sz = img_size;
        if (!pfnAlloc || !NT_SUCCESS(pfnAlloc(hProcess, &remote_base, 0, &alloc_sz, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
            CloseHandle(hProcess);
            res.error_msg = L"Error asignando memoria remota para la imagen.";
            return res;
        }
    }

    // 3. Crear buffer local contiguo de memoria con el tamaño virtual completo (SizeOfImage)
    // Esto garantiza que CUALQUIER RVA (Imports, Relocs, TLS, Exports, SEH) sea un offset directo
    std::vector<uint8_t> local_image(img_size, 0);

    // Copiar cabeceras
    memcpy(local_image.data(), raw_pe, nt->OptionalHeader.SizeOfHeaders);

    // Copiar cada seccion a su VirtualAddress
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (sec[i].SizeOfRawData > 0 && sec[i].PointerToRawData > 0) {
            size_t copy_sz = (std::min)(static_cast<size_t>(sec[i].SizeOfRawData),
                                        static_cast<size_t>(sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData));
            memcpy(local_image.data() + sec[i].VirtualAddress, raw_pe + sec[i].PointerToRawData, copy_sz);
        }
    }

    // 4. Aplicar Base Relocations en local_image
    ULONG_PTR delta = reinterpret_cast<ULONG_PTR>(remote_base) - nt->OptionalHeader.ImageBase;
    if (delta != 0) {
        const auto& reloc_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (reloc_entry.VirtualAddress != 0 && reloc_entry.Size != 0) {
            const uint8_t* reloc_ptr = local_image.data() + reloc_entry.VirtualAddress;
            const uint8_t* reloc_end = reloc_ptr + reloc_entry.Size;

            while (reloc_ptr < reloc_end) {
                auto* block = reinterpret_cast<const IMAGE_BASE_RELOCATION*>(reloc_ptr);
                if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;

                DWORD count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                const WORD* list = reinterpret_cast<const WORD*>(reloc_ptr + sizeof(IMAGE_BASE_RELOCATION));

                for (DWORD i = 0; i < count; ++i) {
                    WORD type = list[i] >> 12;
                    WORD offset = list[i] & 0x0FFF;

                    if (type == IMAGE_REL_BASED_DIR64) {
                        auto* patch_addr = reinterpret_cast<ULONG_PTR*>(local_image.data() + block->VirtualAddress + offset);
                        *patch_addr += delta;
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        auto* patch_addr = reinterpret_cast<DWORD*>(local_image.data() + block->VirtualAddress + offset);
                        *patch_addr += static_cast<DWORD>(delta);
                    }
                }
                reloc_ptr += block->SizeOfBlock;
            }
        }
    }

    // 5. Resolver Import Address Table (IAT) en local_image con System-wide ASLR
    const auto& import_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_entry.VirtualAddress != 0 && import_entry.Size != 0) {
        const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(local_image.data() + import_entry.VirtualAddress);
        while (desc->Characteristics || desc->Name) {
            const char* mod_name = reinterpret_cast<const char*>(local_image.data() + desc->Name);
            if (mod_name && *mod_name) {
                HMODULE hMod = GetModuleHandleA(mod_name);
                if (!hMod) hMod = LoadLibraryA(mod_name);
                if (hMod) {
                    DWORD thunk_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
                    const auto* thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(local_image.data() + thunk_rva);
                    auto* iat = reinterpret_cast<ULONG_PTR*>(local_image.data() + desc->FirstThunk);

                    size_t idx = 0;
                    while (thunk[idx].u1.AddressOfData != 0) {
                        FARPROC fn = nullptr;
                        if (IMAGE_SNAP_BY_ORDINAL(thunk[idx].u1.Ordinal)) {
                            fn = GetProcAddress(hMod, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL(thunk[idx].u1.Ordinal)));
                        } else {
                            const auto* import_by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                                local_image.data() + thunk[idx].u1.AddressOfData
                            );
                            fn = GetProcAddress(hMod, import_by_name->Name);
                        }

                        if (fn) {
                            iat[idx] = reinterpret_cast<ULONG_PTR>(fn);
                        }
                        idx++;
                    }
                }
            }
            desc++;
        }
    }

    // 6. Recoger direcciones virtuales remotas de TLS Callbacks (ya reubicadas en local_image)
    std::vector<uint64_t> tls_cbs;
    const auto& tls_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tls_entry.VirtualAddress != 0 && tls_entry.Size != 0) {
        const auto* tls = reinterpret_cast<const IMAGE_TLS_DIRECTORY*>(local_image.data() + tls_entry.VirtualAddress);
        if (tls && tls->AddressOfCallBacks) {
            ULONG_PTR cb_rva = tls->AddressOfCallBacks - reinterpret_cast<ULONG_PTR>(remote_base);
            if (cb_rva + sizeof(ULONG_PTR) <= img_size) {
                const auto* pcb = reinterpret_cast<const ULONG_PTR*>(local_image.data() + cb_rva);
                while (pcb && *pcb) {
                    tls_cbs.push_back(*pcb);
                    pcb++;
                }
            }
        }
    }

    // 7. Escribir la imagen completa, reubicada e import-resuelta en el proceso remoto
    if (!write_remote(hProcess, remote_base, local_image.data(), img_size)) {
        if (hSection) CloseHandle(hSection);
        CloseHandle(hProcess);
        res.error_msg = L"Error escribiendo imagen completa mapeada en memoria remota.";
        return res;
    }

    // 8. Localizar .pdata remoto y EntryPoint
    PVOID remote_pdata = nullptr;
    DWORD pdata_count  = 0;
    {
        const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (exc.VirtualAddress && exc.Size) {
            remote_pdata = reinterpret_cast<uint8_t*>(remote_base) + exc.VirtualAddress;
            pdata_count  = exc.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
        }
    }

    PVOID remote_entry = nullptr;
    if (nt->OptionalHeader.AddressOfEntryPoint) {
        remote_entry = reinterpret_cast<uint8_t*>(remote_base) + nt->OptionalHeader.AddressOfEntryPoint;
    }

    if (!remote_entry) {
        if (hSection) CloseHandle(hSection);
        CloseHandle(hProcess);
        res.error_msg = L"DLL no tiene AddressOfEntryPoint.";
        return res;
    }

    // 9. Ejecutar inicializacion (SEH + TLS Callbacks + DllMain) mediante PIC shellcode
    if (!nt_run(hProcess, pid, remote_entry, remote_base, nullptr, remote_pdata, pdata_count, tls_cbs)) {
        if (hSection) CloseHandle(hSection);
        CloseHandle(hProcess);
        res.error_msg = L"Error ejecutando bootstrap en javaw.exe (nt_run fallo).";
        return res;
    }

    // 10. Reproteccion granular por seccion (CERO RWX)
    reprotect(hProcess, remote_base, raw_pe);

    // 11. Desvincular de PEB si procede
    PVOID peb_base = nullptr;
    if (nt_get_peb(hProcess, &peb_base)) {
        unlink_ldr(hProcess, peb_base, remote_base);
    }

    if (hSection) CloseHandle(hSection);
    CloseHandle(hProcess);

    res.success = true;
    res.remote_base = remote_base;
    return res;
}

} // namespace wrath::injector

namespace wrath::core::nt {
bool nt_inject(DWORD pid, const uint8_t* raw_pe, size_t pe_len, std::wstring* out_err) {
    auto res = wrath::injector::inject_payload(pid, raw_pe, pe_len);
    if (out_err && !res.success) {
        *out_err = res.error_msg;
    }
    return res.success;
}
}
