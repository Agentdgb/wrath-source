#pragma once
#ifndef WRATH_TRACE_SNAPSHOT_H
#define WRATH_TRACE_SNAPSHOT_H

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace wrath::trace {

#pragma pack(push, 8)
struct snapshot_t {
    wchar_t pf_path[MAX_PATH];   // ruta al archivo .pf
    uint8_t* pf_bytes;           // bytes originales del .pf
    size_t   pf_len;
    uint8_t  pf_sha256[32];      // hash SHA-256 de los bytes originales
    FILETIME si[3];              // $STANDARD_INFORMATION: create, access, write
    FILETIME fn[3];              // $FILE_NAME: create, access, write
    uint8_t  bam[8];             // valor REG_BINARY de 8 bytes
    bool     bam_exist;          // si existía la entrada antes de la sesión
    wchar_t  loader_path[MAX_PATH]; // ruta completa del ejecutable
};
#pragma pack(pop)

// Calcula el hash Prefetch de Windows 10/11 para un nombre de ejecutable
uint32_t calculate_prefetch_hash(const wchar_t* full_path);

// Obtiene la ruta del archivo .pf correspondiente al ejecutable
bool get_prefetch_path_for_binary(const wchar_t* binary_path, wchar_t* out_pf_path, size_t max_len);

// Obtiene el SID del usuario actual como cadena
bool get_current_user_sid(std::wstring& out_sid);

// Captura todos los artefactos forenses antes de ejecutar la inyección
bool snapshot_artifacts(const wchar_t* loader_path, snapshot_t* out_snap);

// Libera los buffers en memoria del snapshot
void free_snapshot(snapshot_t* snap);

// Persistencia en disco (.dat dentro del VHD)
bool save_snapshot_to_file(const snapshot_t& snap, const wchar_t* file_path);
bool load_snapshot_from_file(snapshot_t* out_snap, const wchar_t* file_path);

} // namespace wrath::trace

#endif // WRATH_TRACE_SNAPSHOT_H
