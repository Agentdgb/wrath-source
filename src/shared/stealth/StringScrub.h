#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vanta::util {

// Clear the live characters only: capacity beyond size is not writable string
// storage. This does not guarantee removal of copies held elsewhere.
inline void scrubString(std::string& s) noexcept {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    std::string{}.swap(s);
}

inline void scrubStringVector(std::vector<std::string>& vec) noexcept {
    for (auto& s : vec) {
        scrubString(s);
    }
    std::vector<std::string>{}.swap(vec);
}

} // namespace vanta::util
