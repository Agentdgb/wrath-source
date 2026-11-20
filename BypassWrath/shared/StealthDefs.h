#pragma once
// StealthDefs.h -- NT typedefs for the injection pipeline.
// THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER=0x4: hides the remote thread from
// debugger enumeration and from most screenshare scanner thread-list walks.
// NtCreateThreadEx is called with this flag in ManualMap.h (both RAM and TxF
// paths). The flag is set in the CreateFlags parameter (not DesiredAccess).

#include <windows.h>
#include <cstdint>

namespace bp {

using NTSTATUS = LONG;

#ifndef NTAPI
#define NTAPI __stdcall
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((bp::NTSTATUS)(s)) >= 0)
#endif

using pfnNtCreateThreadEx = NTSTATUS(NTAPI*)(
    OUT PHANDLE         ThreadHandle,
    IN  ACCESS_MASK     DesiredAccess,
    IN  PVOID           ObjectAttributes,
    IN  HANDLE          ProcessHandle,
    IN  PVOID           StartRoutine,
    IN  PVOID           Argument,
    IN  ULONG           CreateFlags,
    IN  SIZE_T          ZeroBits,
    IN  SIZE_T          StackSize,
    IN  SIZE_T          MaximumStackSize,
    IN  PVOID           AttributeList
);

// Hides thread from debugger + scanner thread-list enumeration.
constexpr ULONG kHideThread = 0x4;

using pfnNtCreateSection_t =
    NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, PLARGE_INTEGER,
                     ULONG, ULONG, HANDLE);
using pfnNtMapViewOfSection_t =
    NTSTATUS(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T,
                     PLARGE_INTEGER, PSIZE_T, DWORD, ULONG, ULONG);
using pfnNtUnmapViewOfSection_t =
    NTSTATUS(NTAPI*)(HANDLE, PVOID);

#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x00000001
#endif
#ifndef FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002
#endif

#ifndef FileDispositionInfoEx
#define FileDispositionInfoEx static_cast<FILE_INFO_BY_HANDLE_CLASS>(21)
#endif

struct BW_FILE_DISPOSITION_INFO_EX {
    ULONG Flags;
};

// ── Control Flow Guard (CFG) Definitions (P0 - T2) ─────────────────────────
#ifndef CFG_CALL_TARGET_VALID
#define CFG_CALL_TARGET_VALID 0x00000001
#endif
#ifndef CFG_CALL_TARGET_PROCESSED
#define CFG_CALL_TARGET_PROCESSED 0x00000002
#endif

typedef struct _BW_CFG_CALL_TARGET_INFO {
    ULONG_PTR Offset;
    ULONG_PTR Flags;
} BW_CFG_CALL_TARGET_INFO, *PBW_CFG_CALL_TARGET_INFO;

using pfnSetProcessValidCallTargets_t = BOOL(WINAPI*)(
    HANDLE, PVOID, SIZE_T, ULONG, PBW_CFG_CALL_TARGET_INFO);

// ── NtSetInformationKey (P2 - T18) ─────────────────────────────────────────
using pfnNtSetInformationKey_t = NTSTATUS(NTAPI*)(
    HANDLE, ULONG, PVOID, ULONG);

// ── Remote LDR resolution structures (P0 - T1, P1-[7]) ─────────────────────
struct BW_UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

// Full x64 PEB_LDR_DATA layout (all three list heads needed for LDR insertion)
struct BW_PEB_LDR_DATA {
    ULONG     Length;
    BOOLEAN   Initialized;
    PVOID     SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

// Extended LDR_DATA_TABLE_ENTRY for PEB->Ldr manipulation
struct BW_LDR_DATA_TABLE_ENTRY {
    LIST_ENTRY        InLoadOrderLinks;
    LIST_ENTRY        InMemoryOrderLinks;
    LIST_ENTRY        InInitializationOrderLinks;
    PVOID             DllBase;
    PVOID             EntryPoint;
    ULONG             SizeOfImage;
    BW_UNICODE_STRING FullDllName;
    BW_UNICODE_STRING BaseDllName;
    ULONG             Flags;
    USHORT            LoadCount;        // ObsoleteLoadCount on 8+, still present
    USHORT            TlsIndex;
    LIST_ENTRY        HashLinks;        // in module hash table
    ULONG             TimeDateStamp;
};

} // namespace bp
