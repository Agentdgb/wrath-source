#include "EmbeddedResource.h"
#include "platform/Win32Process.h"
#include <windows.h>

EXTERN_C IMAGE_DOS_HEADER __ImageBase;

namespace dt::assets {
std::span<const unsigned char> embeddedResource(int id) {
    HMODULE image = dt::platform::currentModule();
    if (!image) {
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&embeddedResource), &image)) {
            image = reinterpret_cast<HMODULE>(&__ImageBase);
        }
    }
    auto resource = FindResourceW(image, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource)
        return {};
    auto data = LoadResource(image, resource);
    return {static_cast<const unsigned char *>(LockResource(data)), SizeofResource(image, resource)};
}
} // namespace dt::assets
