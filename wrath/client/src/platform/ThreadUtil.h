#pragma once
#include <functional>
#include <string>
#include <windows.h>

namespace vanta::platform {

// Spawn a detached daemon thread with a debugger-visible name.
//
// Built on _beginthreadex, so CRT per-thread state exists — sleep_for, C++
// exceptions and thread_local destructors all depend on it.
//
// The cost, measured in tools/testhost/refexp: a live CRT thread holds a
// reference on the containing module, released only when the thread RETURNS.
// FreeLibraryAndExitThread never returns, so the freeing thread can never be
// one of these; see spawnReaper.
bool spawnNamed(std::string name, std::function<void()> fn);

// Threads spawned by spawnNamed that have not yet finished, INCLUDING the
// caller if it is one. Each of them holds a module reference, so the unload
// path waits for this to reach zero before freeing the image.
int liveThreadCount() noexcept;

// Spawn the bare OS thread that performs the final unload. It holds no module
// reference and must use only Win32 calls and atomics (including a failure
// notification). Returns false if creation fails; the DLL stays resident.
using ReaperThreadCreator = HANDLE(WINAPI*)(LPSECURITY_ATTRIBUTES, SIZE_T,
    LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
bool spawnReaper(void (*fn)(), ReaperThreadCreator create = &CreateThread);

// Set current thread name (safe no-op if unsupported).
void setCurrentThreadName(const char* name);

} // namespace vanta::platform
