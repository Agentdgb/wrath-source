#pragma once
#include <string>
#include <cstddef>
#include <cstdint>

namespace vanta::crypto {

template <size_t N, uint32_t Seed = 0x5A5A5A5Au>
class XorString {
    char data_[N];
    static constexpr uint8_t key(size_t i) noexcept {
        return static_cast<uint8_t>(((Seed >> (i % 24)) ^ (i * 0x9Eu)) & 0xFF | 0x21);
    }
public:
    constexpr XorString(const char (&str)[N]) : data_{} {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = str[i] ^ key(i);
        }
    }

    std::string str() const {
        std::string s(N - 1, '\0');
        for (size_t i = 0; i < N - 1; ++i) {
            s[i] = data_[i] ^ key(i);
        }
        return s;
    }
};

} // namespace vanta::crypto

#define XOR_STR(s) ([]() -> std::string { \
    constexpr uint32_t _seed = static_cast<uint32_t>((__LINE__ * 0x1E35A7BDu) ^ (__COUNTER__ * 0x85EBCA6Bu) ^ 0xA55AA55Au); \
    static constexpr ::vanta::crypto::XorString<sizeof(s), _seed> xs(s); \
    return xs.str(); \
}())

