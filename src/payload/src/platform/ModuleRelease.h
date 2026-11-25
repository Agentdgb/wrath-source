#pragma once
#include <windows.h>

namespace dt::platform {

// A base address from manual mapping is not automatically a loader handle.
// This is a necessary precondition for FreeLibrary, not proof of unloading.
inline bool isRegisteredDll(HMODULE module, DWORD& error) noexcept {
    error = ERROR_SUCCESS;
    if (!module || module == GetModuleHandleW(nullptr)) {
        error = ERROR_INVALID_HANDLE;
        return false;
    }
    HMODULE registered = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(module), &registered)) {
        error = GetLastError();
        if (!error) error = ERROR_MOD_NOT_FOUND;
        return false;
    }
    if (registered != module) {
        error = ERROR_INVALID_ADDRESS;
        return false;
    }
    return true;
}

} // namespace dt::platform
