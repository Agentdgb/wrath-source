#pragma once

// StealthDefs.h -- stub (bypass removed).
// Provides only the NT typedefs required by the injection pipeline.

#include <windows.h>
#include <cstdint>
#include <string>

#include "shared/stealth/Naming.h"

namespace wrath::bypass {

using NTSTATUS = LONG;

#ifndef NTAPI
#define NTAPI __stdcall
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((wrath::bypass::NTSTATUS)(Status)) >= 0)
#endif

// NtCreateThreadEx typedef kept for the injection pipeline.
// CreateFlags = 0 (no HIDE_FROM_DEBUGGER).
using pfnNtCreateThreadEx = NTSTATUS(NTAPI*)(
    OUT PHANDLE ThreadHandle,
    IN  ACCESS_MASK DesiredAccess,
    IN  PVOID ObjectAttributes,
    IN  HANDLE ProcessHandle,
    IN  PVOID StartRoutine,
    IN  PVOID Argument,
    IN  ULONG CreateFlags,
    IN  SIZE_T ZeroBits,
    IN  SIZE_T StackSize,
    IN  SIZE_T MaximumStackSize,
    IN  PVOID AttributeList
);

// No hide flag.
constexpr ULONG kThreadCreateFlagsHideFromDebugger = 0;

// Sentinel / heartbeat names -- centralized in Naming.h.
inline std::wstring sentinelMutexNameForPid(DWORD pid) {
    return std::wstring(kSentinelPrefix) + std::to_wstring(pid);
}

enum StrategyFlag : uint32_t {
    Strategy_LoadLibrary     = 0,
    Strategy_MapPrivate      = 1,
    Strategy_MapSectionImage = 2,
    Strategy_MapStomp        = 3,
};

constexpr uint32_t kHeartbeatFlagAttached = 1u << 0;

#pragma pack(push, 8)
struct HeartbeatSharedContext {
    uint32_t magic;
    uint32_t version;
    uint32_t strategy;
    uint32_t flags;
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
    return std::wstring(kHeartbeatPrefix) + std::to_wstring(pid);
}

} // namespace wrath::bypass
