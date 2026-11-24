#pragma once
#ifndef WRATH_CORE_ENC_H
#define WRATH_CORE_ENC_H

#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <utility>

#include "payload/src/core/hash.h"
#include "shared/strings/build_salt.h"

#ifndef BUILD_SALT
#define BUILD_SALT 0xA5C3E1F9D4B28766ULL
#endif

namespace wrath::core {

namespace detail {

constexpr uint64_t mix_key(uint64_t base, size_t line, uint64_t file_hash) {
    uint64_t k = base ^ file_hash;
    k ^= (static_cast<uint64_t>(line) * 0x9E3779B97F4A7C15ULL);
    k = (k ^ (k >> 30)) * 0xBF58476D1CE4E5B9ULL;
    k = (k ^ (k >> 27)) * 0x94D049BB133111EBULL;
    k ^= (k >> 31);
    return k ? k : 0xD1B54A32987E10CUL;
}

} // namespace detail

template <size_t N, uint64_t Key>
struct EncryptedBuffer {
    char data[N];

    constexpr EncryptedBuffer(const char (&str)[N]) : data{} {
        for (size_t i = 0; i < N; ++i) {
            uint8_t k = static_cast<uint8_t>((Key >> ((i % 8) * 8)) ^ (i * 0x5B + 0x33));
            data[i] = static_cast<char>(str[i] ^ k);
        }
    }
};

template <size_t N, uint64_t Key>
class StackString {
public:
    inline explicit StackString(const EncryptedBuffer<N, Key>& enc) noexcept {
        for (size_t i = 0; i < N; ++i) {
            uint8_t k = static_cast<uint8_t>((Key >> ((i % 8) * 8)) ^ (i * 0x5B + 0x33));
            buf_[i] = static_cast<char>(enc.data[i] ^ k);
        }
    }

    inline ~StackString() noexcept {
        SecureZeroMemory(buf_, N);
    }

    StackString(const StackString&) = delete;
    StackString& operator=(const StackString&) = delete;

    StackString(StackString&& other) noexcept {
        for (size_t i = 0; i < N; ++i) {
            buf_[i] = other.buf_[i];
        }
        SecureZeroMemory(other.buf_, N);
    }

    inline const char* get() const noexcept { return buf_; }
    inline operator const char*() const noexcept { return buf_; }
    inline size_t length() const noexcept { return N > 0 ? N - 1 : 0; }
    inline size_t capacity() const noexcept { return N; }

private:
    char buf_[N];
};

template <size_t N>
struct FixedLiteral {
    char value[N];
    constexpr FixedLiteral(const char (&str)[N]) {
        for (size_t i = 0; i < N; ++i) value[i] = str[i];
    }
    static constexpr size_t size = N;
};

template <FixedLiteral Lit>
struct LiteralWrapper {
    static constexpr uint64_t kKey = detail::mix_key(BUILD_SALT, Lit.size, fnv1a(Lit.value, Lit.size));
    static constexpr EncryptedBuffer<Lit.size, kKey> kEnc{Lit.value};

    inline StackString<Lit.size, kKey> decrypt() const noexcept {
        return StackString<Lit.size, kKey>{kEnc};
    }

    inline operator StackString<Lit.size, kKey>() const noexcept {
        return decrypt();
    }
};

} // namespace wrath::core

// C++20 Literal operator: "my_string"_e
template <wrath::core::FixedLiteral Lit>
constexpr auto operator""_e() {
    return wrath::core::LiteralWrapper<Lit>{};
}

// Macro with line + file entropy: WRATH_ENC("my_string")
#define WRATH_ENC(str) []() -> auto { \
    constexpr uint64_t _k = ::wrath::core::detail::mix_key( \
        BUILD_SALT, __LINE__, ::wrath::core::fnv1a(__FILE__) \
    ); \
    constexpr ::wrath::core::EncryptedBuffer<sizeof(str), _k> _enc(str); \
    return ::wrath::core::StackString<sizeof(str), _k>(_enc); \
}()

#endif // WRATH_CORE_ENC_H
