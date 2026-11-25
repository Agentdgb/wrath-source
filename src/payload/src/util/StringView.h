#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dt::str {

inline std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    std::size_t                   from = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == sep) { out.emplace_back(s.substr(from, i - from)); from = i + 1; }
    }
    out.emplace_back(s.substr(from));
    return out;
}

inline bool starts_with(std::string_view s, std::string_view p) noexcept {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

inline bool ends_with(std::string_view s, std::string_view p) noexcept {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

inline std::string hex(const std::uint8_t* p, std::size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out[2 * i]     = kHex[(p[i] >> 4) & 0xF];
        out[2 * i + 1] = kHex[p[i] & 0xF];
    }
    return out;
}

inline std::string replace_all(std::string s, std::string_view what, std::string_view with) {
    std::size_t pos = 0;
    while ((pos = s.find(what, pos)) != std::string::npos) {
        s.replace(pos, what.size(), with);
        pos += with.size();
    }
    return s;
}

} // namespace dt::str
