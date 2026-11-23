#pragma once
#ifndef WRATH_OVERLOAD_INJECTOR_H
#define WRATH_OVERLOAD_INJECTOR_H

#include <windows.h>
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

#include "overload_abi.h"

namespace wrath::injector::overload {

struct Slot {
    void*             payload    = nullptr;
    void*             remoteStub = nullptr;
    SIZE_T            stubSize   = 0;
    DWORD             oldProtect = 0;
    std::vector<BYTE> original;
    HANDLE            hSection   = nullptr;
    OverloadParams    params{};
    bool              live       = false;
};

inline std::unordered_map<DWORD, Slot>& ActiveSlots() {
    static std::unordered_map<DWORD, Slot> s;
    return s;
}

inline std::mutex& SlotsMutex() {
    static std::mutex m;
    return m;
}

#ifndef WRATH_PAYLOAD_NAME
#define WRATH_PAYLOAD_NAME L"display_service64.dll"
#endif
#define WRATH_BOOTSTRAP "WrathBootstrap"
#define WRATH_SHUTDOWN  "WrathShutdown"

using PFN_NtCreateSection = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
using PFN_NtMapView        = LONG(NTAPI*)(HANDLE, HANDLE, PVOID*, ULONG_PTR, SIZE_T, PLARGE_INTEGER, PSIZE_T, ULONG, ULONG, ULONG);
using PFN_NtUnmapView      = LONG(NTAPI*)(HANDLE, PVOID);
using pfnNtCreateThreadEx  = LONG(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE, PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);

inline void* NtFn(const char* n) { return (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), n); }

inline bool RemoteText(HANDLE hp, void* base, WORD& machine, IMAGE_SECTION_HEADER& text) {
    IMAGE_DOS_HEADER dos{}; SIZE_T r{};
    if (!ReadProcessMemory(hp, base, &dos, sizeof(dos), &r) || dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS nt{};
    if (!ReadProcessMemory(hp, (BYTE*)base + dos.e_lfanew, &nt, sizeof(nt), &r)) return false;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return false;
    machine = nt.FileHeader.Machine;

    ULONG_PTR secBase = (ULONG_PTR)base + dos.e_lfanew + FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    IMAGE_SECTION_HEADER sh{};
    for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        if (!ReadProcessMemory(hp, (void*)(secBase + i * sizeof(IMAGE_SECTION_HEADER)), &sh, sizeof(sh), &r)) return false;
        if (!memcmp(sh.Name, ".text", 5) && (sh.Characteristics & IMAGE_SCN_MEM_EXECUTE)) {
            text = sh;
            return true;
        }
    }
    return false;
}

inline bool FindTarget(HANDLE hp, DWORD pid, WORD wantMachine, SIZE_T need, void*& outBase, IMAGE_SECTION_HEADER& outText) {
    static const wchar_t* kPref[] = { L"dbghelp.dll", L"version.dll", L"uxtheme.dll", L"wintrust.dll" };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;

    struct Hit { std::wstring name; void* base; };
    std::vector<Hit> hits;
    MODULEENTRY32W me{ sizeof(me) };
    if (Module32FirstW(snap, &me)) {
        do {
            for (auto* p : kPref) {
                if (_wcsicmp(me.szModule, p) == 0) {
                    hits.push_back({ me.szModule, me.modBaseAddr });
                    break;
                }
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);

    for (auto& h : hits) {
        WORD mach{}; IMAGE_SECTION_HEADER txt{};
        if (!RemoteText(hp, h.base, mach, txt)) continue;
        if (mach != wantMachine) continue;
        SIZE_T guard = (txt.Misc.VirtualSize / 8 > 0x1000) ? (txt.Misc.VirtualSize / 8) : 0x1000;
        if (txt.Misc.VirtualSize < need + guard + 0x1000) continue;
        outBase = h.base;
        outText = txt;
        return true;
    }
    return false;
}

inline HANDLE SpawnThread(HANDLE hp, void* start, void* arg) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto pNt = reinterpret_cast<pfnNtCreateThreadEx>(GetProcAddress(ntdll, "NtCreateThreadEx"));
        if (pNt) {
            HANDLE h = nullptr;
            // 0x4 = THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER
            if (pNt(&h, THREAD_ALL_ACCESS, nullptr, hp, start, arg, 0x4, 0, 0, 0, nullptr) >= 0 && h) {
                return h;
            }
        }
    }
    return CreateRemoteThread(hp, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(start), arg, 0, nullptr);
}

inline bool MapPayload(HANDLE hp, const std::wstring& path, void*& base, HANDLE& hSec) {
    HANDLE hf = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_EXECUTE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return false;

    auto pCreate = (PFN_NtCreateSection)NtFn("NtCreateSection");
    auto pMap    = (PFN_NtMapView)NtFn("NtMapViewOfSection");
    if (!pCreate || !pMap) { CloseHandle(hf); return false; }

    HANDLE sec = nullptr;
    LONG st = pCreate(&sec, SECTION_ALL_ACCESS, nullptr, nullptr, PAGE_EXECUTE_READ, SEC_IMAGE, hf);
    CloseHandle(hf);
    if (st < 0) return false;

    void* addr = nullptr;
    SIZE_T view = 0;
    st = pMap(sec, hp, &addr, 0, 0, nullptr, &view, 1 /*ViewShare*/, 0, PAGE_EXECUTE_READ);
    if (st < 0) { CloseHandle(sec); return false; }

    base = addr;
    hSec = sec;
    return true;
}

inline bool OverloadInject(DWORD pid, const std::wstring& payloadPath, Slot& slot, std::wstring& out_err) {
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) {
        out_err = L"OpenProcess falló para PID " + std::to_wstring(pid);
        return false;
    }

    HANDLE f = CreateFileW(payloadPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        CloseHandle(hp);
        out_err = L"No se pudo abrir el payload en disco: " + payloadPath;
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS nt{};
    DWORD rd = 0;
    bool ok = ReadFile(f, &dos, sizeof(dos), &rd, nullptr) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    if (ok) {
        LARGE_INTEGER off{};
        off.QuadPart = dos.e_lfanew;
        ok = SetFilePointerEx(f, off, nullptr, FILE_BEGIN)
          && ReadFile(f, &nt, sizeof(nt), &rd, nullptr)
          && nt.Signature == IMAGE_NT_SIGNATURE
          && (nt.FileHeader.Characteristics & IMAGE_FILE_DLL);
    }
    CloseHandle(f);
    if (!ok) {
        CloseHandle(hp);
        out_err = L"El payload no es un binario PE DLL válido.";
        return false;
    }

    WORD self = 0;
    {
        HMODULE me = GetModuleHandleW(nullptr);
        auto* d = (IMAGE_DOS_HEADER*)me;
        self = ((IMAGE_NT_HEADERS*)((BYTE*)me + d->e_lfanew))->FileHeader.Machine;
    }
    if (self != nt.FileHeader.Machine) {
        CloseHandle(hp);
        out_err = L"Incompatibilidad de arquitectura entre loader y payload.";
        return false;
    }

    BYTE* img = (BYTE*)GetModuleHandleW(nullptr);
    auto* snt = (IMAGE_NT_HEADERS*)(img + ((IMAGE_DOS_HEADER*)img)->e_lfanew);
    IMAGE_SECTION_HEADER* sst = nullptr;
    auto* secFirst = IMAGE_FIRST_SECTION(snt);
    for (WORD i = 0; i < snt->FileHeader.NumberOfSections; ++i) {
        auto* s = secFirst + i;
        if (!memcmp(s->Name, ".wstub", 6)) {
            sst = s;
            break;
        }
    }
    if (!sst) {
        CloseHandle(hp);
        out_err = L"Sección .wstub no encontrada en el loader.";
        return false;
    }

    SIZE_T stubSize = sst->Misc.VirtualSize;
    SIZE_T stubOff  = OVERLOAD_PARAMS_END; // 0x80
    SIZE_T strOff   = stubOff + stubSize;
    SIZE_T total    = strOff + sizeof(WRATH_BOOTSTRAP) + sizeof(WRATH_SHUTDOWN);

    void* modBase = nullptr;
    IMAGE_SECTION_HEADER txt{};
    if (!FindTarget(hp, pid, nt.FileHeader.Machine, total, modBase, txt)) {
        CloseHandle(hp);
        out_err = L"No se encontró DLL víctima (dbghelp/version) con espacio suficiente en .text";
        return false;
    }

    SIZE_T guard = (txt.Misc.VirtualSize / 8 > 0x1000) ? (txt.Misc.VirtualSize / 8) : 0x1000;
    BYTE* stub = (BYTE*)modBase + txt.VirtualAddress + guard;

    void* mapBase = nullptr;
    HANDLE sec = nullptr;
    if (!MapPayload(hp, payloadPath, mapBase, sec)) {
        CloseHandle(hp);
        out_err = L"NtCreateSection/NtMapViewOfSection (SEC_IMAGE) falló.";
        return false;
    }

    auto* dir = nt.OptionalHeader.DataDirectory;
    slot.params = {};
    slot.params.magic            = OVERLOAD_MAGIC;
    slot.params.imageBase        = (uint64_t)mapBase;
    slot.params.imageSize        = nt.OptionalHeader.SizeOfImage;
    slot.params.epRva            = nt.OptionalHeader.AddressOfEntryPoint;
    slot.params.excRva           = dir[IMAGE_DIRECTORY_ENTRY_EXCEPTION].VirtualAddress;
    slot.params.excCount         = dir[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size / sizeof(RUNTIME_FUNCTION);
    slot.params.bootstrapNameRva = strOff;
    slot.params.shutdownNameRva  = strOff + sizeof(WRATH_BOOTSTRAP);

    DWORD tlsRva = dir[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress;
    if (tlsRva) {
        IMAGE_TLS_DIRECTORY tls{};
        if (ReadProcessMemory(hp, (BYTE*)mapBase + tlsRva, &tls, sizeof(tls), nullptr)) {
            slot.params.tlsCbPtr = tls.AddressOfCallBacks;
        }
    }

    std::vector<BYTE> blob(total, 0);
    memcpy(blob.data(), &slot.params, sizeof(OverloadParams));
    memcpy(blob.data() + stubOff, img + sst->VirtualAddress, stubSize);
    memcpy(blob.data() + strOff, WRATH_BOOTSTRAP, sizeof(WRATH_BOOTSTRAP));
    memcpy(blob.data() + strOff + sizeof(WRATH_BOOTSTRAP), WRATH_SHUTDOWN, sizeof(WRATH_SHUTDOWN));

    auto fail = [&](bool restore) {
        if (restore) WriteProcessMemory(hp, stub, slot.original.data(), total, nullptr);
        ((PFN_NtUnmapView)NtFn("NtUnmapViewOfSection"))(hp, mapBase);
        CloseHandle(sec);
        CloseHandle(hp);
        return false;
    };

    slot.original.resize(total);
    if (!ReadProcessMemory(hp, stub, slot.original.data(), total, nullptr)) {
        out_err = L"ReadProcessMemory del stub original falló.";
        return fail(false);
    }
    if (!VirtualProtectEx(hp, stub, total, PAGE_EXECUTE_READWRITE, &slot.oldProtect)) {
        out_err = L"VirtualProtectEx(RWX) en stub falló.";
        return fail(false);
    }
    if (!WriteProcessMemory(hp, stub, blob.data(), total, nullptr)) {
        out_err = L"WriteProcessMemory del blob en stub falló.";
        return fail(false);
    }
    DWORD tmp = 0;
    VirtualProtectEx(hp, stub, total, slot.oldProtect, &tmp);
    FlushInstructionCache(hp, stub, total);

    HANDLE th = SpawnThread(hp, stub + stubOff, stub);
    if (!th) {
        out_err = L"SpawnThread remoto falló.";
        return fail(true);
    }
    WaitForSingleObject(th, 20000);
    CloseHandle(th);

    ReadProcessMemory(hp, stub, &slot.params, sizeof(OverloadParams), nullptr);
    if (!slot.params.shutdownPtr || !slot.params.teardownPtr) {
        out_err = L"StubInit remoto no resolvió punteros de Shutdown o Teardown.";
        return fail(true);
    }

    slot.payload = mapBase;
    slot.remoteStub = stub;
    slot.stubSize = total;
    slot.hSection = sec;
    slot.live = true;
    CloseHandle(hp);
    return true;
}

inline bool OverloadUnload(DWORD pid, Slot& slot) {
    if (!slot.live || !slot.params.teardownPtr) return false;
    HANDLE hp = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hp) return false;

    HANDLE th = SpawnThread(hp, (void*)slot.params.shutdownPtr, nullptr);
    if (!th) { CloseHandle(hp); return false; }
    WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    if (code != 0) {
        CloseHandle(hp);
        return false;
    }

    HANDLE td = SpawnThread(hp, (void*)slot.params.teardownPtr, (void*)slot.remoteStub);
    if (td) {
        WaitForSingleObject(td, 10000);
        CloseHandle(td);
    }

    SIZE_T isz = (SIZE_T)slot.params.imageSize;
    DWORD old = 0;
    if (isz && VirtualProtectEx(hp, slot.payload, isz, PAGE_READWRITE, &old)) {
        std::vector<BYTE> zero((isz < (1u << 20)) ? isz : (1u << 20), 0);
        for (SIZE_T off = 0; off < isz; off += zero.size()) {
            SIZE_T n = (zero.size() < isz - off) ? zero.size() : isz - off;
            if (!WriteProcessMemory(hp, (BYTE*)slot.payload + off, zero.data(), n, nullptr)) break;
        }
    }
    ((PFN_NtUnmapView)NtFn("NtUnmapViewOfSection"))(hp, slot.payload);

    VirtualProtectEx(hp, slot.remoteStub, slot.stubSize, PAGE_EXECUTE_READWRITE, &old);
    WriteProcessMemory(hp, slot.remoteStub, slot.original.data(), slot.stubSize, nullptr);
    DWORD tmp = 0;
    VirtualProtectEx(hp, slot.remoteStub, slot.stubSize, slot.oldProtect, &tmp);
    FlushInstructionCache(hp, slot.remoteStub, slot.stubSize);

    if (slot.hSection) CloseHandle(slot.hSection);
    slot.live = false;
    CloseHandle(hp);
    return true;
}

} // namespace wrath::injector::overload

#endif // WRATH_OVERLOAD_INJECTOR_H
