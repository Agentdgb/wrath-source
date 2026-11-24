#include <windows.h>
#include "overload_abi.h"

#define SECT ".wstub"
#pragma section(SECT, read, execute)
#pragma code_seg(SECT)

__declspec(allocate(SECT)) const wchar_t kNtdll[]       = L"ntdll.dll";
__declspec(allocate(SECT)) const char    fLdrGetDll[]   = "LdrGetDllHandle";
__declspec(allocate(SECT)) const char    fLdrGetProc[]  = "LdrGetProcedureAddress";
__declspec(allocate(SECT)) const char    fRtlAddFT[]    = "RtlAddFunctionTable";
__declspec(allocate(SECT)) const char    fRtlDelFT[]    = "RtlDeleteFunctionTable";

struct USTR { USHORT Len, Max; PWSTR Buf; };
struct LDR_ENTRY {
    LIST_ENTRY InLoadOrder, InMemoryOrder, InInitOrder;
    PVOID DllBase, EntryPoint; ULONG SizeOfImage;
    USTR FullName, BaseName;
};

struct PEB_LDR_DATA_LOCAL {
    ULONG      Length;
    BOOLEAN    Initialized;
    HANDLE     SsHandle;
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
};

struct PEB_LOCAL {
    BYTE                Reserved1[2];
    BYTE                BeingDebugged;
    BYTE                Reserved2[1];
    PVOID               Reserved3[2];
    PEB_LDR_DATA_LOCAL* Ldr;
};

struct UNICODE_STRING_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};

struct ANSI_STRING_LOCAL {
    USHORT Length;
    USHORT MaximumLength;
    PSTR   Buffer;
};

using PFN_LdrGetDllHandle = LONG(NTAPI*)(PWSTR, PULONG, UNICODE_STRING_LOCAL*, PVOID*);
using PFN_LdrGetProcAddr  = LONG(NTAPI*)(PVOID, ANSI_STRING_LOCAL*, ULONG, PVOID*);

static USHORT StrLenW(const wchar_t* s) { USHORT n = 0; while (s[n]) ++n; return n; }
static USHORT StrLenA(const char* s)    { USHORT n = 0; while (s[n]) ++n; return n; }

static BOOL EqW(const wchar_t* a, const wchar_t* b) {
    while (*a && *b) {
        wchar_t x = *a++, y = *b++;
        if (x >= L'A' && x <= L'Z') x += 32;
        if (y >= L'A' && y <= L'Z') y += 32;
        if (x != y) return FALSE;
    }
    return *a == *b;
}

// Resuelve exports directamente desde la EAT de ntdll (usado únicamente para el bootstrap)
static PVOID GetProcW(const wchar_t* modName, const char* fn) {
    auto* peb = (PEB_LOCAL*)(void*)__readgsqword(0x60);
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    PVOID mod = nullptr;
    for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
        auto* m = CONTAINING_RECORD(e, LDR_ENTRY, InMemoryOrder);
        if (m->BaseName.Buf && EqW(m->BaseName.Buf, modName)) { mod = m->DllBase; break; }
    }
    if (!mod) return nullptr;
    auto* b = (BYTE*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    auto* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress) return nullptr;
    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(b + dir->VirtualAddress);
    auto* names = (DWORD*)(b + exp->AddressOfNames);
    auto* ords  = (WORD*)(b + exp->AddressOfNameOrdinals);
    auto* funcs = (DWORD*)(b + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = (const char*)(b + names[i]);
        const char* q = fn;
        while (*n && *n == *q) { ++n; ++q; }
        if (!*n && !*q) return b + funcs[ords[i]];
    }
    return nullptr;
}

// Resuelve a través del cargador nativo Ldr (cubre forwards y redirecciones ApiSet)
static PVOID NtApi(OverloadParams* p, const wchar_t* mod, const char* fn) {
    auto getDll  = (PFN_LdrGetDllHandle)p->ldrGetDll;
    auto getProc = (PFN_LdrGetProcAddr)p->ldrGetProc;
    if (!getDll || !getProc) return nullptr;

    UNICODE_STRING_LOCAL us{};
    us.Buffer = (PWSTR)mod;
    us.Length = us.MaximumLength = (USHORT)(StrLenW(mod) * sizeof(wchar_t));

    PVOID h = nullptr;
    if (getDll(nullptr, nullptr, &us, &h) < 0 || !h) return nullptr;

    ANSI_STRING_LOCAL as{};
    as.Buffer = (PSTR)fn;
    as.Length = as.MaximumLength = StrLenA(fn);

    PVOID proc = nullptr;
    if (getProc(h, &as, 0, &proc) < 0) return nullptr;
    return proc;
}

static PVOID GetExport(BYTE* img, const char* name) {
    auto* nt = (IMAGE_NT_HEADERS*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    auto* dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress) return nullptr;
    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(img + dir->VirtualAddress);
    auto* names = (DWORD*)(img + exp->AddressOfNames);
    auto* ords  = (WORD*)(img + exp->AddressOfNameOrdinals);
    auto* funcs = (DWORD*)(img + exp->AddressOfFunctions);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* n = (const char*)(img + names[i]);
        const char* q = name;
        while (*n && *n == *q) { ++n; ++q; }
        if (!*n && !*q) return img + funcs[ords[i]];
    }
    return nullptr;
}

extern "C" __declspec(noinline) void StubTeardown(PVOID self);

extern "C" __declspec(noinline) void StubInit(PVOID self) {
    auto* p = (OverloadParams*)self;
    if (p->magic != OVERLOAD_MAGIC) return;
    BYTE* img = (BYTE*)p->imageBase;

    // Asignar dirección de teardown antes de cualquier posible fallo
    p->teardownPtr = (uint64_t)&StubTeardown;
    p->ldrGetDll   = (uint64_t)GetProcW(kNtdll, fLdrGetDll);
    p->ldrGetProc  = (uint64_t)GetProcW(kNtdll, fLdrGetProc);

    // 1. Registrar tabla invertida de excepciones (.pdata)
    if (p->excRva && p->excCount) {
        auto pAdd = (BOOLEAN(WINAPI*)(PVOID, ULONG, DWORD64))NtApi(p, kNtdll, fRtlAddFT);
        if (pAdd) {
            PVOID tbl = img + p->excRva;
            if (pAdd(tbl, (ULONG)p->excCount, (DWORD64)img)) {
                p->funcTable = (uint64_t)tbl;
            }
        }
    }

    // 2. Callbacks TLS en orden directo
    if (p->tlsCbPtr) {
        auto* cb = (ULONG_PTR*)p->tlsCbPtr;
        for (; *cb; ++cb) {
            ((void(NTAPI*)(PVOID, DWORD, PVOID))*cb)(img, DLL_PROCESS_ATTACH, nullptr);
        }
    }

    // 3. DllMain(DLL_PROCESS_ATTACH)
    if (p->epRva) {
        ((BOOL(WINAPI*)(HMODULE, DWORD, PVOID))(img + p->epRva))((HMODULE)img, DLL_PROCESS_ATTACH, nullptr);
    }

    // 4. Resolver dirección de Shutdown
    p->shutdownPtr = (uint64_t)GetExport(img, (const char*)((BYTE*)self + p->shutdownNameRva));

    // 5. Iniciar Bootstrap del payload
    auto boot = (void(*)(void*))GetExport(img, (const char*)((BYTE*)self + p->bootstrapNameRva));
    if (boot) {
        boot(img);
    }
}

extern "C" __declspec(noinline) void StubTeardown(PVOID self) {
    auto* p = (OverloadParams*)self;
    if (p->magic != OVERLOAD_MAGIC) return;
    BYTE* img = (BYTE*)p->imageBase;

    // 1. DllMain(DLL_PROCESS_DETACH)
    if (p->epRva) {
        ((BOOL(WINAPI*)(HMODULE, DWORD, PVOID))(img + p->epRva))((HMODULE)img, DLL_PROCESS_DETACH, nullptr);
    }

    // 2. Callbacks TLS en orden inverso
    if (p->tlsCbPtr) {
        auto* cb = (ULONG_PTR*)p->tlsCbPtr;
        ULONG_PTR* last = cb;
        while (*last) ++last;
        while (last > cb) {
            --last;
            ((void(NTAPI*)(PVOID, DWORD, PVOID))*last)(img, DLL_PROCESS_DETACH, nullptr);
        }
    }

    // 3. Eliminar tabla de excepciones registrada
    if (p->funcTable) {
        auto pDel = (BOOLEAN(WINAPI*)(PVOID))NtApi(p, kNtdll, fRtlDelFT);
        if (pDel) {
            pDel((PVOID)p->funcTable);
        }
        p->funcTable = 0;
    }
}
