#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace vanta::platform {

struct ModuleInfo {
    HMODULE     handle;
    std::wstring path;
    std::uint8_t* base;
    std::size_t   size;
};

std::vector<ModuleInfo> enumProcessModules();
HMODULE findJvmModule();
HMODULE currentModule() noexcept;
void    setCurrentModule(HMODULE m) noexcept;
bool    isAddressInOurModule(const void* ptr) noexcept;

std::wstring processExePath();
std::wstring localAppDataDir(); // empty on failure; never a relative fallback
std::wstring appDataDir();   // %LOCALAPPDATA%\WrathClient
std::wstring logDir();       // <dll_dir>  (post-pivot: user wants logs next to DLL)
std::wstring dllDirectory(); // directory containing vantaclient.dll (currentModule())

} // namespace vanta::platform
