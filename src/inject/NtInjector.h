#pragma once
#ifndef WRATH_NT_INJECTOR_H
#define WRATH_NT_INJECTOR_H

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

#include "core/nt.h"
#include "core/hash.h"

namespace wrath::injector {

struct InjectionResult {
    bool success;
    PVOID remote_base;
    DWORD target_pid;
    std::wstring error_msg;
};

// 1. Localiza el proceso objetivo por hash FNV-1a (ej: "javaw.exe"_h) sin cadenas en claro
bool nt_find_process(core::hash_t name_hash, DWORD* out_pid);

// 2. Obtiene la dirección del PEB remoto
bool nt_get_peb(HANDLE hProcess, PVOID* out_peb_base);

// 3. Localiza la base de un módulo remoto recorriendo InLoadOrderModuleList del PEB
PVOID nt_find_module(HANDLE hProcess, PVOID peb_base, core::hash_t module_hash);

// 4. Resuelve la dirección virtual de un export en un módulo remoto por hash FNV-1a
PVOID nt_resolve_export(HANDLE hProcess, PVOID module_base, core::hash_t fn_hash);

// 5. M09: Asigna una sección anónima SEC_COMMIT pagefile-backed (sin nombre)
bool alloc_section(HANDLE hProcess, size_t size, PVOID preferred_base, PVOID* out_base, HANDLE* out_section);

// 6. M03: Mapea la imagen PE en el proceso remoto aplicando relocs x64 e imports por hash
bool map_image(HANDLE hProcess, PVOID base, const uint8_t* raw_pe, size_t pe_len);

// 7. M04: Desenlaza el módulo de las 3 listas del PEB y pone a cero los nombres
bool unlink_ldr(HANDLE hProcess, PVOID peb_base, PVOID module_base);

// 8. M04: Borra cabeceras MZ, PE y secciones en el orden estricto del spec
bool wipe_headers(HANDLE hProcess, PVOID base, const uint8_t* raw_pe);

// 9. M04: Reprotege cada sección individualmente (.text RX, .rdata R, .data RW). Cero RWX.
bool reprotect(HANDLE hProcess, PVOID base, const uint8_t* raw_pe);

// 10. M03: Ejecución vía Thread Hijacking con respaldos / NtCreateThreadEx
bool nt_run(HANDLE hProcess, DWORD pid, PVOID entry_point, PVOID remote_base, PVOID param,
            PVOID remote_pdata = nullptr, DWORD pdata_count = 0,
            const std::vector<uint64_t>& tls_callbacks = {});

// Pipeline unificado M03 + M09 + M04
InjectionResult inject_payload(DWORD pid, const uint8_t* raw_pe, size_t pe_len);

} // namespace wrath::injector

#endif // WRATH_NT_INJECTOR_H
