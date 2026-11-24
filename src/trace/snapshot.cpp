#include "trace/snapshot.h"
#include "shared/nt/nt.h"
#include "shared/util/enc.h"

#include <sddl.h>
#include <bcrypt.h>
#include <cwctype>
#include <fstream>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace wrath::trace {

namespace {

bool compute_sha256(const uint8_t* data, size_t len, uint8_t out_hash[32]) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!NT_SUCCESS(status)) return false;

    bool ok = false;
    if (NT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        if (NT_SUCCESS(BCryptHashData(hHash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0))) {
            if (NT_SUCCESS(BCryptFinishHash(hHash, out_hash, 32, 0))) {
                ok = true;
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

} // namespace

uint32_t calculate_prefetch_hash(const wchar_t* full_path) {
    if (!full_path) return 0;
    uint32_t hash = 314159;
    while (*full_path) {
        wchar_t c = static_cast<wchar_t>(towupper(*full_path++));
        hash = (hash * 37) + static_cast<uint32_t>(c);
    }
    return hash;
}

bool get_current_user_sid(std::wstring& out_sid) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return false;
    }

    DWORD len = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &len);
    if (!len) {
        CloseHandle(hToken);
        return false;
    }

    std::vector<uint8_t> buffer(len);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), len, &len)) {
        CloseHandle(hToken);
        return false;
    }
    CloseHandle(hToken);

    auto* token_user = reinterpret_cast<TOKEN_USER*>(buffer.data());
    LPWSTR str_sid = nullptr;
    if (ConvertSidToStringSidW(token_user->User.Sid, &str_sid) && str_sid) {
        out_sid = str_sid;
        LocalFree(str_sid);
        return true;
    }
    return false;
}

bool get_prefetch_path_for_binary(const wchar_t* binary_path, wchar_t* out_pf_path, size_t max_len) {
    if (!binary_path || !out_pf_path || max_len < MAX_PATH) return false;

    // Extraer nombre del archivo (ej. BackupManager.exe)
    const wchar_t* filename = wcsrchr(binary_path, L'\\');
    filename = filename ? filename + 1 : binary_path;

    // Obtener ruta de Windows (normalmente C:\Windows)
    wchar_t win_dir[MAX_PATH]{};
    GetWindowsDirectoryW(win_dir, MAX_PATH);

    wchar_t search_pattern[MAX_PATH]{};
    // Buscar sin la extension .exe
    wchar_t name_no_ext[MAX_PATH]{};
    wcscpy_s(name_no_ext, filename);
    wchar_t* dot = wcsrchr(name_no_ext, L'.');
    if (dot) *dot = L'\0';

    swprintf_s(search_pattern, L"%s\\Prefetch\\%s-*.pf", win_dir, name_no_ext);

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search_pattern, &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        swprintf_s(out_pf_path, max_len, L"%s\\Prefetch\\%s", win_dir, fd.cFileName);
        FindClose(hFind);
        return true;
    }

    // Si aún no existe, precalcular el nombre esperado
    uint32_t hash = calculate_prefetch_hash(binary_path);
    swprintf_s(out_pf_path, max_len, L"%s\\Prefetch\\%s-%08X.pf", win_dir, name_no_ext, hash);
    return false;
}

bool snapshot_artifacts(const wchar_t* loader_path, snapshot_t* out_snap) {
    if (!loader_path || !out_snap) return false;
    ZeroMemory(out_snap, sizeof(snapshot_t));
    wcscpy_s(out_snap->loader_path, loader_path);

    // 1. Localizar y leer .pf
    bool pf_exists = get_prefetch_path_for_binary(loader_path, out_snap->pf_path, MAX_PATH);
    if (pf_exists) {
        HANDLE hFile = CreateFileW(
            out_snap->pf_path,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(hFile, nullptr);
            if (size > 0 && size < 16 * 1024 * 1024) {
                out_snap->pf_bytes = new uint8_t[size];
                DWORD read = 0;
                if (ReadFile(hFile, out_snap->pf_bytes, size, &read, nullptr) && read == size) {
                    out_snap->pf_len = size;
                    compute_sha256(out_snap->pf_bytes, out_snap->pf_len, out_snap->pf_sha256);
                } else {
                    delete[] out_snap->pf_bytes;
                    out_snap->pf_bytes = nullptr;
                    out_snap->pf_len = 0;
                }
            }
            CloseHandle(hFile);
        }
    }

    // 2. Leer timestamps del loader en disco ($STANDARD_INFORMATION)
    HANDLE hLoader = CreateFileW(
        loader_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );
    if (hLoader != INVALID_HANDLE_VALUE) {
        GetFileTime(hLoader, &out_snap->si[0], &out_snap->si[1], &out_snap->si[2]);

        // 3. Timestamps $FILE_NAME via NtQueryInformationFile (FileBasicInformation)
        auto pfnQuery = core::nt::get_NtQueryInformationFile();
        if (pfnQuery) {
            IO_STATUS_BLOCK iosb{};
            core::nt::FILE_BASIC_INFORMATION fbi{};
            NTSTATUS status = pfnQuery(
                hLoader,
                &iosb,
                &fbi,
                sizeof(fbi),
                4 /* FileBasicInformation */
            );
            if (NT_SUCCESS(status)) {
                out_snap->fn[0].dwLowDateTime = fbi.CreationTime.LowPart;
                out_snap->fn[0].dwHighDateTime = fbi.CreationTime.HighPart;
                out_snap->fn[1].dwLowDateTime = fbi.LastAccessTime.LowPart;
                out_snap->fn[1].dwHighDateTime = fbi.LastAccessTime.HighPart;
                out_snap->fn[2].dwLowDateTime = fbi.LastWriteTime.LowPart;
                out_snap->fn[2].dwHighDateTime = fbi.LastWriteTime.HighPart;
            }
        }
        CloseHandle(hLoader);
    }

    // 4. Leer valor BAM si existe
    std::wstring sid;
    if (get_current_user_sid(sid)) {
        std::wstring bam_key = L"SYSTEM\\CurrentControlSet\\Services\\bam\\State\\UserSettings\\" + sid;
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, bam_key.c_str(), 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
            DWORD type = 0;
            DWORD size = sizeof(out_snap->bam);
            if (RegQueryValueExW(hKey, loader_path, nullptr, &type, out_snap->bam, &size) == ERROR_SUCCESS) {
                out_snap->bam_exist = true;
            }
            RegCloseKey(hKey);
        }
    }

    return true;
}

void free_snapshot(snapshot_t* snap) {
    if (!snap) return;
    if (snap->pf_bytes) {
        delete[] snap->pf_bytes;
        snap->pf_bytes = nullptr;
    }
    snap->pf_len = 0;
}

bool save_snapshot_to_file(const snapshot_t& snap, const wchar_t* file_path) {
    std::ofstream out(file_path, std::ios::binary);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(snap.pf_path), sizeof(snap.pf_path));
    out.write(reinterpret_cast<const char*>(snap.loader_path), sizeof(snap.loader_path));
    out.write(reinterpret_cast<const char*>(snap.pf_sha256), sizeof(snap.pf_sha256));
    out.write(reinterpret_cast<const char*>(snap.si), sizeof(snap.si));
    out.write(reinterpret_cast<const char*>(snap.fn), sizeof(snap.fn));
    out.write(reinterpret_cast<const char*>(snap.bam), sizeof(snap.bam));
    out.write(reinterpret_cast<const char*>(&snap.bam_exist), sizeof(snap.bam_exist));

    uint64_t len = static_cast<uint64_t>(snap.pf_len);
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0 && snap.pf_bytes) {
        out.write(reinterpret_cast<const char*>(snap.pf_bytes), len);
    }
    return true;
}

bool load_snapshot_from_file(snapshot_t* out_snap, const wchar_t* file_path) {
    if (!out_snap) return false;
    std::ifstream in(file_path, std::ios::binary);
    if (!in.is_open()) return false;

    ZeroMemory(out_snap, sizeof(snapshot_t));
    in.read(reinterpret_cast<char*>(out_snap->pf_path), sizeof(out_snap->pf_path));
    in.read(reinterpret_cast<char*>(out_snap->loader_path), sizeof(out_snap->loader_path));
    in.read(reinterpret_cast<char*>(out_snap->pf_sha256), sizeof(out_snap->pf_sha256));
    in.read(reinterpret_cast<char*>(out_snap->si), sizeof(out_snap->si));
    in.read(reinterpret_cast<char*>(out_snap->fn), sizeof(out_snap->fn));
    in.read(reinterpret_cast<char*>(out_snap->bam), sizeof(out_snap->bam));
    in.read(reinterpret_cast<char*>(&out_snap->bam_exist), sizeof(out_snap->bam_exist));

    uint64_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (len > 0) {
        out_snap->pf_len = static_cast<size_t>(len);
        out_snap->pf_bytes = new uint8_t[out_snap->pf_len];
        in.read(reinterpret_cast<char*>(out_snap->pf_bytes), out_snap->pf_len);
    }
    return true;
}

} // namespace wrath::trace
