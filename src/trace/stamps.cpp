#include "trace/snapshot.h"
#include "core/nt.h"

namespace wrath::trace {

bool restore_stamps(const snapshot_t& snap) {
    if (!snap.loader_path[0]) return true;

    HANDLE hFile = CreateFileW(
        snap.loader_path,
        GENERIC_READ | GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    // 1. $STANDARD_INFORMATION via SetFileTime
    bool ok_si = SetFileTime(hFile, &snap.si[0], &snap.si[1], &snap.si[2]);

    // 2. $FILE_NAME timestamps via NtSetInformationFile (FileBasicInformation)
    bool ok_fn = false;
    auto pfnSetInfo = core::nt::get_NtSetInformationFile();
    if (pfnSetInfo) {
        IO_STATUS_BLOCK iosb{};
        core::nt::FILE_BASIC_INFORMATION fbi{};
        fbi.CreationTime.LowPart = snap.fn[0].dwLowDateTime;
        fbi.CreationTime.HighPart = snap.fn[0].dwHighDateTime;
        fbi.LastAccessTime.LowPart = snap.fn[1].dwLowDateTime;
        fbi.LastAccessTime.HighPart = snap.fn[1].dwHighDateTime;
        fbi.LastWriteTime.LowPart = snap.fn[2].dwLowDateTime;
        fbi.LastWriteTime.HighPart = snap.fn[2].dwHighDateTime;
        fbi.ChangeTime.LowPart = snap.fn[2].dwLowDateTime;
        fbi.ChangeTime.HighPart = snap.fn[2].dwHighDateTime;
        fbi.FileAttributes = 0; // 0 leaves attributes unchanged

        NTSTATUS status = pfnSetInfo(
            hFile,
            &iosb,
            &fbi,
            sizeof(fbi),
            4 /* FileBasicInformation */
        );
        ok_fn = NT_SUCCESS(status);
    }

    CloseHandle(hFile);
    return ok_si && ok_fn;
}

} // namespace wrath::trace
