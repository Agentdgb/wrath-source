#include "trace/snapshot.h"
#include "core/nt.h"

#include <bcrypt.h>

namespace wrath::trace {

namespace {

bool hash_disk_file_sha256(const wchar_t* path, uint8_t out_hash[32]) {
    HANDLE hFile = CreateFileW(
        path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) {
        CloseHandle(hFile);
        return false;
    }

    std::vector<uint8_t> buffer(size);
    DWORD read = 0;
    if (!ReadFile(hFile, buffer.data(), size, &read, nullptr) || read != size) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }

    bool ok = false;
    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        if (NT_SUCCESS(BCryptHashData(hHash, buffer.data(), static_cast<ULONG>(size), 0))) {
            if (NT_SUCCESS(BCryptFinishHash(hHash, out_hash, 32, 0))) {
                ok = true;
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool force_posix_delete(const wchar_t* path) {
    HANDLE hFile = CreateFileW(
        path,
        DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    auto pfnSetInfo = core::nt::get_NtSetInformationFile();
    if (!pfnSetInfo) {
        CloseHandle(hFile);
        return false;
    }

    IO_STATUS_BLOCK iosb{};
    core::nt::FILE_DISPOSITION_INFORMATION_EX disp{};
    disp.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;

    NTSTATUS status = pfnSetInfo(
        hFile,
        &iosb,
        &disp,
        sizeof(disp),
        64 /* FileDispositionInformationEx */
    );
    CloseHandle(hFile);
    return NT_SUCCESS(status);
}

} // namespace

bool restore_prefetch(const snapshot_t& snap) {
    if (!snap.pf_path[0]) return true;

    // Si el archivo no existía antes de la sesión, eliminarlo usando POSIX semantics
    if (snap.pf_len == 0 || !snap.pf_bytes) {
        if (GetFileAttributesW(snap.pf_path) != INVALID_FILE_ATTRIBUTES) {
            if (!force_posix_delete(snap.pf_path)) {
                DeleteFileW(snap.pf_path);
            }
        }
        return true;
    }

    // Si el archivo existía, restaurar bytes y timestamps originales
    for (int retry = 0; retry < 5; ++retry) {
        HANDLE hFile = CreateFileW(
            snap.pf_path,
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile == INVALID_HANDLE_VALUE) {
            // Posiblemente bloqueado por SysMain -> forzar unlink posix y reintentar
            force_posix_delete(snap.pf_path);
            Sleep(200);
            continue;
        }

        DWORD written = 0;
        bool write_ok = WriteFile(hFile, snap.pf_bytes, static_cast<DWORD>(snap.pf_len), &written, nullptr) &&
                        written == snap.pf_len;

        if (write_ok) {
            SetFileTime(hFile, &snap.si[0], &snap.si[1], &snap.si[2]);
        }
        CloseHandle(hFile);

        if (!write_ok) {
            Sleep(200);
            continue;
        }

        // Verificar SHA-256
        uint8_t current_hash[32]{};
        if (hash_disk_file_sha256(snap.pf_path, current_hash)) {
            if (memcmp(current_hash, snap.pf_sha256, 32) == 0) {
                return true;
            }
        }

        Sleep(200);
    }

    return false;
}

} // namespace wrath::trace
