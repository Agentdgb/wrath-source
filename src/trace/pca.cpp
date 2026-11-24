#include "trace/snapshot.h"
#include <vector>
#include <string>
#include <sstream>

namespace wrath::trace {

namespace {

void clean_pca_key(HKEY root, const wchar_t* subkey, const wchar_t* filename) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }

    DWORD val_count = 0;
    DWORD max_val_len = 0;
    if (RegQueryInfoKeyW(hKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         &val_count, &max_val_len, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> val_name(max_val_len + 2);
        std::vector<std::wstring> to_delete;

        for (DWORD i = 0; i < val_count; ++i) {
            DWORD len = max_val_len + 1;
            if (RegEnumValueW(hKey, i, val_name.data(), &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
                if (wcsstr(val_name.data(), filename) != nullptr) {
                    to_delete.push_back(val_name.data());
                }
            }
        }

        for (const auto& name : to_delete) {
            RegDeleteValueW(hKey, name.c_str());
        }
        if (!to_delete.empty()) {
            RegFlushKey(hKey);
        }
    }

    RegCloseKey(hKey);
}

bool sanitize_pca_app_launch_dic(const snapshot_t& snap) {
    wchar_t win_dir[MAX_PATH]{};
    if (!GetWindowsDirectoryW(win_dir, MAX_PATH)) return false;

    std::wstring dic_path = std::wstring(win_dir) + L"\\appcompat\\pca\\PcaAppLaunchDic.txt";
    HANDLE hFile = CreateFileW(
        dic_path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) return false;

    FILETIME ft_create{}, ft_access{}, ft_write{};
    if (!GetFileTime(hFile, &ft_create, &ft_access, &ft_write)) {
        CloseHandle(hFile);
        return false;
    }

    DWORD file_sz = GetFileSize(hFile, nullptr);
    if (file_sz == 0 || file_sz == INVALID_FILE_SIZE) {
        CloseHandle(hFile);
        return false;
    }

    std::vector<char> buffer(file_sz + 1, 0);
    DWORD bytes_read = 0;
    if (!ReadFile(hFile, buffer.data(), file_sz, &bytes_read, nullptr) || bytes_read == 0) {
        CloseHandle(hFile);
        return false;
    }

    const wchar_t* filename_w = wcsrchr(snap.loader_path, L'\\');
    filename_w = filename_w ? filename_w + 1 : snap.loader_path;

    char filename_a[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, filename_w, -1, filename_a, sizeof(filename_a), nullptr, nullptr);

    char fullpath_a[MAX_PATH]{};
    WideCharToMultiByte(CP_UTF8, 0, snap.loader_path, -1, fullpath_a, sizeof(fullpath_a), nullptr, nullptr);

    std::string content(buffer.data(), bytes_read);
    std::string new_content;
    new_content.reserve(content.size());

    std::stringstream ss(content);
    std::string line;
    bool modified = false;

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();

        bool match = false;
        if (filename_a[0] && line.find(filename_a) != std::string::npos) match = true;
        if (fullpath_a[0] && line.find(fullpath_a) != std::string::npos) match = true;
        if (line.find("DisplayTunerService") != std::string::npos ||
            line.find("WrathLoader") != std::string::npos ||
            line.find("BackupManager") != std::string::npos ||
            line.find("display_service64") != std::string::npos ||
            line.find("WrathPayload") != std::string::npos) {
            match = true;
        }

        if (match) {
            modified = true;
        } else {
            new_content += line + "\r\n";
        }
    }

    if (modified) {
        SetFilePointer(hFile, 0, nullptr, FILE_BEGIN);
        DWORD written = 0;
        WriteFile(hFile, new_content.data(), static_cast<DWORD>(new_content.size()), &written, nullptr);
        SetEndOfFile(hFile);
        FlushFileBuffers(hFile);
    }

    // Restaurar sellos de tiempo exactos del log original
    SetFileTime(hFile, &ft_create, &ft_access, &ft_write);
    CloseHandle(hFile);
    return true;
}

} // namespace

bool restore_pca(const snapshot_t& snap) {
    const wchar_t* filename = wcsrchr(snap.loader_path, L'\\');
    filename = filename ? filename + 1 : snap.loader_path;

    clean_pca_key(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\PcaClient",
        filename
    );

    clean_pca_key(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Compatibility Assistant\\Store",
        filename
    );

    // Sanitizar archivo de compatibilidad PcaAppLaunchDic.txt antes de restaurar atributos de fecha
    sanitize_pca_app_launch_dic(snap);

    return true;
}

} // namespace wrath::trace
