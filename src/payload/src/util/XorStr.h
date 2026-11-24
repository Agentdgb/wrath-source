#pragma once
#include <string>
#include <cstddef>
#include <cstdint>

namespace vanta::crypto {

template <size_t N, uint8_t Key = 0x5A>
class XorString {
    char data_[N];
public:
    constexpr XorString(const char (&str)[N]) : data_{} {
        for (size_t i = 0; i < N; ++i) {
            data_[i] = str[i] ^ static_cast<char>(Key + static_cast<uint8_t>(i * 7));
        }
    }

    std::string str() const {
        std::string s(N - 1, '\0');
        for (size_t i = 0; i < N - 1; ++i) {
            s[i] = data_[i] ^ static_cast<char>(Key + static_cast<uint8_t>(i * 7));
        }
        return s;
    }
};

} // namespace vanta::crypto

#define XOR_STR(s) ([]() -> std::string { \
    static constexpr ::vanta::crypto::XorString<sizeof(s), 0x5A> xs(s); \
    return xs.str(); \
}())
