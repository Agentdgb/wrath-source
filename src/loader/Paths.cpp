#include "loader/Paths.h"

#include <windows.h>
#include <fstream>

namespace wrath::loader {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t sep = p.rfind(L'\\');
    return sep != std::wstring::npos ? p.substr(0, sep) : L".";
}

std::wstring resolve_payload(const std::wstring& override_path) {
    if (!override_path.empty()) {
        if (GetFileAttributesW(override_path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return override_path;
        return {};
    }

    const std::wstring dir = exe_dir();
    static const wchar_t* names[] = { L"display_service64.dll", L"payload.dll" };
    for (const wchar_t* name : names) {
        std::wstring candidate = dir + L"\\" + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return {};
}

std::vector<uint8_t> read_payload(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(bytes.data()), sz);
    return bytes;
}

} // namespace wrath::loader
