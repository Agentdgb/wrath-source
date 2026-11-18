#pragma once
// StealthDefs.h -- NT typedefs for the injection pipeline.
// THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER=0x4 hides the thread from debugger
// enumeration and from most screenshare scanner thread-list walks.

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
using pfnCreateTransaction_t =
    HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, LPGUID, DWORD, DWORD,
                    DWORD, DWORD, LPWSTR);
using pfnRollbackTransaction_t = BOOL(WINAPI*)(HANDLE);

} // namespace bp
