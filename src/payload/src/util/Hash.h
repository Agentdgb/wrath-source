#pragma once
#include <cstdint>
#include <string_view>

namespace dt {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnvPrime  = 0x100000001b3ull;

constexpr std::uint64_t fnv1a64(std::string_view s, std::uint64_t seed = kFnvOffset) noexcept {
    std::uint64_t h = seed;
    for (unsigned char c : s) { h ^= c; h *= kFnvPrime; }
    return h;
}

constexpr std::uint64_t operator""_fnv(const char* s, std::size_t n) noexcept {
    return fnv1a64({s, n});
}

} // namespace dt
