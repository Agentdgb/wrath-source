#include "platform/ThreadUtil.h"
#include "platform/StealthThread.h"
#include "jvm/JvmContext.h"

// windows.h MUST come first — it defines the target-architecture symbols
// (_M_X64/_M_IX86) that processthreadsapi.h's #if chain depends on.
#include <windows.h>
#include <processthreadsapi.h>

#include <atomic>
#include <process.h>
#include <utility>

namespace vanta::platform {

// SetThreadDescription lives in KernelBase.dll on modern Windows.
using SetThreadDescription_t = HRESULT (WINAPI*)(HANDLE, PCWSTR);

void setCurrentThreadName(const char* name) {
#ifndef _DEBUG
    (void)name;
#else
    if (!name) return;
    static SetThreadDescription_t p_setName = []() -> SetThreadDescription_t {
        HMODULE k = GetModuleHandleW(L"kernelbase.dll");
        return k ? reinterpret_cast<SetThreadDescription_t>(GetProcAddress(k, "SetThreadDescription"))
                 : nullptr;
    }();
    if (!p_setName) return;

    wchar_t wname[64] = {};
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 63);
    p_setName(GetCurrentThread(), wname);
#endif
}

namespace {

std::atomic<int> g_live{0};

struct Payload {
    std::string           name;
    std::function<void()> fn;
};

unsigned __stdcall threadEntry(void* raw) {
    {
        // Scoped so every destructor has run before the counter drops.
        Payload* p = static_cast<Payload*>(raw);
        setCurrentThreadName(p->name.c_str());
        try { p->fn(); } catch (...) { /* swallow — dispatch threads must not propagate */ }
        delete p;
    }

    vanta::jvm::detachThisThreadIfAttached();
    g_live.fetch_sub(1, std::memory_order_release);
    return 0;
}

} // namespace

bool spawnNamed(std::string name, std::function<void()> fn) {
    auto* p = new Payload{std::move(name), std::move(fn)};

    // Counted BEFORE the thread exists, so the unload path can never observe a
    // gap between "spawned" and "running".
    g_live.fetch_add(1, std::memory_order_acquire);

    /* _beginthreadex, NOT CreateThread. A raw OS thread has no CRT per-thread
       state, and everything these threads run — std::this_thread::sleep_for,
       C++ exceptions, thread_local objects with destructors (jvm::JvmContext
       has one) — assumes it exists. An earlier version used CreateThread to
       dodge the module reference the CRT takes; that reference turned out to be
       the CRT's, not std::thread's, so dodging it that way was buying an unload
       fix with undefined behaviour everywhere else.

       The reference is real and is released when the thread returns normally,
       which is what liveThreadCount() below lets the unload path wait for. */
    HANDLE t = createStealthThread(reinterpret_cast<LPTHREAD_START_ROUTINE>(&threadEntry), p);
    if (!t) {
        const auto error = GetLastError();
        g_live.fetch_sub(1, std::memory_order_release);
        delete p;
        SetLastError(error ? error : ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    CloseHandle(t);
    return true;
}

bool spawnReaper(void (*fn)(), ReaperThreadCreator create) {
    if (!fn || !create) { SetLastError(ERROR_INVALID_PARAMETER); return false; }

    // Production and tests use the same path. The callback owns the checks
    // that must finish before releasing the image; never replace it with a
    // timer or a direct FreeLibrary call.
    struct Bare {
        static DWORD WINAPI entry(LPVOID p) {
            reinterpret_cast<void(*)()>(p)();
            return 0;
        }
    };
    HANDLE t = create(nullptr, 0, &Bare::entry, reinterpret_cast<LPVOID>(fn), 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

int liveThreadCount() noexcept {
    return g_live.load(std::memory_order_acquire);
}

} // namespace vanta::platform
