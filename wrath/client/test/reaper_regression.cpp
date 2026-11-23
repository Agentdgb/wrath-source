#include "platform/ThreadUtil.h"
#include "platform/ModuleRelease.h"
#include <iostream>

namespace vanta::jvm { void detachThisThreadIfAttached() {} }

namespace {
HANDLE completed = nullptr;
volatile LONG callbacks = 0;
int customCreations = 0;

void finish() {
    InterlockedIncrement(&callbacks);
    SetEvent(completed);
}

HANDLE WINAPI failCreation(LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE,
    LPVOID, DWORD, LPDWORD) {
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

HANDLE WINAPI customCreation(LPSECURITY_ATTRIBUTES attributes, SIZE_T stack,
    LPTHREAD_START_ROUTINE entry, LPVOID argument, DWORD flags, LPDWORD id) {
    ++customCreations;
    return CreateThread(attributes, stack, entry, argument, flags, id);
}
}

int main() {
    using vanta::platform::spawnReaper;
    DWORD releaseError = ERROR_SUCCESS;
    if (vanta::platform::isRegisteredDll(nullptr, releaseError)
        || releaseError != ERROR_INVALID_HANDLE) return 9;
    if (vanta::platform::isRegisteredDll(GetModuleHandleW(nullptr), releaseError)) return 10;
    if (!vanta::platform::isRegisteredDll(GetModuleHandleW(L"kernel32.dll"), releaseError)
        || releaseError != ERROR_SUCCESS) return 11;
    void* allocation = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocation) return 12;
    const bool acceptedPrivateMemory = vanta::platform::isRegisteredDll(
        reinterpret_cast<HMODULE>(allocation), releaseError);
    VirtualFree(allocation, 0, MEM_RELEASE);
    if (acceptedPrivateMemory || releaseError == ERROR_SUCCESS) return 13;
    if (spawnReaper(nullptr) || GetLastError() != ERROR_INVALID_PARAMETER) return 1;
    if (spawnReaper(&finish, nullptr) || GetLastError() != ERROR_INVALID_PARAMETER) return 2;
    if (spawnReaper(&finish, &failCreation)
        || GetLastError() != ERROR_NOT_ENOUGH_MEMORY || callbacks != 0) return 3;

    completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completed) return 4;
    // Exercise the default production path, not just a mock creator.
    if (!spawnReaper(&finish) || WaitForSingleObject(completed, 5000) != WAIT_OBJECT_0) return 5;
    if (InterlockedCompareExchange(&callbacks, 0, 0) != 1) return 6;
    if (!spawnReaper(&finish, &customCreation)
        || WaitForSingleObject(completed, 5000) != WAIT_OBJECT_0) return 7;
    if (customCreations != 1 || InterlockedCompareExchange(&callbacks, 0, 0) != 2) return 8;
    CloseHandle(completed);
    std::cout << "Production callback, creation failures and module release guards verified\n";
    return 0;
}
