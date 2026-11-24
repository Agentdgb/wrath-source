#pragma once
#ifndef WRATH_CORE_HASH_H
#define WRATH_CORE_HASH_H

#include <cstdint>
#include <cstddef>

namespace wrath::core {

using hash_t = uint32_t;

namespace detail {
    constexpr uint64_t kFnv64Offset = 14695981039346656037ULL;
    constexpr uint64_t kFnv64Prime  = 1099511628211ULL;

    constexpr char to_lower_char(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    }

    constexpr wchar_t to_lower_wchar(wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
    }
} // namespace detail

// Compile-time or runtime FNV-1a 64-bit truncated to 32-bit (ASCII, case-sensitive)
constexpr hash_t fnv1a(const char* str, size_t len) {
    uint64_t val = detail::kFnv64Offset;
    for (size_t i = 0; i < len; ++i) {
        val ^= static_cast<uint8_t>(str[i]);
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

constexpr hash_t fnv1a(const char* str) {
    uint64_t val = detail::kFnv64Offset;
    while (*str) {
        val ^= static_cast<uint8_t>(*str++);
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

// Case-insensitive ASCII FNV-1a
constexpr hash_t fnv1a_ci(const char* str, size_t len) {
    uint64_t val = detail::kFnv64Offset;
    for (size_t i = 0; i < len; ++i) {
        val ^= static_cast<uint8_t>(detail::to_lower_char(str[i]));
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

constexpr hash_t fnv1a_ci(const char* str) {
    uint64_t val = detail::kFnv64Offset;
    while (*str) {
        val ^= static_cast<uint8_t>(detail::to_lower_char(*str++));
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

// Case-insensitive wide char FNV-1a (for PEB BaseDllName comparison)
constexpr hash_t fnv1a_ci_w(const wchar_t* str, size_t len) {
    uint64_t val = detail::kFnv64Offset;
    for (size_t i = 0; i < len; ++i) {
        wchar_t c = detail::to_lower_wchar(str[i]);
        val ^= static_cast<uint8_t>(c & 0xFF);
        val *= detail::kFnv64Prime;
        val ^= static_cast<uint8_t>((c >> 8) & 0xFF);
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

constexpr hash_t fnv1a_ci_w(const wchar_t* str) {
    uint64_t val = detail::kFnv64Offset;
    while (*str) {
        wchar_t c = detail::to_lower_wchar(*str++);
        val ^= static_cast<uint8_t>(c & 0xFF);
        val *= detail::kFnv64Prime;
        val ^= static_cast<uint8_t>((c >> 8) & 0xFF);
        val *= detail::kFnv64Prime;
    }
    return static_cast<hash_t>(val & 0xFFFFFFFF);
}

} // namespace wrath::core

// String literal operator for compile-time hash resolution: "GetProcAddress"_h
constexpr wrath::core::hash_t operator""_h(const char* str, size_t len) {
    return wrath::core::fnv1a(str, len);
}

#endif // WRATH_CORE_HASH_H
