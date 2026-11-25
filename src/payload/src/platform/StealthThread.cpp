#include "platform/StealthThread.h"
#include "platform/Win32Process.h"
#include <MinHook.h>

extern "C" void MH_LockTrampPool(void);

#include <windows.h>
#include <atomic>

namespace dt::platform {

namespace {

using fnCreateThread = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
fnCreateThread g_origK32CreateThread = nullptr;
fnCreateThread g_origKbCreateThread  = nullptr;
std::atomic<bool> g_hookInstalled{false};

FARPROC getNtdllThreadStart() {
    static FARPROC p = []() -> FARPROC {
        HMODULE h = GetModuleHandleW(L"ntdll.dll");
        return h ? GetProcAddress(h, "RtlUserThreadStart") : nullptr;
    }();
    return p;
}

HANDLE HandleStealthCreation(
    fnCreateThread orig,
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId)
{
    FARPROC fakeStart = getNtdllThreadStart();
    if (fakeStart && isAddressInOurModule(reinterpret_cast<const void*>(lpStartAddress)))
    {
        DWORD tid = 0;
        const bool callerSuspended = (dwCreationFlags & CREATE_SUSPENDED) != 0;
        HANDLE hThread = orig(
            lpThreadAttributes,
            dwStackSize,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(fakeStart),
            nullptr,
            dwCreationFlags | CREATE_SUSPENDED,
            &tid);

        if (hThread)
        {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            if (GetThreadContext(hThread, &ctx))
            {
                ctx.Rcx = reinterpret_cast<DWORD64>(lpStartAddress);
                ctx.Rdx = reinterpret_cast<DWORD64>(lpParameter);
                SetThreadContext(hThread, &ctx);
            }
            if (!callerSuspended)
            {
                ResumeThread(hThread);
            }
            if (lpThreadId) *lpThreadId = tid;
            return hThread;
        }
    }
    return orig(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

HANDLE WINAPI hkK32CreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId)
{
    return HandleStealthCreation(g_origK32CreateThread, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

HANDLE WINAPI hkKbCreateThread(
    LPSECURITY_ATTRIBUTES lpThreadAttributes,
    SIZE_T dwStackSize,
    LPTHREAD_START_ROUTINE lpStartAddress,
    LPVOID lpParameter,
    DWORD dwCreationFlags,
    LPDWORD lpThreadId)
{
    return HandleStealthCreation(g_origKbCreateThread, lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter, dwCreationFlags, lpThreadId);
}

} // namespace

HANDLE createStealthThread(LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, LPDWORD tid) {
    FARPROC fakeStart = getNtdllThreadStart();
    if (!fakeStart) {
        return CreateThread(nullptr, 0, fn, arg, flags, tid);
    }

    DWORD localTid = 0;
    const bool callerSuspended = (flags & CREATE_SUSPENDED) != 0;
    HANDLE th = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(fakeStart), nullptr, flags | CREATE_SUSPENDED, &localTid);
    if (!th) return nullptr;

    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(th, &ctx)) {
        ctx.Rcx = reinterpret_cast<DWORD64>(fn);
        ctx.Rdx = reinterpret_cast<DWORD64>(arg);
        SetThreadContext(th, &ctx);
    }
    if (!callerSuspended) {
        ResumeThread(th);
    }
    if (tid) *tid = localTid;
    return th;
}

bool installStealthThreadHook() {
    if (g_hookInstalled.exchange(true)) return true;

    MH_Initialize();

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        void* targetK32 = reinterpret_cast<void*>(GetProcAddress(hK32, "CreateThread"));
        if (targetK32) {
            if (MH_CreateHook(targetK32, reinterpret_cast<void*>(&hkK32CreateThread), reinterpret_cast<void**>(&g_origK32CreateThread)) == MH_OK) {
                MH_EnableHook(targetK32);
            }
        }
    }

    HMODULE hKb = GetModuleHandleW(L"kernelbase.dll");
    if (hKb) {
        void* targetKb = reinterpret_cast<void*>(GetProcAddress(hKb, "CreateThread"));
        if (targetKb) {
            if (MH_CreateHook(targetKb, reinterpret_cast<void*>(&hkKbCreateThread), reinterpret_cast<void**>(&g_origKbCreateThread)) == MH_OK) {
                MH_EnableHook(targetKb);
            }
        }
    }

    MH_LockTrampPool();
    return true;
}

void uninstallStealthThreadHook() {
    if (!g_hookInstalled.exchange(false)) return;

    HMODULE hK32 = GetModuleHandleW(L"kernel32.dll");
    if (hK32) {
        void* targetK32 = reinterpret_cast<void*>(GetProcAddress(hK32, "CreateThread"));
        if (targetK32) {
            MH_DisableHook(targetK32);
            MH_RemoveHook(targetK32);
        }
    }

    HMODULE hKb = GetModuleHandleW(L"kernelbase.dll");
    if (hKb) {
        void* targetKb = reinterpret_cast<void*>(GetProcAddress(hKb, "CreateThread"));
        if (targetKb) {
            MH_DisableHook(targetKb);
            MH_RemoveHook(targetKb);
        }
    }

    MH_Uninitialize();
}

} // namespace dt::platform
