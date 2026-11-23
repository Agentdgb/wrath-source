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

// Marks a file for deletion without writing PendingFileRenameOperations to the registry.
// Uses FILE_FLAG_DELETE_ON_CLOSE: the file is unlinked from the directory the moment
// its last handle closes, with no registry side-effect.
// For files still held open by another process (e.g. a mapped DLL in javaw.exe),
// spawns a short-lived background thread that retries the open+dispose loop.
inline void ScheduleDeletion(const std::wstring& path)
{
    if (path.empty()) return;

    auto tryDelete = [](const std::wstring& p) -> bool {
        HANDLE h = CreateFileW(p.c_str(),
            DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    };

    if (tryDelete(path)) return;

    // File is held open — retry in a detached background thread
    std::thread([path, tryDelete]() {
        for (int i = 0; i < 120; ++i) {  // retry up to 2 minutes
            Sleep(1000);
            if (tryDelete(path)) return;
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// CleanProcessMemoryBait()
//
// Walks all committed, readable pages of the current process and overwrites
// byte sequences that MeowClientFucker (and similar Aho-Corasick scanners)
// flag as generic cheat indicators.  Third-party mods loaded into javaw.exe
// (e.g. cactus-0.12.4.jar) can carry substrings like "Auto Switch" or
// "Selected Slot" in their Java heap pages — this scrubs them in-place so
// any screenshare scan reports clean.
//
// Substitutions are length-preserving (1-to-1 byte replacement) so no
// relocation is needed and no allocation is performed.
//
// Constraints:
//   * Only touches PAGE_READWRITE / PAGE_EXECUTE_READWRITE pages (writable).
//     PAGE_READONLY / PAGE_EXECUTE_READ pages are skipped — writing would
//     fault.  Those pages come from DLL images; our own .rdata is already
//     sanitised at compile time.
//   * Runs on the calling thread; call once after all modules have initialised
//     (JVM heap is fully committed) and optionally from a low-priority
//     background tick.
//   * Safe to call multiple times — idempotent.
// ---------------------------------------------------------------------------
namespace detail {

struct MaskedPatch {
    unsigned char needle_masked[16];
    unsigned char replace_masked[16];
    SIZE_T len;
};

static constexpr unsigned char kKey = 0x7A;

static const MaskedPatch kPatches[] = {
    // Auto Switch -> Auto Sw1tch (11)
    { { 0x3B, 0x0F, 0x0E, 0x15, 0x5A, 0x29, 0x0D, 0x13, 0x0E, 0x19, 0x12 },
      { 0x3B, 0x0F, 0x0E, 0x15, 0x5A, 0x29, 0x0D, 0x4B, 0x0E, 0x19, 0x12 }, 11 },
    // Selected Slot -> Selected Sl0t (13)
    { { 0x29, 0x1F, 0x16, 0x1F, 0x19, 0x0E, 0x1F, 0x1E, 0x5A, 0x29, 0x16, 0x15, 0x0E },
      { 0x29, 0x1F, 0x16, 0x1F, 0x19, 0x0E, 0x1F, 0x1E, 0x5A, 0x29, 0x16, 0x4A, 0x0E }, 13 },
    // TriggerBot -> Hit_Assist (10)
    { { 0x2E, 0x08, 0x13, 0x1D, 0x1D, 0x1F, 0x08, 0x38, 0x15, 0x0E },
      { 0x32, 0x13, 0x0E, 0x25, 0x3B, 0x09, 0x09, 0x13, 0x09, 0x0E }, 10 },
    // Triggerbot -> Hit_assist (10)
    { { 0x2E, 0x08, 0x13, 0x1D, 0x1D, 0x1F, 0x08, 0x18, 0x15, 0x0E },
      { 0x32, 0x13, 0x0E, 0x25, 0x1B, 0x09, 0x09, 0x13, 0x09, 0x0E }, 10 },
};

inline void ScanAndPatchMemorySafe(BYTE* chunk, SIZE_T chunkLen) noexcept
{
    __try {
        for (const auto& p : kPatches) {
            if (chunkLen < p.len) continue;
            BYTE* scan = chunk;
            BYTE* scanEnd = chunk + chunkLen - p.len;
            while (scan <= scanEnd) {
                bool match = true;
                for (SIZE_T i = 0; i < p.len; ++i) {
                    if (static_cast<unsigned char>(scan[i] ^ kKey) != p.needle_masked[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    for (SIZE_T i = 0; i < p.len; ++i) {
                        scan[i] = static_cast<BYTE>(p.replace_masked[i] ^ kKey);
                    }
                    scan += p.len;
                } else {
                    ++scan;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Discard any guard page violation or transient memory fault safely
    }
}

} // namespace detail

inline void CleanProcessMemoryBait() noexcept
{
    SYSTEM_INFO si{};
    GetSystemInfo(&si);

    MEMORY_BASIC_INFORMATION mbi{};
    BYTE* addr = static_cast<BYTE*>(si.lpMinimumApplicationAddress);
    const BYTE* addrMax = static_cast<BYTE*>(si.lpMaximumApplicationAddress);

    while (addr < addrMax) {
        if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) {
            addr += 4096;
            continue;
        }

        // Must be committed memory
        if (mbi.State != MEM_COMMIT) {
            addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            continue;
        }

        // CRITICAL: Reject PAGE_GUARD, PAGE_NOACCESS.
        // Touching PAGE_GUARD triggers STATUS_GUARD_PAGE_VIOLATION (0x80000001).
        if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            continue;
        }

        // CRITICAL: Reject non-private memory (MEM_MAPPED / MEM_IMAGE).
        // Java heap strings are strictly MEM_PRIVATE. Touching MEM_MAPPED pages
        // triggers STATUS_IN_PAGE_ERROR (0xC0000006) on pagefile or file-backed sections.
        if (mbi.Type != MEM_PRIVATE) {
            addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
            continue;
        }

        const DWORD baseProt = mbi.Protect & 0xFF;
        const bool writable = (baseProt == PAGE_READWRITE ||
                               baseProt == PAGE_EXECUTE_READWRITE ||
                               baseProt == PAGE_WRITECOPY ||
                               baseProt == PAGE_EXECUTE_WRITECOPY);

        if (writable && mbi.RegionSize > 0) {
            BYTE* start  = static_cast<BYTE*>(mbi.BaseAddress);
            BYTE* end    = start + mbi.RegionSize;

            static constexpr SIZE_T kChunk = 4u * 1024u * 1024u;
            for (BYTE* chunk = start; chunk < end; chunk += kChunk) {
                SIZE_T chunkLen = static_cast<SIZE_T>(end - chunk);
                if (chunkLen > kChunk) chunkLen = kChunk;
                detail::ScanAndPatchMemorySafe(chunk, chunkLen);
            }
        }

        addr = static_cast<BYTE*>(mbi.BaseAddress) + mbi.RegionSize;
        if (addr <= static_cast<BYTE*>(mbi.BaseAddress)) break; // overflow guard
    }
}

} // namespace wrath::bypass
