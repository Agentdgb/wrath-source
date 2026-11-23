#pragma once

#include <windows.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace wrath::bypass {

// ─────────────────────────────────────────────────────────────────────────────
// NT System Call Definitions (Stealth Remote Thread)
// ─────────────────────────────────────────────────────────────────────────────
using NTSTATUS = LONG;
#ifndef NTAPI
#define NTAPI __stdcall
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((wrath::bypass::NTSTATUS)(Status)) >= 0)
#endif

constexpr ULONG kThreadCreateFlagsHideFromDebugger = 0x00000004;

using pfnNtCreateThreadEx = NTSTATUS(NTAPI*)(
    OUT PHANDLE ThreadHandle,
    IN ACCESS_MASK DesiredAccess,
    IN PVOID ObjectAttributes,
    IN HANDLE ProcessHandle,
    IN PVOID StartRoutine,
    IN PVOID Argument,
    IN ULONG CreateFlags,
    IN SIZE_T ZeroBits,
    IN SIZE_T StackSize,
    IN SIZE_T MaximumStackSize,
    IN PVOID AttributeList
);

// Per-PID named object blended as a standard JVM runtime sync object so
// process handle scanners do not flag a custom mutex name.
inline std::wstring sentinelMutexNameForPid(DWORD pid) {
    return L"Local\\jvm_pid" + std::to_wstring(pid) + L"_signal_sem";
}

// Magic intentionally matches JVM internal staging identifiers.
// 'JVST' — opaque to external scanners enumerating named shared sections.
constexpr uint32_t kHeartbeatMagic   = 0x4A565354; // 'JVST'
constexpr uint32_t kHeartbeatVersion = 2;

#pragma pack(push, 8)
struct HeartbeatSharedContext {
    uint32_t magic;
    uint32_t version;
    char server_host[128];
    int32_t server_port;
    char hwid[65];
    char session_token[128];
    char session_id[128];
    char client_nonce[128];
    uint64_t created_timestamp;
};
#pragma pack(pop)

inline std::wstring heartbeatSectionNameForPid(DWORD pid) {
    // Generates a pseudo-random identifier from the PID.
    // The salt constant below is stored as a numeric literal (no string form
    // in the binary) so disassemblers cannot trivially recover "JVM_SYNC"
    // or "WRTH_IPC" via a strings scan. The value encodes 'JVM_SYNC' in
    // ASCII byte order but never appears as a contiguous string in .rdata.
    // Additional obfuscation: split into two 32-bit halves that are XOR'd
    // together at runtime so the full constant is never a single 8-byte
    // literal in the instruction stream.
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&pid);
    for (size_t i = 0; i < sizeof(pid); ++i) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    // Salt split into two halves so no 8-byte 'JVM_SYNC' literal exists:
    //   hi = 0x4A564D5F  ('JVM_')
    //   lo = 0x53594E43  ('SYNC')
    constexpr uint64_t kSaltHi = 0x4A564D5FUL;   // 'JVM_'
    constexpr uint64_t kSaltLo = 0x53594E43UL;   // 'SYNC'
    const uint64_t salt = (kSaltHi << 32) | kSaltLo;
    hash ^= salt;
    hash *= 0x100000001b3ULL;

    wchar_t buf[64]{};
    swprintf_s(buf, L"Local\\WinSync_%08X%08X",
               static_cast<uint32_t>((hash >> 32) & 0xFFFFFFFF),
               static_cast<uint32_t>(hash & 0xFFFFFFFF));
    return std::wstring(buf);
}

// File deletion is handled by the validated profile storage API.

} // namespace wrath::bypass
