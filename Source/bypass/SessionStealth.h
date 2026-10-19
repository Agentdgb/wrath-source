#pragma once

// SessionStealth.h – session cleanup helpers.
//
// REMEDIATION NOTE (2026-10): the previous implementation of this header
// manipulated system-global state and has been removed in full:
//   * Wiping BAM values (SYSTEM\CurrentControlSet\Services\bam\State\...)
//     destroyed legitimate Windows execution-history data and left a
//     blatant tampering signature on the machine.
//   * Forcing EnablePrefetcher=0 degraded machine-wide I/O performance.
//   * MoveFileEx(MOVEFILE_DELAY_UNTIL_REBOOT) polluted the global
//     PendingFileRenameOperations registry value with session artifacts
//     that outlived the process.
// This code no longer touches the registry in any way. The remaining
// entry points are compile-compatible: PrepareSession()/RestoreSession()
// are documented no-ops, and ScheduleDeletion() performs an immediate,
// synchronous best-effort deletion.

#include <windows.h>
#include <string>

namespace wrath::bypass {

// No-op retained for source compatibility. Nothing is modified at session
// start, so there is nothing to prepare. The BAM-wipe and Prefetcher
// manipulation that used to live here were removed (2026-10 remediation):
// they damaged the user's system and constituted obvious tampering.
inline void PrepareSession() {}

// No-op retained for source compatibility. Nothing is modified during the
// session, so there is nothing to restore at unload.
inline void RestoreSession() {}

// Attempts an immediate, synchronous deletion of `path`. Files that are
// still in use (a DLL still mapped in the target process, or the running
// image itself) fail the delete and are left behind for normal %TEMP%
// housekeeping. Never defers deletion to next reboot: a
// MOVEFILE_DELAY_UNTIL_REBOOT request pollutes the system-wide
// PendingFileRenameOperations value with session artifacts.
inline void ScheduleDeletion(const std::wstring& path)
{
    if (!path.empty())
        DeleteFileW(path.c_str());
}

} // namespace wrath::bypass
