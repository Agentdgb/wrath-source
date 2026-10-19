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

constexpr uint32_t kHeartbeatMagic = 0x57525448; // 'WRTH'
constexpr uint32_t kHeartbeatVersion = 1;

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
    // Genera un identificador pseudoaleatorio salteado indistinguible de objetos nativos del sistema
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&pid);
    for (size_t i = 0; i < sizeof(pid); ++i) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    hash ^= 0x575254485F495043ULL; // Salteado interno: 'WRTH_IPC'
    hash *= 0x100000001b3ULL;

    wchar_t buf[64]{};
    swprintf_s(buf, L"Local\\WinSync_%08X%08X",
               static_cast<uint32_t>((hash >> 32) & 0xFFFFFFFF),
               static_cast<uint32_t>(hash & 0xFFFFFFFF));
    return std::wstring(buf);
}

// File deletion is handled by the validated profile storage API.

} // namespace wrath::bypass
